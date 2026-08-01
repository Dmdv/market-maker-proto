"""Per-member wire rules, derived FROM the Structs so the two arms cannot drift apart.

Builders take the Struct CLASS and import nothing from `protocol.py`, keeping the import graph
acyclic. Totality guards raise `TypeError` at IMPORT for any member left under no rule at all.
"""

import re
from collections.abc import Callable, Mapping
from typing import Annotated, Any, Final, Literal, NamedTuple, get_args, get_origin, get_type_hints

import msgspec

# --- integer domains: the C++ member types are part of the wire contract ---------------

Uint64 = Annotated[int, "uint64_t"]
"""A C++ ``std::uint64_t`` wire field (``protocol.hpp``)."""

Int64 = Annotated[int, "int64_t"]
"""A C++ ``std::int64_t`` wire field (``protocol.hpp``)."""

# The metadata NAMES the C++ member type and nothing more: msgspec cannot express the uint64
# upper bound, so `Meta` could state only half the policy, and in msgspec's words rather than ours.

_DOMAINS: Final[dict[Any, tuple[int, int]]] = {
    Uint64: (0, 2**64 - 1),
    Int64: (-(2**63), 2**63 - 1),
}

_UINT64_MAX: Final = _DOMAINS[Uint64][1]
_UINT64_DIGITS: Final = len(str(_UINT64_MAX))
# The same domain expressed as a WIDTH, for the check that must be total over arbitrarily large
# values: `bit_length()` never renders the object, so a magnitude cannot decide its own verdict.
_UINT64_BITS: Final = 64
# --- identifier shape policy (mirrors detail::check_inbound_strings) ------------------

# Caps in BYTES, mirroring detail::kMaxClIdLen / kMaxSymbolLen / kMaxSideLen, and enforced on both
# ENCODE and DECODE. `side` is capped at 8 as the engine caps it, so BadSide stays engine-owned.
_IDENTIFIER_CAPS: Final[dict[str, int]] = {"cl_id": 64, "symbol": 32, "side": 8}

# Engine -> client members the engine bounds on NEITHER side. Named explicitly, so a string member
# added to an engine schema with neither a cap above nor a place here raises at IMPORT.
_ENGINE_FREE_TEXT: Final[frozenset[str]] = frozenset({"status", "code", "reason"})

# Bytes < 0x20 and 0x7F, matched on the decoded str. On decode this is the ONLY guard that sees
# them: the grammar refuses RAW control bytes, but their escaped spellings are legal JSON strings,
# and these values land in the engine's order map and in log text (CWE-158/117).
_CONTROL_CHARS: Final = re.compile(r"[\x00-\x1f\x7f]")

# Lone surrogates, ENCODE side only: a decoded frame cannot carry one, but a `str` built in the
# strategy can, and it has no UTF-8 encoding at all.
_SURROGATES: Final = re.compile("[\ud800-\udfff]")

_MINUS_BYTE: Final = 0x2D


def _is_unsigned_integer_token(span: bytes) -> bool:
    """``detail::is_unsigned_integer_token``, applied to a grammar-checked number span.

    Digits and nothing else — a leading ``-`` is refused, ``-0`` included, since negative zero
    is signed SYNTAX whatever its value. The strict RFC 8259 production has already run.
    """
    return span.isdigit()


def _is_integer_token(span: bytes) -> bool:
    """``detail::is_integer_token`` — the wider predicate, for the ``int64_t`` members."""
    return span[1:].isdigit() if span[:1] == b"-" else span.isdigit()


# --- per-field rules, derived FROM the Structs so the arms cannot drift apart ---------


def _as_int(tag: str, name: str, value: Any) -> int:
    # `type(...) is int` and not isinstance: bool is a subclass of int, and `1e3`/`5.0` arrive
    # as float — the integer-TOKEN rule, expressed on the parsed value.
    if type(value) is not int:
        raise ValueError(f"malformed: {tag}.{name} is not an integer")
    return value


