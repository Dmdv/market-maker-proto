"""What the two transports must do at the wire, driven against a real in-test WS server.

These are the cases that cannot be reached from the strategy suite, because they are about
FRAMING and HANDSHAKE rather than about decisions: subprotocol negotiation, the absence of a
negotiated compression extension, control-frame liveness, the reassembly cap, and the close
codes this client sends on its own initiative.

Both arms are driven through the SAME cases wherever the behaviour is shared, because the
§6 measurement is only like-for-like if the two stacks agree on all of it — a difference
here would mean the naive and tuned numbers came from clients that behave differently, not
from clients that are the same client on different transports.

The server is `websockets.serve`, which is deliberately NOT the engine: an in-test server can
be made to do things the engine never would (send a backwards `md_seq`, fragment a message,
refuse a subprotocol), and those are exactly the arms worth pinning.
"""

import asyncio
import json
import re
from collections.abc import AsyncIterator, Awaitable, Callable, Coroutine
from typing import Any, SupportsInt

import picows
import pytest
import websockets
from websockets.asyncio.server import Server, ServerConnection, serve

from mmclient import ws_naive, ws_picows
from mmclient._session import (
    CLOSE_PROTOCOL,
    CloseIntent,
    Ending,
    SessionDriver,
    classify_close_body,
    is_sendable_close_code,
)
from mmclient.app import run_with_reconnect
from mmclient.strategy import Strategy

SYMBOL = "MOCKUSDT"
SUBPROTOCOL = "mm.v1"

Runner = Callable[..., Coroutine[Any, Any, Ending]]


def _strategy(stale_ms: int = 10_000) -> Strategy:
    return Strategy(symbol=SYMBOL, qty=5, max_qty=10, stale_ns=stale_ms * 1_000_000)


def _tob(seq: int, md_seq: int, bid_px: int = 100, ask_px: int = 110, *, epoch: int = 1) -> str:
    return json.dumps(
        {
            "t": "top_of_book",
            "v": 1,
            "seq": seq,
            "epoch": epoch,
            "md_seq": md_seq,
            "symbol": SYMBOL,
            "bid_px": bid_px,
            "bid_qty": 50,
            "ask_px": ask_px,
            "ask_qty": 50,
        }
    )


Script = Callable[["FakeEngine", ServerConnection], Awaitable[None]]


class FakeEngine:
    """One connection's worth of scripted server behaviour, plus what it observed."""

    def __init__(self, script: Script) -> None:
        self.script = script
        self.received: list[dict[str, object]] = []
        self.close_code: int | None = None
        self.close_reason: str = ""
        self.connections = 0
        self.done = asyncio.Event()

    async def handler(self, ws: ServerConnection) -> None:
        self.connections += 1
        try:
            await self.script(self, ws)
        finally:
            self.close_code = ws.close_code
            self.close_reason = ws.close_reason or ""
            self.done.set()

    def _note(self, raw: str | bytes) -> None:
        """Record one client message, INSISTING it arrived as text.

        `websockets` hands back `str` for a TEXT frame and `bytes` for a BINARY one, and
        `json.loads` accepts both — so this double used to record a binary frame as happily as
        a text one, while the real engine closes 1002 on sight of it (`session.cpp`). That gap
        is exactly how the naive arm shipped sending every command as BINARY: the unit suite
        was more permissive than the thing it stood in for, and only Task 10's integration test
        against the real binary caught it. A double that accepts what the real peer rejects is
        worse than no double.
        """
        assert isinstance(raw, str), (
            f"client sent a BINARY frame; the engine answers those with 1002: {raw!r}"
        )
        self.received.append(json.loads(raw))

    async def collect(self, ws: ServerConnection, n: int, timeout: float = 2.0) -> None:
        """Read exactly n client messages, failing the case rather than hanging."""
        for _ in range(n):
            self._note(await asyncio.wait_for(ws.recv(), timeout))

    async def drain(self, ws: ServerConnection, timeout: float = 3.0) -> None:
        """Read until the peer closes. Used where the client CLOSES on its own initiative:
        counting messages there races the close frame, and what the case is actually about
        is what arrived BEFORE it, not how the read loop was timed."""
        try:
            async with asyncio.timeout(timeout):
                async for raw in ws:
                    self._note(raw)
        except TimeoutError, websockets.exceptions.ConnectionClosed:
            pass


Starter = Callable[..., Awaitable[tuple[FakeEngine, str]]]


@pytest.fixture
async def engine() -> AsyncIterator[Starter]:
    """Starts a scripted server on an ephemeral port and hands back its URL."""
    servers: list[Server] = []

    async def start(
        script: Script, *, subprotocol: str | None = SUBPROTOCOL
    ) -> tuple[FakeEngine, str]:
        fake = FakeEngine(script)
        server = await serve(
            fake.handler,
            "127.0.0.1",
            0,
            subprotocols=[websockets.Subprotocol(subprotocol)] if subprotocol else None,
            compression=None,
        )
        servers.append(server)
        port = next(iter(server.sockets)).getsockname()[1]
        return fake, f"ws://127.0.0.1:{port}"

    yield start
    for s in servers:
        s.close()
        await s.wait_closed()


ARMS = [
    pytest.param(ws_naive.run_client, id="naive"),
    pytest.param(ws_picows.run_client, id="tuned"),
]


# --- handshake fluency ------------------------------------------------------------------


@pytest.mark.parametrize("run_client", ARMS)
async def test_the_subprotocol_is_requested_and_verified(
    run_client: Runner, engine: Starter
) -> None:
    """Both arms must ASK for mm.v1 and must check they got it. A client that proceeds on a
    declined subprotocol is speaking a protocol the server never agreed to."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        assert ws.subprotocol == SUBPROTOCOL
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert [m["t"] for m in fake.received] == ["new_order", "new_order"]
    assert [m["cl_id"] for m in fake.received] == ["B-1", "S-1"]
    assert [m["seq"] for m in fake.received] == [1, 2]  # stamped by the driver, contiguous


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_declined_subprotocol_is_refused_not_tolerated(
    run_client: Runner, engine: Starter
) -> None:
    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await asyncio.sleep(0.2)

    _fake, url = await engine(script, subprotocol=None)
    stop = asyncio.Event()
    # RuntimeError EXACTLY, and quickly. `pytest.raises((RuntimeError, Exception))` collapses
    # to `Exception`, so the enclosing wait_for's own TimeoutError satisfied it — the case was
    # equally green on a client that raised correctly and on one that hung until the timeout.
    with pytest.raises(RuntimeError, match=re.escape("mm.v1")):
        await asyncio.wait_for(run_client(url, _strategy(), stop=stop), 5)


@pytest.mark.parametrize("run_client", ARMS)
async def test_no_compression_extension_is_negotiated(run_client: Runner, engine: Starter) -> None:
    """`compression=None` on both sides. An arm that quietly negotiated permessage-deflate
    would be measuring a different experiment — and the engine disables it too."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        assert ws.request is not None
        assert not ws.request.headers.get("Sec-WebSocket-Extensions"), (
            "the client offered a compression extension"
        )
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert len(fake.received) == 2


# --- the two-counter rule, injected at the wire (F-10) -----------------------------------


