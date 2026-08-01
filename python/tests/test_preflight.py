"""Frame-shape battery: byte policy, RFC 8259 grammar, root key policy, resource caps.

The seam with ``test_protocol.py`` is the one ``mmclient/_preflight.py`` draws against
``mmclient/protocol.py``, and the one ``frame_preflight.cpp`` draws against
``protocol.hpp``: everything here decides a frame's fate WITHOUT knowing which message it
is. What a MESSAGE must be — envelope, fields, integer domains, identifier shape, the
encode side — is next door.

Both arms are exercised throughout, because both run the same preflight: a guard that only
one arm applies is an acceptance differential waiting to be found by a peer. Left to
themselves the two libraries disagree with the wire contract in OPPOSITE directions —
``json.loads`` accepts a UTF-8 BOM, resolves duplicate keys last-wins, admits lone
surrogates and the ``NaN``/``Infinity`` extensions, while msgspec accepts escaped member
names, duplicate keys and unbounded nesting.
"""

import json
from collections.abc import Callable
from typing import Any

import pytest
from golden_support import read_fixture
from protocol_support import (
    TOB,
    arm,
    assert_one_wording,
    mutate,
    prepend,
    scan_verdict,
)

from mmclient import _preflight
from mmclient.protocol import Tob, decode, decode_naive

# --- frame grammar and byte policy ---------------------------------------------------


@arm
@pytest.mark.parametrize(
    "frame",
    [
        b"",
        b"{",
        b'{"t":"top_of_book"',
        b"[1]",
        b"1",
        b'"top_of_book"',
        b"null",
    ],
    ids=[
        "empty",
        "open-brace",
        "truncated",
        "array-root",
        "number-root",
        "string-root",
        "null-root",
    ],
)
def test_malformed_or_non_object_root_rejects(dec: Callable[[bytes], Any], frame: bytes) -> None:
    with pytest.raises(ValueError):
        dec(frame)


# The grammar verdicts the malformed battery above reaches only as "some ValueError".
# Each is a distinct branch of the scan of record with its own wording, and each is a
# shape a hand-written or truncated frame really produces.
GRAMMAR_REJECTIONS: list[tuple[bytes, str]] = [
    (b'{"t":}', "malformed: expected a value, found '}'"),
    (b'{"t":"top_of_book",}', "malformed: object member name must be a string"),
    (b"{[1]}", "malformed: object member name must be a string"),
    (b'{"t" "top_of_book"}', "malformed: expected ':'"),
    (b'{"t",1}', "malformed: expected ':'"),
    (b"[1]", "malformed: root is not an object"),
    (TOB + b"{}", "malformed: trailing data after the top-level value"),
]


@pytest.mark.parametrize(
    ("frame", "expected"),
    GRAMMAR_REJECTIONS,
    ids=[
        "value-missing",
        "trailing-comma",
        "array-as-member-name",
        "colon-missing",
        "comma-for-colon",
        "root-not-object",
        "trailing-data",
    ],
)
def test_grammar_rejection_wording(frame: bytes, expected: str) -> None:
    """One wording per grammar branch, through BOTH arms.

    Asserted rather than merely raised because a weakened grammar still rejects — just for
    the wrong reason. Rewriting the root-type or the trailing-data text left the whole
    suite green while neither string appeared anywhere in these tests, and the ``_expect``
    and ``members`` branches were reachable only as a bare raise.
    """
    assert_one_wording(frame, expected)


def test_leading_utf8_bom_rejects() -> None:
    """RFC 8259 §8.1: a frame is exact bytes. ``json.loads`` takes a BOM; the wire does not.

    The WORDING is asserted, not merely the raise: with only ``pytest.raises(ValueError)``
    the entire BOM guard could be deleted and this test still passed — the BOM bytes then
    fail the grammar scan at byte 0 instead, through a path this test does not claim to
    exercise. BOM rejection is one of the cross-language contract items the gate
    established, and it has a dedicated wording precisely so it can be pinned.
    """
    assert_one_wording(b"\xef\xbb\xbf" + TOB, "malformed: leading UTF-8 BOM")


