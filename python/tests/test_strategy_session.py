"""What the strategy decides across a whole CONNECTION.

The two counters and their very different readings, the lifecycle rules
(connect / disconnect / stale feed / reconnect), and the determinism property the sans-IO
split exists for. The per-message half is in ``test_strategy.py``; the shared fixtures are
in ``strategy_support.py``.

Time is a parameter (``now_ns``), so the stale-feed rule is tested by passing a number
rather than by sleeping — the same reason the module takes no clock: a strategy that read
one could not be replayed.
"""

from strategy_support import SYMBOL, sent, strategy, tob

from mmclient.protocol import CancelOrder, OrderAck, Reject
from mmclient.strategy import Cmd, OState, StopQuoting, Strategy


def test_an_envelope_seq_gap_stops_quoting() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    cmds = s.on_tob(tob(3, 2), now_ns=1)  # 2 never arrived
    assert cmds == [StopQuoting("inbound seq gap: expected 2, got 3")]
    assert s.on_tob(tob(4, 3), now_ns=2) == []  # latched: no further decisions


def test_an_md_seq_decrease_stops_quoting() -> None:
    s = strategy()
    s.on_tob(tob(1, 5), now_ns=0)
    cmds = s.on_tob(tob(2, 4), now_ns=1)
    assert cmds == [StopQuoting("md_seq decreased: 5 -> 4")]


def test_the_first_tick_is_never_counted_as_conflation() -> None:
    """The `_md_seq != 0` exemption. A session joining a feed already at md_seq 7 has not
    seen 6 books conflated away — it has seen one book. Renamed from a duplicate-md_seq
    case that could not fail: both sides were PENDING_NEW *and* its bid crossed the ask, so
    its `[]` was over-determined twice and the duplicate guard could be deleted under it.
    The duplicate rule itself is pinned by the short-circuit case below."""
    s = strategy()
    s.on_tob(tob(1, 7), now_ns=0)
    assert s.conflated_seen == 0


def test_an_md_seq_forward_gap_is_counted_and_quoting_continues() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)

    msgs = sent(s.on_tob(tob(4, 9, bid_px=101), now_ns=2))  # md_seq 2..8 conflated away

    assert s.conflated_seen == 1
    assert [m.cl_id for m in msgs] == ["B-1", "B-2"]  # cancel + replace, still quoting


def test_a_report_seq_gap_stops_quoting_too() -> None:
    """The envelope counter spans EVERY inbound message, not just the ticks."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    ack = OrderAck(v=1, seq=5, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(ack, 1) == [StopQuoting("inbound seq gap: expected 2, got 5")]


def test_a_duplicate_md_seq_short_circuits_before_any_decision() -> None:
    """The earlier duplicate case had both sides PENDING_NEW, so falling THROUGH the early
    return also produced [] — it asserted the right value for the wrong reason. With both
    sides live and the prices moved, a fall-through would re-quote both."""
    s = strategy()
    s.on_tob(tob(1, 7), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)

    assert s.on_tob(tob(4, 7, bid_px=101, ask_px=112), now_ns=99) == []


def test_conflated_seen_counts_skips_only_not_every_tick() -> None:
    """A counter that incremented on every step would still pass a single forward-gap case."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_tob(tob(2, 2), now_ns=1)  # consecutive: nothing was conflated away
    s.on_tob(tob(3, 3), now_ns=2)
    assert s.conflated_seen == 0
    s.on_tob(tob(4, 8), now_ns=3)  # 4..7 conflated away: exactly one skip
    assert s.conflated_seen == 1
    s.on_tob(tob(5, 20), now_ns=4)  # a SECOND skip: a counter that saturates reads 1 here
    assert s.conflated_seen == 2


