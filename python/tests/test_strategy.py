"""What the strategy DECIDES about one message at a time.

Quoting at the touch, the never-cross belt (F-28), and how each engine report moves the
local order picture. The connection-scoped half — the two counters, lifecycle, determinism —
is in ``test_strategy_session.py``; the shared fixtures are in ``strategy_support.py``.

Every case asserts the EXACT command list, because "emitted something reasonable" is not a
contract a second transport can be held to: Task 9 runs this same object under two adapters,
and the whole point of the sans-IO split is that both get identical decisions.
"""

from strategy_support import SYMBOL, sent, strategy, tob

from mmclient.protocol import CancelAck, CancelOrder, Fill, NewOrder, OrderAck, Reject
from mmclient.strategy import OState


def test_first_tob_quotes_both_sides_at_the_touch() -> None:
    s = strategy()
    msgs = sent(s.on_tob(tob(1, 1), now_ns=0))

    assert [m.cl_id for m in msgs] == ["B-1", "S-1"]
    bid, ask = msgs
    assert isinstance(bid, NewOrder) and isinstance(ask, NewOrder)
    assert (bid.side, bid.px, bid.qty) == ("B", 100, 5)
    assert (ask.side, ask.px, ask.qty) == ("S", 110, 5)
    # The DECIDING tick is echoed back, so the engine's m3 correlation attributes the
    # reaction to the book that caused it rather than to whatever is freshest on arrival.
    assert bid.md_seq == 1 and ask.md_seq == 1
    assert bid.epoch == 1 and ask.epoch == 1
    # Outbound envelope seq is contiguous from 1 and shared across both sides.
    assert [m.seq for m in msgs] == [1, 2]


def test_qty_is_clamped_to_max_qty() -> None:
    s = strategy(qty=99, max_qty=7)
    msgs = sent(s.on_tob(tob(1, 1), now_ns=0))
    assert [m.qty for m in msgs] == [7, 7]  # type: ignore[union-attr]


def test_an_unchanged_book_emits_nothing() -> None:
    """The acks are ASSERTED, not merely performed — and that is the whole case.

    Acking both sides was supposed to settle them so the unchanged tick reaches the
    unchanged-price guard. It did not: R1's re-evaluate-on-ack landed in the same commit, so
    with the guard deleted the two acks RE-QUOTE, both sides go back to PENDING_NEW, and the
    tick's `[]` still arrives through the in-flight branch. Asserting that each ack returns
    `[]` is what forces the settled state this case needs."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    ack_b = OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    ack_s = OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1)
    assert s.on_report(ack_b, 1) == []
    assert s.on_report(ack_s, 1) == []
    assert s.on_tob(tob(4, 2), now_ns=1) == []


def test_only_the_side_whose_price_moved_is_amended() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)

    msgs = sent(s.on_tob(tob(4, 2, ask_px=111), now_ns=2))

    assert len(msgs) == 2
    cancel, new = msgs
    assert isinstance(cancel, CancelOrder) and cancel.cl_id == "S-1"
    assert isinstance(new, NewOrder) and new.cl_id == "S-2" and new.px == 111
    # The bid was untouched: no duplicate, no churn.
    assert "B-1" in s.live_or_pending()


def test_a_side_with_work_in_flight_draws_no_second_order() -> None:
    """At most one live+pending order per side — the re-evaluation waits for the report."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)  # both sides PENDING_NEW, never acked
    assert s.on_tob(tob(2, 2, bid_px=101, ask_px=111), now_ns=1) == []
    assert sorted(s.live_or_pending()) == ["B-1", "S-1"]


def test_a_locked_book_produces_no_order() -> None:
    s = strategy()
    assert s.on_tob(tob(1, 1, bid_px=100, ask_px=100), now_ns=0) == []
    assert s.live_or_pending() == {}


def test_a_crossed_book_produces_no_order() -> None:
    s = strategy()
    assert s.on_tob(tob(1, 1, bid_px=105, ask_px=100), now_ns=0) == []
    assert s.live_or_pending() == {}


def test_a_full_fill_frees_the_side_and_the_next_tob_requotes_only_it() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    fill = Fill(v=1, seq=4, epoch=1, cl_id="B-1", eng_id=1, px=100, qty=5, leaves=0, exec_id=1)
    assert s.on_report(fill, 2) == []  # the re-quote waits for the freshest book
    assert "B-1" not in s.live_or_pending()

    msgs = sent(s.on_tob(tob(5, 2), now_ns=3))
    assert [m.cl_id for m in msgs] == ["B-2"]  # the ask never moved and is untouched


