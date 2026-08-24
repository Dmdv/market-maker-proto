"""Per-member wire rules, derived FROM the Structs so the two arms cannot drift apart.

The third of the three modules the Task-4 gate split `protocol.py` into, on the seam the
C++ side already uses: `_preflight.py` decides a frame's fate without knowing which message
it is (`frame_preflight.cpp`), `protocol.py` declares the messages and the codec API
(`protocol.hpp`), and this module holds the POLICY those messages carry — the integer
domains, the identifier shape caps, the per-field checkers, and the builders that turn a
Struct into the rule table each pass reads.

Every builder here takes the Struct CLASS as a parameter and imports nothing from
`protocol.py`, which is what keeps the import graph acyclic while leaving the Structs where
the plan puts them. The tables themselves are built once, at import, in `protocol.py`.

The import-time totality guards are the module's real point: a field type with no checker
(`_checker_for` / `_wire_type_checker`), a client string with no cap (`_identifier_rules`)
and an engine string with no inbound policy (`_engine_string_rules`) each raise `TypeError`
at IMPORT rather than letting a member reach the wire under no rule at all.
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

# The metadata NAMES the C++ member type and nothing more — deliberately not
# `msgspec.Meta(ge=..., le=...)` bounds. msgspec cannot express the uint64 upper bound
# (its integer constraints must fit in an int64), so Meta could only ever state HALF the
# policy, and the half it states would answer in msgspec's words while the naive arm
# answered in ours. The range lives here, once; :func:`_check_int_spans` applies it to
# the wire token on DECODE (both arms, before any library conversion) and
# :func:`_check_domains` to the value on ENCODE (both arms).

_DOMAINS: Final[dict[Any, tuple[int, int]]] = {
    Uint64: (0, 2**64 - 1),
    Int64: (-(2**63), 2**63 - 1),
}

_UINT64_MAX: Final = _DOMAINS[Uint64][1]
_UINT64_DIGITS: Final = len(str(_UINT64_MAX))
# The same domain expressed as a WIDTH, for the one check that must be total over values a
# peer or a strategy bug can make arbitrarily large: `int.bit_length()` reads the object's
# size and never renders it, so an oversized magnitude cannot decide how it is judged (see
# :func:`_as_version`, where rendering one leaked CPython's digit-limit message).
_UINT64_BITS: Final = 64
# --- identifier shape policy (mirrors detail::check_inbound_strings) ------------------

# Caps in BYTES, mirroring detail::kMaxClIdLen / kMaxSymbolLen / kMaxSideLen — the caps
# `detail::check_inbound_strings` applies to every inbound identifier. They are enforced
# on BOTH sides here: on ENCODE (:func:`_check_identifiers`) because a frame the engine
# will answer with `Malformed` is a client bug the encoder should stop being one the peer
# has to diagnose, and on DECODE (:func:`_check_engine_strings`) for the members the
# engine itself bounds, so the client's acceptance contract has the same last step the
# engine's does. `side` is capped at 8 rather than 1 exactly as the engine caps it, so the
# semantic BadSide verdict stays engine-owned.
_IDENTIFIER_CAPS: Final[dict[str, int]] = {"cl_id": 64, "symbol": 32, "side": 8}

# Engine -> client members that are engine FREE TEXT, bounded by the engine on neither
# side. Named explicitly rather than defaulted, so a string member added to an engine
# schema with neither a cap above nor a place here raises at IMPORT — the same discipline
# `_identifier_rules` applies to an uncapped client string.
_ENGINE_FREE_TEXT: Final[frozenset[str]] = frozenset({"status", "code", "reason"})

# Bytes < 0x20 and 0x7F, matched on the decoded str: in UTF-8 those byte values occur only
# as the ASCII code points themselves, so a code-point scan and the engine's byte scan
# agree. Identifiers key the engine's order map, are echoed on every report and land in
# log/telemetry text, where a NUL truncates C-string sinks and CR/LF forges records
# (CWE-158/117). On decode this is the ONLY guard that sees them: the grammar refuses RAW
# control bytes, but the escaped spellings of the same bytes are legal JSON strings.
_CONTROL_CHARS: Final = re.compile(r"[\x00-\x1f\x7f]")

# Lone surrogates, on the ENCODE side only: a decoded frame cannot contain one (the
# grammar's pairing rule refuses the escaped spelling and UTF-8 cannot carry the raw
# one), but a Python `str` built in the strategy can, and it has no UTF-8 encoding at all.
_SURROGATES: Final = re.compile("[\ud800-\udfff]")

_MINUS_BYTE: Final = 0x2D


def _is_unsigned_integer_token(span: bytes) -> bool:
    """``detail::is_unsigned_integer_token``, applied to a grammar-checked number span.

    Digits and nothing else — a leading ``-`` is refused outright, ``-0`` included, since
    negative zero is signed SYNTAX whatever its value. Like its C++ twin this predicate
    does not re-examine leading zeros or exponents: the span reached it through the
    strict RFC 8259 number production in ``_preflight._TOKEN``, so "all digits" already
    implies a canonical integer spelling.
    """
    return span.isdigit()


def _is_integer_token(span: bytes) -> bool:
    """``detail::is_integer_token`` — the wider predicate, for the ``int64_t`` members."""
    return span[1:].isdigit() if span[:1] == b"-" else span.isdigit()


# --- per-field rules, derived FROM the Structs so the arms cannot drift apart ---------


def _as_int(tag: str, name: str, value: Any) -> int:
    # `type(...) is int` and not isinstance: bool is a subclass of int, and `1e3`/`5.0`
    # arrive as float — the integer-TOKEN rule, expressed on the parsed value. The span
    # pass has already answered this frame on the raw token; this is the SECOND statement
    # of the rule, and the one that gives every schema field a checker — which is what
    # makes `_checker_for` fail at import on a field type nothing here validates.
    if type(value) is not int:
        raise ValueError(f"malformed: {tag}.{name} is not an integer")
    return value


def _as_str(tag: str, name: str, value: Any) -> str:
    if type(value) is not str:
        raise ValueError(f"malformed: {tag}.{name} is not a string")
    return value


def _as_bool(tag: str, name: str, value: Any) -> bool:
    """``post_only`` is the wire's only boolean, and it has no range half — ``bool`` has
    exactly two values. Written as a strict type check for symmetry with its siblings; for
    ``bool`` alone the two forms are indistinguishable, since it cannot be subclassed."""
    if type(value) is not bool:
        raise ValueError(f"malformed: {tag}.{name} is not a boolean")
    return value


def _as_version(tag: str, name: str, value: Any) -> int:
    """``v`` is a ``Literal[1]``, and on ENCODE there is no envelope check to own it.

    TOTAL over the ``uint64_t`` the wire declares ``v`` to be (``protocol.hpp``), in the
    three wordings the decode side already uses for these three verdicts: :func:`_as_int`'s
    for a non-integer, :func:`_check_domains`' for a value outside the member's wire domain,
    and :func:`_prepare`'s for a version this protocol does not speak. Wording equality with
    the wire side holds for a SUPPORTED-domain unknown version (``v=2``); out-of-domain values
    deliberately carry the domain wording and agree only on the ``Malformed`` classification —
    so a client-side and
    a wire-side rejection of the same value read alike.

    ORDER is what makes it total. Testing ``v == 1`` first classified ``v=-1`` and
    ``v=2**64`` as UnsupportedVersion, which is a verdict the ENGINE reserves for a version
    number it could hold and does not speak: `_prepare` answers both with "not an unsigned
    integer", and both C++ arms answer a 21-digit ``v`` with Malformed rather than
    UnsupportedVersion. Worse, the wording it reached renders the value, so a 5000-digit
    ``v`` left the codec carrying CPython's integer-string-conversion-limit message instead
    of any of this module's. The domain is therefore settled first, by WIDTH — a magnitude
    is never rendered to be judged, and only an in-domain ``v`` can reach the format below.
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

    `status` is the only such field today (`"live"` / `"cancelled"`). It has to be enforced on
    BOTH arms or the two decoders disagree about what a valid frame is — which is a measurement
    defect, not merely a bug, since the two arms are the §6 A/B and must accept exactly the same
    wire.
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

    There is deliberately no boolean rule: no engine -> client schema carries a boolean,
    and an unused checker cannot be pinned by a test (``bool`` cannot be subclassed, so a
    strict type check and ``isinstance`` are indistinguishable for it). A future boolean
    field must add its rule here, and this raise is what forces that.
    """
    if hint is str:
        return _as_str
    if hint in _DOMAINS:
        return _as_int
    # A closed literal — the tuned arm gets this from msgspec for free; the naive arm has to be
    # told, and this branch is what keeps the two decoders accepting the same set.
    if get_origin(hint) is Literal:
        members = get_args(hint)
        if all(isinstance(m, str) for m in members):
            return _as_literal(members)
    raise TypeError(f"no naive-arm rule for field type {hint!r}")


def _wire_type_checker(hint: Any) -> _FieldCheck:
    """The ENCODE-side twin of :func:`_checker_for`, over the members neither rule table
    types on its way to a range or a cap. A field type with no rule raises at IMPORT.

    Separate from :func:`_checker_for` because the two are total over different sets: that
    one reads ENGINE -> client schemas, which carry no boolean and whose ``v`` the envelope
    check owns; this one writes CLIENT -> engine ones, where ``post_only`` IS a boolean and
    nothing upstream has looked at ``v`` at all.
    """
    if hint is bool:
        return _as_bool
    if hint == Literal[1]:
        return _as_version
    raise TypeError(f"no encode-side rule for field type {hint!r}")


def _field_specs(cls: type[msgspec.Struct]) -> tuple[tuple[str, _FieldCheck], ...]:
    """``(name, checker)`` per field, derived FROM the Struct so the arms cannot drift
    apart. ``v`` is excluded: the envelope check owns it, and owns its precedence."""
    hints = get_type_hints(cls, include_extras=True)
    return tuple(
        (f.name, _checker_for(hints[f.name])) for f in msgspec.structs.fields(cls) if f.name != "v"
    )


def _int_domains(cls: type[msgspec.Struct]) -> tuple[tuple[str, int, int], ...]:
    """``(name, low, high)`` for every member declared with a wire integer domain.

    ``v`` is not one of them: it is a ``Literal[1]``, so its only legal value is in range
    by construction and the envelope check owns its verdict.
    """
    hints = get_type_hints(cls, include_extras=True)
    return tuple(
        (f.name, *_DOMAINS[hints[f.name]])
        for f in msgspec.structs.fields(cls)
        if hints[f.name] in _DOMAINS
    )


class _SpanRule(NamedTuple):
    """One integer member's DECODE-side rule, as :func:`_check_int_spans` reads it.

    Named fields rather than a bare 7-tuple: built positionally, `low`/`high` and
    `name`/`kind` could be transposed and still type-check clean under ``mypy --strict``.
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

    Derived FROM the Structs, like every other per-field table here, so a member added to
    a schema is covered by the span pass without a second declaration to keep in step.
    ``v`` is excluded: the envelope check owns it, and owns its precedence.
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
                # Digits in the widest in-domain magnitude. A token with more of them is
                # out of domain by its LENGTH — no conversion, whatever the digit limit.
                digits=len(str(max(high, -low))),
            )
        )
    return tuple(rules)


