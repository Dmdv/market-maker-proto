"""Wire protocol v1 — the Python half of the C++/Python contract.

The msgspec Structs below are the single Python source of truth for the wire (decision
record D4). Member DECLARATION ORDER IS WIRE KEY ORDER: msgspec emits the ``t`` tag first
and then members in declaration order, which is exactly what both C++ codec arms emit
(``cpp/include/mm/protocol.hpp``). The shared fixtures in ``tests/golden/`` pin that
equality byte-for-byte from both sides, so reordering a member here is a wire-format
change and the golden tests will say so.

The frame byte policy and the RFC 8259 grammar scan live next door in ``_preflight.py``,
along the same seam the C++ side uses (``frame_preflight.cpp`` vs ``protocol.hpp``):
everything there decides a frame's fate without knowing which message it is.

Two codec arms, one acceptance contract
---------------------------------------
``decode``/``encode`` are the PRODUCT arm (msgspec); ``decode_naive``/``encode_naive`` are
the BASELINE arm (stdlib ``json``). The naive -> product swap is the measured §6
optimization, so the two arms must agree on what they accept and on the value they
produce — an "optimization" that also changes client behaviour is not a like-for-like
comparison. Both arms therefore run the SAME preflight (:func:`_prepare`) before any
library parser touches the frame, which is the structural lesson the C++ side learned in
Task 2 (``cpp/include/mm/codec.hpp``: any policy that lives in a library's quirks WILL
drift between the arms). Left to themselves the two libraries disagree with the wire
contract in opposite directions: ``json.loads`` accepts a BOM, resolves duplicate keys
last-wins, admits lone surrogates and the non-JSON ``NaN``/``Infinity`` extensions, while
msgspec accepts escaped member names, duplicate keys, unbounded nesting and integers
outside the engine's ``uint64_t``/``int64_t`` members.

A/B honesty note, mirroring the one in ``codec.hpp``: the preflight cost is COMMON-MODE —
both arms pay it — so the measured naive -> tuned delta is stdlib parse + dict extraction
vs msgspec's typed read, NOT whole-library parse vs parse, and Tasks 13/14 must disclose
that for the Python codec row as for the C++ arms, INCLUDING its absolute share of the docx
§5.1 M1/M3 windows (queued as PENDING_AMENDMENTS (p)12 — the preflight sits inside both).
Every figure here comes from ONE run of ``bench/probes/py_codec.py``, which prints its own
provenance line (basis, interpreter, versions, platform, repo SHA): min of 7 x 20k
iterations, decode on ``tob.json``, encode on ``new_order.json``, Python 3.14.6 / msgspec
0.21.1, arm-64 macOS. Run it rather than trust this comment — reruns move decode ~0.5 µs
(encode ~0.1): a shape, not constants. Preflight 6.1 µs, of which ``_scan``'s flat-frame
fast path is 4.7 and the byte policy 0.1; ``decode`` 6.6 µs, ``decode_naive`` 9.0; on the
encode side dispatch plus the three shared passes are 1.0 µs of ``encode``'s 1.2 and
``encode_naive``'s 3.4. The bare library calls underneath are 0.2 and 1.5 µs — that gap is
the price of an acceptance contract Python's libraries lack, and the honest §6 denominator.

Acceptance contract — normative source ``cpp/include/mm/codec.hpp``, enforced in this
order so that error PRECEDENCE matches the engine's (parse -> unknown type -> unsupported
version -> field errors):

1-3. the frame's SHAPE, all of it in ``_preflight.py`` and stated in full by that module's
   docstring: the byte policy (exact ``bytes``, ≤64 KiB, no BOM, strict UTF-8), the RFC 8259
   grammar (number tokens judged LEXICALLY and never converted, escaped surrogates paired,
   raw control bytes and trailing garbage refused, ≤32 nesting levels) and the ROOT key
   policy (no escaped member names, no duplicates, ≤32 keys — root-scoped as the engine
   scopes it). Every one is a rule ``frame_preflight.cpp`` applies too: this side carries no
   bound of its own. All three run FIRST, so a parse error outranks every verdict below;
4. ``t`` present and a string, naming a known engine -> client tag;
5. ``v`` present and an unsigned-integer TOKEN inside ``uint64_t``, equal to 1 (a
   21-digit ``v`` is Malformed and not UnsupportedVersion, exactly as both C++ arms
   classify it);
6. every integer member of the selected schema whose key is present carries an integer
   TOKEN of the right signedness and a magnitude inside the C++ integer domain of that
   member (``protocol.hpp``) — both decided on the raw span, before any library
   conversion, so neither the sign nor the range verdict can drift into a library quirk;
7. every field of the selected schema present and correctly typed;
8. every STRING member of the selected schema within the engine's own identifier shape
   policy (:func:`_check_engine_strings`, the inbound twin of
   ``detail::check_inbound_strings``): no byte < 0x20 or 0x7F anywhere — the ESCAPED
   spellings (``\\u0000``, ``\\n``) that step 2's raw-control-byte rule cannot see, and
   the CWE-158/117 lever, since these strings key the client's order state and land in
   log text — and ``cl_id``/``symbol`` inside the same 64/32 BYTE caps the engine bounds
   them by inbound. ``status``/``code``/``reason`` are engine free text that the engine
   bounds on NEITHER side, so they are left at the frame bound rather than capped here:
   a cap the wire does not have would make this client stricter than its peer.

Unknown keys are ignored for additive forward compatibility, and no conversion of theirs
may decide a frame's fate: the preflight grammar-checks their values LEXICALLY, and the
one conversion the naive arm cannot avoid — CPython's 4300-digit ``int`` limit, which
fires inside ``json.loads`` — is neutralized by a ``parse_int`` guard on any frame long
enough to carry such a token. A 5000-digit unknown integer is therefore accepted by both
arms, while a 5000-digit ``seq`` is rejected by both in the same words: its digit count
alone puts it outside ``uint64_t``, which step 6 settles on the token without ever asking
CPython to build the integer.

The two verdicts a raw span cannot give — a member's PRESENCE and STRING typing — are the
schema pass (:func:`_read_fields`), and both arms answer them in the SAME words: the naive
arm runs the pass directly, and the product arm restates msgspec's ``ValidationError``
through it (:func:`_schema_error`). That mirrors the C++ invariant "both arms return the
same ``DecodeError.detail`` for the same rejection reason" (``codec.hpp``) — a log line
must not depend on which codec the benchmark flag picked — and it costs a second parse on
the REJECT path only, leaving the measured §6 happy path untouched.

ENCODE carries the same contract's producer-side rules, in ``_rules.py``: both encoders
call ``_check_client_message`` on the rule bundle built here, and it runs the three passes
in contract order. A client must not PRODUCE a frame the engine answers with ``Malformed``,
and a RANGE test alone does not deliver that (``low <= value <= high`` is true of ``True``
and of ``1.0``), so every member is TYPED before it is measured or shaped — through the
same checkers the decode side uses, and total by construction.

Both arms raise ``ValueError`` on rejection (msgspec's ``DecodeError``/``ValidationError``
derive from it), so one ``except ValueError`` covers the boundary whichever arm is wired; a
non-``bytes`` frame is a caller bug and raises ``TypeError``.
"""