def test_a_partial_fill_leaves_the_order_working() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    partial = Fill(v=1, seq=3, epoch=1, cl_id="B-1", eng_id=1, px=100, qty=2, leaves=3, exec_id=1)
    assert s.on_report(partial, 2) == []
    assert s.live_or_pending()["B-1"] is OState.LIVE


def test_a_reject_leaves_the_side_idle_until_the_book_moves() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    rej = Reject(v=1, seq=2, epoch=1, cl_id="S-1", code="TICK_SIZE", reason="px off grid")
    assert s.on_report(rej, 1) == []
    assert "S-1" not in s.live_or_pending()
    # An unchanged book must NOT retry: that is the tight-resend loop this rule forbids.
    assert s.on_tob(tob(3, 2), now_ns=2) == []
    # A moved book may quote again.
    msgs = sent(s.on_tob(tob(4, 3, ask_px=111), now_ns=3))
    assert [m.cl_id for m in msgs] == ["S-2"]


def test_a_fill_racing_its_own_cancel_terminates_cleanly() -> None:
    """Invariant 3, client side: the engine may fill an order the client already cancelled,
    and then answer the cancel ALREADY_TERMINAL. Both must retire the order exactly once — it
    leaves the map on the fill, and the late report takes the unknown-order path — with no
    resend and the replacement untouched.

    (This said "land on one DONE". `OState` has no DONE: a terminal order leaves the map
    rather than being marked, which is the change recorded as PENDING_AMENDMENTS item (v).)"""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # cancels B-1, sends B-2
    assert s.live_or_pending()["B-1"] is OState.PENDING_CANCEL

    fill = Fill(v=1, seq=5, epoch=1, cl_id="B-1", eng_id=1, px=100, qty=5, leaves=0, exec_id=1)
    assert s.on_report(fill, 3) == []
    late = Reject(v=1, seq=6, epoch=1, cl_id="B-1", code="ALREADY_TERMINAL", reason="filled")
    assert s.on_report(late, 3) == []
    assert "B-1" not in s.live_or_pending()
    assert s.live_or_pending()["B-2"] is OState.PENDING_NEW  # the replacement is untouched


def test_a_report_for_an_unknown_cl_id_is_ignored() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    stray = Reject(v=1, seq=2, epoch=1, cl_id="", code="MALFORMED", reason="no id")
    assert s.on_report(stray, 1) == []
    assert sorted(s.live_or_pending()) == ["B-1", "S-1"]


def test_a_tob_arriving_as_a_report_still_drives_quoting() -> None:
    """on_report takes EngineMsg, and Tob is one: the adapters must not need to demux."""
    s = strategy()
    msgs = sent(s.on_report(tob(1, 1), 0))
    assert [m.cl_id for m in msgs] == ["B-1", "S-1"]


def test_a_cancel_ack_retires_the_order() -> None:
    """The ordinary end of an amend: B-1 leaves the map without waiting for the replacement's
    own ack, and the side stays owned by B-2 — `_retire`'s guard declines to free a side the
    replacement already holds.

    (This said "the side is free before the replacement's own ack", which is the opposite of
    what the module does and of what the body below asserts: `_current` reads {'B': 'B-2'}
    both before and after the CancelAck. The guard it misdescribes is pinned by three OTHER
    cases, never by this one.)"""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # cancel B-1, send B-2

    ack = CancelAck(v=1, seq=5, epoch=1, cl_id="B-1", eng_id=1, status="cancelled")
    assert s.on_report(ack, 3) == []
    assert "B-1" not in s.live_or_pending()
    assert s.live_or_pending()["B-2"] is OState.PENDING_NEW


def test_a_second_ack_for_a_cancelled_order_changes_nothing() -> None:
    """PENDING_NEW -> LIVE is a one-way step; a repeat must not resurrect a cancelled order."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # B-1 -> PENDING_CANCEL
    stale = OrderAck(v=1, seq=5, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(stale, 3) == []
    assert s.live_or_pending()["B-1"] is OState.PENDING_CANCEL


def test_the_symbol_on_every_emitted_order_is_the_configured_one() -> None:
    """Nothing else asserted it, so a strategy quoting the WRONG INSTRUMENT passed the whole
    suite. The cheapest possible mistake to make and the most expensive to ship."""
    s = strategy()
    msgs = sent(s.on_tob(tob(1, 1), now_ns=0))
    assert [m.symbol for m in msgs] == [SYMBOL, SYMBOL]  # type: ignore[union-attr]


def test_a_reject_for_a_superseded_order_does_not_bar_its_side() -> None:
    """The ALREADY_TERMINAL answer to a cancel names an order the side has already replaced.
    Arming the barrier from it would bar a price the engine never actually refused."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # B-1 superseded by B-2 at 101
    late = Reject(v=1, seq=5, epoch=1, cl_id="B-1", code="ALREADY_TERMINAL", reason="gone")
    s.on_report(late, 3)
    s.on_report(OrderAck(v=1, seq=6, epoch=1, cl_id="B-2", eng_id=3, status="live", svc_ns=1), 3)

    msgs = sent(s.on_tob(tob(7, 3, bid_px=100), now_ns=4))  # back to 100, never refused
    assert [m.cl_id for m in msgs] == ["B-2", "B-3"]


