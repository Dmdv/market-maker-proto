"""The market-making decision core: a pure state machine, driven, never driving.

No sockets, no clocks, no awaits (time is the ``now_ns`` parameter), so both transports drive it
unchanged. An envelope ``seq`` gap or an ``md_seq`` DECREASE stops quoting; a skip is conflation.
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

    The client's view is a report behind the engine's, so PENDING_NEW and PENDING_CANCEL are
    states: deciding again on one is how a resend storm starts. A terminal order LEAVES the map.
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

    WIRE-VISIBLE, and the first words are load-bearing: `_session._note_stop` picks 4000 when the
    reason starts with `"stale feed"` and 1002 otherwise; rewording that prefix changes the wire.
    """

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
    # init=False: a session METRIC that on_connect owns, not a figure a caller may seed.
    conflated_seen: int = field(default=0, init=False)

    _orders: dict[str, _Order] = field(default_factory=dict, init=False)
    _current: dict[str, str] = field(default_factory=dict, init=False)
    _next_cl: dict[str, int] = field(default_factory=dict, init=False)
    _rejected_px: dict[str, int] = field(default_factory=dict, init=False)
    _out_seq: int = field(default=0, init=False)
    _in_seq: int = field(default=0, init=False)
    _md_seq: int = field(default=0, init=False)
    _last_tob_ns: int | None = field(default=None, init=False)
    # The decision inputs as PLAIN INTS, never the caller's Tob: msgspec.Struct is not frozen, so
    # retaining it let a transport mutate a tick after handing it in and change later decisions.
    _bid_px: int | None = field(default=None, init=False)
    _ask_px: int | None = field(default=None, init=False)
    _stopped: bool = field(default=False, init=False)
    # SEPARATE from _stopped: one flag carrying both meanings let a protocol-fatal stop disarm the
    # sweep. _stopped means "emit no new quotes"; this means "the one-shot pull has already run".
    _swept: bool = field(default=False, init=False)

    # --- lifecycle ---------------------------------------------------------------------

    def on_connect(self, epoch: int) -> list[Cmd]:
        """A new connection is a new epoch, and a new epoch is a CLEAN SLATE.

        The engine cancels the prior epoch's live orders, so any local record of them would mean
        quoting against orders that no longer exist. Both counters are per-session claims.
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
        # The staleness origin goes with the connection: left set, a timer firing after the
        # transport died would report the dead session as a STALE FEED.
        self._last_tob_ns = None
        self._stopped = True
        return [StopQuoting("disconnect")]

    def on_timer(self, now_ns: int) -> list[Cmd]:
        """The stale-feed check, and the only rule that fires without an inbound message.

        A quiet feed is indistinguishable from a still venue, and quoting into the first is how a
        market maker gets picked off. Cancels what is resting, and latches so it fires once.
        """
        # Guarded by _swept, NOT by _stopped: quoting having stopped is exactly when resting
        # orders most need pulling. Only the sweep's own one-shot latch may skip it.
        if self._swept or self._last_tob_ns is None:
            return []
        age = now_ns - self._last_tob_ns
        if age <= self.stale_ns:
            return []
        # The stale reason is announced ONLY when staleness is genuinely the cause: `_last_tob_ns`
        # freezes at a fault, so a session already stopped gets its cancels and no second reason.
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
        # Freshness is updated BEFORE the duplicate return: a repeated md_seq is a tick that
        # arrived, and the stale rule asks whether the FEED is alive, not whether the book moved.
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

        A foreign EPOCH is judged BEFORE the envelope counter and a foreign SYMBOL after —
        declining its seq would manufacture a gap. A foreign symbol STOPS: ignoring it is silent.
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
        accepted book. Every other report ENDS something and defers safely: what it freed is FLAT.
        """
        if isinstance(msg, Tob):
            return self.on_tob(msg, now_ns)
        # Combined rather than sequential: both are "this message is not ours to act on".
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
                # The design's words: "no new command for a side with anything in flight
                # (RE-EVALUATE ON NEXT REPORT)" — deferring to the next BOOK leaves a stale quote.
                return self._reconsider(_side_of(order.cl_id))
        elif isinstance(msg, CancelAck):
            self._retire(order)
        elif isinstance(msg, Fill):
            if msg.leaves == 0:
                self._retire(order)
        # Reject is NAMED, not caught by a trailing else: an else would silently absorb any future
        # EngineMsg variant into "this order is dead". The cost is one unreachable branch.
        elif isinstance(msg, Reject):  # pragma: no branch - EngineMsg has no sixth variant
            # The refused price is remembered for this side, and the side stays silent until the
            # desired price MOVES off it — the feed heartbeats, so "idle" would re-send forever.
            if self._current.get(_side_of(order.cl_id)) == order.cl_id:
                self._rejected_px[_side_of(order.cl_id)] = order.px
            self._retire(order)
        return []

    def _reconsider(self, side: str) -> list[Cmd]:
        """Re-run one side's decision against the freshest ACCEPTED book, after a report settled
        whatever was in flight there."""
        if self._bid_px is None or self._ask_px is None:  # pragma: no cover - order ⇒ book
            return []
        desired = self._bid_px if side == _BID else self._ask_px
        return self._quote(side, desired, self._ask_px, self._bid_px, self._md_seq)

    def live_or_pending(self) -> dict[str, OState]:
        """Every order this client still believes the engine is carrying; inspection only."""
        # No filter is possible: _retire DELETES the entry, so there is no terminal state to hide.
        return {cl_id: o.state for cl_id, o in self._orders.items()}

    # --- internals ---------------------------------------------------------------------

    def _advance_seq(self, seq: int) -> list[Cmd] | None:
        """The envelope counter, which spans EVERY inbound message rather than just the ticks:
        one contiguous sequence per session, so a report can expose a loss a tick never would."""
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
        # The reject barrier is resolved FIRST, before the never-cross and in-flight returns: the
        # bar is discharged by the first EVALUATED tick whose desired price differs, emit or not.
        if self._rejected_px.get(side) is not None:
            if self._rejected_px[side] == desired_px:
                return []  # exactly the price the engine refused; wait for the book to move
            del self._rejected_px[side]
        # The never-cross belt, applied to the DESIRED price: a locked or crossed book produces
        # silence rather than an order the engine would reject. post_only is the braces.
        if side == _BID and desired_px >= ask_px:
            return []
        if side == _ASK and desired_px <= bid_px:
            return []

        cl_id = self._current.get(side)
        order = self._orders.get(cl_id) if cl_id is not None else None
        # PENDING_NEW only: a side's CURRENT order is never PENDING_CANCEL here, because _cancel
        # runs only from this function and from the stale sweep, which latches _stopped.
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
                # PLACEHOLDERS: `_session._stamp` overwrites both at emit, because the envelope
                # counter and the epoch are properties of a CONNECTION. No wire guarantee here.
                seq=self._out_seq,
                epoch=self.epoch,
                md_seq=md_seq,
                cl_id=cl_id,
                symbol=self.symbol,
                side=side,
                px=px,
                qty=min(self.qty, self.max_qty),
                # Stated here rather than inherited from the Struct default: this is the braces to
                # the never-cross belt above, and spec 2.3 makes it a requirement of a quote.
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

        The guard is load-bearing for the SUPERSEDED case — an order cancelled during an amend
        retires while the replacement already owns the side, which freeing would strand.
        """
        side = _side_of(order.cl_id)
        if self._current.get(side) == order.cl_id:
            del self._current[side]
        # DROPPED, not merely marked: keeping terminal orders made _orders grow with the session's
        # lifetime order count, and the stale sweep walks that map.
        del self._orders[order.cl_id]


def _side_of(cl_id: str) -> str:
    """``B-3`` -> ``B``. The prefix is assigned by ``_new`` and never parsed off the wire."""
    return cl_id[0]
