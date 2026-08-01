"""What this client may PRODUCE: the encode-side contract, through both encoders.

Chapter three of the message-level battery — see ``test_protocol.py`` for the split and the
contract source. A client must not PRODUCE a frame the engine answers with ``Malformed``,
so the producer side carries the same rules as the consumer side, applied to a value
instead of to a wire token: the member's TYPE first, then its integer domain, then its
identifier shape. The three passes PARTITION a client message's members, and the two
totality tests at the bottom are what keep that true as the schemas grow.
"""

import re
from collections.abc import Callable
from typing import Any, Literal, get_args, get_type_hints

import msgspec
import pytest
from protocol_support import ENCODERS, UnpolicedString, new_order

from mmclient import _rules, protocol
from mmclient.protocol import (
    CancelAck,
    CancelOrder,
    Fill,
    NewOrder,
    OrderAck,
    Reject,
    Tob,
    encode,
    encode_naive,
)

# --- encode ---------------------------------------------------------------------------


def test_both_encoders_agree_on_non_ascii_payloads() -> None:
    """UTF-8 passes through raw in both arms (``ensure_ascii=False`` mirrors msgspec)."""
    msg = new_order(cl_id="C-é中", symbol="MÖCKUSDT")
    assert encode_naive(msg) == encode(msg)
    assert "é".encode() in encode(msg)


def test_post_only_default_is_still_emitted_on_the_wire() -> None:
    """``post_only``'s Struct default is a CONSTRUCTION convenience only: the engine's
    inbound field set is closed, so every encoded frame must carry the field."""
    msg = new_order()
    assert msg.post_only is True
    assert b'"post_only":true' in encode(msg)
    assert b'"post_only":true' in encode_naive(msg)


def test_encoders_emit_no_whitespace() -> None:
    blob = encode(CancelOrder(v=1, seq=1, epoch=1, cl_id="C-1"))
    assert blob == encode_naive(CancelOrder(v=1, seq=1, epoch=1, cl_id="C-1"))
    assert b" " not in blob and b"\n" not in blob


@ENCODERS
@pytest.mark.parametrize("px", [2**63, -(2**63) - 1], ids=["above", "below"])
def test_encoders_refuse_a_value_outside_the_wire_domain(
    enc: Callable[[NewOrder], bytes], px: int
) -> None:
    """The client must not PRODUCE what the engine's ``int64_t`` cannot hold: an
    out-of-domain price is a strategy bug, and the encoder is where it stops being one
    the peer has to diagnose."""
    with pytest.raises(ValueError, match="outside its wire integer domain"):
        enc(new_order(px=px))


@ENCODERS
def test_encoders_refuse_a_negative_unsigned_field(enc: Callable[[CancelOrder], bytes]) -> None:
    with pytest.raises(ValueError, match="outside its wire integer domain"):
        enc(CancelOrder(v=1, seq=-1, epoch=1, cl_id="C-1"))


@ENCODERS
@pytest.mark.parametrize(
    "msg",
    [
        new_order(px=2**63 - 1),
        new_order(px=-(2**63)),
        new_order(qty=2**63 - 1),
        CancelOrder(v=1, seq=0, epoch=2**64 - 1, cl_id="C-1"),
        CancelOrder(v=1, seq=2**64 - 1, epoch=0, cl_id="C-1"),
    ],
    ids=["i64-max", "i64-min", "qty-i64-max", "u64-max-epoch", "u64-max-seq"],
)
def test_encoders_accept_the_exact_domain_boundaries(enc: Callable[[Any], bytes], msg: Any) -> None:
    """The ACCEPT side of the domain rule, which had no test at all: every encode-side
    case used px=5/qty=10 or px=499995, so both ``<=`` -> ``<`` mutants of the range
    comparison survived and an off-by-one narrowing would silently refuse legal extreme
    prices and sequences."""
    assert enc(msg)


# --- encode-side identifier shape (the twin of detail::check_inbound_strings) ----------
#
# Same principle as the integer domains, from the same contract (codec.hpp): the engine
# caps cl_id/symbol/side at 64/32/8 BYTES and refuses control bytes in them, so a client
# that emits a 65-byte cl_id has produced a frame the engine answers with Malformed. The
# encoder is where that stops being something the peer has to diagnose.