def test_a_repeated_or_backward_envelope_seq_stops_quoting() -> None:
    """`!=` not `>`: the counter is contiguity, so a REPLAY is as much a fault as a gap."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    assert s.on_tob(tob(1, 2), now_ns=1) == [StopQuoting("inbound seq gap: expected 2, got 1")]


def test_a_report_for_an_unknown_order_still_consumes_its_envelope_seq() -> None:
    """The counter belongs to the CONNECTION, not to the orders. Skipping the advance for a
    message we cannot route would manufacture a gap out of the next good one."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    assert s.on_report(Reject(v=1, seq=2, epoch=1, cl_id="", code="MALFORMED", reason=""), 1) == []
    ack = OrderAck(v=1, seq=3, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(ack, 1) == []
    assert s.live_or_pending()["B-1"] is OState.LIVE


def test_a_fatal_stop_latches_against_later_reports() -> None:
    """Stopping is not "return one StopQuoting"; it is a state. A later report must not
    mutate the order picture of a session that has already given up on it."""
    s = strategy()
    s.on_tob(tob(1, 5), now_ns=0)
    assert s.on_tob(tob(2, 4), now_ns=1) == [StopQuoting("md_seq decreased: 5 -> 4")]
    ack = OrderAck(v=1, seq=3, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(ack, 2) == []
    assert s.live_or_pending()["B-1"] is OState.PENDING_NEW  # untouched
    assert s.on_tob(tob(4, 6), now_ns=3) == []


def test_the_stale_timer_stops_quoting_and_cancels_both_live_sides_once() -> None:
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)

    cmds = s.on_timer(now_ns=2000)
    assert cmds[0] == StopQuoting("stale feed: 2000 ns since the last book, limit 1000")
    assert [m.cl_id for m in sent(cmds[1:])] == ["B-1", "S-1"]
    # ONCE: a second timer past the same deadline must not re-cancel.
    assert s.on_timer(now_ns=3000) == []


def test_the_timer_is_quiet_inside_the_staleness_window() -> None:
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    assert s.on_timer(now_ns=999) == []


def test_the_timer_before_any_book_is_quiet() -> None:
    """No book yet is not a stale book — the deadline starts at the first tick."""
    assert strategy(stale_ns=1).on_timer(now_ns=10**9) == []


def test_disconnect_stops_quoting_and_drops_state() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    assert s.on_disconnect() == [StopQuoting("disconnect")]
    assert s.live_or_pending() == {}


def test_reconnect_is_a_clean_slate() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_tob(tob(3, 2), now_ns=1)  # seq gap: latched stopped
    assert s.on_connect(2) == []

    msgs = sent(s.on_tob(tob(1, 1, epoch=2), now_ns=2))
    assert s.epoch == 2
    assert [(m.cl_id, m.epoch, m.seq) for m in msgs] == [("B-1", 2, 1), ("S-1", 2, 2)]
    assert s.conflated_seen == 0


def test_a_stale_epoch_tick_is_ignored_and_consumes_no_envelope_seq() -> None:
    """The engine mints a fresh epoch per connection; a tick from the old one is not ours —
    and must not advance the counter, or the first GOOD tick of the new session would look
    like a gap. Asserting only `[]` left that half invisible."""
    s = strategy()
    assert s.on_tob(tob(1, 1, epoch=99), now_ns=0) == []
    assert s.live_or_pending() == {}
    msgs = sent(s.on_tob(tob(1, 1), now_ns=1))  # still seq 1: the counter never moved
    assert [m.cl_id for m in msgs] == ["B-1", "S-1"]


def test_a_disconnect_latches_against_later_ticks() -> None:
    """Drives a TICK, not a report: on_disconnect empties _orders, so a report afterwards
    takes the unknown-order path and returns [] whether or not the latch exists. A tick is
    the input that reaches it — and without the latch a dead transport gets quoted onto."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    assert s.on_disconnect() == [StopQuoting("disconnect")]
    assert s.on_tob(tob(2, 2, bid_px=101), now_ns=1) == []
    assert s.live_or_pending() == {}


def test_a_report_from_a_stale_epoch_is_ignored() -> None:
    """A report the engine minted for the PREVIOUS connection must not move this one's
    counters — accepting it would advance the envelope seq and manufacture a gap."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    stale = OrderAck(v=1, seq=99, epoch=98, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(stale, 1) == []
    assert s.live_or_pending()["B-1"] is OState.PENDING_NEW
    # ...and the real next message still lands on the expected seq.
    good = OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(good, 1) == []
    assert s.live_or_pending()["B-1"] is OState.LIVE


def test_the_stale_deadline_is_exclusive_at_the_boundary() -> None:
    """`>` not `>=`: a book exactly at the limit is still inside it."""
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    assert s.on_timer(now_ns=1000) == []
    assert s.on_timer(now_ns=1001) != []


def test_the_stale_sweep_cancels_orders_still_pending_new() -> None:
    """An order the client has not seen acked may already be RESTING on the engine — the
    ack is merely in flight. Cancelling only what is LIVE would leave it quoting."""
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)  # both sides PENDING_NEW, never acked
    cmds = s.on_timer(now_ns=2000)
    assert [m.cl_id for m in sent(cmds[1:])] == ["B-1", "S-1"]