def test_trailing_nul_rejects() -> None:
    """A NUL is valid UTF-8 and survives ``json.loads``'s tolerance; it is trailing data."""
    assert_one_wording(TOB + b"\x00", "malformed: trailing data after the top-level value")


@arm
def test_trailing_garbage_rejects(dec: Callable[[bytes], Any]) -> None:
    """A smuggled second frame must never be silently discarded."""
    with pytest.raises(ValueError):
        dec(TOB + b"{}")


@arm
def test_trailing_whitespace_is_accepted(dec: Callable[[bytes], Any]) -> None:
    assert dec(TOB + b" \t\r\n") == dec(TOB)


@arm
def test_raw_control_byte_in_string_rejects(dec: Callable[[bytes], Any]) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b"MOCKUSDT", b"MOCK\x01USDT"))


@arm
def test_invalid_utf8_rejects(dec: Callable[[bytes], Any]) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b"MOCKUSDT", b"MOCK\xffUSDT"))


@arm
def test_lone_escaped_surrogate_rejects(dec: Callable[[bytes], Any]) -> None:
    """``json.loads`` builds a lone-surrogate ``str`` that later UTF-8 encoding cannot carry."""
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"MOCKUSDT"', b'"\\ud800"'))


@arm
@pytest.mark.parametrize(
    ("escaped", "expected"),
    [
        (b'"\\ud800x\\udc00"', "malformed: unpaired high surrogate escape"),
        (b'"\\ud800\\u0041"', "malformed: unpaired high surrogate escape"),
        (b'"\\udc00"', "malformed: unpaired low surrogate escape"),
    ],
    ids=["high-then-separated-low", "high-then-non-low", "lone-low"],
)
def test_every_unpaired_surrogate_shape_rejects(
    dec: Callable[[bytes], Any], escaped: bytes, expected: str
) -> None:
    """The pairing rule has three failure shapes, not one: a high half followed by
    something that is not its low half (separated by a literal, or by a non-surrogate
    escape) and a low half with no high half before it. The lone TRAILING high half is
    covered above; these are the three the loop itself decides, and the low-half verdict
    has its own wording."""
    with pytest.raises(ValueError) as caught:
        dec(mutate(TOB, b'"MOCKUSDT"', escaped))
    assert str(caught.value) == expected


@arm
@pytest.mark.parametrize(
    ("escaped", "expected"),
    [
        (b'"\\ud800\\udc00"', "\U00010000"),
        (b'"\\udbff\\udfff"', "\U0010ffff"),
        (b'"\\ud83d\\ude00"', "\U0001f600"),
        (b'"\\ud7ff"', "퟿"),
        (b'"\\ue000"', ""),
    ],
    ids=["first-astral", "last-astral", "emoji", "below-high-surrogates", "above-low-surrogates"],
)
def test_paired_and_non_surrogate_escapes_are_accepted(
    dec: Callable[[bytes], Any], escaped: bytes, expected: str
) -> None:
    """The surrogate RANGES are pinned from the outside as well as the inside.

    ``_HIGH_SURROGATE``/``_LOW_SURROGATE`` could each be narrowed — to ``0xD800..0xDB00``
    and ``0xDC00..0xDF00`` — and every rejection test above would still pass, while
    U+10FFFF (spelled ``\\udbff\\udfff``) became an "unpaired surrogate": a client
    STRICTER than the engine, which accepts it. The two non-surrogate neighbours U+D7FF
    and U+E000 pin the ranges' other edges, where a widened range would start refusing
    ordinary characters.
    """
    msg = dec(mutate(TOB, b'"MOCKUSDT"', escaped))
    assert isinstance(msg, Tob)
    assert msg.symbol == expected


@arm
@pytest.mark.parametrize("token", [b"NaN", b"Infinity", b"-Infinity"])
def test_json_float_extensions_reject(dec: Callable[[bytes], Any], token: bytes) -> None:
    """``json.loads`` accepts these non-JSON extensions by default; the wire does not."""
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"seq":7', b'"seq":' + token))


