"""The NAIVE arm: `websockets` + stdlib `json`, on stock asyncio.

This is the "before" half of the §6 optimization and it is deliberately not a straw man — it
is what a competent engineer writes first, with compression explicitly disabled so the
comparison is about the stack rather than about one arm accidentally negotiating deflate.
At ~6.7 µs/cycle it is profiler-visible against a 50 µs budget, which is what makes the
before/after real rather than staged.

Everything except bytes-on-a-socket lives in `_session.SessionDriver`, shared with the tuned
arm. Two hand-copied session loops would diverge, and the measurement's validity rests on
them not diverging.
"""

import asyncio
import socket
import time
from collections.abc import Callable

import websockets
from websockets.asyncio.client import ClientConnection, connect

from mmclient import protocol
from mmclient._session import (
    CLOSE_NORMAL,
    CLOSE_PROTOCOL,
    MAX_MSG_BYTES,
    Codec,
    Ending,
    SessionDriver,
    is_sendable_close_code,
)
from mmclient.strategy import Strategy

__all__ = ["run_client"]

SUBPROTOCOL = "mm.v1"
TIMER_INTERVAL_S = 0.1
# Matched to picows' own handshake timeout, so neither arm waits longer than the other for a
# connection that is not coming.
HANDSHAKE_TIMEOUT_S = 5.0
# A single send may not block forever. `websockets` applies flow control, so a peer that keeps
# publishing while never reading our commands parks `ws.send()` indefinitely — and because the
# outbox lock spans stamp-and-send, that one stalled send also starves the stale-feed timer,
# which is the one thing guaranteed to still have work to do. A bounded wait turns a silent
# hang into a close we can name.
SEND_TIMEOUT_S = 2.0
# Matched to the tuned arm's CLOSE_GRACE_S, so neither arm waits longer than the other on a
# peer that never answers a close.
CLOSE_GRACE_S = 1.0
# Close codes `websockets` produces ITSELF when it refuses a frame. They are our verdicts even
# though the library reached them, so they end the session as DECIDED rather than PEER_GONE.
_LOCAL_VERDICTS = frozenset({1002, 1007, 1009})

NAIVE_CODEC = Codec(decode=protocol.decode_naive, encode=protocol.encode_naive, name="naive")


def _apply_tcp_nodelay(ws: ClientConnection) -> None:
    """Disable Nagle's algorithm on the client socket connection."""
    raw_sock = ws.transport.get_extra_info("socket")
    if raw_sock is not None:
        raw_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


async def run_client(
    url: str,
    strategy: Strategy,
    *,
    codec: Codec = NAIVE_CODEC,
    stop: asyncio.Event,
    on_sample: Callable[[str, int], None] | None = None,
    timer_interval_s: float = TIMER_INTERVAL_S,
) -> Ending:
    """One connection, driven until the strategy stops or `stop` is set.

    Raises on handshake non-compliance rather than degrading: a session that negotiated
    compression, or that did not get `mm.v1`, is not the session being measured, and
    continuing would silently report a different experiment's numbers.

    Returns WHY it ended, because the caller's reconnect policy is a different decision from
    the close code and cannot be derived from a bare return.
    """
    async with connect(
        url,
        subprotocols=[websockets.Subprotocol(SUBPROTOCOL)],
        compression=None,  # the A/B is the stack, not an accidental deflate negotiation
        max_size=MAX_MSG_BYTES,
        # EVERY POLICY BELOW IS PINNED, not defaulted, because a default that differs between
        # the two libraries is a difference between the two ARMS — and §6 attributes its delta
        # to transport and codec alone.
        #   ping_interval: `websockets` pings every 20 s and drops an unanswered peer 20 s
        #     later; picows sends none. The ENGINE owns keepalive (it pings, and applies its
        #     own idle timeout), so the correct client policy on both arms is to send none.
        #     Answering a server PING is unaffected — the library still auto-PONGs.
        #   open_timeout: 5 s to match picows' handshake timeout; the default is 10.
        #   proxy: `websockets` discovers proxies from the environment and picows dials
        #     directly, so under HTTP_PROXY the two arms would take different network paths
        #     and be timed over different routes.
        #   close_timeout: 1 s to match picows' own close grace. The default is 10, so a peer
        #     that ignored our CLOSE held the naive arm ten times as long as the tuned one.
        ping_interval=None,
        open_timeout=HANDSHAKE_TIMEOUT_S,
        close_timeout=CLOSE_GRACE_S,
        proxy=None,
    ) as ws:
        _apply_tcp_nodelay(ws)
        _assert_handshake(ws)
        driver = SessionDriver(strategy, codec)
        # ONE writer. `_dispatch` stamps a whole batch with consecutive envelope seqs and the
        # caller then awaits each send; between two of those awaits the timer task can stamp
        # and send a batch of its own, putting [1, 3, 4, 2] on a wire whose contract is
        # "contiguous, in order" — and the engine answers a seq gap with 1002. Holding this
        # across stamp AND send makes a batch atomic with respect to the other producer.
        outbox = asyncio.Lock()
        timer = asyncio.create_task(_timer(ws, driver, stop, outbox, timer_interval_s))
        # RACED against `stop`, not merely checked inside the read loop. `_pump` parks on
        # `async for` and only sees the flag after a frame arrives, and the timer's `while
        # not stop.is_set()` exits WITHOUT closing — so on a quiet feed an operator's Ctrl-C
        # closed nothing and the client hung. Same shape as the stale-stop deadlock: a
        # condition that is not a frame cannot be noticed by a loop that only wakes on frames.
        pump = asyncio.create_task(_pump(ws, driver, stop, on_sample, outbox))
        stopper = asyncio.create_task(stop.wait())
        try:
            # The TIMER is supervised too. It was previously started and never awaited, so a
            # codec or send failure inside it died in a task nobody inspected and the client
            # went quiet while looking healthy.
            done, _ = await asyncio.wait(
                {pump, stopper, timer}, return_when=asyncio.FIRST_COMPLETED
            )
            # RAISES what a completed task raised. Ignoring the result turned "the pump died
            # of a RuntimeError" into "run_client returned normally", which the reconnect
            # policy then read as a clean session.
            for task in done:
                task.result()
            return _ending(driver, stopped=stopper in done)
        finally:
            for task in (timer, pump, stopper):
                task.cancel()
            # GATHERED, so cancellation actually completes before the socket goes away and a
            # send in flight cannot outlive the close below.
            await asyncio.gather(timer, pump, stopper, return_exceptions=True)
            driver.on_disconnect()
            async with outbox:
                await _close(ws, driver)