@pytest.mark.parametrize("run_client", ARMS)
async def test_an_envelope_seq_gap_closes_1002(run_client: Runner, engine: Starter) -> None:
    """The client's verdict on the ENGINE's framing. 1002 is the plan's normative code for
    it, and it must be distinguishable from the stale-stop's 4000."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        await ws.send(_tob(9, 2, bid_px=101))  # seq 2..8 never arrived
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert fake.close_code == 1002
    assert "seq gap" in fake.close_reason


@pytest.mark.parametrize("run_client", ARMS)
async def test_an_md_seq_decrease_closes_1002(run_client: Runner, engine: Starter) -> None:
    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 5))
        await fake.collect(ws, 2)
        await ws.send(_tob(2, 4))  # the book went backwards, which the engine cannot do
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert fake.close_code == 1002
    assert "md_seq decreased" in fake.close_reason


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_stale_feed_closes_4000_after_cancelling(
    run_client: Runner, engine: Starter
) -> None:
    """4000 is PRIVATE and deliberate: a stale-feed stop is a strategy decision, and an
    operator reading close codes must be able to tell it from every transport cause. The
    cancels go out FIRST — a stopping client does not abandon resting orders."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        # DRAIN rather than count: the client sends its cancels and closes immediately
        # after, so a fixed-count read races the close frame. What this case is about is
        # what arrived before the close, and with which code.
        await fake.drain(ws)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(stale_ms=1), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert [m["t"] for m in fake.received[2:]] == ["cancel_order", "cancel_order"]
    assert fake.close_code == 4000
    assert "stale feed" in fake.close_reason


# --- framing hardening -------------------------------------------------------------------