def test_a_reconnect_lifts_every_reject_barrier() -> None:
    """A new epoch is a new engine-side state; a price refused under the old one says
    nothing about the new one, and keeping the bar would silently drop a side."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    rej = Reject(v=1, seq=2, epoch=1, cl_id="S-1", code="TICK_SIZE", reason="px off grid")
    s.on_report(rej, 1)
    s.on_connect(2)

    msgs = sent(s.on_tob(tob(1, 1, epoch=2), now_ns=2))
    assert [m.cl_id for m in msgs] == ["B-1", "S-1"]  # both sides quote, at the same prices


def test_a_duplicate_tick_is_proof_of_life_for_the_stale_rule() -> None:
    """A repeated md_seq changes no price but proves the FEED is alive. Reading freshness
    after the duplicate return declared a heartbeating feed stale and cancelled on it."""
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_tob(tob(2, 1), now_ns=900)  # same book, 900 ns later
    assert s.on_timer(now_ns=1500) == []  # 600 ns since the last tick, not 1500
    assert s.on_timer(now_ns=1901) != []


def test_a_later_book_resets_the_staleness_origin() -> None:
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_tob(tob(2, 2, bid_px=101), now_ns=900)
    assert s.on_timer(now_ns=1900) == []  # exactly at the limit, measured from 900
    assert s.on_timer(now_ns=1901) != []


def test_reconnect_resets_every_counter_the_session_carried() -> None:
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(Reject(v=1, seq=2, epoch=1, cl_id="S-1", code="TICK_SIZE", reason="grid"), 1)
    s.on_tob(tob(3, 9), now_ns=2)  # forward gap: conflated_seen = 1
    assert s.conflated_seen == 1

    s.on_connect(2)
    assert s.conflated_seen == 0
    assert s.on_timer(now_ns=10**9) == []  # the staleness origin went with the epoch

    msgs = sent(s.on_tob(tob(1, 1, epoch=2), now_ns=3))
    assert [m.cl_id for m in msgs] == ["B-1", "S-1"]  # the barred price quotes again


def test_replaying_the_same_inputs_produces_identical_outputs() -> None:
    """The property the sans-IO split exists for: no clock, no socket, no hidden state."""

    def run() -> list[list[Cmd]]:
        s = strategy()
        out = [s.on_tob(tob(1, 1), now_ns=0)]
        ack = OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
        out.append(s.on_report(ack, 1))
        out.append(s.on_tob(tob(3, 2, bid_px=101), now_ns=2))
        out.append(s.on_timer(now_ns=3))
        return out

    assert run() == run()


# --- R2 (critical-reviewer): the latch split, purity, and the barrier's blind spot --------


def test_a_protocol_fatal_stop_still_pulls_resting_quotes() -> None:
    """One latch carrying two meanings left a seq gap disarming the sweep — measured: both
    sides LIVE, `on_timer(10**12)` returns [], and the only control that can pull them is
    dead for the session. A stale feed cancels because unseen data is dangerous; a protocol
    fault is a strictly WORSE position to hold unmanaged quotes from, and cancelled nothing.
    """
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    assert s.on_tob(tob(9, 2), now_ns=2) == [StopQuoting("inbound seq gap: expected 4, got 9")]

    # The sweep pulls, and says NOTHING — the reason was already given, truthfully, by the
    # fault. A second StopQuoting here reported "stale feed: N ns since the last book" for a
    # session whose books had been arriving, with an age measured from the fault rather than
    # from the feed: a fabricated diagnosis on the one channel that exists to distinguish
    # causes, which the demo prints.
    cmds = s.on_timer(now_ns=10**6)
    assert [m.cl_id for m in sent(cmds)] == ["B-1", "S-1"]

    # ONE-SHOT, asserted on the latch itself. A second sweep returns [] whether or not the
    # latch exists — the reason is suppressed because quoting already stopped, and the
    # resting filter is empty because the first sweep left every order PENDING_CANCEL. So
    # the observable that used to pin this went away when the reason guard landed, and the
    # whole latch became deletable in silence. This is the same private-state pin the
    # terminal-orders case uses, for the same reason: the state is real and nothing else
    # can see it.
    assert s._swept is True
    assert s.on_timer(now_ns=10**9) == []


def test_the_strategy_never_reads_a_tick_the_caller_kept() -> None:
    """msgspec.Struct is not frozen. Retaining the caller's Tob let a transport that touched
    a tick AFTER handing it in change decisions taken later — a quote at a price and an
    md_seq no accepted tick ever carried. Two transports drive this object and the whole
    naive-vs-tuned comparison rests on identical decisions from identical input."""
    s = strategy()
    t = tob(1, 1)
    s.on_tob(t, now_ns=0)
    t.bid_px = 105  # the caller reuses its buffer, as a tuned transport plausibly would
    t.md_seq = 77

    ack = OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(ack, 1) == []  # the accepted book said 100, and still does


def test_a_tick_for_another_instrument_stops_rather_than_going_quiet() -> None:
    """The engine validates only the symbol the CLIENT stamps, so a foreign tick would be
    quoted at that instrument's prices under this one's name — and accepted.

    It STOPS rather than being ignored, because ignoring is the silent failure that hurts
    most here: a misconfigured `--symbol` makes EVERY tick foreign, so the staleness origin
    is never written, the sweep can never arm, and the client runs indefinitely quoting
    nothing and never saying why."""
    s = strategy()
    foreign = tob(1, 1, bid_px=7, ask_px=9)
    foreign.symbol = "NOTMINE"
    assert s.on_tob(foreign, now_ns=0) == [
        StopQuoting("unexpected symbol: NOTMINE, expected MOCKUSDT")
    ]
    assert s.live_or_pending() == {}


def test_a_swept_session_quotes_nothing_on_a_later_tick() -> None:
    """The sweep's `_stopped` latch. Without it a tick after the sweep emits a DUPLICATE
    cancel for an order already cancelled, plus a new order into a session that has just
    declared the feed stale — and it is also the sole support for narrowing the in-flight
    check to PENDING_NEW, which the code comment claims but nothing else defends."""
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    assert len(s.on_timer(now_ns=2000)) == 3  # StopQuoting + two cancels
    assert s._swept is True

    assert s.on_tob(tob(4, 2, bid_px=101), now_ns=2001) == []


def test_a_reconnect_re_arms_the_stale_pull() -> None:
    """`_swept` is one-shot PER CONNECTION. Left set across a reconnect, the very defect the
    latch split was introduced to fix comes back at the epoch boundary: both sides live on
    the new connection and no control able to pull them."""
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_timer(now_ns=2000)  # the sweep FIRES on connection 1
    s.on_connect(2)

    s.on_tob(tob(1, 1, epoch=2), now_ns=3000)
    s.on_report(OrderAck(v=1, seq=2, epoch=2, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 3001)
    s.on_report(OrderAck(v=1, seq=3, epoch=2, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 3001)
    cmds = s.on_timer(now_ns=9000)
    assert [m.cl_id for m in sent(cmds[1:])] == ["B-1", "S-1"]


def test_a_reconnect_forgets_orders_the_previous_epoch_left_behind() -> None:
    """Epoch 1 mints MORE orders than epoch 2, so a surviving map is visible. With equal
    counts the new ids overwrite the old ones and the leak hides — which is why the earlier
    clean-slate case, running one order per side, could not see it. A phantom order here is
    one the demo prints and the stale sweep would try to cancel under a retired epoch."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # B-2
    s.on_report(OrderAck(v=1, seq=5, epoch=1, cl_id="B-2", eng_id=3, status="live", svc_ns=1), 3)
    s.on_tob(tob(6, 3, bid_px=102), now_ns=4)  # B-3
    assert len(s.live_or_pending()) >= 3

    s.on_connect(2)
    s.on_tob(tob(1, 1, epoch=2), now_ns=5)
    assert sorted(s.live_or_pending()) == ["B-1", "S-1"]


