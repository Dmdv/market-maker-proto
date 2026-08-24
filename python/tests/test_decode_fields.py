"""What each member may hold on the way IN: integer domains, the schema pass, string shape.

Chapter two of the message-level battery — see ``test_protocol.py`` for the split and the
contract source. Everything here is a verdict on ONE member of an already-identified
message, and almost all of it is asserted through ``assert_one_wording``: the C++ arms were
forced to return one ``DecodeError.detail`` per rejection reason so a log line cannot depend
on which codec the benchmark flag picked, and this is where Python holds that line.
"""

import json
import re
from collections.abc import Callable
from typing import Any, get_type_hints

import msgspec
import pytest
from golden_support import read_fixture
from protocol_support import (
    TOB,
    UnpolicedString,
    arm,
    assert_one_wording,
    both_arms_decode,
    mutate,
)

from mmclient import _rules, protocol
from mmclient.protocol import CancelAck, Fill, OrderAck, Reject, Tob

ORDER_ACK = read_fixture("order_ack")
REJECT = read_fixture("reject")

# --- integer domains (C++ member types are part of the contract) ---------------------


@arm
@pytest.mark.parametrize("value", [b"0", b"18446744073709551615"], ids=["min", "max"])
def test_uint64_field_accepts_its_domain(dec: Callable[[bytes], Any], value: bytes) -> None:
    assert dec(mutate(TOB, b'"seq":7', b'"seq":' + value)).seq == int(value)


@arm
@pytest.mark.parametrize("value", [b"-1", b"18446744073709551616"], ids=["below", "above"])
def test_uint64_field_rejects_outside_its_domain(dec: Callable[[bytes], Any], value: bytes) -> None:
    """`seq` is a ``std::uint64_t`` on the C++ side; Python's unbounded int is not."""
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"seq":7', b'"seq":' + value))


@arm
@pytest.mark.parametrize(
    "value", [b"-9223372036854775808", b"9223372036854775807"], ids=["min", "max"]
)
def test_int64_field_accepts_its_domain(dec: Callable[[bytes], Any], value: bytes) -> None:
    assert dec(mutate(TOB, b'"bid_px":500000', b'"bid_px":' + value)).bid_px == int(value)


@arm
@pytest.mark.parametrize(
    "value", [b"-9223372036854775809", b"9223372036854775808"], ids=["below", "above"]
)
def test_int64_field_rejects_outside_its_domain(dec: Callable[[bytes], Any], value: bytes) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"bid_px":500000', b'"bid_px":' + value))


@arm
@pytest.mark.parametrize("value", [b"-0", b"-1"], ids=["negative-zero", "negative"])
def test_unsigned_field_rejects_a_signed_token(dec: Callable[[bytes], Any], value: bytes) -> None:
    """A ``uint64_t`` member refuses the SIGN itself, not merely negative magnitudes.

    ``-0`` is the spelling that separates a token rule from a value rule: it is
    grammar-valid, its VALUE is a perfectly in-domain 0, and both Python libraries hand it
    over as ``0`` — so an arm that judged the parsed value would accept a frame
    ``cpp/tests/test_codec_equivalence.cpp`` pins as Malformed ("seq as -0 on an unsigned
    field"), where the verdict comes from the shared ``is_unsigned_integer_token``
    predicate in front of both C++ arms. The Python span pass is that predicate.
    """
    with pytest.raises(ValueError, match=re.escape("top_of_book.seq is not an unsigned integer")):
        dec(mutate(TOB, b'"seq":7', b'"seq":' + value))


@arm
def test_unsigned_field_accepts_plain_zero(dec: Callable[[bytes], Any]) -> None:
    """The accept side of the same rule: it is the sign that is refused, not the value."""
    assert dec(mutate(TOB, b'"seq":7', b'"seq":0')).seq == 0


