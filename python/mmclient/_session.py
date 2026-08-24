"""The transport-independent half of a client session.

Both adapters own exactly one thing — bytes on and off a socket — and everything else lives
here: envelope sequencing, epoch tracking, driving the strategy, and turning its commands
back into frames. That split is not tidiness, it is what makes the §6 measurement valid. The
naive-vs-tuned number is only a like-for-like comparison if the two arms differ in transport
and codec and in NOTHING else, and two hand-copied session loops diverge — one grows a retry
the other lacks, one stamps a counter a beat earlier. Written once, they cannot.

Addition to the plan's Task 9 file list (`ws_naive.py`, `ws_picows.py`, `app.py`), queued as
a PENDING amendment for the same reason the C++ side queued its own splits.
"""

from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum, auto

from mmclient.protocol import ClientMsg, EngineMsg
from mmclient.strategy import Cmd, SendCmd, StopQuoting, Strategy

__all__ = [
    "MAX_MSG_BYTES",
    "CloseIntent",
    "Codec",
    "Ending",
    "SessionDriver",
    "classify_close_body",
    "is_sendable_close_code",
]

# The transport cap, and the reason it lives here rather than in each adapter: it is ONE
# number that must agree in three places — both adapters' frame limits and the codec's own
# restatement of it in `_preflight._MAX_FRAME_BYTES` — and the engine enforces a two-tier
# bound around exactly this figure (policy cap 64 KiB, transport ceiling 128 KiB). A client
# that reassembled past it would be building a message the engine has already refused.
MAX_MSG_BYTES = 64 * 1024

# Close codes this client may send, from the plan's normative table. 1002 is the client's
# verdict on the ENGINE's framing — an envelope-seq gap or an md_seq decrease — and 4000 is
# the private code that distinguishes a stale-feed stop from every transport cause, which is
# the whole reason it is not just another 1002.
CLOSE_NORMAL = 1000
CLOSE_PROTOCOL = 1002
# The size verdict, distinct from the framing one. A message that breaches the cap is not
# malformed — it is well-formed and too large — and the two arms disagreed about this: a single
# oversized frame yields 1009 from both libraries, but a REASSEMBLED breach was closing 1002 on
# the tuned arm because the only latch available said "protocol".
CLOSE_TOO_BIG = 1009
# The PAYLOAD verdict: a TEXT frame whose bytes are not valid UTF-8. RFC 6455 §8.1 makes that
# 1007, and Beast answers it with `close_code::bad_payload` — so an engine that receives one from
# us closes 1007. The two client arms disagreed until this existed: `websockets` validates UTF-8
# itself and hands up a `str`, while picows hands up raw bytes that then failed in the JSON
# decoder and were latched 1002 — "you broke the framing" for what is actually "your text is not
# text". Same wire, same fault, two different codes depending on which arm was measured.
CLOSE_BAD_PAYLOAD = 1007
CLOSE_STALE = 4000

# Codes this client may put on the wire — initiate or echo. One table, both arms.
#
# Beast is the reference (docs/PROTOCOL.md §8.5b): an EMPTY close body is legal and carries
# no code at all, while an ENCODED zero is not a close code and is rejected; 1014 is reserved
# in this Boost version even though both Python libraries accept it. The two Python stacks
# disagreed with each other AND with the engine on both edges, and the tuned arm collapsed
# empty and encoded-zero into the same branch via picows' `NO_INFO = 0`. A shared predicate
# stops the arms inventing three tables.
#
# 1005/1006/1015 are reserved for local use and never transmitted; 0-999 are not status codes;
# 1014 is excluded deliberately to match the engine. The registered-application (3000-3999)
# and private (4000-4999) ranges stay open — 4000 is our stale-stop code.
_SENDABLE_CLOSE_CODES = frozenset(
    {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013}
)
_APP_CLOSE_MIN, _PRIVATE_CLOSE_MAX = 3000, 4999


def is_sendable_close_code(code: int) -> bool:
    """Whether `code` is legal to put in a CLOSE frame this client emits.

    Used by both arms so the §6 A/B cannot drift on which statuses leave the process.
    """
    return code in _SENDABLE_CLOSE_CODES or _APP_CLOSE_MIN <= code <= _PRIVATE_CLOSE_MAX