async def test_a_fragmented_message_past_the_cap_is_refused_tuned(engine: Starter) -> None:
    """picows hands fragments up as it parses them, so the reassembly bound is OURS. A
    per-frame cap alone is not a bound: two 40 KiB fragments are each legal and together
    exceed the 64 KiB the engine itself enforces.

    1009, not 1002. This case asserted 1002 until the Task 9 gate pointed out that the two
    codes say different things: a message past the cap is well-formed and TOO LARGE, which is
    the whole reason 1009 exists, and both libraries already answer a single oversized FRAME
    with it. Closing 1002 told the peer it had broken the framing rules when it had not, and
    made the reassembled case disagree with the single-frame case for no reason but that the
    adapter had only one verdict available to latch."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        # str fragments, so the message is TEXT: bytes would open with a BINARY frame, which
        # this client refuses outright for a different and equally deliberate reason.
        await ws.send(["x" * 40_000, "y" * 40_000])  # one message, two fragments
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(ws_picows.run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert fake.close_code == 1009
    assert "64 KiB" in fake.close_reason


async def test_auto_pong_answers_a_server_ping_tuned(engine: Starter) -> None:
    """`enable_auto_pong` is the library's, but "the library does it" is not evidence that
    it did — and a peer that stops answering pings is reaped by the engine's idle timeout."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        pong = await ws.ping()
        await asyncio.wait_for(pong, 3)  # resolves only when the pong comes back
        fake.received.append({"t": "pong_observed"})

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(ws_picows.run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert fake.received == [{"t": "pong_observed"}]


# --- lifecycle ---------------------------------------------------------------------------


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_server_close_resets_the_local_order_picture(
    run_client: Runner, engine: Starter
) -> None:
    """The engine cancels our orders on disconnect (cancel-on-disconnect), so the client
    must NOT carry a stale local picture across the gap — a reconnect is a clean slate by
    construction, and `live_or_pending()` is how that is observable."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        await ws.close(1001, "engine shutdown")

    fake, url = await engine(script)
    strategy = _strategy()
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, strategy, stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert strategy.live_or_pending() == {}


async def test_a_dead_engine_is_retried_exactly_once_then_exits_nonzero() -> None:
    """Not a loop and not a backoff: a client that retries forever is indistinguishable from
    one that is working, which is the worst thing a measurement harness can be."""
    attempts = 0

    async def never_connects(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        nonlocal attempts
        attempts += 1
        msg = "connection refused"
        raise OSError(msg)

    rc = await run_with_reconnect(
        never_connects, "ws://127.0.0.1:1", _strategy(), stop=asyncio.Event(), delay_s=0.01
    )
    assert (attempts, rc) == (2, 1)


async def test_a_transport_blip_is_survived_by_the_single_retry() -> None:
    attempts = 0

    async def fails_once(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            msg = "connection reset"
            raise OSError(msg)
        return Ending.STOPPED

    rc = await run_with_reconnect(
        fails_once, "ws://127.0.0.1:1", _strategy(), stop=asyncio.Event(), delay_s=0.01
    )
    assert (attempts, rc) == (2, 0)


async def test_a_binary_frame_is_refused_1002_tuned() -> None:
    """The mm.v1 wire is text-only and the engine closes 1002 on a binary frame from us, so
    the rule is symmetric. Refused rather than skipped, because a fragmented message OPENS
    with its type frame — skipping that one left the continuations behind it reassembling
    into a message with no head, which reached the decoder as garbage and closed for the
    wrong reason."""
    servers = []

    async def handler(ws: ServerConnection) -> None:
        await ws.send(b"\x00\x01\x02")
        await asyncio.wait_for(ws.wait_closed(), 3)

    server = await serve(
        handler,
        "127.0.0.1",
        0,
        subprotocols=[websockets.Subprotocol(SUBPROTOCOL)],
        compression=None,
    )
    servers.append(server)
    port = next(iter(server.sockets)).getsockname()[1]
    stop = asyncio.Event()
    try:
        task = asyncio.create_task(
            ws_picows.run_client(f"ws://127.0.0.1:{port}", _strategy(), stop=stop)
        )
        await asyncio.sleep(0.3)
        stop.set()
        await asyncio.wait_for(task, 5)
    finally:
        server.close()
        await server.wait_closed()


# --- coverage of the arms nothing above reaches -------------------------------------------


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_negotiated_extension_is_refused(run_client: Runner) -> None:
    """The other half of the compression guard. `compression=None` on our side stops us
    OFFERING deflate, but a server that answers with an extension we never asked for would
    still change what is being measured — so the client checks the response, not its own
    request, and refuses. Uses a hand-rolled server because `websockets.serve` will not
    answer with an extension the client did not offer."""
    offered: list[str] = []

    async def handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        request = await reader.read(2048)
        offered.append(request.decode(errors="replace"))
        key = ""
        for line in request.decode(errors="replace").splitlines():
            if line.lower().startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
        import base64
        import hashlib

        accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        writer.write(
            b"HTTP/1.1 101 Switching Protocols\r\n"
            b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n"
            b"Sec-WebSocket-Protocol: mm.v1\r\n"
            b"Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n"
        )
        await writer.drain()
        await asyncio.sleep(0.4)
        writer.close()

    server = await asyncio.start_server(handler, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    try:
        with pytest.raises(Exception, match=r"extensions|deflate"):
            await asyncio.wait_for(
                run_client(f"ws://127.0.0.1:{port}", _strategy(), stop=asyncio.Event()), 5
            )
    finally:
        server.close()
        await server.wait_closed()


@pytest.mark.parametrize("run_client", ARMS)
async def test_the_sample_hook_sees_every_inbound_frame(
    run_client: Runner, engine: Starter
) -> None:
    """`on_sample` is how Task 11's harness reads per-frame cost out of a live client. It is
    optional, so both arms have a branch that skips it — and an optional hook nothing calls
    is an optional hook nobody notices breaking."""
    samples: list[tuple[str, int]] = []

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        await ws.send(_tob(2, 2, bid_px=101))
        await fake.drain(ws, timeout=1.0)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(
        run_client(url, _strategy(), stop=stop, on_sample=lambda k, v: samples.append((k, v)))
    )
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert len(samples) >= 2
    assert {k for k, _ in samples} == {"decode_dispatch_ns"}
    assert all(v >= 0 for _, v in samples)


async def test_a_control_frame_drives_no_decision_tuned(engine: Starter) -> None:
    """PING and PONG are the library's business, not the protocol's: they must reach neither
    the codec nor the strategy."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        pong = await ws.ping()
        await asyncio.wait_for(pong, 3)
        await fake.drain(ws, timeout=1.0)

    fake, url = await engine(script)
    strategy = _strategy()
    stop = asyncio.Event()
    task = asyncio.create_task(ws_picows.run_client(url, strategy, stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert len([m for m in fake.received if m["t"] == "new_order"]) == 2


async def test_a_multi_fragment_message_inside_the_cap_is_reassembled_tuned(
    engine: Starter,
) -> None:
    """The success path of reassembly, which the breach case cannot reach: three fragments
    of one legal message must arrive at the strategy as ONE message and draw quotes."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        whole = _tob(1, 1)
        third = len(whole) // 3
        await ws.send([whole[:third], whole[third : 2 * third], whole[2 * third :]])
        await fake.collect(ws, 2)
        await fake.drain(ws, timeout=1.0)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(ws_picows.run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)
    assert [m["cl_id"] for m in fake.received] == ["B-1", "S-1"]


@pytest.mark.parametrize("run_client", ARMS)
async def test_the_stop_event_ends_the_session_cleanly(run_client: Runner, engine: Starter) -> None:
    """The operator's Ctrl-C path: `stop` set while the feed is healthy closes 1000, not one
    of the fault codes. A clean exit must be distinguishable from a stale feed."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        await fake.drain(ws, timeout=2.0)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.sleep(0.3)
    stop.set()
    await asyncio.wait_for(task, 5)
    await asyncio.wait_for(fake.done.wait(), 5)
    assert fake.close_code == 1000


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_second_connection_quotes_against_the_engine_s_new_epoch(
    run_client: Runner, engine: Starter
) -> None:
    """What a reconnect actually looks like: the engine assigns epoch 2 and the client must
    follow it. Hardcoding 1 made every tick foreign, so the client sat silent — quoting
    nothing, reporting nothing, exiting never."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1, epoch=2))
        await fake.collect(ws, 2)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert [m["t"] for m in fake.received] == ["new_order", "new_order"]
    assert [m["epoch"] for m in fake.received] == [2, 2]


# --- what the Task 9 gate found: arm divergence and lifecycle ----------------------------
#
# Every case below pins a defect that shipped because nothing looked for it. They are grouped
# here rather than scattered because they share one cause: `websockets` enforces RFC 6455 for
# you and picows does not, so each rule the library was silently applying had to become code
# in the tuned arm — and until it was, the two arms disagreed on the wire while passing a
# suite that only ever asked whether each arm worked on its own.


@pytest.mark.parametrize("run_client", ARMS)
async def test_an_inbound_binary_frame_is_refused_by_both_arms(
    run_client: Runner, engine: Starter
) -> None:
    """The mm.v1 wire is text-only in BOTH directions, and the arms must agree on that.

    The naive arm used to accept a binary frame carrying valid JSON and quote on it, because
    it normalised `str`/`bytes` away before looking — while the tuned arm closed 1002 on the
    same bytes. The frame TYPE is a protocol fact, not a representation detail.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1).encode())  # bytes => a BINARY frame
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert fake.close_code == 1002
    assert fake.received == [], "a binary frame must be refused, never quoted on"


def _raw_frame(opcode: int, payload: bytes, *, fin: bool = True, rsv1: bool = False) -> bytes:
    """One server-to-client WebSocket frame, built by hand.

    These cases are ABOUT raw framing — a reserved bit, an orphan continuation — and neither
    library will emit an illegal frame through its public API, which is the point: a peer that
    does is exactly what the client must survive. Building the bytes states the malformation
    directly instead of reaching into another library's internals to provoke it.

    Server-to-client frames are unmasked (RFC 6455 §5.1), so this is just the two header bytes
    plus an extended length when the payload needs one.
    """
    first = opcode | (0x80 if fin else 0) | (0x40 if rsv1 else 0)
    if len(payload) < 126:
        header = bytes([first, len(payload)])
    else:
        header = bytes([first, 126]) + len(payload).to_bytes(2, "big")
    return header + payload


OP_TEXT = 0x1
OP_CONTINUATION = 0x0
OP_CLOSE = 0x8
OP_PING = 0x9


async def _send_raw(ws: ServerConnection, frame: bytes) -> None:
    ws.transport.write(frame)


async def test_a_reserved_bit_is_refused_tuned(engine: Starter) -> None:
    """No extension is negotiated, so RFC 6455 §5.2 makes any set RSV bit a protocol error.

    `websockets` fails the connection itself, which is why only the tuned arm needed this
    written out: picows hands the frame up with the bit set and the listener used to decode it
    and quote on it.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await _send_raw(ws, _raw_frame(OP_TEXT, _tob(1, 1).encode(), rsv1=True))
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(ws_picows.run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert fake.received == [], "a frame with a reserved bit must never reach the decoder"
    # The REFUSAL, not merely the silence. Without this the case passes on a client that
    # ignored the frame and let the server's own timeout close the connection — which is the
    # shape of a test that is green whether or not the behaviour exists.
    assert fake.close_code == 1002


async def test_an_orphan_continuation_is_refused_tuned(engine: Starter) -> None:
    """A CONTINUATION with no message open is a framing error, not a complete message.

    The listener treated a FIN continuation as a whole message and quoted on its contents.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await _send_raw(ws, _raw_frame(OP_CONTINUATION, _tob(1, 1).encode()))
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(ws_picows.run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert fake.received == [], "an orphan continuation must never reach the decoder"
    assert fake.close_code == 1002  # refused, not merely ignored — see the RSV case above


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_peer_close_is_echoed_with_the_code_it_carried(
    run_client: Runner, engine: Starter
) -> None:
    """RFC 6455 §5.5.1: a CLOSE we did not initiate is answered, and the sensible answer is
    the code we were given. The tuned arm used to reply 1000 "client shutdown" to a 1001
    "going away" — so the engine's telemetry recorded a reason neither side held, for the
    commonest shutdown there is."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        await ws.close(1001, "going away")

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert fake.close_code == 1001


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_peer_close_reports_peer_gone_so_the_retry_can_fire(
    run_client: Runner, engine: Starter
) -> None:
    """The reconnect policy's ONE reachable trigger, which used to be unreachable.

    Both adapters return normally when the peer closes — that is what a close handshake is —
    so `run_with_reconnect` saw success and exited 0 against an engine that had gone away. The
    ending has to be reported, because "the peer left" and "the operator stopped us" are the
    same absence of an exception.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        await ws.close(1001, "going away")

    _fake, url = await engine(script)
    stop = asyncio.Event()
    ending = await asyncio.wait_for(run_client(url, _strategy(), stop=stop), 5)
    assert ending is Ending.PEER_GONE


@pytest.mark.parametrize("run_client", ARMS)
async def test_an_operator_stop_reports_stopped_not_peer_gone(
    run_client: Runner, engine: Starter
) -> None:
    """The other side of the same distinction: a stop must NOT be retried, or Ctrl-C would
    dial the engine again."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        await asyncio.sleep(2)

    fake, url = await engine(script)
    stop = asyncio.Event()
    strategy = _strategy()
    task = asyncio.create_task(run_client(url, strategy, stop=stop))
    # Stop only once the session is genuinely up and quoting, so this pins the stop path
    # rather than a race against connect.
    for _ in range(250):
        if len(fake.received) >= 2:
            break
        await asyncio.sleep(0.02)
    assert len(fake.received) >= 2, "the client never quoted, so this proves nothing"
    stop.set()
    assert await asyncio.wait_for(task, 5) is Ending.STOPPED


async def test_the_retry_fires_when_the_engine_goes_away() -> None:
    """`run_with_reconnect`'s documented single retry, against the ending that means it."""
    attempts = 0

    async def peer_gone(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        nonlocal attempts
        attempts += 1
        return Ending.PEER_GONE

    rc = await run_with_reconnect(
        peer_gone, "ws://x", _strategy(), stop=asyncio.Event(), delay_s=0.01
    )
    assert (attempts, rc) == (2, 1), "a vanished engine must be retried once, then reported"


async def test_a_deliberate_close_is_never_retried() -> None:
    """A close WE chose is terminal. Retrying it would re-run the decision that ended the
    session — a stale feed is still stale, a protocol fault still faulty."""
    attempts = 0

    async def decided(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        nonlocal attempts
        attempts += 1
        return Ending.DECIDED

    rc = await run_with_reconnect(
        decided, "ws://x", _strategy(), stop=asyncio.Event(), delay_s=0.01
    )
    assert (attempts, rc) == (1, 0)


async def test_a_pump_failure_is_raised_not_swallowed_naive(engine: Starter) -> None:
    """`asyncio.wait` returns completed tasks; ignoring their results turned "the read loop
    died" into "the client finished cleanly", and the reconnect policy then read that as a
    good session."""

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await asyncio.sleep(1)

    _fake, url = await engine(script)

    def boom(_name: str, _value: int) -> None:
        msg = "sample boom"
        raise RuntimeError(msg)

    stop = asyncio.Event()
    with pytest.raises(RuntimeError, match="sample boom"):
        await asyncio.wait_for(ws_naive.run_client(url, _strategy(), stop=stop, on_sample=boom), 5)


async def test_a_blocked_send_cannot_let_the_timer_interleave_a_batch_naive() -> None:
    """The outbox lock, pinned deterministically rather than incidentally.

    The seq assertions elsewhere in this file run UNCONTENDED — one producer at a time — so
    they read [1, 2] whether or not the lock exists. This drives both producers at once with
    the first send deliberately parked: the read loop stamps and begins sending a two-message
    batch, the stale timer wakes mid-batch and wants to send its own cancels, and the wire must
    still carry 1, 2, 3, 4. Without the lock the timer's batch overtakes the parked one and the
    engine sees a gap, which it answers with 1002.

    Driven against the module's own helpers rather than a socket, because "the send blocks
    HERE, and the timer fires WHILE it is blocked" is not something a real peer can be asked
    for reliably.
    """
    released = asyncio.Event()
    timer_tried = asyncio.Event()
    order: list[int] = []

    class ParkedWS:
        """A ws whose FIRST send parks until released, then records every frame's seq."""

        def __init__(self) -> None:
            self.sends = 0

        async def send(self, frame: bytes, text: bool = False) -> None:
            self.sends += 1
            if self.sends == 1:
                await released.wait()
            order.append(json.loads(frame)["seq"])

        async def close(self, code: int, reason: str) -> None:
            return

        async def __aiter__(self) -> AsyncIterator[str]:
            yield _tob(1, 1)
            await asyncio.sleep(3600)  # never a second frame: the timer is the other producer

    ws = ParkedWS()
    strategy = _strategy(stale_ms=0)  # every tick is stale, so the timer always has work
    driver = SessionDriver(strategy, ws_naive.NAIVE_CODEC)
    outbox = asyncio.Lock()
    stop = asyncio.Event()

    # The timer's OWN attempt on the lock is observed, so this case cannot pass by the timer
    # never running: replacing `_timer` with a no-op used to leave order == [1, 2], which
    # satisfied the assertions while proving nothing about contention at all.
    real_timer = ws_naive._timer

    async def watched_timer(*args: object, **kwargs: object) -> None:
        timer_tried.set()
        await real_timer(*args, **kwargs)  # type: ignore[arg-type]

    pump = asyncio.create_task(ws_naive._read_frames(ws, driver, stop, None, outbox))
    await asyncio.sleep(0.05)  # the batch is stamped and parked inside the lock
    timer = asyncio.create_task(watched_timer(ws, driver, stop, outbox))
    await asyncio.wait_for(timer_tried.wait(), 2)
    await asyncio.sleep(0.25)  # the timer has woken, ticked, and wants the lock
    released.set()
    await asyncio.sleep(0.35)
    for task in (pump, timer):
        task.cancel()
    await asyncio.gather(pump, timer, return_exceptions=True)

    # The EXACT order, not merely a sorted one: two orders from the parked batch, then the
    # timer's two cancels. Without the lock the timer's batch overtakes and this reads
    # [1, 3, 4, 2].
    assert order == [1, 2, 3, 4], f"the batch was split by the timer: {order}"


@pytest.mark.parametrize("run_client", ARMS)
async def test_a_server_ping_is_answered_by_both_arms(run_client: Runner, engine: Starter) -> None:
    """Liveness parity, and the case that pins the tuned arm's HAND-ROLLED pong.

    `enable_auto_pong` is now off there, so that RSV validation can see control frames — which
    means the pong is this client's own code and "the library does it" no longer applies. The
    engine keeps its sessions alive by pinging, so an arm that stopped answering would be
    dropped by the engine's idle timeout mid-benchmark.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send(_tob(1, 1))
        await fake.collect(ws, 2)
        pong = await ws.ping(b"are you there")
        await asyncio.wait_for(pong, 3)  # resolves only when the client's PONG arrives
        fake.received.append({"t": "pong_seen"})

    fake, url = await engine(script)
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(url, _strategy(), stop=stop))
    await asyncio.wait_for(fake.done.wait(), 5)
    stop.set()
    await asyncio.wait_for(task, 5)

    assert {"t": "pong_seen"} in fake.received, "the arm never answered the server's ping"


# --- listener-level framing: the checks a socket double cannot observe --------------------
#
# The cases below drive `_Listener` directly instead of over a socket, and they are here rather
# than in the socket suite for a concrete reason: a malformed frame has to be written RAW, and a
# raw frame bypasses `websockets`' own state machine, so the server side never records a proper
# close and `fake.close_code` reads None. The client's verdict is invisible from out there.
#
# Driven directly, the exact close code, the absence of a PONG and the absence of a quote are
# all observable — which is what these findings are actually about. The socket suite still owns
# everything reachable through a real peer.


class _FakeFrame:
    """The subset of `picows.WSFrame` the listener reads. A real one cannot be constructed
    from Python — picows is Cython — and these cases exist precisely to present frames no
    library will emit through its public API.

    CLOSE bodies: an empty payload is an empty close (legal, no status). A two-byte payload
    is an encoded status. Passing `close_code=N` with an empty payload encodes N into the
    body — except N=0, which means the empty form, because that is the only way to spell
    "no status" without colliding with an encoded zero (picows reports both as `NO_INFO`).
    """

    def __init__(
        self,
        msg_type: object,
        payload: bytes = b"",
        *,
        fin: bool = True,
        rsv1: bool = False,
        close_code: object = None,
    ) -> None:
        self.msg_type = msg_type
        self.fin = fin
        self.rsv1 = rsv1
        self.rsv2 = False
        self.rsv3 = False
        if isinstance(close_code, SupportsInt) and not payload:
            code_int = int(close_code)
            # 0 + empty payload = empty body. Encoded zero must pass payload b"\x00\x00".
            if code_int != 0:
                payload = code_int.to_bytes(2, "big")
        self._payload = payload
        self._close_code = close_code

    def get_payload_as_bytes(self) -> bytes:
        return self._payload

    def get_close_code(self) -> object:
        # Mirror picows: empty body -> NO_INFO (0); else the two-byte status.
        if self._close_code is not None:
            return self._close_code
        if len(self._payload) < 2:
            return _Code(0)
        return _Code(int.from_bytes(self._payload[:2], "big"))

    def get_close_message(self) -> bytes:
        if len(self._payload) <= 2:
            return b""
        return self._payload[2:]


class _FakeTransport:
    """Records what the listener asked the wire to do.

    Models picows' post-CLOSE drop: once `is_close_frame_sent` is True every send is a no-op
    (`picows.pyx` `_send` / `_send_buffer`). A double that kept recording was more permissive
    than the real transport, so tests asserting a PONG after our CLOSE were false greens.
    """

    def __init__(self) -> None:
        self.sent: list[bytes] = []
        self.pongs: list[bytes] = []
        self.closes: list[tuple[int, bytes]] = []
        self.is_close_frame_sent = False
        self.is_disconnected = False

    def send(self, msg_type: object, data: bytes) -> None:
        if self.is_close_frame_sent or self.is_disconnected:
            return
        self.sent.append(data)

    def send_pong(self, data: bytes = b"") -> None:
        if self.is_close_frame_sent or self.is_disconnected:
            return
        self.pongs.append(data)

    def send_close(self, code: SupportsInt | None = None, message: bytes = b"") -> None:
        # picows passes a WSCloseCode enum; `_Code` stands in for values that enum has no
        # member for, which is the point of the illegal-code cases. Both are SupportsInt.
        # A second close is also dropped once the first has gone out — same guard.
        if self.is_close_frame_sent or self.is_disconnected:
            return
        self.closes.append((int(code) if code is not None else 0, message))
        self.is_close_frame_sent = True

    def disconnect(self, graceful: bool = True) -> None:
        self.is_disconnected = True


def _listener(
    on_sample: Callable[[str, int], None] | None = None,
) -> tuple[Any, _FakeTransport]:
    return ws_picows._Listener(_strategy(), ws_picows.TUNED_CODEC, on_sample), _FakeTransport()


def _feed(listener: Any, transport: Any, frame: _FakeFrame) -> None:
    """One frame into the listener, through the one deliberately-untyped seam.

    `picows.WSFrame` and `WSTransport` are Cython extension types that cannot be constructed
    from Python — which is the whole reason these cases need doubles, since the frames they
    present are ones no library will emit through its public API. The loose typing lives here
    only, rather than as an ignore comment on each of a dozen call sites.
    """
    listener.on_ws_frame(transport, frame)


async def test_a_reserved_bit_on_a_control_frame_is_refused_tuned() -> None:
    """RSV is validated BEFORE the frame type is dispatched.

    The check originally sat on the data path, so a CONTROL frame carrying a reserved bit
    skipped it entirely: a CLOSE with RSV1 was politely echoed and a PING with RSV1 was
    answered, where `websockets` fails the connection on either. Both directions asserted here,
    because the two took different wrong paths.
    """
    for msg_type, label in (
        (picows.WSMsgType.CLOSE, "close"),
        (picows.WSMsgType.PING, "ping"),
    ):
        listener, transport = _listener()
        frame = _FakeFrame(msg_type, b"\x03\xe9", rsv1=True, close_code=picows.WSCloseCode.OK)
        _feed(listener, transport, frame)

        intent = listener.driver.close_intent
        assert intent is not None, f"an RSV1 {label} must be refused"
        assert intent.code == 1002, f"an RSV1 {label} is a framing fault: {intent}"
        assert transport.closes and transport.closes[0][0] == 1002
        assert transport.pongs == [], f"an RSV1 {label} must not be answered"
        assert transport.sent == [], f"an RSV1 {label} must not draw a quote"


async def test_a_ping_after_our_own_close_is_still_answered_tuned() -> None:
    """Liveness must not stop when quoting does.

    RFC 6455 §5.4 permits control frames right up to the peer's CLOSE, and the naive arm's
    library keeps answering them. The tuned arm's terminal flag used to silence PING the moment
    we decided to close, so between our 4000 and the engine's reply the two arms behaved
    differently — and an engine applying an idle timeout could drop us mid-handshake.
    """
    listener, transport = _listener()
    listener.end_data()  # exactly what run_client does before sending its close frame

    _feed(listener, transport, _FakeFrame(picows.WSMsgType.PING, b"still there?"))
    assert transport.pongs == [b"still there?"], "a ping during our close went unanswered"


async def test_no_data_frame_is_processed_once_the_close_is_decided_tuned() -> None:
    """The other half of the same state split: DATA stops, control does not.

    The ordinary operator-stop path sends a close with no strategy verdict behind it, and it
    used to spend the whole peer-close grace still decoding books and emitting orders down a
    socket it had already said goodbye on.
    """
    listener, transport = _listener()
    listener.end_data()

    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, _tob(1, 1).encode()))
    assert transport.sent == [], "a book arriving after our close must not draw an order"


async def test_an_illegal_peer_close_code_is_latched_as_our_verdict_tuned() -> None:
    """Answering 1002 is not enough — it has to be recorded as OUR verdict.

    Replying correctly while still reporting the session as "the peer went away" made the
    reconnect policy dial a peer whose close handling is broken, which it will be again.
    """
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.CLOSE, b"", close_code=_Code(5000)))

    intent = listener.driver.close_intent
    assert intent is not None and intent.code == 1002, f"illegal code not latched: {intent}"
    assert transport.closes and transport.closes[0][0] == 1002


async def test_a_legal_empty_close_is_answered_with_a_legal_code_tuned() -> None:
    """An empty close body is legal and carries NO status. picows reports it as `NO_INFO = 0`,
    which is the same enum value as an encoded zero — so the body length, not the enum, must
    decide. Echoing 0 puts a two-byte zero status on the wire, which is not a close code."""
    listener, transport = _listener()
    # close_code=0 + empty payload is the EMPTY form (see _FakeFrame); not an encoded zero.
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.CLOSE, b"", close_code=_Code(0)))

    assert transport.closes == [(1000, b"")], f"empty close answered wrongly: {transport.closes}"
    assert listener.driver.close_intent is None, "an empty close is not a fault"


async def test_an_encoded_close_code_zero_is_refused_tuned() -> None:
    """Encoded status 0 is not an empty close. Beast rejects it; so must we.

    The previous branch treated `code == 0` as "empty" and answered 1000, which is exactly how
    an illegal two-byte zero status was laundered into a legal close. Payload `b"\\x00\\x00"`
    is the wire form; empty payload is the other form — see the twin case above.
    """
    listener, transport = _listener()
    _feed(
        listener,
        transport,
        _FakeFrame(picows.WSMsgType.CLOSE, b"\x00\x00", close_code=_Code(0)),
    )

    intent = listener.driver.close_intent
    assert intent is not None and intent.code == 1002, f"encoded 0 must be refused: {intent}"
    assert transport.closes and transport.closes[0][0] == 1002


async def test_a_callback_failure_latches_the_terminal_state_tuned() -> None:
    """An escaping callback exception used to leave the terminal state UNSET, so picows — which
    reacts by closing 1011 — then handed the next already-buffered frame to the same listener,
    which processed it happily."""
    strategy = _strategy()
    listener = ws_picows._Listener(strategy, ws_picows.TUNED_CODEC, _boom)
    transport = _FakeTransport()

    with pytest.raises(RuntimeError, match="sample boom"):
        _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, _tob(1, 1).encode()))

    assert listener.callback_error is not None, "the failure must be recorded with provenance"
    before = len(transport.sent)
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, _tob(2, 2).encode()))
    assert len(transport.sent) == before, "a buffered frame ran after the callback failed"


def _boom(_name: str, _value: int) -> None:
    msg = "sample boom"
    raise RuntimeError(msg)


class _Code:
    """A stand-in for picows' close-code enum, which only accepts values it HAS members for —
    so a test about an illegal code cannot use the real one."""

    def __init__(self, value: int) -> None:
        self.value = value

    def __int__(self) -> int:
        return self.value


# --- the error paths R3 added, which had no cases until the coverage gate said so ----------
#
# Every case below covers a branch that only a misbehaving peer reaches. They are listener-level
# for the same reason the framing cases are: the situations are ones a cooperating server cannot
# be asked to produce on demand.


async def test_nothing_is_processed_after_the_peer_closes_tuned() -> None:
    """A frame arriving behind the peer's CLOSE is not answered and not decided on.

    picows can deliver an already-parsed frame after the CLOSE that preceded it in the buffer;
    the session is over by then, and answering would put a frame on a wire both ends have agreed
    is finished.
    """
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.CLOSE, b"", close_code=_Code(1000)))
    before = (len(transport.sent), len(transport.pongs), len(transport.closes))

    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, _tob(1, 1).encode()))
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.PING, b"late"))
    assert (len(transport.sent), len(transport.pongs), len(transport.closes)) == before


async def test_an_unsolicited_pong_is_ignored_tuned() -> None:
    """We send no pings, so a PONG answers nothing. It is legal and must be dropped without
    reaching the decoder — a PONG payload is not JSON."""
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.PONG, b"unsolicited"))
    assert transport.sent == [] and transport.closes == []
    assert listener.driver.close_intent is None


async def test_a_new_data_frame_during_an_open_message_is_refused_tuned() -> None:
    """Interleaving two messages is a framing error (RFC 6455 §5.4): a TEXT frame while a
    fragmented message is still open would splice two payloads into one."""
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, b'{"a":', fin=False))
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, b'{"b":1}'))

    intent = listener.driver.close_intent
    assert intent is not None and intent.code == 1002, f"interleaving not refused: {intent}"
    assert transport.sent == []


async def test_the_close_grace_reports_a_peer_that_never_answers_tuned() -> None:
    """`await_peer_close` returns False on timeout, and that return is load-bearing: it is what
    tells the teardown path NOT to attempt a graceful drain against a peer that has stopped
    acknowledging, where the flush can block for a kernel TCP timeout."""
    listener, _transport = _listener()
    answered = await listener.await_peer_close(timeout=0.05)
    assert answered is False


async def test_the_close_grace_reports_a_peer_that_does_answer_tuned() -> None:
    """The other half: once the peer's CLOSE has arrived the grace returns immediately and
    True, so the teardown may flush."""
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.CLOSE, b"", close_code=_Code(1000)))
    assert await listener.await_peer_close(timeout=2.0) is True


async def test_a_blocked_send_is_named_rather_than_waited_on_naive() -> None:
    """An unbounded send holds the outbox lock, which also stops the stale-feed timer from ever
    running again — so a peer that stops reading takes the client's liveness with it. Bounded,
    the same situation becomes a close with a reason a reader can act on."""

    class NeverSends:
        async def send(self, frame: bytes, text: bool = False) -> None:
            await asyncio.sleep(3600)

    # The production bound is 2 s; patched down so the case costs milliseconds. The VALUE is
    # not what is being pinned — that a blocked send is named rather than awaited forever is.
    original = ws_naive.SEND_TIMEOUT_S
    ws_naive.SEND_TIMEOUT_S = 0.05
    try:
        with pytest.raises(ConnectionResetError, match="not reading"):
            await asyncio.wait_for(ws_naive._send_text(NeverSends(), b'{"t":"x"}'), 3)
    finally:
        ws_naive.SEND_TIMEOUT_S = original


async def test_an_oversized_frame_is_a_local_verdict_not_a_peer_departure_naive(
    engine: Starter,
) -> None:
    """A close the LIBRARY generated on our behalf is still OUR verdict.

    `websockets` refuses a frame past `max_size` itself and closes 1009. That is a decision this
    client made — through a library — so it must end the session as DECIDED, not PEER_GONE:
    reported as a departure, the reconnect policy dials again after a fault that will repeat
    identically on the next connection. This is the case that reads `exc.sent.code` rather than
    the code we RECEIVED.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await ws.send("x" * (70 * 1024))  # past MAX_MSG_BYTES (64 KiB)
        await asyncio.sleep(1)

    _fake, url = await engine(script)
    stop = asyncio.Event()
    ending = await asyncio.wait_for(ws_naive.run_client(url, _strategy(), stop=stop), 10)
    assert ending is Ending.DECIDED, "a library-generated 1009 is our verdict, not a departure"


async def test_a_second_close_from_the_peer_is_a_parser_verdict_tuned(engine: Starter) -> None:
    """picows refuses a peer that sends CLOSE twice, and transfers that verdict through
    `wait_disconnected()`.

    The teardown path has to CLASSIFY that rather than let it escape: unhandled, "the peer's
    framer is broken" became an exception the reconnect policy could not read. Provenance
    matters here — the same channel carries exceptions from our OWN callbacks, and those must
    still propagate.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        # A RAW close bypasses websockets' state machine, so the library then sends its own on
        # handler exit — two CLOSE frames, which is what picows refuses.
        payload = (1000).to_bytes(2, "big") + b"bye"
        await _send_raw(ws, _raw_frame(OP_CLOSE, payload))
        await asyncio.sleep(0.3)

    _fake, url = await engine(script)
    stop = asyncio.Event()
    ending = await asyncio.wait_for(ws_picows.run_client(url, _strategy(), stop=stop), 10)
    # Either classification is defensible here — what must NOT happen is the exception escaping.
    assert ending in (Ending.DECIDED, Ending.PEER_GONE)


async def test_a_vanished_engine_is_retried_once_then_reported() -> None:
    """`run_with_reconnect`'s second-attempt path: the retry fires, fails again, and the process
    exits non-zero so a supervisor can tell "engine down" from "client patient"."""
    attempts = 0

    async def gone(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        nonlocal attempts
        attempts += 1
        return Ending.PEER_GONE

    rc = await run_with_reconnect(gone, "ws://x", _strategy(), stop=asyncio.Event(), delay_s=0.01)
    assert (attempts, rc) == (2, 1)


class _RaisingTransport(_FakeTransport):
    """A transport whose `wait_disconnected` raises what picows raises there."""

    def __init__(self, exc: BaseException) -> None:
        super().__init__()
        self._exc = exc

    async def wait_disconnected(self) -> None:
        raise self._exc


async def test_a_parser_verdict_from_the_peer_is_classified_not_escaped_tuned() -> None:
    """picows transfers a parser error to whoever awaits the disconnect.

    It is a verdict about the PEER's framing, so the teardown latches it and the session ends
    DECIDED — terminal, because dialling again asks a peer with a broken framer the same
    question. Unhandled, it escaped `run_client` as an exception the reconnect policy could not
    classify at all.
    """
    listener, _t = _listener()
    transport = _RaisingTransport(
        picows.WSProtocolError(picows.WSCloseCode.PROTOCOL_ERROR, "bad frame from the peer")
    )

    await ws_picows._teardown(transport, listener, graceful=False)  # type: ignore[arg-type]

    intent = listener.driver.close_intent
    assert intent is not None, "a parser verdict must be latched"
    assert "parser" in intent.reason, intent.reason
    assert transport.is_disconnected, "the transport must still be torn down"


async def test_a_failure_from_our_own_callback_is_re_raised_not_swallowed_tuned() -> None:
    """PROVENANCE. picows sends exceptions from our OWN callbacks through the same channel as
    its parser errors, so classifying every `WSProtocolError` as a peer fault silently swallowed
    bugs in this module. A callback failure must propagate."""
    listener, _t = _listener(_boom)
    # Make the listener record a callback failure, exactly as a raising on_sample would.
    with pytest.raises(RuntimeError, match="sample boom"):
        _feed(listener, _FakeTransport(), _FakeFrame(picows.WSMsgType.TEXT, _tob(1, 1).encode()))
    assert listener.callback_error is not None

    transport = _RaisingTransport(
        picows.WSProtocolError(picows.WSCloseCode.PROTOCOL_ERROR, "from our callback")
    )
    with pytest.raises(picows.WSProtocolError):
        await ws_picows._teardown(transport, listener, graceful=False)  # type: ignore[arg-type]


async def test_an_unresponsive_peer_gets_an_abrupt_teardown_tuned() -> None:
    """A peer that never finishes disconnecting must not hold the client to a kernel TCP
    timeout: the bounded wait expires and the close becomes abrupt."""

    class Hangs(_FakeTransport):
        async def wait_disconnected(self) -> None:
            await asyncio.sleep(3600)

    transport = Hangs()
    listener, _t = _listener()
    original = ws_picows.CLOSE_GRACE_S
    ws_picows.CLOSE_GRACE_S = 0.05
    try:
        await asyncio.wait_for(
            ws_picows._teardown(transport, listener, graceful=True),  # type: ignore[arg-type]
            3,
        )
    finally:
        ws_picows.CLOSE_GRACE_S = original
    assert transport.is_disconnected


# --- the two arms must answer the SAME wire fault with the SAME code (Task Z, codex #9/#12) ---


async def test_invalid_utf8_in_a_text_frame_is_1007_not_1002() -> None:
    """RFC 6455 §8.1: a TEXT frame whose bytes are not valid UTF-8 is 1007, not 1002.

    This is the A/B divergence codex found. `websockets` decodes to `str` and rejects the frame
    itself, so the naive arm produced 1007; picows hands up raw bytes, which reached the JSON
    decoder, failed there, and were latched 1002 — "you broke the framing" for what is actually
    "your text is not text". The engine answers 1007 too (Beast's `close_code::bad_payload`), so
    before this the TUNED arm was the only one of the three that disagreed.

    It matters beyond the code being wrong: the two arms are the §6 A/B, so a peer that sends
    one bad byte gets a different verdict depending on which arm is being measured.
    """
    listener, transport = _listener()
    # 0xFF is not a valid UTF-8 start byte anywhere.
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, b'{"t":"\xff"}'))

    intent = listener._driver.close_intent
    assert intent is not None, "invalid UTF-8 must produce a verdict, not be ignored"
    assert intent.code == 1007, f"expected 1007 bad-payload, got {intent}"
    assert transport.closes and transport.closes[0][0] == 1007
    assert transport.sent == [], "a frame that is not text must not draw a quote"


async def test_valid_utf8_that_is_bad_json_is_still_1002() -> None:
    """The other side of the same boundary, so 1007 cannot swallow the framing verdict: a frame
    that IS valid UTF-8 but is not valid JSON remains a protocol error, not a payload error."""
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, b"{ not json at all"))

    intent = listener._driver.close_intent
    assert intent is not None and intent.code == 1002, f"expected 1002, got {intent}"


async def test_a_ping_after_our_close_is_not_answered_because_picows_drops_it() -> None:
    """codex #12: the tuned arm CLAIMED to answer a PING after sending CLOSE, and the only test
    covering it used a fake transport that never set `is_close_frame_sent` — so it asserted a
    reply the real library discards.

    picows silently ignores every send once the close frame has gone out. This pins the REAL
    behaviour rather than the intended one: after our CLOSE, a PING draws no PONG. Asserting the
    opposite would be asserting something the transport cannot do.
    """
    listener, transport = _listener()
    # Reach a decided state and send our close, exactly as the stale-stop path does.
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, b'{"t":"\xff"}'))
    assert transport.is_close_frame_sent, "precondition: our CLOSE went out"

    before = len(transport.pongs)
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.PING, b"after close"))
    assert len(transport.pongs) == before, (
        "picows drops sends after CLOSE; a test that expects a PONG here is testing the fake"
    )


# --- F-B: one close-code table, both arms -------------------------------------------------


# Cases the shared predicate must settle identically for both arms. A future arm-local table
# fails here rather than on the wire mid-benchmark.
_CLOSE_BODY_CASES: list[tuple[bytes, int | None, bool, str]] = [
    (b"", None, True, "empty-body"),
    (b"\x00\x00", 0, False, "encoded-zero"),
    ((1000).to_bytes(2, "big"), 1000, True, "normal"),
    ((1001).to_bytes(2, "big"), 1001, True, "going-away"),
    ((1014).to_bytes(2, "big"), 1014, False, "beast-reserved-1014"),
    ((1006).to_bytes(2, "big"), 1006, False, "never-on-wire-1006"),
    ((4000).to_bytes(2, "big"), 4000, True, "private-stale"),
    ((5000).to_bytes(2, "big"), 5000, False, "past-private"),
    ((999).to_bytes(2, "big"), 999, False, "below-1000"),
    (b"\x00", 0, False, "one-byte-body"),
]


@pytest.mark.parametrize(
    ("payload", "want_code", "want_legal", "_label"),
    _CLOSE_BODY_CASES,
    ids=[c[3] for c in _CLOSE_BODY_CASES],
)
def test_classify_close_body_is_the_shared_table(
    payload: bytes, want_code: int | None, want_legal: bool, _label: str
) -> None:
    """F-B: empty ≠ encoded-zero, and 1014 is not sendable — one predicate, both arms import it."""
    code, legal = classify_close_body(payload)
    assert (code, legal) == (want_code, want_legal)
    if code is not None:
        assert is_sendable_close_code(code) is (
            code in {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013}
            or 3000 <= code <= 4999
        )


async def test_tuned_close_helper_rewrites_unsendable_code_to_1002() -> None:
    """F-B (send path, tuned): a latched intent carrying 1014 leaves as 1002, not 1014."""
    listener, transport = _listener()
    listener.driver._close = CloseIntent(1014, "should not go out")
    ws_picows._close(transport, listener.driver)  # type: ignore[arg-type]
    assert transport.closes == [(CLOSE_PROTOCOL, b"should not go out")]


async def test_naive_close_helper_rewrites_unsendable_code_to_1002() -> None:
    """F-B (send path, naive): same rewrite, same table — an arm-local allow-list would drift."""
    recorded: list[tuple[int, str]] = []

    class Rec:
        async def close(self, code: int, reason: str) -> None:
            recorded.append((code, reason))

    driver = SessionDriver(_strategy(), ws_naive.NAIVE_CODEC)
    driver._close = CloseIntent(1014, "should not go out")
    await ws_naive._close(Rec(), driver)
    assert recorded == [(CLOSE_PROTOCOL, "should not go out")]


async def test_reserved_1014_is_refused_on_echo_tuned() -> None:
    """F-B: 1014 is in both Python libraries' allowed sets and reserved in this Beast.

    Echoing it made us the endpoint emitting a status the engine rejects. Shared table
    excludes it; the peer hears 1002.
    """
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.CLOSE, close_code=_Code(1014)))

    intent = listener.driver.close_intent
    assert intent is not None and intent.code == 1002, f"1014 must not be echoed: {intent}"
    assert transport.closes and transport.closes[0][0] == 1002


# --- F-C: naive arm also yields 1007 for invalid UTF-8 ------------------------------------


async def test_invalid_utf8_in_a_text_frame_is_1007_naive(engine: Starter) -> None:
    """F-C: the naive half of the UTF-8 pin. `websockets` rejects the frame itself as 1007
    (INVALID_DATA), and the arm latches that as a local verdict so the session ends DECIDED.

    Same input as the tuned unit case (`0xFF` in a TEXT frame). Both arms must answer 1007 so
    the §6 A/B cannot attribute a code difference to the stack under measurement.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        await _send_raw(ws, _raw_frame(OP_TEXT, b'{"t":"\xff"}'))
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    ending = await asyncio.wait_for(ws_naive.run_client(url, _strategy(), stop=stop), 10)
    assert ending is Ending.DECIDED, "library-generated 1007 is our verdict, not a departure"
    assert fake.close_code == 1007, f"expected 1007, got {fake.close_code}"


# --- F-D: CLOSE mid-fragmentation — pin each arm's actual behaviour -----------------------


async def test_close_during_fragmentation_is_handled_tuned() -> None:
    """F-D (tuned): control frames are dispatched before fragment state, so a CLOSE mid-
    fragment clears the partial message and completes the handshake. Beast does the same.
    """
    listener, transport = _listener()
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.TEXT, b'{"a":', fin=False))
    assert listener._opened is True  # partial message open
    _feed(listener, transport, _FakeFrame(picows.WSMsgType.CLOSE, close_code=_Code(1000)))

    assert listener._opened is False, "CLOSE must clear fragment state"
    assert transport.closes and transport.closes[0][0] == 1000
    assert listener.driver.close_intent is None, "a legal peer close is not our verdict"


async def test_close_during_fragmentation_is_protocol_error_naive(engine: Starter) -> None:
    """F-D (naive): pinned `websockets` rejects CLOSE with a fragmented message still open.

    Observed behaviour (websockets 16.1.1): the peer's CLOSE is accepted first, the library
    then answers 1002 with reason `incomplete fragmented message`, and because the peer's
    frame arrived first the arm classifies the session as PEER_GONE (not DECIDED). The tuned
    arm, by contrast, clears fragment state and echoes 1000 — see the twin case above.

    Upstream: not patched. The pin exists so a library upgrade that changes the close code,
    the reason text, or the PEER_GONE/DECIDED classification cannot drift silently, which
    would re-open the arm divergence without a red test.
    """

    async def script(fake: FakeEngine, ws: ServerConnection) -> None:
        # TEXT fin=0, then CLOSE 1000 — the shape websockets rejects.
        await _send_raw(ws, _raw_frame(OP_TEXT, b'{"a":', fin=False))
        await _send_raw(ws, _raw_frame(OP_CLOSE, (1000).to_bytes(2, "big")))
        await asyncio.wait_for(ws.wait_closed(), 3)

    fake, url = await engine(script)
    stop = asyncio.Event()
    ending = await asyncio.wait_for(ws_naive.run_client(url, _strategy(), stop=stop), 10)
    # Peer-first close → PEER_GONE; the code WE sent is still 1002 (incomplete fragment).
    assert ending is Ending.PEER_GONE, f"expected PEER_GONE (peer-first close), got {ending}"
    assert fake.close_code == 1002, f"expected client 1002, got {fake.close_code}"
    assert "incomplete" in (fake.close_reason or "").lower()


# --- F-E: counter exhaustion is a latched close, not a wrap and not an arm-divergent raise -


def test_outbound_seq_past_uint64_latches_a_protocol_close() -> None:
    """F-E: `seq` is uint64 on the wire. C++ increments unchecked and wraps; Python ints do
    not, and the encoder refuses a value past the domain. Reaching the bound needs ~1.8e19
    messages, so there is no hot-path wrap check — the encoder is the gate, and the shared
    driver turns its refusal into a latched 1002 so both arms end the same way.

    Constructs the driver at the boundary rather than spinning: adopt an epoch, set
    `_out_seq = 2**64 - 1`, dispatch one more command. The next stamp is `2**64`, encode
    refuses, close latches. Not an incidental raise out of `run_client`.
    """
    from mmclient.protocol import CancelOrder
    from mmclient.strategy import SendCmd

    d = SessionDriver(_strategy(), ws_picows.TUNED_CODEC)
    # Adopt epoch without depending on quote state (PENDING_NEW would suppress a second book).
    assert d.on_bytes(_tob(1, 1).encode(), now_ns=0)
    d._out_seq = 2**64 - 1
    more = d._dispatch([SendCmd(CancelOrder(v=1, seq=0, epoch=0, cl_id="C-1"))])
    assert more == [], "no frame past the domain may leave"
    intent = d.close_intent
    assert intent is not None and intent.code == CLOSE_PROTOCOL, f"expected 1002, got {intent}"
    assert "outside wire domain" in intent.reason
    # A frame already buffered behind the latch must not rewrite the verdict (or escape).
    assert d.on_bytes(b"late garbage", now_ns=2) == []
    assert d.close_intent is intent


def test_outbound_seq_at_uint64_max_still_encodes() -> None:
    """F-E accept side: the last legal value must still go out, or the gate is off-by-one."""
    from mmclient.protocol import CancelOrder, encode

    blob = encode(CancelOrder(v=1, seq=2**64 - 1, epoch=1, cl_id="C-1"))
    assert b"18446744073709551615" in blob


def test_apply_tcp_nodelay_picows_and_naive() -> None:
    """Pins that _apply_tcp_nodelay configures TCP_NODELAY when socket is present
    and handles None."""
    import socket
    from unittest.mock import MagicMock

    # Test picows helper with valid socket
    mock_sock = MagicMock()
    mock_transport = MagicMock()
    mock_transport.underlying_transport.get_extra_info.return_value = mock_sock
    ws_picows._apply_tcp_nodelay(mock_transport)
    mock_sock.setsockopt.assert_called_once_with(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # Test picows helper with None socket
    mock_transport_none = MagicMock()
    mock_transport_none.underlying_transport.get_extra_info.return_value = None
    ws_picows._apply_tcp_nodelay(mock_transport_none)

    # Test naive helper with valid socket
    mock_ws = MagicMock()
    mock_sock_naive = MagicMock()
    mock_ws.transport.get_extra_info.return_value = mock_sock_naive
    ws_naive._apply_tcp_nodelay(mock_ws)
    mock_sock_naive.setsockopt.assert_called_once_with(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # Test naive helper with None socket
    mock_ws_none = MagicMock()
    mock_ws_none.transport.get_extra_info.return_value = None
    ws_naive._apply_tcp_nodelay(mock_ws_none)
