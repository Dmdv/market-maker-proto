"""The market-making decision core: a pure state machine, driven, never driving.

Decision record D2's binding condition is that this module holds NO sockets, NO clocks and
NO awaits. Every input arrives as an argument and every output is a returned command, so the
same object runs unchanged under both Task 9 transports — which is what makes the naive-vs-
product comparison a like-for-like measurement instead of two different clients. Time is a
parameter (``now_ns``) for the same reason: a strategy that read a clock could not be
replayed, and replay is how its decisions are pinned.

WHAT IT DECIDES. Quote both sides at the touch, one order per side, fixed ``qty`` clamped to
``max_qty``. Re-quote only the side whose desired price actually moved; a side with anything
in flight waits for its report rather than stacking a second order. Never emit a bid at or
above the ask, or an ask at or below the bid — a belt over the engine's ``post_only``, and
the reason a locked or crossed book produces nothing at all rather than a crossing order.

THE TWO COUNTERS (08 F-10), which are read very differently:
  * the ENVELOPE ``seq`` is the engine's per-session contiguity claim. A gap means a message
    was lost, so the local order picture is no longer trustworthy and quoting STOPS.
  * ``md_seq`` may skip — conflation is deliberate and visible — so a forward gap is COUNTED
    and quoting continues. A DECREASE is a different animal: the book went backwards, which
    the engine's monotonic feed cannot do, so that stops quoting too. A repeat is neither; it
    is the same book restated, and the answer to an unchanged book is silence.
"""

from dataclasses import dataclass, field
from enum import Enum, auto

from mmclient.protocol import (
    CancelAck,
    CancelOrder,
    EngineMsg,
    Fill,
    NewOrder,
    OrderAck,
    Reject,
    Tob,
)

__all__ = ["Cmd", "OState", "SendCmd", "StopQuoting", "Strategy"]

_BID = "B"
_ASK = "S"


class OState(Enum):
    """Where one order stands, from THIS side of the wire.

    The client's view is always a report behind the engine's, which is the whole reason
    PENDING_NEW and PENDING_CANCEL exist as states rather than as booleans: an order in
    either is one the engine may still be deciding about, and deciding again on top of it is
    how a resend storm starts.

    THREE states, not four: a terminal order LEAVES the map, so there is no DONE to observe.
    A fourth member would be a state `live_or_pending()` can never return and a branch a
    consumer could never take — the API would be documenting something it cannot express.
    """

    PENDING_NEW = auto()
    LIVE = auto()
    PENDING_CANCEL = auto()


@dataclass(frozen=True)
class SendCmd:
    """Put this message on the wire. The transport owns HOW; this module owns WHAT."""

    msg: NewOrder | CancelOrder


@dataclass(frozen=True)
class StopQuoting:
    """Stop, and here is why.

    THE REASON IS WIRE-VISIBLE, and its first words are load-bearing. Both adapters send it as
    the close-frame reason (truncated at 120 bytes), and `_session._note_stop` selects the
    close code by testing whether it starts with `"stale feed"` — 4000 if so, 1002 otherwise.
    So rewording that prefix is a PROTOCOL change, not a copy edit, and a mutation of it is
    caught by three consumer-side tests.

    (An earlier version of this docstring said the adapters log the reason and the demo prints
    it. Neither was true — there is no logging call anywhere in the client — and the one thing
    the field actually does went unstated. If the text is ever to be free prose, the
    discrimination must move to a typed field on this class first.)"""

    reason: str


Cmd = SendCmd | StopQuoting


@dataclass
class _Order:
    cl_id: str
    px: int
    state: OState