@arm
@pytest.mark.parametrize("token", [b"NaN", b"Infinity", b"-Infinity"])
def test_json_float_extensions_reject_in_unknown_fields_too(
    dec: Callable[[bytes], Any], token: bytes
) -> None:
    """The case a field type-check cannot catch: unknown values are never type-checked,
    so only a grammar pass over the WHOLE frame keeps the naive arm from accepting an
    extension msgspec calls malformed."""
    with pytest.raises(ValueError):
        dec(mutate(TOB, b"{", b'{"zzz_unknown":' + token + b","))


@arm
@pytest.mark.parametrize("token", [b"07", b"+7", b"7.", b".7", b"0x7"])
def test_invalid_number_grammar_rejects(dec: Callable[[bytes], Any], token: bytes) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"seq":7', b'"seq":' + token))


@arm
def test_unpaired_surrogate_escape_in_an_unknown_value_rejects(
    dec: Callable[[bytes], Any],
) -> None:
    """The byte policy is frame-wide, not field-wide: msgspec rejects this on its own and
    stdlib json builds a lone-surrogate str out of it, so only a shared guard agrees."""
    frame = prepend(TOB, b'"zzz_unknown":"\\ud800"')
    with pytest.raises(ValueError, match="surrogate"):
        dec(frame)


# --- top-level key policy (root only, exactly like the engine) -----------------------


@arm
def test_duplicate_top_level_key_rejects(dec: Callable[[bytes], Any]) -> None:
    """No last-wins/first-wins ambiguity: msgspec resolves last-wins on its own, so the
    preflight is what keeps the two arms from disagreeing about which `seq` was sent."""
    frame = mutate(TOB, b'"seq":7', b'"seq":7,"seq":8')
    with pytest.raises(ValueError, match="duplicate"):
        dec(frame)


@arm
def test_escaped_top_level_member_name_rejects(dec: Callable[[bytes], Any]) -> None:
    """``"\\u0074"`` is NOT an alias for ``"t"`` — the S1 parser differential the C++ gate
    found, and it exists in Python too because both parsers hand their consumers keys
    that are ALREADY unescaped. Only a scan of the raw bytes can see it."""
    frame = mutate(TOB, b'"t":', b'"\\u0074":')
    with pytest.raises(ValueError, match="escape sequence"):
        dec(frame)


@arm
def test_nested_duplicate_and_escaped_keys_are_accepted(dec: Callable[[bytes], Any]) -> None:
    """The key policy is ROOT-only in the engine (codec.hpp: keys inside nested values
    get the grammar pass alone), so the client must not be stricter than the wire."""
    frame = prepend(TOB, b'"zzz_unknown":{"a":1,"a":2,"\\u0074":3}')
    assert dec(frame) == dec(TOB)


@arm
def test_top_level_key_cap(dec: Callable[[bytes], Any]) -> None:
    """detail::kMaxTopLevelKeys = 32: at the cap accepted, one past it Malformed."""
    fixture_keys = len(json.loads(TOB))
    padding = [b'"x%d":0' % k for k in range(32 - fixture_keys)]
    assert dec(prepend(TOB, b",".join(padding))) == dec(TOB)
    with pytest.raises(ValueError, match="too many top-level keys"):
        dec(prepend(TOB, b",".join([*padding, b'"x99":0'])))


@arm
@pytest.mark.parametrize("extra", [31, 32, 30_000], ids=["at-cap", "past-cap", "bomb"])
def test_nesting_depth_cap(dec: Callable[[bytes], Any], extra: int) -> None:
    """detail::kMaxNestingDepth = 32 open levels, the root object included. The 30k case
    is the stack bomb: rejected by policy at level 33, never recursed into (assignment
    the protocol rule) — and sized to fit INSIDE the 64 KiB frame cap, so it is the depth
    guard being
    tested here and not the byte cap that would otherwise refuse it one step earlier."""
    frame = prepend(TOB, b'"zzz_unknown":' + b"[" * extra + b"]" * extra)
    if extra < 32:
        assert dec(frame) == dec(TOB)
    else:
        with pytest.raises(ValueError, match="nesting depth"):
            dec(frame)


