"""Wire protocol v1 — the Python half of the C++/Python contract.

Member DECLARATION ORDER IS WIRE KEY ORDER, pinned by ``tests/golden/``. Both codec arms run the
SAME preflight, so they share an accept-set, a rejection wording and the engine's error precedence.
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
    # Construction-side convenience ONLY: the engine's inbound field set is CLOSED, and msgspec
    # emits the field on every encode, so the wire always carries it.
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
    # A CLOSED LITERAL, not a free string: the strategy dispatches on message TYPE, so a free
    # `status` would let an `order_ack` claiming "cancelled" still mark the order LIVE.
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
    """Everything both arms share: byte policy, grammar, key policy, tag, version, and the
    integer members' token and domain verdicts. Returns the engine tag.

    Error precedence is the engine's (codec.hpp), which is why tag and version are read from the
    SCAN's spans rather than from whatever a library consumed first, and why integers run LAST.
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
    # The DOMAIN verdict belongs here too: `v` is excluded from the span pass because this check
    # owns it. A 21-digit `v` is Malformed, not UnsupportedVersion, and digit count settles it.
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

    Settled in front of both libraries, because neither implements the ``uint64_t`` SIGN rule as
    the wire states it. Digit COUNT decides range, so a 5000-digit ``seq`` never reaches ``int()``.
    """
    # Unpacked positionally although `_SpanRule` is a NamedTuple: this is the module's densest
    # loop, and tuple unpacking beats seven attribute reads.
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
    """The DECODE-side twin of ``detail::check_inbound_strings``, the contract's LAST step.

    In the engine's order: its byte caps (``cl_id`` 64, ``symbol`` 32 — free text stays uncapped,
    as on the wire), then no byte < 0x20 or 0x7F in ANY string member (CWE-158/117). The second
    is the one the grammar cannot give, since escaped spellings are legal JSON.
    """
    for name, cap in _ENGINE_STRINGS[tag]:
        value: str = getattr(msg, name)
        if cap is not None:
            # `isascii()` is a flag read: only a non-ASCII identifier pays for a UTF-8 encode.
            size = len(value) if value.isascii() else len(value.encode())
            if size > cap:
                raise ValueError(f"malformed: {tag}.{name} exceeds the length cap")
        if _CONTROL_CHARS.search(value):
            raise ValueError(f"malformed: control byte in {tag}.{name}")


# DERIVED from the unions rather than restated beside them: a variant added to either joins every
# table below, and one carrying a member no rule covers stops the IMPORT.
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

# Client messages only: the encode-side passes are reached from the two encoders and nowhere
# else. ONE bundle per class, because the three passes are one contract with a load-bearing order.
_CLIENT_RULES: Final[dict[Any, _ClientRules]] = {
    cls: (_tag_of(cls), _wire_type_rules(cls), _int_domains(cls), _identifier_rules(cls))
    for cls in _CLIENT_CLASSES
}

# Tag + member order per client message, resolved ONCE: msgspec.structs.fields() rebuilds its
# FieldInfo tuple on every call (~14 µs), which would land on the measured encode path.
_WIRE_FIELDS: Final[dict[Any, tuple[str, tuple[str, ...]]]] = {
    cls: (_tag_of(cls), tuple(f.name for f in msgspec.structs.fields(cls)))
    for cls in _CLIENT_CLASSES
}

# --- schema pass: the two field verdicts a raw span cannot give -----------------------
#
# Presence and string typing are all the preflight leaves undecided, and both arms answer them
# HERE so the rejection text cannot depend on which arm is wired.

_OVERSIZED_INT: Final = object()


def _int_or_oversized(token: str) -> Any:
    """``parse_int`` for a frame long enough to carry an integer past CPython's conversion limit.

    Only UNKNOWN fields reach it — a known member's oversized token is already out of domain by
    digit count — and its value is discarded, so no digit limit can decide a frame's fate.
    """
    try:
        return int(token)
    except ValueError:
        return _OVERSIZED_INT


def _payload_of(data: Frame) -> Any:
    """``json.loads`` behind the shared preflight, which has already settled grammar, byte policy
    and key policy — so this parse needs no hooks except the digit-limit guard."""
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
    """Restate msgspec's schema rejection in THIS module's words, via the pass the naive arm runs
    — so a missing member reads identically whichever arm produced it."""
    try:
        _read_fields(tag, _payload_of(data))
    except ValueError as exc:
        return exc
    # Not reachable while the two passes agree: after the preflight, presence and string typing
    # are all msgspec has left to give. Kept total rather than re-raising msgspec's text.
    return ValueError(f"malformed: {tag}: does not match its schema")  # pragma: no cover


# --- product arm (msgspec) ------------------------------------------------------------

_dec: Final = msgspec.json.Decoder(EngineMsg)
_enc: Final = msgspec.json.Encoder()


def decode(data: Frame) -> EngineMsg:
    """Decode one engine frame. Raises ``ValueError`` on any contract rejection."""
    tag = _prepare(data)
    try:
        msg: EngineMsg = _dec.decode(data)
    # `DecodeError`, not its subclass `ValidationError`, as defence in depth: a msgspec version
    # bump could let a parse-class error escape carrying msgspec's peer-influenced wording.
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

    Same verdicts, same rejection TEXT and same Struct as :func:`decode`; only the field
    extraction differs, which is exactly what the A/B measures.
    """
    tag = _prepare(data)
    cls, _ = _ENGINE_SCHEMAS[tag]
    msg: EngineMsg = cls(v=1, **_read_fields(tag, _payload_of(data)))
    _check_engine_strings(tag, msg)
    return msg


def encode_naive(msg: ClientMsg) -> bytes:
    """Encode one client command with stdlib ``json``, byte-identical to :func:`encode`.

    ``ensure_ascii=False`` is what makes it byte-identical: both C++ encoders and msgspec pass
    valid UTF-8 through raw and escape only control characters.
    """
    _check_client_message(msg, _CLIENT_RULES)
    tag, names = _WIRE_FIELDS[type(msg)]
    payload: dict[str, Any] = {"t": tag}
    for name in names:
        payload[name] = getattr(msg, name)
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode()