def _as_str(tag: str, name: str, value: Any) -> str:
    if type(value) is not str:
        raise ValueError(f"malformed: {tag}.{name} is not a string")
    return value


def _as_bool(tag: str, name: str, value: Any) -> bool:
    """``post_only`` is the wire's only boolean; a strict type check, for symmetry with siblings."""
    if type(value) is not bool:
        raise ValueError(f"malformed: {tag}.{name} is not a boolean")
    return value


def _as_version(tag: str, name: str, value: Any) -> int:
    """``v`` is a ``Literal[1]``, and on ENCODE there is no envelope check to own it.

    TOTAL over the wire's ``uint64_t``. The domain is settled FIRST, by WIDTH: an out-of-domain
    ``v`` is Malformed rather than UnsupportedVersion, and no magnitude is rendered to be judged.
    """
    version = _as_int(tag, name, value)
    if version < 0 or version.bit_length() > _UINT64_BITS:
        raise ValueError(f"malformed: {tag}.{name} outside its wire integer domain")
    if version != 1:
        raise ValueError(f"unsupported version: v={version}")
    return version


_FieldCheck = Callable[[str, str, Any], Any]


def _as_literal(allowed: tuple[str, ...]) -> _FieldCheck:
    """A closed set of string values, checked with the SAME verdict text as the tuned arm.

    Enforced on BOTH arms or the two decoders disagree about what a valid frame is — a
    measurement defect, since the arms must accept exactly the same wire.
    """

    def check(tag: str, name: str, value: Any) -> str:
        if type(value) is not str:
            raise ValueError(f"malformed: {tag}.{name} is not a string")
        if value not in allowed:
            raise ValueError(f"malformed: {tag}.{name} is not one of {'/'.join(allowed)}")
        return value

    return check


def _checker_for(hint: Any) -> _FieldCheck:
    """A field type with no explicit rule raises at IMPORT, never passes unvalidated.

    No boolean rule deliberately: no engine -> client schema carries one, and an unused checker
    cannot be pinned by a test. A future boolean field must add its rule here.
    """
    if hint is str:
        return _as_str
    if hint in _DOMAINS:
        return _as_int
    # A closed literal: msgspec gives the tuned arm this for free, the naive arm has to be told.
    if get_origin(hint) is Literal:
        members = get_args(hint)
        if all(isinstance(m, str) for m in members):
            return _as_literal(members)
    raise TypeError(f"no naive-arm rule for field type {hint!r}")


def _wire_type_checker(hint: Any) -> _FieldCheck:
    """The ENCODE-side twin of :func:`_checker_for`; a field type with no rule raises at IMPORT.

    Separate because the two are total over different sets: that one reads ENGINE -> client
    schemas, this one writes CLIENT -> engine ones, where ``post_only`` IS a boolean.
    """
    if hint is bool:
        return _as_bool
    if hint == Literal[1]:
        return _as_version
    raise TypeError(f"no encode-side rule for field type {hint!r}")


def _field_specs(cls: type[msgspec.Struct]) -> tuple[tuple[str, _FieldCheck], ...]:
    """``(name, checker)`` per field; ``v`` is excluded — the envelope check owns it."""
    hints = get_type_hints(cls, include_extras=True)
    return tuple(
        (f.name, _checker_for(hints[f.name])) for f in msgspec.structs.fields(cls) if f.name != "v"
    )


def _int_domains(cls: type[msgspec.Struct]) -> tuple[tuple[str, int, int], ...]:
    """``(name, low, high)`` per member with a wire integer domain; ``v`` is excluded — a
    ``Literal[1]`` is in range by construction, and the envelope check owns its verdict."""
    hints = get_type_hints(cls, include_extras=True)
    return tuple(
        (f.name, *_DOMAINS[hints[f.name]])
        for f in msgspec.structs.fields(cls)
        if hints[f.name] in _DOMAINS
    )