def _identifier_rules(cls: type[msgspec.Struct]) -> tuple[tuple[str, int], ...]:
    """``(name, byte cap)`` per STRING member of a client message.

    Derived FROM the Struct, like every other per-field table here, and total by
    construction: a string member with no cap in :data:`_IDENTIFIER_CAPS` raises at IMPORT
    rather than shipping an identifier the engine bounds and this client does not.
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

    The decode-side twin of :func:`_identifier_rules`, and total in the same way: a string
    member that is neither capped by :data:`_IDENTIFIER_CAPS` nor named as engine free
    text in :data:`_ENGINE_FREE_TEXT` raises at IMPORT, so a schema cannot grow a string
    the client accepts under no policy at all.
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
    """``(name, checker)`` per client-message member with no integer domain and no
    identifier cap — the members whose encode-side contract is their TYPE alone.

    Derived FROM the Struct like every other per-field table here, and the piece that makes
    the encode side total: together with :func:`_int_domains` and :func:`_identifier_rules`
    it covers EVERY member, so a field added to a client message either falls into one of
    those two or states its rule in :func:`_wire_type_checker` — and otherwise stops the
    import rather than being encoded unvalidated.
    """
    hints = get_type_hints(cls, include_extras=True)
    return tuple(
        (field.name, _wire_type_checker(hints[field.name]))
        for field in msgspec.structs.fields(cls)
        if hints[field.name] is not str and hints[field.name] not in _DOMAINS
    )


def _tag_of(cls: type[msgspec.Struct]) -> str:
    """The wire ``t`` spelling. msgspec types ``__struct_config__.tag`` loosely (a tag may
    be an int, or absent); every Struct in this module declares a string one."""
    return str(cls.__struct_config__.tag)


# --- the producer-side contract: three passes both encoders share --------------------
#
# The tables these read are built once, at import, in `protocol.py` — the only place that
# knows the Struct classes. What lives here is the POLICY and the ORDER, so the two
# encoders share one statement of both and neither can drift.

_TypeRules = tuple[tuple[str, _FieldCheck], ...]
_DomainRules = tuple[tuple[str, int, int], ...]
_IdentifierRules = tuple[tuple[str, int], ...]
_ClientRules = tuple[str, _TypeRules, _DomainRules, _IdentifierRules]
"""One client message's whole encode-side rule set: `(tag, types, domains, identifiers)`.