@ENCODERS
@pytest.mark.parametrize(
    ("name", "cap"), [("cl_id", 64), ("symbol", 32), ("side", 8)], ids=["cl_id", "symbol", "side"]
)
def test_encoders_enforce_the_identifier_length_caps(
    enc: Callable[[NewOrder], bytes], name: str, cap: int
) -> None:
    """At the cap encoded, one byte past it refused — per identifier, since one shared cap
    would silently accept a 64-byte ``symbol`` the engine bounds at 32."""
    assert enc(new_order(**{name: "x" * cap}))
    with pytest.raises(ValueError, match=re.escape(f"new_order.{name} exceeds the length cap")):
        enc(new_order(**{name: "x" * (cap + 1)}))


@ENCODERS
def test_identifier_caps_count_bytes_not_code_points(enc: Callable[[NewOrder], bytes]) -> None:
    """The engine's caps are BYTE caps (``detail::kMaxSymbolLen``), and the wire is UTF-8:
    17 two-byte code points are 34 bytes and do not fit a 32-byte member, however short
    Python calls the ``str``."""
    assert len("é" * 17) == 17
    with pytest.raises(ValueError, match=re.escape("new_order.symbol exceeds the length cap")):
        enc(new_order(symbol="é" * 17))
    assert enc(new_order(symbol="é" * 16))


@ENCODERS
@pytest.mark.parametrize("char", ["\x00", "\n", "\x1f", "\x7f"], ids=["nul", "lf", "us", "del"])
def test_encoders_refuse_a_control_byte_in_an_identifier(
    enc: Callable[[NewOrder], bytes], char: str
) -> None:
    """An identifier keys the engine's order map and is echoed into log/telemetry text: a
    NUL truncates C-string sinks (two cl_ids collide) and CR/LF forges records
    (CWE-158/117). Refused at the source, exactly as the engine refuses them inbound."""
    with pytest.raises(ValueError, match=re.escape("control byte in new_order.cl_id")):
        enc(new_order(cl_id=f"C{char}1"))


@ENCODERS
def test_an_encoded_identifier_breaking_both_rules_names_the_cap_first(
    enc: Callable[[NewOrder], bytes],
) -> None:
    """Which of the two rules speaks first is contract text, and until this case nothing
    pinned it on THIS side either: every identifier case above breaks exactly one rule, so
    swapping the cap block and the control-byte block inside ``_check_identifiers`` left
    the whole suite green. (``test_the_version_verdict_outranks_every_field_error`` pins
    the order of the three PASSES, which is a different statement.) The engine checks the
    cap first — ``detail::check_identifier`` in ``cpp/src/frame_preflight.cpp`` — and
    ``test_decode_fields.py`` pins the same order on the consume side, so the two
    directions of one verdict must name the same rule.
    """
    with pytest.raises(ValueError) as caught:
        enc(new_order(cl_id="C" * 64 + "\x00"))
    assert str(caught.value) == "malformed: new_order.cl_id exceeds the length cap"


@ENCODERS
@pytest.mark.parametrize(
    ("name", "value"),
    [("cl_id", "C-\ud800"), ("symbol", "MOCK\udfffUSDT"), ("side", "\ud800")],
    ids=["cl_id-high", "symbol-low", "side"],
)
def test_encoders_refuse_a_lone_surrogate_in_an_identifier(
    enc: Callable[[NewOrder], bytes], name: str, value: str
) -> None:
    """A lone surrogate has NO UTF-8 encoding, so both encoders used to answer it with the
    stdlib's ``UnicodeEncodeError`` — a ``ValueError`` subclass, so the boundary contract
    held and the arms agreed, but the module's ONLY rejection with no wording of its own,
    while the DECODE side answers the same content with "unpaired high surrogate escape".
    Stated in the module's words instead."""
    with pytest.raises(ValueError) as caught:
        enc(new_order(**{name: value}))
    assert str(caught.value) == f"malformed: unpaired surrogate in new_order.{name}"