class _SpanRule(NamedTuple):
    """One integer member's DECODE-side rule, as :func:`_check_int_spans` reads it.

    Named fields rather than a bare 7-tuple: built positionally, `low`/`high` and `name`/`kind`
    could be transposed and still type-check clean.
    """

    key: bytes
    name: str
    is_token: Callable[[bytes], bool]
    kind: str
    low: int
    high: int
    digits: int


def _span_rules(cls: type[msgspec.Struct]) -> tuple[_SpanRule, ...]:
    """Per integer member: ``(key, name, token predicate, its noun, low, high, digits)``.

    Derived FROM the Structs, so a member added to a schema is covered without a second
    declaration to keep in step. ``v`` is excluded: the envelope check owns it.
    """
    hints = get_type_hints(cls, include_extras=True)
    rules: list[_SpanRule] = []
    for field in msgspec.structs.fields(cls):
        hint = hints[field.name]
        if field.name == "v" or hint not in _DOMAINS:
            continue
        low, high = _DOMAINS[hint]
        signed = low < 0
        rules.append(
            _SpanRule(
                key=field.name.encode(),
                name=field.name,
                is_token=_is_integer_token if signed else _is_unsigned_integer_token,
                kind="an integer" if signed else "an unsigned integer",
                low=low,
                high=high,
                # Digits in the widest in-domain magnitude: a longer token is out of domain
                # by its LENGTH — no conversion, whatever the digit limit.
                digits=len(str(max(high, -low))),
            )
        )
    return tuple(rules)


def _identifier_rules(cls: type[msgspec.Struct]) -> tuple[tuple[str, int], ...]:
    """``(name, byte cap)`` per STRING member of a client message.

    Total by construction: a string member with no cap in :data:`_IDENTIFIER_CAPS` raises at
    IMPORT rather than shipping an identifier the engine bounds and this client does not.
    """
    hints = get_type_hints(cls, include_extras=True)
    rules: list[tuple[str, int]] = []
    for field in msgspec.structs.fields(cls):
        if hints[field.name] is not str:
            continue
        if field.name not in _IDENTIFIER_CAPS:
            raise TypeError(f"no identifier cap for {cls.__name__}.{field.name}")
        rules.append((field.name, _IDENTIFIER_CAPS[field.name]))
    return tuple(rules)


def _engine_string_rules(cls: type[msgspec.Struct]) -> tuple[tuple[str, int | None], ...]:
    """``(name, byte cap or None)`` per STRING member of an ENGINE -> client message.

    The decode-side twin of :func:`_identifier_rules`, total in the same way: a string that is
    neither capped by :data:`_IDENTIFIER_CAPS` nor named in :data:`_ENGINE_FREE_TEXT` raises.
    """
    hints = get_type_hints(cls, include_extras=True)
    rules: list[tuple[str, int | None]] = []
    for field in msgspec.structs.fields(cls):
        if hints[field.name] is not str:
            continue
        if field.name in _IDENTIFIER_CAPS:
            rules.append((field.name, _IDENTIFIER_CAPS[field.name]))
        elif field.name in _ENGINE_FREE_TEXT:
            rules.append((field.name, None))
        else:
            raise TypeError(f"no inbound string policy for {cls.__name__}.{field.name}")
    return tuple(rules)


def _wire_type_rules(cls: type[msgspec.Struct]) -> tuple[tuple[str, _FieldCheck], ...]:
    """``(name, checker)`` per client-message member with no integer domain and no identifier
    cap — the members whose encode-side contract is their TYPE alone.

    With :func:`_int_domains` and :func:`_identifier_rules` this covers EVERY member, so a field
    none of the three reaches stops the import rather than being encoded unvalidated.
    """
    hints = get_type_hints(cls, include_extras=True)
    return tuple(
        (field.name, _wire_type_checker(hints[field.name]))
        for field in msgspec.structs.fields(cls)
        if hints[field.name] is not str and hints[field.name] not in _DOMAINS
    )


def _tag_of(cls: type[msgspec.Struct]) -> str:
    """The wire ``t`` spelling; msgspec types the tag loosely, and every Struct here uses str."""
    return str(cls.__struct_config__.tag)