def test_a_successful_quote_clears_the_side_s_reject_barrier() -> None:
    """Without clearing, a price the engine refused ONCE would be barred for the session —
    long after the condition that caused the reject is gone."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    rej = Reject(v=1, seq=2, epoch=1, cl_id="S-1", code="TICK_SIZE", reason="px off grid")
    s.on_report(rej, 1)
    s.on_tob(tob(3, 2, ask_px=111), now_ns=2)  # S-2 at 111: barrier must lift
    s.on_report(OrderAck(v=1, seq=4, epoch=1, cl_id="S-2", eng_id=1, status="live", svc_ns=1), 3)

    msgs = sent(s.on_tob(tob(5, 3, ask_px=110), now_ns=4))  # back to the once-refused 110
    assert [m.cl_id for m in msgs] == ["S-2", "S-3"]


def test_a_settled_side_takes_the_decision_the_book_deferred() -> None:
    """The plan's words are "re-evaluate on NEXT REPORT". Deferring to the next BOOK instead
    leaves the side resting at a price the freshest accepted book already moved off — for as
    long as the feed takes to tick again, which on a quiet market is exactly when a stale
    quote is most dangerous."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)  # B-1@100, S-1@110, both PENDING_NEW
    assert s.on_tob(tob(2, 2, bid_px=101), now_ns=1) == []  # suppressed: B-1 in flight

    ack = OrderAck(v=1, seq=3, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    msgs = sent(s.on_report(ack, 2))

    assert [m.cl_id for m in msgs] == ["B-1", "B-2"]  # cancel the stale price, quote 101
    assert isinstance(msgs[1], NewOrder) and msgs[1].px == 101
    assert msgs[1].md_seq == 2  # the book that decided it, not the one that created B-1


def test_a_settled_side_at_an_unchanged_price_stays_quiet() -> None:
    """The mirror: re-evaluation must not become a re-quote on every ack."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    ack = OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(ack, 1) == []


def test_the_reject_barrier_lifts_as_soon_as_the_desired_price_moves() -> None:
    """Clearing it only where a replacement is EMITTED stranded a side: a locked book moved
    the desired price off the refused one but returned early on never-cross, so the bar
    survived and the side stayed silent even back at a price never refused."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(Reject(v=1, seq=2, epoch=1, cl_id="S-1", code="TICK_SIZE", reason="grid"), 1)
    assert s.on_tob(tob(3, 2, bid_px=111, ask_px=111), now_ns=2) == []  # locked: no order

    msgs = sent(s.on_tob(tob(4, 3, bid_px=100, ask_px=110), now_ns=3))
    assert [m.cl_id for m in msgs] == ["S-2"]


def test_an_amendment_stamps_fresh_outbound_seqs_and_the_deciding_md_seq() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)  # outbound seq 1, 2
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)

    msgs = sent(s.on_tob(tob(4, 2, ask_px=111), now_ns=2))
    assert [m.seq for m in msgs] == [3, 4]  # contiguous, never reused
    assert isinstance(msgs[1], NewOrder) and msgs[1].md_seq == 2 and msgs[1].symbol == SYMBOL


def test_the_replacement_survives_late_reports_for_the_order_it_replaced() -> None:
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # B-2@101 replaces B-1
    s.on_report(
        Fill(v=1, seq=5, epoch=1, cl_id="B-1", eng_id=1, px=100, qty=5, leaves=0, exec_id=1), 3
    )
    s.on_report(Reject(v=1, seq=6, epoch=1, cl_id="B-1", code="ALREADY_TERMINAL", reason=""), 3)
    s.on_report(OrderAck(v=1, seq=7, epoch=1, cl_id="B-2", eng_id=3, status="live", svc_ns=1), 3)

    msgs = sent(s.on_tob(tob(8, 3, bid_px=100), now_ns=4))
    assert [m.cl_id for m in msgs] == ["B-2", "B-3"]  # B-2 is current and gets replaced