@ENCODERS
def test_cancel_order_identifiers_are_checked_too(enc: Callable[[CancelOrder], bytes]) -> None:
    """``CancelOrder`` carries only ``cl_id``, and ``check_inbound_strings`` has an
    overload for exactly that — the rule is per message, not per field name."""
    assert enc(CancelOrder(v=1, seq=1, epoch=1, cl_id="C" * 64))
    with pytest.raises(ValueError, match=re.escape("cancel_order.cl_id exceeds the length cap")):
        enc(CancelOrder(v=1, seq=1, epoch=1, cl_id="C" * 65))
    with pytest.raises(ValueError, match=re.escape("control byte in cancel_order.cl_id")):
        enc(CancelOrder(v=1, seq=1, epoch=1, cl_id="C\x00"))


def test_every_client_string_member_has_a_cap() -> None:
    """The table is derived from the Structs and is TOTAL: a string member added to a
    client message with no cap in ``_IDENTIFIER_CAPS`` raises at import rather than
    shipping an identifier the engine bounds and this client does not."""
    for cls in (NewOrder, CancelOrder):
        tag, _, _, identifiers = protocol._CLIENT_RULES[cls]
        hints = get_type_hints(cls, include_extras=True)
        capped = {name for name, _ in identifiers}
        assert capped == {f.name for f in msgspec.structs.fields(cls) if hints[f.name] is str}, tag
    with pytest.raises(TypeError, match="no identifier cap"):
        _rules._identifier_rules(UnpolicedString)


# --- encode-side member types (the verdict a RANGE test cannot give) -------------------
#
# `low <= value <= high` is true of `True` and of a whole-valued `float`, and `post_only`
# had no rule of any kind, so the encoders emitted `"px":true`, `"qty":1.0` and
# `"post_only":"yes"` — each of which BOTH C++ arms answer with
# `Malformed new_order: missing or mistyped field`, probed through the engine's own
# `mm::make_codec` against `build/dev/libmm_core.a`, with `encode_naive` byte-identical to
# `encode` in every case (so it was never a one-arm defect). `mypy --strict` does not
# cover for the gap: it rejects `qty=1.0` and `post_only="yes"` but NOT `px=True`, because
# `bool` subclasses `int` — a static checker cannot stand in for the runtime guard.


@ENCODERS
@pytest.mark.parametrize(
    ("member", "value", "expected"),
    [
        ("px", True, "malformed: new_order.px is not an integer"),
        ("qty", 1.0, "malformed: new_order.qty is not an integer"),
        ("post_only", "yes", "malformed: new_order.post_only is not a boolean"),
        ("post_only", 0, "malformed: new_order.post_only is not a boolean"),
        ("post_only", 1, "malformed: new_order.post_only is not a boolean"),
        ("symbol", 5, "malformed: new_order.symbol is not a string"),
        ("v", 2, "unsupported version: v=2"),
        ("v", True, "malformed: new_order.v is not an integer"),
        ("v", 1.0, "malformed: new_order.v is not an integer"),
    ],
    ids=[
        "bool-price",
        "float-qty",
        "string-flag",
        "int-zero-flag",
        "int-one-flag",
        "int-symbol",
        "wrong-version",
        "bool-version",
        "float-version",
    ],
)
def test_encoders_refuse_a_mistyped_member(
    enc: Callable[[NewOrder], bytes], member: str, value: Any, expected: str
) -> None:
    """One wording per member, both arms — the encode-side twin of ``assert_one_wording``.

    The ``wrong-version`` row is the envelope: the engine answers ``v":2`` with
    ``UNSUPPORTED_VERSION v=2`` rather than ``Malformed``, and this client says
    ``unsupported version: v=2`` on both the produce and the consume side, so the two
    directions of the same verdict read alike.

    The four rows added at the Task-4 gate are the ones that make the two strict type checks
    FALSIFIABLE, and each was reached by a surviving mutant rather than by symmetry. Relaxing
    ``_as_bool`` to ``isinstance(value, int)`` accepts ``post_only=0`` and ``post_only=1``
    and emits ``"post_only":0`` — a token both C++ arms answer with
    ``Malformed new_order: missing or mistyped field``, since the wire member is a JSON
    boolean and not an integer — while every case above it still passed. Dropping
    ``_as_version``'s ``_as_int`` call accepts ``v=True`` and ``v=1.0``, because ``True == 1``
    and ``1.0 == 1`` in Python, and emits ``"v":true`` / ``"v":1.0``; the whole suite stayed
    green through both mutants until these rows landed.
    """
    with pytest.raises(ValueError) as caught:
        enc(new_order(**{member: value}))
    assert str(caught.value) == expected