import json
import sys
from typing import Any, Final, Literal, get_args

import msgspec

from mmclient._preflight import (
    _MAX_TAG_DETAIL,
    Frame,
    _check_frame_bytes,
    _decode_string_span,
    _sanitized_tag,
    _scan,
)
from mmclient._rules import (
    _CONTROL_CHARS,
    _MINUS_BYTE,
    _UINT64_DIGITS,
    _UINT64_MAX,
    Int64,
    Uint64,
    _check_client_message,
    _ClientRules,
    _engine_string_rules,
    _field_specs,
    _FieldCheck,
    _identifier_rules,
    _int_domains,
    _is_unsigned_integer_token,
    _span_rules,
    _SpanRule,
    _tag_of,
    _wire_type_rules,
)

__all__ = [
    "CancelAck",
    "CancelOrder",
    "ClientMsg",
    "EngineMsg",
    "Fill",
    "Frame",
    "NewOrder",
    "OrderAck",
    "Reject",
    "Tob",
    "decode",
    "decode_naive",
    "encode",
    "encode_naive",
]


# --- Structs: declaration order is wire key order -------------------------------------


class Tob(msgspec.Struct, tag="top_of_book", tag_field="t"):
    v: Literal[1]
    seq: Uint64
    epoch: Uint64
    md_seq: Uint64
    symbol: str
    bid_px: Int64
    bid_qty: Int64
    ask_px: Int64
    ask_qty: Int64