@arm
def test_signed_field_accepts_a_signed_token(dec: Callable[[bytes], Any]) -> None:
    """``int64_t`` members take the wider predicate — the two are not interchangeable."""
    assert dec(mutate(TOB, b'"bid_px":500000', b'"bid_px":-0')).bid_px == 0
    assert dec(mutate(TOB, b'"bid_px":500000', b'"bid_px":-1')).bid_px == -1


@pytest.mark.parametrize(
    ("anchor", "value", "expected"),
    [
        (b'"seq":7', b"-1", "malformed: top_of_book.seq is not an unsigned integer"),
        (b'"seq":7', b"-0", "malformed: top_of_book.seq is not an unsigned integer"),
        (
            b'"seq":7',
            b"18446744073709551616",
            "malformed: top_of_book.seq outside its wire integer domain",
        ),
        (
            b'"seq":7',
            b"9" * 5000,
            "malformed: top_of_book.seq outside its wire integer domain",
        ),
        (
            b'"bid_px":500000',
            b"9223372036854775808",
            "malformed: top_of_book.bid_px outside its wire integer domain",
        ),
        (
            b'"bid_px":500000',
            b"-9223372036854775809",
            "malformed: top_of_book.bid_px outside its wire integer domain",
        ),
        (b'"bid_px":500000', b'"x"', "malformed: top_of_book.bid_px is not an integer"),
        (b'"bid_px":500000', b"1.5", "malformed: top_of_book.bid_px is not an integer"),
    ],
    ids=[
        "u64-negative",
        "u64-negative-zero",
        "u64-above",
        "u64-huge",
        "i64-above",
        "i64-below",
        "i64-string-token",
        "i64-fractional-token",
    ],
)
def test_both_arms_reject_a_bad_integer_token_in_the_same_words(
    anchor: bytes, value: bytes, expected: str
) -> None:
    """The C++ arms were forced to return one detail string per rejection reason, so a log
    line cannot depend on which codec the benchmark flag picked. The span pass is where
    Python holds that line FOR THE INTEGER CLASSES — which is why the Structs carry no
    ``msgspec.Meta`` bounds that would answer the same frame in msgspec's words, and why
    the verdict is taken before either library converts the token rather than after. The
    classes the span pass cannot reach are pinned by the tests below it.

    Every value here is one the two libraries disagree about when left alone: msgspec
    raises its own ``ValidationError`` for the out-of-range cases and silently accepts the
    signed ones; stdlib ``json`` accepts all of them but refuses to convert the 5000-digit
    token at all.

    Both NOUNS are pinned, and by both shapes. The span pass words its token verdict
    "is not an unsigned integer" for a ``uint64_t`` member and "is not an integer" for an
    ``int64_t`` one (``_rules._span_rules``); the last two rows are the signed half, which
    the rows above reach only with valid tokens that are out of RANGE, so without them the
    signed noun could be reworded ("a number") with the suite still green.
    """
    frame = mutate(TOB, anchor, anchor.split(b":")[0] + b":" + value)
    assert_one_wording(frame, expected)


# The classes the PREFLIGHT does not own: presence and string typing are settled by the
# schema pass, which msgspec would otherwise answer in its own words ("Object missing
# required field `seq`", "Expected `str`, got `int` - at `$.symbol`") while the naive arm
# answered in the module's. `decode` restates them through the same pass the naive arm
# runs, so the equality above holds for EVERY field-error class and not only the integer
# ones — which is what the project-wide TRY003 suppression in pyproject.toml rests on.


@pytest.mark.parametrize(
    "field",
    ["seq", "epoch", "md_seq", "symbol", "bid_px", "bid_qty", "ask_px", "ask_qty"],
)
def test_both_arms_report_a_missing_field_in_the_same_words(field: str) -> None:
    payload = json.loads(TOB)
    del payload[field]
    frame = json.dumps(payload, separators=(",", ":")).encode()
    assert_one_wording(frame, f"malformed: top_of_book: missing field {field!r}")