# --- resource bounds: the C++ side's, and no more -------------------------------------
#
# The depth and key caps above bound a frame's SHAPE and are `frame_preflight.cpp`'s own;
# the byte cap below is the transport's, restated at the codec. NOTHING else bounds a frame
# here, and the accept cases in this section are the pin on that.
#
# Until the Task-4 gate this module also carried a scan-token budget, a per-token byte
# charge and an escape cap, and every one of them REJECTED a grammar-valid frame the engine
# accepts. The three tests that used to live here pinned those rejections — i.e. they pinned
# the divergence. Each shape below carries nothing but an unknown member, which `codec.hpp`
# says is ignored for additive forward compatibility, so refusing one made this client's
# accept set a strict subset of the engine's in the single direction that breaks a future
# field: one the engine ignores and this client would have refused to receive at all.


def padded_frame(total: int) -> bytes:
    """The canonical frame grown to exactly ``total`` bytes with one unknown string."""
    empty = prepend(TOB, b'"zzz_unknown":""')
    return prepend(TOB, b'"zzz_unknown":"' + b"x" * (total - len(empty)) + b'"')


def escaped_padded_frame(total: int) -> bytes:
    """:func:`padded_frame` with ONE escape sequence in the padding."""
    empty = prepend(TOB, b'"zzz_unknown":"\\n"')
    return prepend(TOB, b'"zzz_unknown":"\\n' + b"x" * (total - len(empty)) + b'"')


def token_heavy(elements: int) -> bytes:
    """The canonical frame plus one unknown array: the most tokens per byte on offer."""
    return prepend(TOB, b'"zzz_unknown":[' + b"1," * elements + b"0]")


def sized_elements(count: int, width: int) -> bytes:
    """The canonical frame plus one unknown array of ``count`` ``width``-byte strings."""
    element = b'"' + b"x" * (width - 2) + b'"'
    return prepend(TOB, b'"zzz_unknown":[' + b",".join([element] * count) + b"]")


def escape_heavy(count: int) -> bytes:
    """The canonical frame plus one unknown string of ``count`` escape sequences."""
    return prepend(TOB, b'"zzz_unknown":"' + b"\\n" * count + b'"')


@arm
def test_frame_byte_cap(dec: Callable[[bytes], Any]) -> None:
    """The transport's 64 KiB cap, restated at the codec so any other caller inherits it.

    The one bound in this section that REJECTS, and the only one the C++ side also applies
    to a frame's size — through the transport (``read_message_max`` / picows
    ``max_frame_size``) rather than through ``frame_preflight``, which is why restating it
    at the codec is parity and not strictness: no frame larger than this reaches either
    peer's decoder in the first place.
    """
    assert len(padded_frame(_preflight._MAX_FRAME_BYTES)) == _preflight._MAX_FRAME_BYTES
    assert dec(padded_frame(_preflight._MAX_FRAME_BYTES)) == dec(TOB)
    with pytest.raises(ValueError, match="frame exceeds the 65536-byte cap"):
        dec(padded_frame(_preflight._MAX_FRAME_BYTES + 1))


@arm
@pytest.mark.parametrize(
    "frame",
    [
        token_heavy(489),
        token_heavy(5_000),
        sized_elements(400, 130),
        escape_heavy(1025),
        escape_heavy(10_000),
    ],
    ids=["489-tokens", "5000-tokens", "400-long-tokens", "1025-escapes", "10000-escapes"],
)
def test_an_expensive_but_grammar_valid_unknown_value_is_accepted(
    dec: Callable[[bytes], Any], frame: bytes
) -> None:
    """Parity, pinned at the exact shapes the removed budgets rejected.

    ``token_heavy(489)`` was one token past the scan budget, ``sized_elements(400, 130)``
    one byte-charge past it, and 1025 escapes one past the escape cap — the three frames the
    tests this replaces asserted were Malformed. Every one of them is grammar-valid, carries
    a single UNKNOWN member and is ACCEPTED by ``cpp/src/frame_preflight.cpp``, which has no
    token, length or escape bound at all; the wider counts beside them show the boundary was
    the budget's and not the grammar's. Accepting them is the whole point: unknown members
    are ignored for additive forward compatibility (``codec.hpp``), and a client that refuses
    to RECEIVE one the engine ignores cannot be forward-compatible whatever it then does
    with it. What this costs in pure-Python decode time is measured, not guessed
    (``bench/probes/py_codec.py``), and the byte cap above is what bounds it.
    """
    assert dec(frame) == dec(TOB)