class NewOrder(msgspec.Struct, tag="new_order", tag_field="t"):
    v: Literal[1]
    seq: Uint64
    epoch: Uint64
    md_seq: Uint64
    cl_id: str
    symbol: str
    side: str  # "B" / "S"
    px: Int64
    qty: Int64
    # Construction-side convenience ONLY. The engine's inbound field set is CLOSED, and
    # msgspec emits the field on every encode, so the wire always carries it.
    post_only: bool = True


class CancelOrder(msgspec.Struct, tag="cancel_order", tag_field="t"):
    v: Literal[1]
    seq: Uint64
    epoch: Uint64
    cl_id: str


class OrderAck(msgspec.Struct, tag="order_ack", tag_field="t"):
    v: Literal[1]
    seq: Uint64
    epoch: Uint64
    cl_id: str
    eng_id: Uint64
    # A CLOSED LITERAL, not a free string. `status` was `str`, so a frame claiming
    # `t="order_ack"` with `status="cancelled"` decoded cleanly — and the strategy dispatches on
    # the message TYPE, so it marked the order LIVE on the strength of a field that said the
    # opposite. The engine never emits that combination, which is exactly why it went unnoticed:
    # the defect is in what the client ACCEPTS from a peer, not in what this engine sends.
    status: Literal["live"]
    svc_ns: Int64


class CancelAck(msgspec.Struct, tag="cancel_ack", tag_field="t"):
    v: Literal[1]
    seq: Uint64
    epoch: Uint64
    cl_id: str
    eng_id: Uint64
    status: Literal["cancelled"]  # closed, for the same reason as OrderAck.status


class Reject(msgspec.Struct, tag="reject", tag_field="t"):
    v: Literal[1]
    seq: Uint64
    epoch: Uint64
    cl_id: str  # may be ""
    code: str
    reason: str


class Fill(msgspec.Struct, tag="fill", tag_field="t"):
    v: Literal[1]
    seq: Uint64
    epoch: Uint64
    cl_id: str
    eng_id: Uint64
    px: Int64
    qty: Int64
    leaves: Int64
    exec_id: Uint64


EngineMsg = Tob | OrderAck | CancelAck | Reject | Fill
"""Engine -> client. The only tags the client accepts."""

ClientMsg = NewOrder | CancelOrder
"""Client -> engine. The only messages the client produces."""