@dataclass
class Strategy:
    """One session's quoting decisions. Construct once, then ``on_connect`` per connection."""

    symbol: str
    qty: int
    max_qty: int
    stale_ns: int
    epoch: int = 0
    # init=False: this is a session METRIC that on_connect owns, and a constructor argument
    # would let a caller seed the figure the demo reports.
    conflated_seen: int = field(default=0, init=False)

    _orders: dict[str, _Order] = field(default_factory=dict, init=False)
    _current: dict[str, str] = field(default_factory=dict, init=False)
    _next_cl: dict[str, int] = field(default_factory=dict, init=False)
    _rejected_px: dict[str, int] = field(default_factory=dict, init=False)
    _out_seq: int = field(default=0, init=False)
    _in_seq: int = field(default=0, init=False)
    _md_seq: int = field(default=0, init=False)
    _last_tob_ns: int | None = field(default=None, init=False)
    # The decision inputs as PLAIN INTS, never the caller's Tob. msgspec.Struct is not
    # frozen, so retaining the object let a transport that touched a tick after handing it
    # in change decisions taken later — measured: a caller mutating bid_px after on_tob
    # returned produced a quote at a price no accepted tick ever carried. Two transports
    # drive this object and the whole A/B rests on identical decisions from identical input.
    _bid_px: int | None = field(default=None, init=False)
    _ask_px: int | None = field(default=None, init=False)
    _stopped: bool = field(default=False, init=False)
    # SEPARATE from _stopped, deliberately. One flag carrying both meanings let a
    # protocol-fatal stop disarm the sweep, leaving both sides RESTING on the engine with
    # nothing left to pull them — and a protocol fault is a strictly worse position to hold
    # unmanaged quotes from than a stale feed, which does cancel. _stopped means "emit no
    # new quotes"; this means "the one-shot pull has already run".
    _swept: bool = field(default=False, init=False)

    # --- lifecycle ---------------------------------------------------------------------

    def on_connect(self, epoch: int) -> list[Cmd]:
        """A new connection is a new epoch, and a new epoch is a CLEAN SLATE.

        The engine cancels the prior epoch's live orders on its side; keeping any local
        record of them would mean quoting against orders that no longer exist. Both counters
        restart because both are per-session claims.
        """
        self.epoch = epoch
        self.conflated_seen = 0
        self._orders.clear()
        self._current.clear()
        self._next_cl.clear()
        self._rejected_px.clear()
        self._out_seq = 0
        self._in_seq = 0
        self._md_seq = 0
        self._last_tob_ns = None
        self._bid_px = None
        self._ask_px = None
        self._stopped = False
        self._swept = False
        return []

    def on_disconnect(self) -> list[Cmd]:
        """The transport is gone, so nothing can be cancelled — only forgotten. The engine
        retires the epoch itself; a reconnect gets a fresh one."""
        self._orders.clear()
        self._current.clear()
        self._bid_px = None
        self._ask_px = None
        # The staleness origin goes with the connection, exactly as on_connect treats it.
        # Left set, a timer firing after the transport died reported the dead session as a
        # STALE FEED — there is no feed to be stale — and burned the one-shot pull with it.
        self._last_tob_ns = None
        self._stopped = True
        return [StopQuoting("disconnect")]

    def on_timer(self, now_ns: int) -> list[Cmd]:
        """The stale-feed check, and the only rule that fires without an inbound message.

        A feed that has gone quiet is indistinguishable from a venue that has stopped
        moving, and quoting into the first is how a market maker gets picked off. Cancels
        what is actually resting; a PENDING_CANCEL order is skipped because its cancel is
        already in flight, not because it is terminal. Latches, so a timer that keeps firing
        does not keep cancelling.
        """
        # Guarded by _swept, NOT by _stopped: quoting having stopped is exactly when
        # resting orders most need pulling. Only the sweep's own one-shot latch may skip it.
        #
        # The GUARD TERM's independent effect is unobservable today — `_stopped` prevents any
        # order returning to a resting state, so a second sweep finds an empty filter and
        # emits nothing whether or not this short-circuit exists. It is kept as the explicit
        # statement of the one-shot rule (the assignment below IS pinned), and it becomes
        # load-bearing the moment anything can re-arm a side after a stop.
        if self._swept or self._last_tob_ns is None:
            return []
        age = now_ns - self._last_tob_ns
        if age <= self.stale_ns:
            return []
        # The stale reason is announced ONLY when staleness is genuinely the cause. Once the
        # sweep was decoupled from `_stopped` so a protocol fault could still pull quotes, it
        # started reporting "stale feed: N ns since the last book" for faults where books had
        # in fact been arriving — `_last_tob_ns` freezes at the fault, because on_tob returns
        # before the freshness write, so both the diagnosis and the age were fabricated. A
        # session that already stopped for a stated reason gets its cancels and no second,
        # contradictory reason.
        cmds: list[Cmd] = []
        if not self._stopped:
            cmds.append(
                StopQuoting(f"stale feed: {age} ns since the last book, limit {self.stale_ns}")
            )
        self._stopped = True
        self._swept = True
        resting = [o for o in self._orders.values() if o.state in (OState.PENDING_NEW, OState.LIVE)]
        cmds.extend(self._cancel(order) for order in resting)
        return cmds

    # --- inbound -----------------------------------------------------------------------

    def on_tob(self, tob: Tob, now_ns: int) -> list[Cmd]:
        """A book. Both counters are judged BEFORE it is allowed to move any quote."""
        refused = self._admit(tob)
        if refused is not None:
            return refused

        if tob.md_seq < self._md_seq:
            return self._stop(f"md_seq decreased: {self._md_seq} -> {tob.md_seq}")
        # Freshness is updated here, BEFORE the duplicate return: a repeated md_seq is a
        # tick that arrived, and the stale rule asks whether the FEED is alive, not whether
        # the book changed. Reading it after the return declared a heartbeating feed stale
        # and cancelled everything on it.
        self._last_tob_ns = now_ns
        if tob.md_seq == self._md_seq:
            return []  # the same book restated: an unchanged book has no answer
        if self._md_seq != 0 and tob.md_seq > self._md_seq + 1:
            self.conflated_seen += 1

        self._md_seq = tob.md_seq
        self._bid_px = tob.bid_px
        self._ask_px = tob.ask_px
        return [
            *self._quote(_BID, tob.bid_px, tob.ask_px, tob.bid_px, tob.md_seq),
            *self._quote(_ASK, tob.ask_px, tob.ask_px, tob.bid_px, tob.md_seq),
        ]

    def _admit(self, tob: Tob) -> list[Cmd] | None:
        """Whether this tick is ours to act on. None means proceed.

        The ORDER is the content: a foreign EPOCH belongs to a different session and must
        not advance this one's envelope counter, so it is judged BEFORE the counter. A
        foreign SYMBOL arrives inside THIS session's contiguous stream, so it is judged
        AFTER — declining to consume its seq would manufacture a gap the engine never
        created and close 1002 on a fault of our own making.

        And it STOPS on a foreign symbol rather than ignoring it, because ignoring is the
        silent failure that hurts most: a misconfigured symbol makes every tick foreign, so
        the staleness origin is never written, the sweep can never arm, and the client runs
        indefinitely quoting nothing and never saying why.
        """
        if self._stopped:
            return []
        if tob.epoch != self.epoch:
            return []
        gap = self._advance_seq(tob.seq)
        if gap is not None:
            return gap
        if tob.symbol != self.symbol:
            return self._stop(f"unexpected symbol: {tob.symbol}, expected {self.symbol}")
        return None

    def on_report(self, msg: EngineMsg, now_ns: int) -> list[Cmd]:
        """Any engine message.

        An OrderAck SETTLES a side, so it re-runs that side's decision against the freshest
        accepted book — the plan's "re-evaluate on next report". Every other report ENDS
        something and defers to the next tick.

        Deliberately not "each of those frees a side": a partial Fill frees nothing (only
        `leaves == 0` retires), and the CancelAck of an amend frees nothing either, because
        `_retire`'s guard declines to release a side the replacement already owns. What makes
        the deferral safe is that a side an ENDING report did free is FLAT, and holding no
        quote is never unsafe — unlike the ack case above, where the danger is a resting
        order at a price the book has moved off.
        """
        if isinstance(msg, Tob):
            return self.on_tob(msg, now_ns)
        # Combined rather than sequential: both are "this message is not ours to act on",
        # and separating them bought a seventh return statement, not clarity.
        if self._stopped or msg.epoch != self.epoch:
            return []
        gap = self._advance_seq(msg.seq)
        if gap is not None:
            return gap

        order = self._orders.get(msg.cl_id)
        if order is None:
            return []  # a reject with no cl_id, or a report for an epoch we have dropped
        if isinstance(msg, OrderAck):
            if order.state is OState.PENDING_NEW:
                order.state = OState.LIVE
                # The plan's words: "no new command for a side with anything in flight
                # (RE-EVALUATE ON NEXT REPORT)". Deferring to the next BOOK instead leaves
                # the side resting at a price the freshest accepted book already moved off —
                # for however long the feed takes to tick again, which on a quiet market is
                # exactly when a stale quote is most dangerous.
                return self._reconsider(_side_of(order.cl_id))
        elif isinstance(msg, CancelAck):
            self._retire(order)
        elif isinstance(msg, Fill):
            if msg.leaves == 0:
                self._retire(order)
        # Reject is NAMED, not caught by a trailing else. An else would silently absorb any
        # future EngineMsg variant into "this order is dead", which is the wrong default for
        # a message type nobody has considered yet — and it would do so invisibly, because
        # the tests would still pass. The cost is one branch coverage cannot reach: with
        # EngineMsg closed at five variants and Tob handled above, falling past this elif is
        # impossible by construction, which is what the pragma records (same use as
        # protocol.py:439 and _preflight.py:219).
        elif isinstance(msg, Reject):  # pragma: no branch - EngineMsg has no sixth variant
            # The price the engine just refused is remembered for this side, and the side
            # stays silent until the desired price MOVES off it. "Idle until the next tick"
            # is not enough: the feed heartbeats, so the next tick arrives on its own and
            # would re-send the identical rejected order, once per tick, forever.
            if self._current.get(_side_of(order.cl_id)) == order.cl_id:
                self._rejected_px[_side_of(order.cl_id)] = order.px
            self._retire(order)
        return []

    def _reconsider(self, side: str) -> list[Cmd]:
        """Re-run one side's decision against the freshest ACCEPTED book, after a report
        settled whatever was in flight there."""
        if self._bid_px is None or self._ask_px is None:  # pragma: no cover - order ⇒ book
            return []
        desired = self._bid_px if side == _BID else self._ask_px
        return self._quote(side, desired, self._ask_px, self._bid_px, self._md_seq)

    def live_or_pending(self) -> dict[str, OState]:
        """Every order this client still believes the engine is carrying. Inspection surface
        for the tests and for Task 10's demo output; not part of the decision path."""
        # No filter is possible: _retire DELETES the entry, so every key still in the map
        # is an order the engine may still be carrying. There is no terminal state to hide.
        return {cl_id: o.state for cl_id, o in self._orders.items()}

    # --- internals ---------------------------------------------------------------------

    def _advance_seq(self, seq: int) -> list[Cmd] | None:
        """The envelope counter, which spans EVERY inbound message rather than just the
        ticks: the engine stamps one contiguous sequence per session, so a report can expose
        a loss a tick never would."""
        expected = self._in_seq + 1
        if seq != expected:
            return self._stop(f"inbound seq gap: expected {expected}, got {seq}")
        self._in_seq = seq
        return None

    def _stop(self, reason: str) -> list[Cmd]:
        self._stopped = True
        return [StopQuoting(reason)]

    def _quote(
        self, side: str, desired_px: int, ask_px: int, bid_px: int, md_seq: int
    ) -> list[Cmd]:
        # The reject barrier is resolved FIRST, before the never-cross and in-flight returns
        # below, because the bar is discharged by the first EVALUATED tick whose desired
        # price differs — whether or not that tick could emit anything. Clearing it only
        # where a replacement went out stranded a side: a locked book moved the desired
        # price off the refused one, which should have lifted the bar, but returned early on
        # never-cross, so the bar survived and the side stayed silent even once the book came
        # back to the refused price that a later tick had already discharged.
        if self._rejected_px.get(side) is not None:
            if self._rejected_px[side] == desired_px:
                return []  # exactly the price the engine refused; wait for the book to move
            del self._rejected_px[side]
        # The never-cross belt (F-28), applied to the DESIRED price before anything is sent:
        # a locked or crossed book fails both tests, so it produces silence rather than an
        # order the engine would reject. post_only would catch it too; this is the belt.
        if side == _BID and desired_px >= ask_px:
            return []
        if side == _ASK and desired_px <= bid_px:
            return []

        cl_id = self._current.get(side)
        order = self._orders.get(cl_id) if cl_id is not None else None
        # PENDING_NEW only: a side's CURRENT order is never PENDING_CANCEL here, because
        # _cancel runs only from this function — where _new immediately takes the side — and
        # from the stale sweep, which latches _stopped and blocks every later quote.
        if order is not None and order.state is OState.PENDING_NEW:
            return []  # already deciding on this side; re-evaluate when the report lands
        if order is not None and order.px == desired_px:
            return []  # the quote we would send is the quote already resting

        cmds: list[Cmd] = []
        if order is not None:
            cmds.append(self._cancel(order))
        cmds.append(self._new(side, desired_px, md_seq))
        return cmds

    def _new(self, side: str, px: int, md_seq: int) -> SendCmd:
        n = self._next_cl.get(side, 0) + 1
        self._next_cl[side] = n
        cl_id = f"{side}-{n}"
        self._orders[cl_id] = _Order(cl_id=cl_id, px=px, state=OState.PENDING_NEW)
        self._current[side] = cl_id
        self._out_seq += 1
        return SendCmd(
            NewOrder(
                v=1,
                # PLACEHOLDERS. `_session._stamp` overwrites both at emit, because the envelope
                # counter and the epoch are properties of a CONNECTION and this module is
                # deliberately ignorant of connections. They are written at all only because
                # msgspec requires every field at construction. Do not read a wire guarantee
                # into them: a mutant setting both to 999 here passes every Task 9 test.
                seq=self._out_seq,
                epoch=self.epoch,
                md_seq=md_seq,
                cl_id=cl_id,
                symbol=self.symbol,
                side=side,
                px=px,
                qty=min(self.qty, self.max_qty),
                # Stated here rather than inherited from the Struct default: this is the
                # BRACES to the never-cross belt above, and spec 2.3 makes it a requirement
                # of a strategy quote. A default can be changed in another module.
                post_only=True,
            )
        )

    def _cancel(self, order: _Order) -> SendCmd:
        order.state = OState.PENDING_CANCEL
        self._out_seq += 1
        # seq/epoch are placeholders here too — see `_new`.
        return SendCmd(CancelOrder(v=1, seq=self._out_seq, epoch=self.epoch, cl_id=order.cl_id))

    def _retire(self, order: _Order) -> None:
        """Terminal: the order leaves the map, and frees its side only if it still held it.

        The guard is load-bearing for the SUPERSEDED case — an order cancelled during an
        amend retires while the replacement already owns the side, and freeing the side
        there would strand the replacement. (A fill racing its own cancel no longer reaches
        here twice: the entry is gone after the first, so the engine's ALREADY_TERMINAL
        answer takes the unknown-order path.)
        """
        side = _side_of(order.cl_id)
        if self._current.get(side) == order.cl_id:
            del self._current[side]
        # DROPPED, not merely marked. Keeping every terminal order made _orders grow with
        # the session's LIFETIME order count — 2002 entries after 1000 amendment cycles —
        # and the stale sweep walks that map. A later report for a dropped cl_id takes the
        # unknown-order path, which already advances the envelope counter and returns [].
        del self._orders[order.cl_id]


def _side_of(cl_id: str) -> str:
    """``B-3`` -> ``B``. The prefix is assigned by ``_new`` and never parsed off the wire."""
    return cl_id[0]
