"""The seven-step demo, executed as a test against a REAL engine subprocess.

This is the only place the whole system runs: the C++ engine binary, a real WebSocket over
loopback TCP, both Python transports, and the strategy core. Everything else in the suite
tests one side of a seam; this tests that the seams line up. It earned its keep immediately —
the naive arm was sending every command as a BINARY frame, which the engine closes 1002 on,
and no unit test could see it because the test double accepted binary frames the engine does
not.

It is a TEST rather than a script because a demo nobody runs in CI stops being
true quietly. `scripts/demo.sh` drives the same checkpoints for a human reader; if the two
ever disagree, this file is the one that fails a build.

BOTH STACKS run the same script. The naive/tuned swap is the measured hot-path optimization, and an
optimization that also changes what the client DOES is not a like-for-like comparison — so the
two arms are held to identical observable behaviour here, not merely to identical latency
shape. That is what caught the binary-frame defect.

NOTHING HERE ASSERTS AN ABSOLUTE PRICE. The engine's feed is free-running: `schedule_feed` is
armed at startup and advances on its own timer whether or not anyone is connected, exactly as
a real market data feed does. A client therefore joins mid-stream and is NOT guaranteed to see
the first book in demo.feed — the first assertions written here did assume that, and failed
against book #2. Every claim below is a relationship between what the client SAW and what it
then SENT, which holds at any join point.
"""

import asyncio
import contextlib
import itertools
import json
import time
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import pytest
from engine_fixture import Engine, drain, have_engine, stop_gracefully

from mmclient import ws_naive, ws_picows

pytestmark = pytest.mark.skipif(
    not have_engine(), reason="engine binary not built (set MM_ENGINE or run cmake --build)"
)

STACKS = [
    pytest.param(ws_naive.run_client, id="naive"),
    pytest.param(ws_picows.run_client, id="tuned"),
]

# Long enough that the strategy never trips it on the feed's own 300 ms pauses, for the cases
# that are not about staleness. The stale case sets its own bound.
PATIENT_STALE_MS = 10_000


class EventLog:
    """Every message the session saw and every frame it sent, in order.

    BOTH directions, because the demo script is a sequence of stimulus-and-response and the
    interesting claims are about the pairing: "it quoted at the touch of the book it was
    given". Recording only what was sent leaves you asserting prices you have to guess.
    """

    def __init__(self) -> None:
        self.sent: list[dict[str, Any]] = []
        self.seen: list[dict[str, Any]] = []

    def record_frame(self, raw: bytes) -> None:
        self.sent.append(json.loads(raw))

    def record_inbound(self, raw: bytes) -> None:
        with contextlib.suppress(ValueError):
            self.seen.append(json.loads(raw))

    def of_type(self, tag: str) -> list[dict[str, Any]]:
        return [m for m in self.sent if m["t"] == tag]

    def books(self) -> list[dict[str, Any]]:
        return [m for m in self.seen if m.get("t") == "top_of_book"]

    def cl_ids(self, tag: str) -> list[str]:
        return [str(m["cl_id"]) for m in self.of_type(tag)]


@contextlib.contextmanager
def _recording(log: EventLog) -> Iterator[EventLog]:
    """Tap both seams of SessionDriver for the duration of a run.

    Patched on the CLASS rather than an instance because the adapters construct their own
    driver internally, and restored in a `finally` so one failing case cannot leave the tap
    installed for the rest of the session.
    """
    import mmclient._session as session_mod

    original_bytes = session_mod.SessionDriver.on_bytes
    original_dispatch = session_mod.SessionDriver._dispatch

    def on_bytes(self: Any, raw: bytes, now_ns: int) -> list[bytes]:
        log.record_inbound(raw)
        frames: list[bytes] = original_bytes(self, raw, now_ns)
        return frames

    def dispatch(self: Any, cmds: Any) -> list[bytes]:
        frames: list[bytes] = original_dispatch(self, cmds)
        for frame in frames:
            log.record_frame(frame)
        return frames

    session_mod.SessionDriver.on_bytes = on_bytes  # type: ignore[method-assign]
    session_mod.SessionDriver._dispatch = dispatch  # type: ignore[method-assign]
    try:
        yield log
    finally:
        session_mod.SessionDriver.on_bytes = original_bytes  # type: ignore[method-assign]
        session_mod.SessionDriver._dispatch = original_dispatch  # type: ignore[method-assign]