def _prepare(data: Frame) -> str:
    """Everything both arms share: byte policy, grammar, key policy, tag, version, and
    the integer members' token and domain verdicts.

    Returns the engine tag. Error precedence is the engine's (codec.hpp): parse ->
    unknown type -> unsupported version -> field errors — which is why the tag and the
    version are read from the SCAN's spans rather than from whatever a library happened
    to consume first, and why the integer pass runs LAST of the four. The field errors
    left to the caller are the ones a span cannot answer — presence, string typing and
    identifier shape — and both arms answer those through :func:`_read_fields` and
    :func:`_check_engine_strings`, in one wording.
    """
    _check_frame_bytes(data)
    spans = _scan(data)

    tag_span = spans.get(b"t")
    if tag_span is None or not tag_span.startswith(b'"'):
        raise ValueError("malformed: t missing or not a string")
    tag = _decode_string_span(tag_span)
    if tag not in _ENGINE_SCHEMAS:
        raise ValueError(f"unknown type: {_sanitized_tag(tag)}")

    version = spans.get(b"v")
    # The DOMAIN verdict belongs here too, not only the token one: `v` is excluded from
    # the span pass on the ground that this check owns it, so this check must own ALL of
    # it. Both C++ arms answer a 21-digit `v` with "v missing or not an unsigned integer"
    # — Malformed, not UnsupportedVersion (glaze's `from_chars` errc, nlohmann's
    # `is_number_unsigned`) — and digit count settles it without a conversion, exactly as
    # :func:`_check_int_spans` settles it for every other unsigned member.
    if (
        version is None
        or not _is_unsigned_integer_token(version)
        or len(version) > _UINT64_DIGITS
        or (len(version) == _UINT64_DIGITS and int(version) > _UINT64_MAX)
    ):
        raise ValueError("malformed: v missing or not an unsigned integer")
    if version != b"1":
        # Sliced: the token is grammar-valid but peer-sized, and this text reaches logs.
        raise ValueError(f"unsupported version: v={version[:_MAX_TAG_DETAIL].decode()}")
    _check_int_spans(tag, spans)
    return tag


def _check_int_spans(tag: str, spans: dict[bytes, bytes]) -> None:
    """Integer members: the TOKEN and the DOMAIN verdict, decided on the raw span.

    Both verdicts are settled here, in front of both libraries, for the same reason
    ``frame_preflight.cpp`` settles them in front of nlohmann and glaze: a policy left to
    a library is a policy that differs between the arms. Concretely, neither library
    implements the ``uint64_t`` SIGN rule the way the wire states it — msgspec reads
    ``-0`` into an ``int`` and stdlib ``json`` likewise, so both arms would accept a
    frame `cpp/tests/test_codec_equivalence.cpp` pins as Malformed — and msgspec's range
    verdict arrives as its own ``ValidationError`` text while the naive arm's arrives as
    ours.

    Digit COUNT decides the range for any token too long to be in domain, so a 5000-digit
    ``seq`` never reaches ``int()`` and CPython's conversion digit limit never gets a vote
    (that limit fires at 4300 digits — a parser detail, not a wire rule).

    A key that is absent is not this pass's business: the missing-field verdict belongs to
    the schema pass, which runs after and in declaration order.
    """
    # Unpacked positionally although `_SpanRule` is a NamedTuple: this is the module's
    # densest loop, and tuple unpacking beats seven attribute reads. Construction is
    # keyword-only and import-time (:func:`_span_rules`), which is where the field names
    # earn their keep.
    for key, name, is_token, kind, low, high, digits in _SPAN_RULES[tag]:
        span = spans.get(key)
        if span is None:
            continue
        if not is_token(span):
            raise ValueError(f"malformed: {tag}.{name} is not {kind}")
        size = len(span) - (span[0] == _MINUS_BYTE)  # a sign is not a digit
        if size < digits:
            continue  # fewer digits than the widest in-domain magnitude: in domain
        if size > digits or not low <= int(span) <= high:
            raise ValueError(f"malformed: {tag}.{name} outside its wire integer domain")