def classify_close_body(payload: bytes) -> tuple[int | None, bool]:
    """Classify a peer CLOSE payload into (code, legal_to_echo).

    Distinguishes the two wire shapes picows collapses into `NO_INFO = 0`:
      * empty body (len == 0) — legal, no code on the wire; caller answers 1000
      * encoded status 0      — illegal; not a WebSocket close code at all
      * any other status     — legal iff :func:`is_sendable_close_code` says so

    Returns `(None, True)` for the empty body so the caller cannot mistake it for zero.
    Returns `(code, False)` for every illegal encoded status, including 0 and 1014.
    """
    if len(payload) == 0:
        return None, True
    # A one-byte body is illegal (RFC 6455 §5.5.1); picows rejects it itself. Classified
    # here so a double that delivers one still lands on the illegal branch, not the empty one.
    if len(payload) == 1:
        return 0, False
    code = int.from_bytes(payload[:2], "big")
    return code, is_sendable_close_code(code)


class Ending(Enum):
    """Why a connection ended. Returned by `run_client`, which used to return `None`.

    Without this, "the operator stopped us", "the engine went away" and "we closed on our own
    verdict" are indistinguishable to the caller — and `run_with_reconnect`'s documented single
    retry was therefore DEAD CODE: both adapters return normally when the peer closes, so the
    retry loop saw success and exited 0 on an engine that had gone away.

    The distinction is a policy one, not a transport one: a peer that went away may be worth
    one more dial, while a close WE chose (a protocol verdict, a stale feed) must not be
    retried — retrying it would re-run the exact decision that just ended the session.
    """

    STOPPED = auto()  # the operator's stop event: terminal
    DECIDED = auto()  # we closed on our own verdict (1002/1009/4000): terminal
    PEER_GONE = auto()  # the peer closed, or the transport died: retryable


@dataclass(frozen=True)
class Codec:
    """One arm's serialization pair.

    One of FOUR things that differ between the arms — codec (here), transport, event loop and the
    engine's own codec flag. This said "the ONLY thing besides the socket library", which quietly
    dropped uvloop and the C++ leg and so overstated how attributable the A/B is: it is an
    aggregate stack comparison, and OPTIMIZATION.md decomposes it per layer from microbenchmarks.
    """

    decode: Callable[[bytes], EngineMsg]
    encode: Callable[[ClientMsg], bytes]
    name: str


@dataclass(frozen=True)
class CloseIntent:
    """Why the session is ending, and with which code. Returned rather than acted on so the
    adapter owns every socket call and this module owns none."""

    code: int
    reason: str