def test_the_stale_sweep_does_not_re_cancel_a_cancel_already_in_flight() -> None:
    """A superseded order is PENDING_CANCEL and still in the map when the sweep runs. Its
    cancel is already on the wire — re-sending one is the duplicate the exclusion exists to
    prevent, and nothing pinned that the exclusion was doing anything."""
    s = strategy(stale_ns=1000)
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # B-1 -> PENDING_CANCEL, B-2 -> PENDING_NEW
    assert s.live_or_pending()["B-1"] is OState.PENDING_CANCEL

    cmds = s.on_timer(now_ns=5000)
    assert [m.cl_id for m in sent(cmds[1:])] == ["S-1", "B-2"]  # never B-1 a second time


def test_a_cancel_carries_this_session_s_epoch() -> None:
    """Asserted nowhere, while the same stamp on a NewOrder was. The engine answers a
    mismatched command epoch with Reject{StaleEpoch} and keeps the session up — so during an
    amend the cancel would be refused while its replacement, stamped identically but
    asserted, is accepted: two orders resting on one side, and the client then drops the one
    it thinks was rejected while the engine still carries it."""
    s = strategy()
    s.on_connect(7)
    s.on_tob(tob(1, 1, epoch=7), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=7, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=7, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)

    msgs = sent(s.on_tob(tob(4, 2, bid_px=101, epoch=7), now_ns=2))
    assert isinstance(msgs[0], CancelOrder)
    assert msgs[0].epoch == 7
    assert msgs[1].epoch == 7