def _check_engine_strings(tag: str, msg: EngineMsg) -> None:
    """The DECODE-side twin of ``detail::check_inbound_strings``, and the LAST step of the
    acceptance contract — the engine applies this policy to every inbound identifier, and
    a client that skipped it would accept from its peer what its peer refuses from it.

    Two rules, in the engine's own order (cap, then control bytes) and its own wordings:
    the byte caps for the members the engine bounds (``cl_id`` 64, ``symbol`` 32), and no
    byte < 0x20 or 0x7F in ANY string member. The second is the one the grammar cannot
    give: raw control bytes are refused by the string production, but ``"\\u0000"`` and
    ``"\\n"`` are legal JSON spellings of the same bytes, and a decoded ``cl_id`` keys the
    client's order state (Tasks 8/9) while ``reason`` is written straight to logs — NUL
    truncation and CR/LF record forgery, CWE-158/117.

    ``code``/``reason``/``status`` are deliberately uncapped: the engine bounds them on
    neither side, and a client-side cap would make this client stricter than the wire.
    """
    for name, cap in _ENGINE_STRINGS[tag]:
        value: str = getattr(msg, name)
        if cap is not None:
            # `isascii()` is a flag read, so the common all-ASCII identifier is measured
            # without an encode; only a non-ASCII one pays for its UTF-8 byte length.
            size = len(value) if value.isascii() else len(value.encode())
            if size > cap:
                raise ValueError(f"malformed: {tag}.{name} exceeds the length cap")
        if _CONTROL_CHARS.search(value):
            raise ValueError(f"malformed: control byte in {tag}.{name}")


# DERIVED from the unions rather than restated beside them: a variant added to either joins
# every table below by construction, and one carrying a member no rule covers stops the
# IMPORT (`_rules`' totality guards) instead of reaching a runtime lookup that is not there.
_ENGINE_CLASSES: Final = get_args(EngineMsg)
_CLIENT_CLASSES: Final = get_args(ClientMsg)

_ENGINE_SCHEMAS: Final[dict[str, tuple[Any, tuple[tuple[str, _FieldCheck], ...]]]] = {
    _tag_of(cls): (cls, _field_specs(cls)) for cls in _ENGINE_CLASSES
}

_SPAN_RULES: Final[dict[str, tuple[_SpanRule, ...]]] = {
    _tag_of(cls): _span_rules(cls) for cls in _ENGINE_CLASSES
}

_ENGINE_STRINGS: Final[dict[str, tuple[tuple[str, int | None], ...]]] = {
    _tag_of(cls): _engine_string_rules(cls) for cls in _ENGINE_CLASSES
}

# Client messages only: the encode-side passes are reached from the two encoders and from
# nowhere else, so an engine-schema entry here would be a lookup no runtime path performs —
# and one line coverage cannot show as dead, since the comprehension runs at import. The
# engine schemas' decode-side domain rule lives in `_SPAN_RULES`, where it answers on the
# raw token before any conversion. ONE bundle per class rather than three parallel dicts:
# the three passes are one contract with a load-bearing order (`_check_client_message`,
# which takes this registry whole, so the dispatch has one home too).
_CLIENT_RULES: Final[dict[Any, _ClientRules]] = {
    cls: (_tag_of(cls), _wire_type_rules(cls), _int_domains(cls), _identifier_rules(cls))
    for cls in _CLIENT_CLASSES
}

# Tag + member order per client message, resolved ONCE: msgspec.structs.fields() rebuilds
# its FieldInfo tuple on every call (~14 µs here), which would land on the measured encode
# path. Read only AFTER `_CLIENT_RULES`, so an unknown type is already a TypeError by here.
_WIRE_FIELDS: Final[dict[Any, tuple[str, tuple[str, ...]]]] = {
    cls: (_tag_of(cls), tuple(f.name for f in msgspec.structs.fields(cls)))
    for cls in _CLIENT_CLASSES
}

# --- schema pass: the two field verdicts a raw span cannot give -----------------------
#
# Presence and string typing are all the preflight leaves undecided, and both arms answer
# them HERE so the rejection text cannot depend on which arm is wired (codec.hpp's
# detail-string equality invariant). The naive arm reaches this pass on its way to the
# Struct; the product arm reaches it only to restate a msgspec ValidationError.

_OVERSIZED_INT: Final = object()


def _int_or_oversized(token: str) -> Any:
    """``parse_int`` for a frame long enough to carry an integer past CPython's
    conversion limit. Only UNKNOWN fields can still reach it — a known member's
    oversized token is already out of domain by digit count — and an unknown value is
    discarded, so the frame's fate is decided by the contract and never by how many
    digits CPython is willing to convert.
    """
    try:
        return int(token)
    except ValueError:
        return _OVERSIZED_INT