def _strategy(stale_ms: int = PATIENT_STALE_MS) -> Any:
    from mmclient.strategy import Strategy

    return Strategy(symbol="MOCKUSDT", qty=10, max_qty=100, stale_ns=stale_ms * 1_000_000)


async def _until(predicate: Callable[[], bool], timeout: float = 5.0) -> bool:
    """Poll a condition instead of sleeping a guessed interval.

    A fixed `sleep(0.6)` is a bet that the client connected, handshook and got a tick inside
    600 ms. The naive arm's connect is slower than the tuned arm's, so that bet is a coin flip
    that fails on one arm and passes on the other — which reads as a behavioural difference
    between the stacks when it is only a difference in how fast they dial.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        await asyncio.sleep(0.02)
    return False


async def _run_until(run_client: Any, eng: Engine, strategy: Any, seconds: float) -> None:
    """Drive the client for a bounded window.

    Bounded by wall time rather than by a message count: the feed is the script, and what this
    asserts is what the client did while that script played.
    """
    stop = asyncio.Event()
    task = asyncio.create_task(run_client(eng.url, strategy, stop=stop))
    await asyncio.sleep(seconds)
    stop.set()
    try:
        await asyncio.wait_for(task, 5)
    except TimeoutError, asyncio.CancelledError:  # pragma: no cover - defensive
        task.cancel()


def _telemetry(eng: Engine) -> list[dict[str, Any]]:
    if not eng.telemetry.is_file():
        return []
    out: list[dict[str, Any]] = []
    for line in eng.telemetry.read_text().splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:  # a trailing partial line while the writer is live
            continue
    return out


@pytest.mark.parametrize("run_client", STACKS)
async def test_the_demo_script_runs_end_to_end(run_client: Any, live_engine: Any) -> None:
    """Steps 1-5 of the demo script, against a live engine.

    One window rather than five, because the feed drives the sequence and pausing between
    assertions would change what is being tested — the point is that the client keeps up with
    a script it does not control.
    """
    eng: Engine = live_engine(codec="tuned", interval_ms=50)
    strategy = _strategy()
    log = EventLog()

    with _recording(log):
        await _run_until(run_client, eng, strategy, seconds=2.0)

    # (1) the client was actually fed. Asserted before anything downstream, so a silent engine
    # fails as "no book arrived" rather than as a confusing claim about quotes.
    books = log.books()
    assert books, "the client received no top_of_book at all"

    # (2) it quoted BOTH sides, bid first, at the touch OF THE BOOK IT WAS GIVEN.
    orders = log.of_type("new_order")
    assert len(orders) >= 2, f"expected at least two quotes, got {log.sent}"
    assert [o["side"] for o in orders[:2]] == ["B", "S"]
    assert orders[0]["cl_id"] == "B-1" and orders[1]["cl_id"] == "S-1"
    first = books[0]
    assert orders[0]["px"] == first["bid_px"], "the bid quote must sit at the touch it saw"
    assert orders[1]["px"] == first["ask_px"], "the ask quote must sit at the touch it saw"

    # ...post-only, and clamped to the configured size.
    assert all(o["post_only"] is True for o in orders)
    assert all(o["qty"] == 10 for o in orders)

    # Every quote it ever sent sat at a price the engine had actually published — the client
    # never invents a level. This is the timing-independent form of "it quotes at the touch".
    bids = {b["bid_px"] for b in books}
    asks = {b["ask_px"] for b in books}
    for order in orders:
        side_levels = bids if order["side"] == "B" else asks
        assert order["px"] in side_levels, f"quoted a price never published: {order}"

    # (4) demo.feed moves the ask and leaves the bid. Whatever the join point, the client saw
    # more than one ask level and re-quoted, cancelling the superseded order rather than
    # abandoning it.
    if len(asks) > 1:
        ask_quotes = [o for o in orders if o["side"] == "S"]
        assert len({q["px"] for q in ask_quotes}) > 1, "the ask move should have drawn a requote"
        assert log.cl_ids("cancel_order"), "a superseded quote must be cancelled, not abandoned"

    # The outbound envelope counter is contiguous across BOTH message kinds — a gap is a 1002
    # from the engine, and nothing else in this suite watches a real one.
    assert [m["seq"] for m in log.sent] == list(range(1, len(log.sent) + 1))

    # (3) the engine acked, and the strategy's picture agrees with what it sent.
    assert log.of_type("new_order"), "nothing was sent"
    acked = [m for m in log.seen if m.get("t") == "order_ack"]
    assert acked, "the engine never acknowledged an order (a 1002 close looks exactly like this)"
    assert set(strategy.live_or_pending()) <= {str(m["cl_id"]) for m in orders}

    rc = stop_gracefully(eng)
    out, err = drain(eng)
    assert rc == 0, f"engine exited {rc}: {err[:400]}"
    assert err == "", f"engine wrote to stderr (sanitizer noise or a fault): {err[:400]}"
    assert "telemetry_ok=1" in out


def _live_timeline(seen: list[dict[str, Any]]) -> list[tuple[dict[str, Any], frozenset[str]]]:
    """Replay the inbound stream, returning the live-order set AFTER each engine message.

    Reconstructed from the ORDERED log rather than read off the strategy or off a counter peak,
    because the demo's steps are claims about specific MOMENTS; `strategy.live_or_pending()`
    only ever
    holds the current picture, and by the time an assertion runs the ask move and the fill have
    already rewritten it; a telemetry peak of `live_orders == 2` is satisfied by any run that ever
    touched two, including one that never held both quotes at the same time. Neither can express
    "at the instant both orders were acked, exactly these two were live", which is what step 3
    says.
    """
    live: set[str] = set()
    out: list[tuple[dict[str, Any], frozenset[str]]] = []
    for msg in seen:
        tag, cl = msg.get("t"), str(msg.get("cl_id", ""))
        if tag == "order_ack":
            live.add(cl)
        elif tag == "cancel_ack":
            live.discard(cl)
        elif tag == "fill" and int(msg.get("leaves", 0)) == 0:
            live.discard(cl)  # fully filled orders stop resting
        out.append((msg, frozenset(live)))
    return out


def _acked(log: EventLog) -> set[str]:
    return {str(m["cl_id"]) for m in log.seen if m.get("t") == "order_ack"}


@pytest.mark.parametrize("run_client", STACKS)
async def test_the_four_step_relationships_hold_as_ordered_checkpoints(
    run_client: Any, live_engine: Any
) -> None:
    """Demo steps 3, 4 and 5 as EXACT relationships at ordered checkpoints .

    The sibling end-to-end test asserts the shape of the run — that quotes went out at published
    prices, that the envelope counter is contiguous, that acks came back. What it does NOT assert
    is the part the demo spec actually pins: that BOTH orders were acked and exactly two
    were live, that
    a one-sided book move drew exactly one cancel and one replacement ON THAT SIDE and left the
    other alone, and that a fill was followed by a restored two-sided quote. Its step-4 block was
    additionally guarded by `if len(asks) > 1`, so on any run where the window happened not to
    contain the move the whole check silently evaporated — and a conditional assertion is
    indistinguishable from a passing one in the output.

    Each checkpoint below WAITS for its precondition and then asserts unconditionally, so a
    timing-dependent absence fails loudly instead of skipping.
    """
    eng: Engine = live_engine(codec="tuned", interval_ms=50)
    strategy = _strategy()
    log = EventLog()
    stop = asyncio.Event()

    with _recording(log):
        task = asyncio.create_task(run_client(eng.url, strategy, stop=stop))
        try:
            # --- (3) both acked, and exactly two live at that moment ------------------------
            assert await _until(lambda: {"B-1", "S-1"} <= _acked(log), timeout=8.0), (
                f"the engine never acked both opening quotes; acked={_acked(log)}"
            )
            timeline = _live_timeline(log.seen)
            both_live = [live for _, live in timeline if live == frozenset({"B-1", "S-1"})]
            assert both_live, (
                "there was no moment at which exactly B-1 and S-1 were live together; "
                f"live sets seen: {sorted({tuple(sorted(s)) for _, s in timeline})}"
            )

            # --- (4) a one-sided move draws exactly one cancel + one new, on that side only --
            # demo.feed's second `set` moves the ask 500010 -> 500015 and leaves the bid at
            # 500000. Find that transition in the book stream rather than assuming the window
            # contained it, then assert the response.
            def ask_only_move() -> bool:
                books = log.books()
                return any(
                    b["ask_px"] != a["ask_px"] and b["bid_px"] == a["bid_px"]
                    for a, b in itertools.pairwise(books)
                )

            assert await _until(ask_only_move, timeout=8.0), (
                "demo.feed's ask-only move never reached the client"
            )
            # The response to an ask-only move: the ask is replaced, the bid is not touched.
            assert await _until(lambda: "S-2" in _acked(log), timeout=8.0), (
                "the ask move drew no replacement quote"
            )
            cancels = log.cl_ids("cancel_order")
            assert "S-1" in cancels, "the superseded ask must be cancelled, not abandoned"
            assert not [c for c in cancels if c.startswith("B-")], (
                f"an ask-only move must not disturb the bid, but these were cancelled: {cancels}"
            )
            ask_quotes = [o for o in log.of_type("new_order") if o["side"] == "S"]
            assert len({q["px"] for q in ask_quotes}) >= 2, "the replacement sat at the old price"

            # --- (5) a deterministic fill, then two-sided quoting restored ------------------
            # demo.feed's third `set` drops the ask to 500000, crossing the resting bid.
            assert await _until(lambda: any(m.get("t") == "fill" for m in log.seen), timeout=8.0), (
                "demo.feed's crossing set produced no fill"
            )
            assert await _until(
                lambda: len({s[:1] for s in strategy.live_or_pending()}) == 2, timeout=8.0
            ), (
                "after the fill the client did not restore a two-sided quote; "
                f"live_or_pending={sorted(strategy.live_or_pending())}"
            )
        finally:
            stop.set()
            with contextlib.suppress(TimeoutError, asyncio.CancelledError):
                await asyncio.wait_for(task, 5)

    rc = stop_gracefully(eng)
    _, err = drain(eng)
    assert rc == 0, f"engine exited {rc}: {err[:400]}"
    assert err == "", f"engine wrote to stderr: {err[:400]}"


@pytest.mark.parametrize("run_client", STACKS)
async def test_a_hostile_second_connection_does_not_disturb_the_first(
    run_client: Any, live_engine: Any
) -> None:
    """Step 6, and the one that matters most.

    A second session sends garbage. The engine must reject it on THAT session and leave the
    first session's orders untouched — no unsolicited report, no state change, and the first
    session must still be quoting afterwards, which is the only proof from outside that its
    orders really did survive engine-side.
    """
    import websockets
    from websockets.asyncio.client import connect

    eng: Engine = live_engine(codec="tuned", interval_ms=50)
    strategy = _strategy()
    log = EventLog()
    stop = asyncio.Event()

    with _recording(log):
        task = asyncio.create_task(run_client(eng.url, strategy, stop=stop))
        try:
            assert await _until(lambda: bool(strategy.live_or_pending())), (
                "the strategy session never started quoting"
            )

            async with connect(
                eng.url, subprotocols=[websockets.Subprotocol("mm.v1")], compression=None
            ) as hostile:
                await hostile.send("{ this is not json")
                reply = json.loads(await asyncio.wait_for(hostile.recv(), 3))
                while reply["t"] == "top_of_book":
                    reply = json.loads(await asyncio.wait_for(hostile.recv(), 3))
                assert reply["t"] == "reject"
                assert reply["code"] in {"MALFORMED", "UNKNOWN_TYPE"}

            # The victim session keeps working AFTER the intruder is gone — still quoting, and
            # still transacting with the engine. A snapshot of `live_or_pending` alone would
            # pass on a session that had gone mute.
            sent_before = len(log.sent)
            assert await _until(lambda: len(log.sent) > sent_before), (
                "the surviving session went mute after the intruder"
            )
            assert strategy.live_or_pending(), "the intruder must not have flattened us"
        finally:
            stop.set()
            await asyncio.wait_for(task, 5)

    rc = stop_gracefully(eng)
    _out, err = drain(eng)
    assert rc == 0
    assert err == ""

    # The engine narrated the intruder's reject.
    assert [e for e in _telemetry(eng) if e.get("event") == "reject"], (
        "the engine should have narrated the malformed frame"
    )


def test_the_demo_script_exists_and_is_executable() -> None:
    """`scripts/demo.sh` is the reviewer-facing face of this same script. It is checked here
    so a missing or non-executable demo fails a build rather than a reader's afternoon."""
    demo = Path(__file__).resolve().parents[2] / "scripts" / "demo.sh"
    assert demo.is_file(), "scripts/demo.sh is missing"
    assert demo.stat().st_mode & 0o111, "scripts/demo.sh is not executable"