def test_a_multi_error_frame_is_reported_against_the_first_declared_field() -> None:
    """``_read_fields`` documents "in DECLARATION order", and nothing pinned it: every
    missing-field case above deletes exactly ONE field, so reversing the iteration left
    the suite green. Which field a multi-error frame is named against is contract text on
    an error path the two Python arms and the C++ engine are all supposed to word alike.

    ``symbol`` is declared fourth and ``ask_qty`` last, so declaration order names
    ``symbol`` while byte order and reverse order both name something else.
    """
    payload = json.loads(TOB)
    del payload["symbol"]
    del payload["ask_qty"]
    frame = json.dumps(payload, separators=(",", ":")).encode()
    assert_one_wording(frame, "malformed: top_of_book: missing field 'symbol'")


@pytest.mark.parametrize(
    "token", [b"5", b"true", b"null", b"[]", b"{}"], ids=["int", "bool", "null", "array", "object"]
)
def test_both_arms_report_a_wrong_typed_string_in_the_same_words(token: bytes) -> None:
    frame = mutate(TOB, b'"symbol":"MOCKUSDT"', b'"symbol":' + token)
    assert_one_wording(frame, "malformed: top_of_book.symbol is not a string")


# --- inbound identifier shape (the LAST step of the acceptance contract) ---------------
#
# The engine applies `detail::check_inbound_strings` to every inbound identifier; the
# client applies the same policy to what the engine sends it, so the two ends of the wire
# agree about what a string may be. Without it a peer could hand this client a 60 KB
# `cl_id` (which Tasks 8/9 will store, keyed, for the session) or a `reason` carrying a
# NUL and a CR/LF pair, straight into the client's logs — CWE-158/117/770, the class the
# C++ side names and refuses.


def test_the_engine_string_caps_accept_what_the_engine_can_send() -> None:
    """Accept side first: the caps are the engine's OWN, so every value it can legally
    produce still decodes. The five golden fixtures cover the ordinary case; these are the
    boundaries."""
    at_cap = mutate(ORDER_ACK, b'"cl_id":"C-1"', b'"cl_id":"' + b"C" * 64 + b'"')
    for ack in both_arms_decode(OrderAck, at_cap):
        assert ack.cl_id == "C" * 64
    symbol_at_cap = mutate(TOB, b'"MOCKUSDT"', b'"' + b"S" * 32 + b'"')
    for tob in both_arms_decode(Tob, symbol_at_cap):
        assert tob.symbol == "S" * 32


@pytest.mark.parametrize(
    ("frame", "expected"),
    [
        (
            mutate(ORDER_ACK, b'"cl_id":"C-1"', b'"cl_id":"' + b"C" * 65 + b'"'),
            "malformed: order_ack.cl_id exceeds the length cap",
        ),
        (
            mutate(TOB, b'"MOCKUSDT"', b'"' + b"S" * 33 + b'"'),
            "malformed: top_of_book.symbol exceeds the length cap",
        ),
        (
            mutate(TOB, b'"MOCKUSDT"', b'"' + "é".encode() * 17 + b'"'),
            "malformed: top_of_book.symbol exceeds the length cap",
        ),
    ],
    ids=["cl_id", "symbol", "symbol-utf8-bytes"],
)
def test_an_over_long_engine_identifier_rejects(frame: bytes, expected: str) -> None:
    """One byte past the engine's own cap, in the engine's own wording. The third row is
    the BYTE-cap half: 17 two-byte code points are 34 bytes and do not fit a 32-byte
    ``symbol``, however short Python calls the ``str``."""
    assert_one_wording(frame, expected)


def test_engine_identifier_caps_count_bytes_not_code_points() -> None:
    """The accept side of the same rule: 16 two-byte code points are exactly 32 bytes."""
    frame = mutate(TOB, b'"MOCKUSDT"', b'"' + "é".encode() * 16 + b'"')
    for tob in both_arms_decode(Tob, frame):
        assert tob.symbol == "é" * 16