def _payload_of(data: Frame) -> Any:
    """``json.loads`` behind the shared preflight, which has already settled the frame's
    grammar, byte policy and key policy — so this parse needs none of its hooks except the
    digit-limit guard, and that only for a frame long enough to trip it."""
    if len(data) <= sys.get_int_max_str_digits():
        return json.loads(data)
    return json.loads(data, parse_int=_int_or_oversized)


def _read_fields(tag: str, payload: dict[str, Any]) -> dict[str, Any]:
    """Presence + typing for the selected schema, in DECLARATION order."""
    _, specs = _ENGINE_SCHEMAS[tag]
    fields: dict[str, Any] = {}
    for name, check in specs:
        if name not in payload:
            raise ValueError(f"malformed: {tag}: missing field {name!r}")
        fields[name] = check(tag, name, payload[name])
    return fields


def _schema_error(tag: str, data: Frame) -> ValueError:
    """Restate msgspec's schema rejection in THIS module's words, via the same pass the
    naive arm runs — so a missing member and a mistyped string read identically whichever
    arm produced them."""
    try:
        _read_fields(tag, _payload_of(data))
    except ValueError as exc:
        return exc
    # Not reachable while the two passes agree: after the preflight, presence and string
    # typing are the only verdicts msgspec has left to give, and `_read_fields` gives both.
    # Kept total rather than re-raising msgspec's text, which is what this exists to avoid.
    return ValueError(f"malformed: {tag}: does not match its schema")  # pragma: no cover


# --- product arm (msgspec) ------------------------------------------------------------

_dec: Final = msgspec.json.Decoder(EngineMsg)
_enc: Final = msgspec.json.Encoder()


def decode(data: Frame) -> EngineMsg:
    """Decode one engine frame. Raises ``ValueError`` on any contract rejection."""
    tag = _prepare(data)
    try:
        msg: EngineMsg = _dec.decode(data)
    # `DecodeError`, not its strict subclass `ValidationError`, as defence in depth: what
    # makes the non-validation branch unreachable is that the hand-written preflight is a
    # SUPERSET of msgspec's parse checks — precisely the coupling this module says WILL
    # drift, since a msgspec version bump is all it takes. Narrowed, a plain parse-class
    # DecodeError would escape carrying msgspec's peer-influenced wording and silently
    # break the one-wording invariant the TRY003 suppression rests on. Costs nothing on
    # the happy path.
    except msgspec.DecodeError:
        raise _schema_error(tag, data) from None
    _check_engine_strings(tag, msg)
    return msg


def encode(msg: ClientMsg) -> bytes:
    """Encode one client command in canonical wire form."""
    _check_client_message(msg, _CLIENT_RULES)
    return _enc.encode(msg)


# --- baseline arm (stdlib json) -------------------------------------------------------


def decode_naive(data: Frame) -> EngineMsg:
    """Decode one engine frame with stdlib ``json`` behind the shared preflight.

    Same acceptance verdicts, the same rejection TEXT and the same returned Struct as
    :func:`decode`; only the field extraction differs, which is exactly what §6 measures.
    """
    tag = _prepare(data)
    cls, _ = _ENGINE_SCHEMAS[tag]
    msg: EngineMsg = cls(v=1, **_read_fields(tag, _payload_of(data)))
    _check_engine_strings(tag, msg)
    return msg


def encode_naive(msg: ClientMsg) -> bytes:
    """Encode one client command with stdlib ``json``, byte-identical to :func:`encode`.

    ``ensure_ascii=False`` is what makes it byte-identical: both the C++ encoders and
    msgspec pass valid UTF-8 through raw and escape only control characters.
    """
    _check_client_message(msg, _CLIENT_RULES)
    tag, names = _WIRE_FIELDS[type(msg)]
    payload: dict[str, Any] = {"t": tag}
    for name in names:
        payload[name] = getattr(msg, name)
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode()