def test_terminal_orders_do_not_accumulate_across_the_session() -> None:
    """_orders is walked by the stale sweep, so retaining every terminal order made both
    memory and that sweep grow with the session's LIFETIME order count."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    seq = 4
    for i in range(20):
        px = 100 + (i % 2)
        s.on_tob(tob(seq, 2 + i, bid_px=px), now_ns=i)
        seq += 1
        cl = f"B-{i + 1}"
        s.on_report(CancelAck(v=1, seq=seq, epoch=1, cl_id=cl, eng_id=1, status="cancelled"), i)
        seq += 1
    # The public surface suffices: live_or_pending() is a plain copy of the map now that
    # _retire deletes rather than marks, so a retained order IS visible through it. (An
    # earlier comment here claimed otherwise and justified reaching into `_orders`; both
    # halves of that claim stopped being true when DONE was removed.)
    assert len(s.live_or_pending()) <= 2


def test_every_quote_is_post_only() -> None:
    """The BRACES to the never-cross belt, and spec 2.3 requires it of a strategy quote.
    Nothing asserted it: flipping the Struct default to False passed both suites."""
    s = strategy()
    msgs = sent(s.on_tob(tob(1, 1), now_ns=0))
    assert all(isinstance(m, NewOrder) and m.post_only is True for m in msgs)


def test_a_superseded_reject_does_not_bar_the_price_the_replacement_returns_to() -> None:
    """Asserts at the point the barrier would BITE. The earlier case could not see the guard
    because the intervening ack re-ran the side's decision at a different price, clearing a
    wrongly-armed bar before the assertion — two correct fixes blinding each other."""
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)
    s.on_report(OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1), 1)
    s.on_report(OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=2, status="live", svc_ns=1), 1)
    s.on_tob(tob(4, 2, bid_px=101), now_ns=2)  # B-2@101 supersedes B-1@100
    s.on_report(Reject(v=1, seq=5, epoch=1, cl_id="B-1", code="ALREADY_TERMINAL", reason=""), 3)
    s.on_tob(tob(6, 3, bid_px=100), now_ns=4)  # book BACK to 100 while B-2 is still pending

    ack = OrderAck(v=1, seq=7, epoch=1, cl_id="B-2", eng_id=3, status="live", svc_ns=1)
    msgs = sent(s.on_report(ack, 5))
    assert [m.cl_id for m in msgs] == ["B-2", "B-3"]  # 100 was never refused


def test_the_ask_side_also_takes_the_decision_the_book_deferred() -> None:
    """The ASK mirror of `test_a_settled_side_takes_the_decision_the_book_deferred`.

    Added because the Task 8 coverage gate proved the ask half was executed but never
    OBSERVED: `_reconsider` was entered 21 times for the ask across both suites and returned
    an empty list every single time, so branch coverage read 100% while two independent
    mutants — hardcoding the desired price to the bid, and returning [] for any non-bid side —
    survived the entire 481-test suite. The code is side-symmetric, which is exactly why one
    side's case does not stand in for the other's: a defect that keys on the side is invisible
    to a suite that only ever exercises one.
    """
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)  # B-1@100, S-1@110, both PENDING_NEW
    assert s.on_tob(tob(2, 2, ask_px=111), now_ns=1) == []  # suppressed: S-1 in flight

    ack = OrderAck(v=1, seq=3, epoch=1, cl_id="S-1", eng_id=1, status="live", svc_ns=1)
    msgs = sent(s.on_report(ack, 2))

    assert [m.cl_id for m in msgs] == ["S-1", "S-2"]  # cancel the stale price, quote 111
    assert isinstance(msgs[1], NewOrder) and msgs[1].px == 111
    assert msgs[1].md_seq == 2  # the book that decided it, not the one that created S-1


def test_a_reject_bars_only_the_side_it_names() -> None:
    """The barrier is per-side, and nothing pinned the WRITE half of that.

    Reading the barrier from the wrong side is caught by four cases; ARMING both sides from
    one reject survived all 481, because every reject case in the suite refuses S-1 while the
    bid happens to be blocked or wanting a different price anyway. Here the bid is settled and
    then asked for exactly the price the ASK was refused at, so a barrier that leaked across
    sides would silence a bid that has no reason to be silent.
    """
    s = strategy()
    s.on_tob(tob(1, 1), now_ns=0)  # B-1@100, S-1@110
    ack = OrderAck(v=1, seq=2, epoch=1, cl_id="B-1", eng_id=1, status="live", svc_ns=1)
    assert s.on_report(ack, 1) == []  # bid settled at 100, unchanged book

    reject = Reject(v=1, seq=3, epoch=1, cl_id="S-1", code="TICK_SIZE", reason="off grid")
    assert sent(s.on_report(reject, 2)) == []  # ask barred at 110

    # The bid now WANTS 110 — the very price the ask was refused at.
    msgs = sent(s.on_tob(tob(4, 4, bid_px=110, ask_px=120), now_ns=3))
    assert [m.cl_id for m in msgs] == ["B-1", "B-2", "S-2"], (
        "the bid must amend to 110 despite the ask having been refused there"
    )