def test_the_smallest_countable_gap_is_counted() -> None:
    """md_seq n -> n+2: exactly one book conflated away, the minimum that must count.

    Every conflation case in the suite used a large jump (1->9, 8->20), so the predicate's
    lower boundary was pinned in one direction only — widening it to `> _md_seq + 2` survived
    all 481 tests. `conflated_seen` is a headline figure the demo reports, so an undercount is
    a wrong number in the deliverable, not merely a soft spot in the suite.
    """
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_tob(tob(2, 3), now_ns=1)  # md_seq 1 -> 3: one book skipped
    assert s.conflated_seen == 1


def test_a_never_connected_strategy_does_not_call_its_first_timer_stale() -> None:
    """ "No book yet" is not "the book has gone stale", and the distinction was untested.

    Every other case reaches the strategy through the shared fixture, which calls on_connect
    and overwrites the field defaults — so `_last_tob_ns: int | None = None` changing to `0`
    survived all 481 tests, and with it the whole class of mutants over this dataclass's
    defaults. Under that mutant a freshly built Strategy answers its first timer by declaring
    a feed it has never heard from stale. Constructed directly, deliberately.
    """
    s = Strategy(symbol=SYMBOL, qty=5, max_qty=10, stale_ns=1)
    assert s.on_timer(now_ns=10**9) == []
    assert s.live_or_pending() == {}