@ENCODERS
@pytest.mark.parametrize(
    "version", [-1, 2**64, 10**4999], ids=["negative", "past-u64", "5000-digit"]
)
def test_encoders_refuse_a_version_outside_the_wire_domain(
    enc: Callable[[NewOrder], bytes], version: int
) -> None:
    """``v`` is a ``uint64_t`` on the wire, so the encode-side rule must be TOTAL over it.

    Until the Task-4 gate ``_as_version`` tested ``v == 1`` and nothing else, so it gave the
    wrong verdict twice and no verdict of its own once. ``v=-1`` and ``v=2**64`` came back as
    ``unsupported version``, which is what the engine reserves for a version number it could
    HOLD and does not speak — ``_prepare`` answers both with "not an unsigned integer" on the
    consume side, and both C++ arms classify a 21-digit ``v`` as Malformed rather than
    UnsupportedVersion. And the third row never reached this module's vocabulary at all: the
    ``unsupported version: v={version}`` f-string rendered the integer, so a 5000-digit ``v``
    left the codec carrying CPython's ``Exceeds the limit (4300 digits) for integer string
    conversion`` — a parser implementation detail as a wire rejection message. The domain is
    now settled first and by ``bit_length()``, which reads the magnitude without rendering it.
    """
    with pytest.raises(ValueError) as caught:
        enc(new_order(v=version))
    assert str(caught.value) == "malformed: new_order.v outside its wire integer domain"


@ENCODERS
def test_encoders_refuse_a_runtime_type_the_rule_table_does_not_carry(
    enc: Callable[[Any], bytes],
) -> None:
    """Dispatch is on the EXACT type, and msgspec Structs may be subclassed.

    ``_CLIENT_RULES`` is keyed by class, so a subclass of a client message — legal msgspec,
    and the shape a later task wrapping a command would reach for — left both encoders as a
    bare ``KeyError`` naming a class object: neither of the two exception types this module's
    contract names, and no diagnosis. It is a caller bug rather than a wire verdict, so it
    answers in ``TypeError``, the class ``_check_frame_bytes`` already gives a caller who
    hands the DECODER a ``str``.
    """

    class Amended(NewOrder):
        pass

    with pytest.raises(TypeError, match="Amended is not a client message"):
        enc(Amended(**msgspec.structs.asdict(new_order())))


@ENCODERS
def test_the_version_verdict_outranks_every_field_error(enc: Callable[[NewOrder], bytes]) -> None:
    """The ORDER of the three encode passes is contract, not convenience: the engine
    classifies a bad ``v`` as ``UnsupportedVersion`` and everything else as ``Malformed``,
    so the version must be answered first even when the frame is also unencodable for
    another reason. One function states the sequence (``_check_client_message``); before
    that it was restated in each encoder, where one arm could drift.
    """
    with pytest.raises(ValueError) as caught:
        enc(new_order(v=2, px=2**63, cl_id="C" * 65))
    assert str(caught.value) == "unsupported version: v=2"


@ENCODERS
def test_each_adjacent_pass_boundary_is_pinned(enc: Callable[[NewOrder], bytes]) -> None:
    """The composite case above only proves version-first; it stays green if the two passes
    BELOW it are reordered. Pin each adjacent boundary so any swap fails:
    type-only before domain, and domain before identifier.
    """
    # type-only (v) vs type-only (post_only): both are wire-type rules, version answers first.
    with pytest.raises(ValueError) as caught:
        enc(new_order(v=2, post_only=0))
    assert str(caught.value) == "unsupported version: v=2"

    # domain (px out of wire range) vs identifier (cl_id over its cap): domain answers first.
    with pytest.raises(ValueError) as caught:
        enc(new_order(px=2**63, cl_id="C" * 65))
    assert "px" in str(caught.value)


@ENCODERS
def test_encoders_accept_the_flags_other_value(enc: Callable[[NewOrder], bytes]) -> None:
    """The accept side of the boolean rule: ``False`` is a legal wire value, not merely
    the default's opposite — a checker written as a truthiness test would pass every case
    above and still refuse this one."""
    assert b'"post_only":false' in enc(new_order(post_only=False))