def _ending(driver: SessionDriver, *, stopped: bool) -> Ending:
    """Why the connection ended.

    A LATCHED INTENT WINS over the stop flag, and the order matters: the timer can latch 4000
    and be part-way through sending its cancels when the operator hits Ctrl-C, and reporting
    STOPPED there would describe a session that closed 4000 on the wire as an ordinary
    shutdown. The wire and the return value must agree on why the session ended.
    """
    if driver.close_intent is not None:
        return Ending.DECIDED
    return Ending.STOPPED if stopped else Ending.PEER_GONE


def _assert_handshake(ws: object) -> None:
    """WS-handshake fluency, asserted rather than assumed (plan Task 9). `websockets` will
    happily hand back a session with no subprotocol if the server declined ours."""
    negotiated = getattr(ws, "subprotocol", None)
    if negotiated != SUBPROTOCOL:
        msg = f"server did not accept {SUBPROTOCOL!r} (got {negotiated!r})"
        raise RuntimeError(msg)
    # DEFENCE IN DEPTH, and unreachable through `websockets`: the library rejects an
    # extension the client never offered during the handshake itself, so this branch is the
    # check that would matter if that ever stopped being true. The tuned arm's equivalent IS
    # reachable, and is tested.
    extensions = getattr(ws, "extensions", None) or []
    if extensions:  # pragma: no cover - websockets refuses the response before we see it
        names = ", ".join(str(e) for e in extensions)
        msg = f"server negotiated extensions, which the measured run forbids: {names}"
        raise RuntimeError(msg)


async def _pump(
    ws: object,
    driver: SessionDriver,
    stop: asyncio.Event,
    on_sample: Callable[[str, int], None] | None,
    outbox: asyncio.Lock,
) -> None:
    """The read loop. A CLOSE ends it, and is not an error.

    `websockets` raises ConnectionClosedError for any close whose code is not 1000 — which
    includes the 4000 and 1002 this client sends ON ITS OWN INITIATIVE. Letting that escape
    turned a correct stale-stop into a traceback out of `run_client`, and would have made
    the reconnect policy retry a session that ended exactly as designed.
    """
    try:
        await _read_frames(ws, driver, stop, on_sample, outbox)
    except websockets.exceptions.ConnectionClosed as exc:
        # A close the LIBRARY generated on our behalf — an oversized frame is 1009, invalid
        # UTF-8 is 1007 — is still a verdict WE reached, and must be latched as one. Left
        # unlatched it read as "the peer went away", so the reconnect policy dialled again
        # after a fault that would repeat identically on the next connection.
        #
        # `exc.sent`, NOT `ws.close_code`. The latter is the code we RECEIVED, which gets this
        # wrong in both directions: a peer that initiates 1002 and receives our echo reads as a
        # local verdict when it was theirs, and a close WE generated that the peer never
        # answered reads back as 1006 and is missed entirely. Which side sent it is exactly the
        # distinction being made here.
        # `rcvd_then_sent` is what actually decides it, and the paragraph above described this
        # case without the code testing for it: when the PEER initiates 1002 and we echo it,
        # `exc.sent` is 1002 too, so `sent.code in _LOCAL_VERDICTS` was true for a close that was
        # entirely theirs. The consequence is not cosmetic — a local verdict is DECIDED and
        # terminal, a peer close is PEER_GONE and retryable, so this arm refused to reconnect
        # after an engine-initiated 1002 while the tuned arm retried. That is a behavioural
        # difference between the two §6 arms, which makes it a measurement defect and not just a
        # bug. True means the peer's close arrived FIRST and ours is the echo.
        sent = exc.sent
        peer_first = exc.rcvd_then_sent is True
        if sent is not None and not peer_first and sent.code in _LOCAL_VERDICTS:
            driver.note_protocol_error(f"transport closed {sent.code}", sent.code)


