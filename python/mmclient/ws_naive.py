"""The NAIVE arm: `websockets` + stdlib `json`, on stock asyncio.

The "before" half of the A/B, deliberately not a straw man: compression is explicitly disabled so
the comparison is about the stack. Everything but bytes-on-a-socket lives in `SessionDriver`.
"""

import asyncio
import time
from collections.abc import Callable

import websockets
from websockets.asyncio.client import connect

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
# A single send may not block forever: `websockets` applies flow control, and because the outbox
# lock spans stamp-and-send, one stalled send also starves the stale-feed timer.
SEND_TIMEOUT_S = 2.0
# Matched to the tuned arm's CLOSE_GRACE_S, so neither arm waits longer than the other on a
# peer that never answers a close.
CLOSE_GRACE_S = 1.0
# Close codes `websockets` produces ITSELF when it refuses a frame. They are our verdicts even
# though the library reached them, so they end the session as DECIDED rather than PEER_GONE.
_LOCAL_VERDICTS = frozenset({1002, 1007, 1009})

NAIVE_CODEC = Codec(decode=protocol.decode_naive, encode=protocol.encode_naive, name="naive")


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
    compression or missed `mm.v1` is not the one being measured. Returns WHY it ended.
    """
    async with connect(
        url,
        subprotocols=[websockets.Subprotocol(SUBPROTOCOL)],
        compression=None,  # the A/B is the stack, not an accidental deflate negotiation
        max_size=MAX_MSG_BYTES,
        # EVERY POLICY BELOW IS PINNED, not defaulted: a library default that differs is a
        # difference between the ARMS. Keepalive is the ENGINE's; timeouts/proxy match picows.
        ping_interval=None,
        open_timeout=HANDSHAKE_TIMEOUT_S,
        close_timeout=CLOSE_GRACE_S,
        proxy=None,
    ) as ws:
        _assert_handshake(ws)
        driver = SessionDriver(strategy, codec)
        # ONE writer: `_dispatch` stamps a batch with consecutive envelope seqs, and the timer can
        # otherwise interleave its own — [1, 3, 4, 2] on a wire the engine answers 1002 to.
        outbox = asyncio.Lock()
        timer = asyncio.create_task(_timer(ws, driver, stop, outbox, timer_interval_s))
        # RACED against `stop`, not merely checked inside the read loop: `_pump` parks on
        # `async for` and would only see the flag after a frame arrives, hanging on a quiet feed.
        pump = asyncio.create_task(_pump(ws, driver, stop, on_sample, outbox))
        stopper = asyncio.create_task(stop.wait())
        try:
            # The TIMER is supervised too: a codec or send failure inside it must not die in a
            # task nobody inspects while the client goes quiet looking healthy.
            done, _ = await asyncio.wait(
                {pump, stopper, timer}, return_when=asyncio.FIRST_COMPLETED
            )
            # RAISES what a completed task raised: ignoring the result turned "the pump died"
            # into "run_client returned normally", which the reconnect policy read as clean.
            for task in done:
                task.result()
            return _ending(driver, stopped=stopper in done)
        finally:
            for task in (timer, pump, stopper):
                task.cancel()
            # GATHERED, so cancellation completes before the socket goes away and a send in
            # flight cannot outlive the close below.
            await asyncio.gather(timer, pump, stopper, return_exceptions=True)
            driver.on_disconnect()
            async with outbox:
                await _close(ws, driver)


def _ending(driver: SessionDriver, *, stopped: bool) -> Ending:
    """Why the connection ended.

    A LATCHED INTENT WINS over the stop flag: the timer can latch 4000 and still be sending its
    cancels when the operator hits Ctrl-C, and the wire and the return value must agree.
    """
    if driver.close_intent is not None:
        return Ending.DECIDED
    return Ending.STOPPED if stopped else Ending.PEER_GONE


def _assert_handshake(ws: object) -> None:
    """WS-handshake fluency, asserted rather than assumed: `websockets` hands back a session
    with no subprotocol if the server declined ours."""
    negotiated = getattr(ws, "subprotocol", None)
    if negotiated != SUBPROTOCOL:
        msg = f"server did not accept {SUBPROTOCOL!r} (got {negotiated!r})"
        raise RuntimeError(msg)
    # DEFENCE IN DEPTH, and unreachable through `websockets`: the library rejects an extension the
    # client never offered during the handshake itself. The tuned arm's equivalent IS reachable.
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
    includes the 4000 and 1002 this client sends ON ITS OWN INITIATIVE.
    """
    try:
        await _read_frames(ws, driver, stop, on_sample, outbox)
    except websockets.exceptions.ConnectionClosed as exc:
        # A close the LIBRARY generated on our behalf (1009, 1007) is still OUR verdict and must
        # be latched. `exc.sent` + `rcvd_then_sent` decide it, NOT `ws.close_code`: peer-first
        # means ours is only the echo, so the session is PEER_GONE (retryable), not DECIDED.
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
        # No stop check here: run_client RACES this against the stop event. THE TYPE IS THE
        # OPCODE — `str` is TEXT, `bytes` is BINARY — and the mm.v1 wire is text-only both ways.
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
    """The stale-feed clock. Its own task because the read loop blocks on the socket: a feed that
    has gone silent produces no wakeup, which is precisely the case being watched for."""
    while True:
        # WAITS on stop rather than sleeping through it: a plain sleep makes shutdown pay a full
        # interval, and leaves the loop's own exit unreachable in a test.
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
            # CLOSES, rather than merely returning: the read loop is parked on `async for` and a
            # stale feed produces no frame to wake it. Closing here ends the iterator too.
            async with outbox:
                await _close(ws, driver)
            return


async def _send_text(ws: object, frame: bytes) -> None:
    """Send one command as a TEXT frame, within a bounded wait.

    `websockets` picks the opcode from the ARGUMENT'S TYPE and both codecs return bytes, so a bare
    `send()` frames BINARY, which the engine closes 1002 on. `text=True` avoids a transcode.
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
    # `websockets` rejects 0 itself and accepts 1014; Beast rejects both.
    if not is_sendable_close_code(code):
        code = CLOSE_PROTOCOL
    await ws.close(code, reason[:120])  # type: ignore[attr-defined]