def test_every_client_member_has_an_encode_side_checker() -> None:
    """The three encode passes PARTITION a client message's members: integer domains,
    identifier caps, and the type-only table. Exactly one pass per member, and no member
    left over — a field added to a client message whose type none of them covers raises at
    IMPORT, the same discipline ``_identifier_rules`` applies to an uncapped string.
    """
    for cls in (NewOrder, CancelOrder):
        _, wire_types, domains, identifiers = protocol._CLIENT_RULES[cls]
        covered = [
            *(name for name, _, _ in domains),
            *(name for name, _ in identifiers),
            *(name for name, _ in wire_types),
        ]
        assert sorted(covered) == sorted(f.name for f in msgspec.structs.fields(cls))
        assert len(covered) == len(set(covered)), cls.__name__
    with pytest.raises(TypeError, match="no encode-side rule"):
        _rules._wire_type_checker(float)


def test_the_encode_rule_table_covers_the_client_messages_and_nothing_else() -> None:
    """``_CLIENT_RULES`` is read by ``_check_client_message`` alone, which is reached from
    the two encoders alone. The integer-domain half used to carry entries for the five
    ENGINE schemas as well — lookups no runtime path ever performed, and ones line coverage
    could not show as dead since the comprehension runs at import. The engine schemas'
    decode-side domain rule lives in ``_SPAN_RULES``, where it belongs: it answers on the
    raw token before any conversion.

    Asserted against the UNIONS and not against a hand-written class list, because that is
    what the module now derives them from: totality is a property of the two registries with
    respect to ``ClientMsg`` and ``EngineMsg``, and a test restating the members by name
    would go stale in exactly the round a variant is added — the round it exists to cover.
    """
    client, engine = set(get_args(protocol.ClientMsg)), set(get_args(protocol.EngineMsg))
    assert client == {NewOrder, CancelOrder}
    assert engine == {Tob, OrderAck, CancelAck, Reject, Fill}
    assert set(protocol._CLIENT_RULES) == set(protocol._WIRE_FIELDS) == client
    assert set(protocol._SPAN_RULES) == {_rules._tag_of(cls) for cls in engine}


def test_a_field_type_with_no_naive_arm_rule_stops_the_import() -> None:
    """The DECODE-side twin of the guard above, and deliberately NOT total over ``bool``:
    no engine -> client schema carries one, so a boolean checker there would be dead code
    no test could pin. A future boolean field must add its rule, and this raise is what
    forces that — the sibling raise in ``_identifier_rules`` is pinned just above, so the
    two halves of the module's import-time discipline are pinned alike."""
    with pytest.raises(TypeError, match="no naive-arm rule"):
        _rules._checker_for(bool)


def test_the_naive_arm_enforces_a_closed_literal_exactly_as_msgspec_does() -> None:
    """`status` is a closed set on the wire (`"live"` / `"cancelled"`), and the tuned arm gets
    that from msgspec for free. The naive arm has to be told, so this pins the checker the
    dispatch hands back — both of its refusals and its acceptance.

    It matters because the two arms are the A/B A/B: if one accepts a frame the other refuses,
    they are not decoding the same protocol, and a latency comparison between them is measuring
    two different things.
    """
    check = _rules._checker_for(Literal["live"])

    assert check("order_ack", "status", "live") == "live"

    # Wrong TYPE — the same verdict text every other field type produces.
    with pytest.raises(ValueError, match=re.escape("order_ack.status is not a string")):
        check("order_ack", "status", 1)

    # Right type, value outside the closed set — the case that let an `order_ack` claiming
    # `status="cancelled"` through, which the strategy then acted on as a live order.
    with pytest.raises(ValueError, match=re.escape("order_ack.status is not one of live")):
        check("order_ack", "status", "cancelled")


def test_a_non_string_literal_has_no_naive_arm_rule() -> None:
    """The dispatch accepts `Literal[...]` only when every member is a string. A numeric literal
    falls through to the same import-time raise as any other unhandled type, rather than being
    silently checked by a string rule that would reject every value it saw."""
    with pytest.raises(TypeError, match="no naive-arm rule"):
        _rules._checker_for(Literal[1, 2])