class SessionDriver:
    """Drives one connection's protocol state. Feed it bytes, take back frames to send.

    Holds no socket and no clock: `now_ns` is supplied by the adapter, exactly as the
    strategy requires it, so a session can be replayed frame-for-frame in a test with no
    timing at all.
    """

    def __init__(self, strategy: Strategy, codec: Codec) -> None:
        self._strategy = strategy
        self._codec = codec
        self._out_seq = 0
        # ADOPTED from the first inbound message, never assumed. The engine mints a fresh
        # epoch on every accept (`++next_epoch_`, from 1), so a client that hardcoded 1 was
        # correct exactly once: after a reconnect the engine is on 2, the strategy's epoch
        # guard drops every tick, and the client quotes nothing, says nothing, and never
        # exits. A silent hang is the worst failure this client can have, because the demo
        # and the benchmark both look like a quiet market while it happens.
        self._epoch: int | None = None
        self._close: CloseIntent | None = None

    @property
    def close_intent(self) -> CloseIntent | None:
        """Set once the strategy has given up; the adapter closes with this code."""
        return self._close

    def on_bytes(self, raw: bytes, now_ns: int) -> list[bytes]:
        """One inbound frame in, zero or more outbound frames out.

        A decode failure is the engine breaking its own protocol, which is a 1002 on our
        initiative — not an exception for the adapter to interpret. Every arm of this method
        returns frames; none of them raises.
        """
        # Already closing: say nothing further, and in particular do not let a frame that was
        # already in the socket buffer REPLACE the verdict we reached. A stale stop latches
        # 4000, then buffered garbage arriving behind it used to overwrite the intent with
        # 1002 — the peer would be told "you broke the protocol" for a session that ended
        # because our feed went quiet.
        if self._close is not None:
            return []
        try:
            msg = self._codec.decode(raw)
        except ValueError as exc:
            self._latch(CLOSE_PROTOCOL, f"undecodable frame: {exc}")
            return []
        if self._epoch is None:
            # The engine's epoch, learned from the first message it sends. on_connect must
            # run BEFORE the message is handled, or the strategy judges it against epoch 0.
            self._epoch = msg.epoch
            self._strategy.on_connect(msg.epoch)
        return self._dispatch(self._strategy.on_report(msg, now_ns))

    def note_protocol_error(self, reason: str, code: int = CLOSE_PROTOCOL) -> None:
        """A FRAMING verdict the adapter reached and the driver cannot see — a binary frame, a
        reserved bit, an orphan continuation, a reassembly past the cap.

        `code` because not every adapter-side verdict is 1002: a reassembled message past the
        cap is 1009, the same code both libraries produce for a single oversized frame.
        """
        self._latch(code, reason)

    def on_timer(self, now_ns: int) -> list[bytes]:
        """The stale-feed tick. Its cancels still go out — the strategy emits them precisely
        so a stopping client does not abandon resting orders — and the close follows."""
        return self._dispatch(self._strategy.on_timer(now_ns))

    def on_disconnect(self) -> None:
        self._strategy.on_disconnect()

    def _dispatch(self, cmds: list[Cmd]) -> list[bytes]:
        frames: list[bytes] = []
        for cmd in cmds:
            if isinstance(cmd, SendCmd):
                # Encode can refuse a stamped field that left the wire domain — most relevantly
                # `seq` past uint64, which C++ wraps unchecked while Python ints do not. Catching
                # HERE, in the shared driver, is what keeps the two arms agreeing: an uncaught
                # raise killed the tuned arm as a callback fault and the naive arm as a pump
                # exception, two different endings for the same exhaustion. No hot-path counter
                # check: the encoder already range-checks every field, and that is the deliberate
                # gate (docs/PROTOCOL.md §8.5b / F-E).
                try:
                    frames.append(self._codec.encode(self._stamp(cmd)))
                except ValueError as exc:
                    self._latch(CLOSE_PROTOCOL, f"outbound field outside wire domain: {exc}")
                    return frames
            else:
                self._note_stop(cmd)
        return frames

    def _stamp(self, cmd: SendCmd) -> ClientMsg:
        """The outbound envelope counter lives HERE, not in the strategy: it is a property of
        the connection, and the strategy is deliberately ignorant of connections. Stamped at
        emit so the sequence is contiguous in the order the frames actually leave.

        Unchecked increment, deliberately: a wrap-check on every stamp would sit on the
        measured path for a case that needs ~1.8e19 messages to reach. The encoder refuses a
        value past uint64, and `_dispatch` turns that refusal into a latched close — so the
        behaviour at the boundary is defined without a per-message branch here.
        """
        self._out_seq += 1
        msg = cmd.msg
        msg.seq = self._out_seq
        # `_epoch` is set before any command can exist: a command comes only from a report, and
        # the first report adopts the epoch above. The MESSAGE is not decoration — the §5.2
        # harness drives a probe whose timer fires before the first book arrives, and this
        # assertion firing bare (an `AssertionError` with no text) cost real time to locate.
        assert self._epoch is not None, (
            "no epoch adopted yet: a command was emitted before the first inbound message, so "
            "there is no session epoch to stamp it with"
        )
        msg.epoch = self._epoch
        return msg

    def _note_stop(self, cmd: StopQuoting) -> None:
        stale = cmd.reason.startswith("stale feed")
        self._latch(CLOSE_STALE if stale else CLOSE_PROTOCOL, cmd.reason)

    def _latch(self, code: int, reason: str) -> None:
        """FIRST intent wins, mirroring the engine's own close-latch discipline.

        THE ONLY writer of `_close`. It was previously enforced at each of three call sites and
        one of them — the decode-failure path — simply assigned, so a late malformed frame could
        rewrite a stale stop's 4000 into 1002. A rule spelled out three times is a rule with two
        chances to be got wrong; spelled once, the invariant is structural.
        """
        if self._close is None:
            self._close = CloseIntent(code, reason)