# --- the producer-side contract: three passes both encoders share --------------------
#
# The tables these read are built once, at import, in `protocol.py`. What lives here is the
# POLICY and the ORDER, so the two encoders share one statement of both and neither can drift.

_TypeRules = tuple[tuple[str, _FieldCheck], ...]
_DomainRules = tuple[tuple[str, int, int], ...]
_IdentifierRules = tuple[tuple[str, int], ...]
_ClientRules = tuple[str, _TypeRules, _DomainRules, _IdentifierRules]
"""One client message's whole encode-side rule set: `(tag, types, domains, identifiers)`.

Bundled because the three passes are ONE contract with a load-bearing order. Reached only through
`_CLIENT_RULES`, whose keys are DERIVED from `protocol.ClientMsg` rather than restated beside it.
"""


def _check_client_message(msg: msgspec.Struct, registry: Mapping[Any, _ClientRules]) -> None:
    """The whole ENCODE-side contract, stated ONCE and in contract ORDER.

    ``v`` is owned by :func:`_check_wire_types`, and its ``UnsupportedVersion`` verdict must
    outrank every field error. An unregistered type is a caller bug, so it answers ``TypeError``.
    """
    try:
        tag, wire_types, domains, identifiers = registry[type(msg)]
    except KeyError:
        name = type(msg).__name__
        raise TypeError(f"{name} is not a client message this codec encodes") from None
    _check_wire_types(tag, wire_types, msg)
    _check_domains(tag, domains, msg)
    _check_identifiers(tag, identifiers, msg)


def _check_domains(tag: str, domains: _DomainRules, msg: msgspec.Struct) -> None:
    """The ENCODE-side half of the domain rule: the client must not PRODUCE a frame the engine's
    ``uint64_t``/``int64_t`` members cannot hold. Decode's half is :func:`_check_int_spans`.

    TYPE before range, through the same :func:`_as_int` the decode side uses. A range test alone
    cannot stand: ``low <= value <= high`` is true of a ``bool`` and of a whole-valued ``float``.
    """
    for name, low, high in domains:
        value = _as_int(tag, name, getattr(msg, name))
        if not low <= value <= high:
            raise ValueError(f"malformed: {tag}.{name} outside its wire integer domain")


def _check_identifiers(tag: str, rules: _IdentifierRules, msg: msgspec.Struct) -> None:
    """The ENCODE-side twin of ``detail::check_inbound_strings``: an identifier this client
    produces must fit the engine's byte cap and carry no control byte.

    Same precedence as the engine's — field errors, then identifier shape — and the caps are
    the engine's own (:data:`_IDENTIFIER_CAPS`).
    """
    for name, cap in rules:
        # TYPE before shape, because the measurement itself assumes the type: a non-`str`
        # reached `isascii()` and left the encoder as an `AttributeError`.
        value = _as_str(tag, name, getattr(msg, name))
        # `isascii()` is a flag read: only a non-ASCII identifier pays for a UTF-8 encode.
        if value.isascii():
            size = len(value)
        else:
            # A lone surrogate has NO UTF-8 encoding, so `value.encode()` below would leave the
            # encoder as a stdlib `UnicodeEncodeError`. Stated in the module's words instead.
            if _SURROGATES.search(value):
                raise ValueError(f"malformed: unpaired surrogate in {tag}.{name}")
            size = len(value.encode())
        if size > cap:
            raise ValueError(f"malformed: {tag}.{name} exceeds the length cap")
        if _CONTROL_CHARS.search(value):
            raise ValueError(f"malformed: control byte in {tag}.{name}")


def _check_wire_types(tag: str, rules: _TypeRules, msg: msgspec.Struct) -> None:
    """The members whose WHOLE encode-side rule is their type: no range, no shape.

    Today ``post_only`` (a ``bool``) and ``v`` (a ``Literal[1]``). This pass is what makes the
    encode side TOTAL — a member type none of the three passes covers raises at IMPORT.
    """
    for name, check in rules:
        check(tag, name, getattr(msg, name))