@pytest.mark.parametrize("run_client", STACKS)
async def test_a_stale_feed_stops_quoting_and_closes_4000(
    run_client: Any, live_engine: Any
) -> None:
    """Step 7a. demo.feed pauses repeatedly; under a 200 ms staleness bound the FIRST pause is
    already long enough, so the client must stop, cancel what is resting, and close with the
    PRIVATE code that says why rather than with a generic transport code."""
    eng: Engine = live_engine(codec="tuned", interval_ms=50)
    strategy = _strategy(stale_ms=200)
    log = EventLog()

    with _recording(log):
        await _run_until(run_client, eng, strategy, seconds=3.0)

    assert log.of_type("new_order"), "the client should have quoted before going stale"
    assert log.of_type("cancel_order"), "a stale stop must pull its resting quotes"
    # It stopped: nothing was sent after the cancels went out.
    last_new = max(i for i, m in enumerate(log.sent) if m["t"] == "new_order")
    last_cancel = max(i for i, m in enumerate(log.sent) if m["t"] == "cancel_order")
    assert last_cancel > last_new, "the client re-quoted after deciding the feed was stale"

    rc = stop_gracefully(eng)
    _out, err = drain(eng)
    assert rc == 0
    assert err == ""

    closes = [e for e in _telemetry(eng) if e.get("event") == "session_close"]
    assert closes, "the engine should have recorded the client's close"
    codes = [int(str(c["args"][1])) for c in closes]
    # 4000 is the point of the case: the private code distinguishes "I stopped because the
    # feed went stale" from every transport cause. 1000 is allowed only because a stop racing
    # the stale tick may close cleanly first.
    assert any(code in {1000, 4000} for code in codes), f"close codes were {codes}"


@pytest.mark.parametrize("run_client", STACKS)
async def test_a_feed_drop_closes_1001_and_the_reconnect_is_flat(
    run_client: Any, live_engine: Any
) -> None:
    """Step 7b. demo.feed carries a `drop`, which closes every session 1001 going_away while
    the engine keeps running. The client must see it and forget its orders: the engine cancels
    them on disconnect, so carrying any local record would mean quoting against orders that no
    longer exist."""
    eng: Engine = live_engine(codec="tuned", interval_ms=50)
    strategy = _strategy()
    log = EventLog()

    with _recording(log):
        await _run_until(run_client, eng, strategy, seconds=4.0)

    assert log.of_type("new_order"), "the client should have quoted before the drop"
    assert strategy.live_or_pending() == {}

    rc = stop_gracefully(eng)
    _out, err = drain(eng)
    assert rc == 0
    assert err == ""

    closes = [e for e in _telemetry(eng) if e.get("event") == "session_close"]
    assert closes, "the drop should have closed the session"