async def _read_frames(
    ws: object,
    driver: SessionDriver,
    stop: asyncio.Event,
    on_sample: Callable[[str, int], None] | None,
    outbox: asyncio.Lock,
) -> None:
    async for raw in ws:  # type: ignore[attr-defined]
        # No stop check here: run_client RACES this coroutine against the event, so a stop
        # is noticed whether or not a frame ever arrives — which a check inside the loop
        # cannot do, and was the reason a quiet feed made Ctrl-C hang.
        #
        # THE TYPE IS THE OPCODE, and that is a protocol fact, not a convenience. `websockets`
        # hands back `str` for TEXT and `bytes` for BINARY; treating them alike accepted a
        # binary frame carrying valid JSON and quoted on it, while the tuned arm closed 1002
        # on the same bytes. The wire is text-only in BOTH directions — the engine applies
        # exactly this rule to us (session.cpp) — so the arms must apply it identically.
        if not isinstance(raw, str):
            driver.note_protocol_error("binary frame: the mm.v1 wire is text-only")
            return
        # The codec takes bytes, so the encode cost is paid on the arm that incurred it.
        payload = raw.encode()
        received = time.perf_counter_ns()
        async with outbox:
            for frame in driver.on_bytes(payload, received):
                await _send_text(ws, frame)
        if on_sample is not None:
            on_sample("decode_dispatch_ns", time.perf_counter_ns() - received)
        if driver.close_intent is not None:
            return


async def _timer(
    ws: object,
    driver: SessionDriver,
    stop: asyncio.Event,
    outbox: asyncio.Lock,
    interval_s: float = TIMER_INTERVAL_S,
) -> None:
    """The stale-feed clock. Its own task because the read loop blocks on the socket: a
    feed that has gone silent produces no wakeup, which is precisely the case being watched
    for."""
    while True:
        # WAITS on stop rather than sleeping through it: a plain sleep makes shutdown pay a
        # full interval it has no reason to, and leaves the loop's own exit unreachable in a
        # test because the task is always cancelled before its condition is re-read.
        try:
            await asyncio.wait_for(stop.wait(), interval_s)
        except TimeoutError:
            pass  # the interval elapsed: do the tick
        else:
            return  # stop was set
        async with outbox:
            for frame in driver.on_timer(time.perf_counter_ns()):
                await _send_text(ws, frame)
        if driver.close_intent is not None:
            # CLOSES, rather than merely returning. The read loop is parked on `async for`
            # and a stale feed produces no frame to wake it — so a timer that only returned
            # left the session hung until something else happened to arrive, which on a dead
            # feed is never. Closing here ends the iterator too.
            async with outbox:
                await _close(ws, driver)
            return


async def _send_text(ws: object, frame: bytes) -> None:
    """Send one command as a TEXT frame, within a bounded wait.

    `websockets` picks the opcode from the ARGUMENT'S PYTHON TYPE — `str` is text, anything
    bytes-like is binary — and both codecs return bytes. So a plain `ws.send(frame)` put every
    command on the wire as a binary frame, which `session.cpp` answers with a 1002 close: this
    arm could not trade against the real engine at all, and the in-test server used by the unit
    suite was more permissive than the engine, so nothing caught it until Task 10.

    `text=True` rather than `frame.decode()` on purpose. Decoding would round-trip
    bytes -> str -> bytes inside the library and charge the naive arm a transcode the tuned arm
    never pays, which would flatter the §6 result by an amount that has nothing to do with the
    stack being measured. The tuned arm needs no equivalent: picows makes the caller NAME the
    opcode (`WSMsgType.TEXT`), so it was right by construction.
    """
    try:
        await asyncio.wait_for(ws.send(frame, text=True), SEND_TIMEOUT_S)  # type: ignore[attr-defined]
    except TimeoutError as exc:
        # The peer is not reading. Naming it beats hanging: an unbounded send holds the outbox
        # lock, which also stops the stale-feed timer from ever running again.
        msg = f"outbound send blocked for {SEND_TIMEOUT_S:g}s: the peer is not reading"
        raise ConnectionResetError(msg) from exc


async def _close(ws: object, driver: SessionDriver) -> None:
    intent = driver.close_intent
    code = intent.code if intent is not None else CLOSE_NORMAL
    reason = intent.reason if intent is not None else "client shutdown"
    # Same table the tuned arm uses: a status we would refuse to echo is not one we initiate.
    # `websockets` rejects 0 itself and accepts 1014; Beast rejects both. Pinning the send
    # path here means neither arm can invent a third table for codes we put on the wire.
    if not is_sendable_close_code(code):
        code = CLOSE_PROTOCOL
    await ws.close(code, reason[:120])  # type: ignore[attr-defined]