@arm
def test_a_64_kib_single_escape_frame_stays_accepted(dec: Callable[[bytes], Any]) -> None:
    """The exact frame that first proved the amplification, kept as an accept pin.

    A 64 KiB frame whose single unknown string carries ONE escape cost ~1.9 ms in both arms
    — ~310x the canonical frame — because ``_TOKEN``'s string production stepped the regex
    VM once per character, and the one backslash was enough to force the frame off the
    flat-frame fast path onto that production. What fixed it is the POSSESSIVE-run form of
    that production (~8x), which is a cheaper way to accept the same language — not a budget,
    which would have been a narrower language. With the budgets gone the production is the
    only thing between a legal 64 KiB frame and a per-character regex walk, so its accept
    case is pinned here. ``test_frame_byte_cap`` pins the escape-FREE twin, which never
    leaves the fast path.
    """
    frame = escaped_padded_frame(_preflight._MAX_FRAME_BYTES)
    assert len(frame) == _preflight._MAX_FRAME_BYTES
    assert frame.count(b"\\") == 1
    assert dec(frame) == dec(TOB)


# --- preflight fast path vs the scan of record -----------------------------------------
#
# Every frame `mutate`/`prepend` build is pinned against `_scan_general` at construction
# (see `protocol_support.pin_scan_agreement`), which covers the mutations, the caps, the
# precedence permutations, the domain boundaries and anything a later task adds. What is
# left for a list are the frames those helpers cannot express: whole fixtures,
# concatenations and raw literals.

SCAN_CORPUS: list[bytes] = [
    TOB,
    b" \t" + TOB + b"\r\n",
    read_fixture("reject"),
    read_fixture("new_order"),
    TOB + b"\x00",  # trailing NUL
    TOB[:-1],  # truncated
    b"{}",
    b"",
    *(frame for frame, _ in GRAMMAR_REJECTIONS),
]


@pytest.mark.parametrize("frame", SCAN_CORPUS, ids=range(len(SCAN_CORPUS)))
def test_preflight_fast_path_never_disagrees_with_the_scan_of_record(frame: bytes) -> None:
    assert scan_verdict(_preflight._scan, frame) == scan_verdict(_preflight._scan_general, frame)


def test_the_canonical_frame_really_takes_the_fast_path(monkeypatch: Any) -> None:
    """Otherwise a regressed fast path would be invisible: the general scanner would
    quietly answer every frame, correctly and ~4x slower, and only the A/B numbers would
    know."""
    monkeypatch.setattr(_preflight, "_scan_general", lambda frame: pytest.fail("fast path missed"))
    assert decode(TOB).seq == 7


# --- frame entry type ------------------------------------------------------------------


@arm
@pytest.mark.parametrize("frame", [TOB.decode(), memoryview(TOB)], ids=["str", "memoryview"])
def test_both_arms_require_bytes(dec: Callable[[bytes], Any], frame: Any) -> None:
    """A ``str`` frame would silently bypass the BOM and UTF-8 byte policy, and a
    ``memoryview`` would slip past it into whichever library the arm wraps — msgspec
    accepts both on its own, so the guard has to live in front of both arms.

    The message is asserted because the guard's value IS the diagnosis: without it the
    input still fails, deeper in and with an unrelated message.
    """
    with pytest.raises(TypeError, match="frames are bytes"):
        dec(frame)


def test_bytearray_frames_are_accepted() -> None:
    """The transports hand over whatever the WS library produced; ``bytearray`` is not a
    contract violation, it is the same bytes."""
    assert decode(bytearray(TOB)) == decode(TOB)
    assert decode_naive(bytearray(TOB)) == decode_naive(TOB)