Bundled rather than three separate per-class dicts because the three passes are ONE
contract with a load-bearing order — and because the encode path then does one dict lookup
instead of three.

The passes below take `msgspec.Struct` rather than `protocol.ClientMsg`: naming the union
here would invert the import graph, and the public `encode`/`encode_naive` signatures are
where the caller-visible type lives. Every one of them is reached ONLY through
`_CLIENT_RULES`, whose keys are `typing.get_args(protocol.ClientMsg)` — DERIVED from the
union rather than restated beside it, so the table cannot fall behind a new variant.
"""


def _check_client_message(msg: msgspec.Struct, registry: Mapping[Any, _ClientRules]) -> None:
    """The whole ENCODE-side contract, stated ONCE and in contract ORDER.

    The order is load-bearing, which is why it does not live inline in each encoder:
    :func:`_check_wire_types` owns ``v`` (through :func:`_as_version`), and the engine
    classifies a bad ``v`` as ``UnsupportedVersion`` rather than ``Malformed`` — a verdict
    that must outrank every field error on this side exactly as :func:`_prepare` makes it
    outrank them on the other. Restated in two encoders, one contract needed two mutants
    to pin and could drift in one arm; here it is one rule with one home.

    The DISPATCH lives here for the same reason. Both encoders used to index the registry
    themselves, on the EXACT runtime type, so a legal ``NewOrder`` subclass — msgspec
    Structs may be subclassed — left the codec as a bare ``KeyError`` naming a class object:
    neither of the two exception types this module's contract names, and no diagnosis. It is
    a caller bug rather than a wire verdict, so it answers in ``TypeError``, the same class
    ``_check_frame_bytes`` gives a caller who hands the decoder a ``str``.
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
    """The ENCODE-side half of the domain rule, applied by both encoders: the client must
    not PRODUCE a frame the engine's ``uint64_t``/``int64_t`` members cannot hold.

    Decode's half is :func:`_check_int_spans`, which answers on the wire token instead —
    same ``_DOMAINS`` table, one step earlier, because on that side there is a raw span to
    judge and a library conversion to keep out of the verdict.

    TYPE before range, through the same :func:`_as_int` that states the rule on the decode
    side. A range test cannot stand alone here: ``low <= value <= high`` is true of a
    ``bool`` and of a whole-valued ``float``, so ``px=True`` and ``qty=1.0`` used to reach
    the wire as ``"px":true`` and ``"qty":1.0`` — frames BOTH C++ arms answer with
    ``Malformed new_order: missing or mistyped field`` (probed against the engine's own
    ``mm::make_codec``). ``mypy --strict`` is not a substitute: it catches the float and
    NOT the bool, because ``bool`` subclasses ``int``.
    """
    for name, low, high in domains:
        value = _as_int(tag, name, getattr(msg, name))
        if not low <= value <= high:
            raise ValueError(f"malformed: {tag}.{name} outside its wire integer domain")