@pytest.mark.parametrize(
    ("frame", "expected"),
    [
        (
            mutate(ORDER_ACK, b'"cl_id":"C-1"', b'"cl_id":"C\\u0000-1"'),
            "malformed: control byte in order_ack.cl_id",
        ),
        (
            mutate(TOB, b'"MOCKUSDT"', b'"MOCK\\nUSDT"'),
            "malformed: control byte in top_of_book.symbol",
        ),
        (
            mutate(REJECT, b'"TICK_SIZE"', b'"OK\\u000aFAKE"'),
            "malformed: control byte in reject.code",
        ),
        (
            mutate(REJECT, b'"px not a multiple of tick_size"', b'"tail\\u007f"'),
            "malformed: control byte in reject.reason",
        ),
    ],
    ids=["cl_id-nul", "symbol-lf", "code-escaped-lf", "reason-del"],
)
def test_an_escaped_control_byte_in_an_engine_string_rejects(frame: bytes, expected: str) -> None:
    """The verdict the GRAMMAR cannot give: raw control bytes are refused by the string
    production, but ``"\\u0000"`` and ``"\\n"`` are legal JSON spellings of the same
    bytes, and both libraries hand the decoded NUL/LF straight through. A NUL truncates
    C-string sinks (two ``cl_id``s collide) and CR/LF forges log records — CWE-158/117 —
    which is exactly why the engine refuses them inbound.
    """
    assert_one_wording(frame, expected)


def test_an_identifier_breaking_both_rules_is_named_by_the_engines_own_order() -> None:
    """Which of the two rules speaks first is contract text, and nothing pinned it: every
    case above breaks exactly ONE of them, so swapping the cap block and the control-byte
    block inside ``_check_engine_strings`` left the whole suite green. Its encode-side
    sibling had the identical gap, closed in the same round by
    ``test_encode.py::test_an_encoded_identifier_breaking_both_rules_names_the_cap_first``
    — that file's ``test_the_version_verdict_outranks_every_field_error`` pins the order
    of the three encode PASSES, not the two rules inside one. The engine checks the cap
    first (``detail::check_identifier`` in ``cpp/src/frame_preflight.cpp``) and the module
    states that order as its contract, so a value that is both over-long and
    control-carrying must come back named against the cap.
    """
    over_long_and_control = b'"cl_id":"' + b"C" * 64 + b'\\u0000"'
    frame = mutate(ORDER_ACK, b'"cl_id":"C-1"', over_long_and_control)
    assert_one_wording(frame, "malformed: order_ack.cl_id exceeds the length cap")


def test_engine_free_text_is_bounded_by_the_frame_and_not_by_a_client_side_cap() -> None:
    """A DECLARED asymmetry, not an oversight: the engine bounds ``code``/``reason``/
    ``status`` on NEITHER side, so a client-side cap would make this client stricter than
    the wire and would silently drop a long ``Reject`` reason a later engine version
    writes. They get the control-byte rule and the 64 KiB frame bound, nothing narrower.
    """
    long_reason = b"x" * 4000
    frame = mutate(REJECT, b'"px not a multiple of tick_size"', b'"' + long_reason + b'"')
    for reject in both_arms_decode(Reject, frame):
        assert reject.reason == "x" * 4000


def test_every_engine_string_member_has_an_inbound_policy() -> None:
    """The decode-side table is derived from the Structs and is TOTAL, exactly like its
    encode-side twin: a string member added to an engine schema that is neither in
    ``_IDENTIFIER_CAPS`` nor named engine free text raises at IMPORT rather than being
    accepted from a peer under no policy at all."""
    for cls in (Tob, OrderAck, CancelAck, Reject, Fill):
        rules = protocol._ENGINE_STRINGS[_rules._tag_of(cls)]
        hints = get_type_hints(cls, include_extras=True)
        assert {name for name, _ in rules} == {
            f.name for f in msgspec.structs.fields(cls) if hints[f.name] is str
        }
    with pytest.raises(TypeError, match="no inbound string policy"):
        _rules._engine_string_rules(UnpolicedString)
