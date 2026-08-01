"""The transport-independent half of a client session.

Both adapters own only bytes on and off a socket; envelope sequencing, epoch tracking and the
strategy driving live here, so the two arms cannot diverge and the A/B stays like-for-like.
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

# The transport cap, here rather than in each adapter: ONE number must agree in three places —
# both adapters' frame limits and the codec's restatement in `_preflight._MAX_FRAME_BYTES`.
MAX_MSG_BYTES = 64 * 1024

# Close codes this client may send. 1002 is our verdict on the ENGINE's framing (an envelope-seq
# gap, an md_seq decrease); 4000 distinguishes a stale-feed stop from every transport cause.
CLOSE_NORMAL = 1000
CLOSE_PROTOCOL = 1002
# The SIZE verdict, distinct from the framing one: a message past the cap is well-formed and too
# large. Both libraries yield 1009 for a single oversized frame, and a REASSEMBLED breach is one.
CLOSE_TOO_BIG = 1009
# The PAYLOAD verdict: a TEXT frame whose bytes are not valid UTF-8 is 1007 (RFC 6455 §8.1), which
# is what Beast answers with. Without it the tuned arm failed in the JSON decoder and latched 1002.
CLOSE_BAD_PAYLOAD = 1007
CLOSE_STALE = 4000

# Codes this client may put on the wire, initiate or echo — one table, both arms, Beast as the
# reference: 1005/1006/1015 local-only, 0-999 not status codes, 1014 reserved; 3000-4999 open.
_SENDABLE_CLOSE_CODES = frozenset(
    {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013}
)
_APP_CLOSE_MIN, _PRIVATE_CLOSE_MAX = 3000, 4999


def is_sendable_close_code(code: int) -> bool:
    """Whether `code` is legal in a CLOSE frame this client emits. Shared by both arms."""
    return code in _SENDABLE_CLOSE_CODES or _APP_CLOSE_MIN <= code <= _PRIVATE_CLOSE_MAX


def classify_close_body(payload: bytes) -> tuple[int | None, bool]:
    """Classify a peer CLOSE payload into (code, legal_to_echo).

    Distinguishes the two shapes picows collapses into `NO_INFO = 0`: an EMPTY body returns
    `(None, True)` — legal, no code on the wire — while an encoded 0 (and 1014) is illegal.
    """
    if len(payload) == 0:
        return None, True
    # A one-byte body is illegal (RFC 6455 §5.5.1); classified here so a double that delivers
    # one still lands on the illegal branch rather than the empty one.
    if len(payload) == 1:
        return 0, False
    code = int.from_bytes(payload[:2], "big")
    return code, is_sendable_close_code(code)


class Ending(Enum):
    """Why a connection ended.

    The distinction is a policy one: a peer that went away may be worth one more dial, while a
    close WE chose (a protocol verdict, a stale feed) must not be retried.
    """

    STOPPED = auto()  # the operator's stop event: terminal
    DECIDED = auto()  # we closed on our own verdict (1002/1009/4000): terminal
    PEER_GONE = auto()  # the peer closed, or the transport died: retryable


@dataclass(frozen=True)
class Codec:
    """One arm's serialization pair.

    One of FOUR things that differ between the arms — codec, transport, event loop and the
    engine's own codec flag — so the A/B is an AGGREGATE stack comparison, not a codec one.
    """

    decode: Callable[[bytes], EngineMsg]
    encode: Callable[[ClientMsg], bytes]
    name: str


@dataclass(frozen=True)
class CloseIntent:
    """Why the session is ending, and with which code; returned so the adapter owns the socket."""

    code: int
    reason: str


class SessionDriver:
    """Drives one connection's protocol state. Feed it bytes, take back frames to send.

    Holds no socket and no clock — `now_ns` comes from the adapter — so a session can be
    replayed frame-for-frame in a test with no timing at all.
    """

    def __init__(self, strategy: Strategy, codec: Codec) -> None:
        self._strategy = strategy
        self._codec = codec
        self._out_seq = 0
        # ADOPTED from the first inbound message, never assumed: the engine mints a fresh epoch
        # on every accept, so a hardcoded 1 makes the client quote nothing after a reconnect.
        self._epoch: int | None = None
        self._close: CloseIntent | None = None

    @property
    def close_intent(self) -> CloseIntent | None:
        """Set once the strategy has given up; the adapter closes with this code."""
        return self._close

    def on_bytes(self, raw: bytes, now_ns: int) -> list[bytes]:
        """One inbound frame in, zero or more outbound frames out.

        A decode failure is the engine breaking its own protocol — a 1002 on our initiative, not
        an exception for the adapter to interpret. Every arm returns frames; none raises.
        """
        # Already closing: say nothing further, and in particular do not let a buffered frame
        # REPLACE the verdict already reached — a stale stop's 4000 must not become 1002.
        if self._close is not None:
            return []
        try:
            msg = self._codec.decode(raw)
        except ValueError as exc:
            self._latch(CLOSE_PROTOCOL, f"undecodable frame: {exc}")
            return []
        if self._epoch is None:
            # The engine's epoch, learned from its first message. on_connect must run BEFORE
            # the message is handled, or the strategy judges it against epoch 0.
            self._epoch = msg.epoch
            self._strategy.on_connect(msg.epoch)
        return self._dispatch(self._strategy.on_report(msg, now_ns))

    def note_protocol_error(self, reason: str, code: int = CLOSE_PROTOCOL) -> None:
        """A FRAMING verdict the adapter reached and the driver cannot see — a binary frame, a
        reserved bit, an orphan continuation, a reassembly past the cap.

        `code` because not every adapter-side verdict is 1002: a reassembled breach is 1009.
        """
        self._latch(code, reason)

    def on_timer(self, now_ns: int) -> list[bytes]:
        """The stale-feed tick. Its cancels still go out — a stopping client must not abandon
        resting orders — and the close follows."""
        return self._dispatch(self._strategy.on_timer(now_ns))

    def on_disconnect(self) -> None:
        self._strategy.on_disconnect()

    def _dispatch(self, cmds: list[Cmd]) -> list[bytes]:
        frames: list[bytes] = []
        for cmd in cmds:
            if isinstance(cmd, SendCmd):
                # Encode can refuse a stamped field that left the wire domain — most relevantly
                # `seq` past uint64. Caught HERE so both arms end the same way on exhaustion.
                try:
                    frames.append(self._codec.encode(self._stamp(cmd)))
                except ValueError as exc:
                    self._latch(CLOSE_PROTOCOL, f"outbound field outside wire domain: {exc}")
                    return frames
            else:
                self._note_stop(cmd)
        return frames

    def _stamp(self, cmd: SendCmd) -> ClientMsg:
        """The outbound envelope counter lives HERE, not in the strategy: it is a property of the
        connection. Stamped at emit, so the sequence is contiguous in the order frames leave.

        Unchecked increment, deliberately: the encoder refuses a value past uint64 and
        `_dispatch` turns that refusal into a latched close, so no per-message branch is needed.
        """
        self._out_seq += 1
        msg = cmd.msg
        msg.seq = self._out_seq
        # `_epoch` is set before any command can exist: a command comes only from a report, and
        # the first report adopts the epoch above. The MESSAGE is not decoration.
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

        THE ONLY writer of `_close`: spelled once, the invariant is structural rather than a
        rule three call sites each get a chance to break.
        """
        if self._close is None:
            self._close = CloseIntent(code, reason)