def _check_identifiers(tag: str, rules: _IdentifierRules, msg: msgspec.Struct) -> None:
    """The ENCODE-side twin of ``detail::check_inbound_strings``: an identifier this client
    produces must fit the engine's byte cap and carry no control byte.

    The engine applies this policy to every inbound ``cl_id``/``symbol``/``side`` and
    answers a breach with ``Malformed``; without this guard the client would happily emit
    a 65-byte ``cl_id`` and learn about it from a ``Reject``. Same principle as
    :func:`_check_domains`, same precedence as the engine's (field errors, then identifier
    shape), and the caps are the engine's own — :data:`_IDENTIFIER_CAPS`.
    """
    for name, cap in rules:
        # TYPE before shape, for the reason :func:`_check_domains` types before it
        # measures — and because the measurement itself assumes the type: a non-`str`
        # reached `isascii()` and left the encoder as an `AttributeError`, which is
        # neither of the two exceptions this module's contract names.
        value = _as_str(tag, name, getattr(msg, name))
        # `isascii()` is a flag read, so the common all-ASCII identifier is measured
        # without an encode; only a non-ASCII one pays for its UTF-8 byte length.
        if value.isascii():
            size = len(value)
        else:
            # A lone surrogate has NO UTF-8 encoding, so `len(value.encode())` below would
            # leave the encoder as a stdlib `UnicodeEncodeError` — the module's only
            # rejection with no wording of its own, while the decode side answers the same
            # content in the module's words. Stated here instead.
            if _SURROGATES.search(value):
                raise ValueError(f"malformed: unpaired surrogate in {tag}.{name}")
            size = len(value.encode())
        if size > cap:
            raise ValueError(f"malformed: {tag}.{name} exceeds the length cap")
        if _CONTROL_CHARS.search(value):
            raise ValueError(f"malformed: control byte in {tag}.{name}")


def _check_wire_types(tag: str, rules: _TypeRules, msg: msgspec.Struct) -> None:
    """The members whose WHOLE encode-side rule is their type: no range, no shape.

    Today that is ``post_only`` (a ``bool``) and ``v`` (a ``Literal[1]``). The integer
    members are typed by :func:`_check_domains` on their way to the range test and the
    identifiers by :func:`_check_identifiers` on their way to the cap, so this pass is what
    makes the encode side TOTAL: every member of a client message is now reached by exactly
    one of the three, and a member type none of them covers raises at IMPORT
    (:func:`_wire_type_checker`), exactly as an uncapped string member does.

    Not a formality — ``post_only`` had no checker of any kind, so
    ``NewOrder(post_only="yes")`` encoded to ``"post_only":"yes"`` and both C++ arms
    answered ``Malformed new_order: missing or mistyped field``.
    """
    for name, check in rules:
        check(tag, name, getattr(msg, name))
