"""Envelope, field typing, forward compatibility and error PRECEDENCE, through both arms.

Contract source: ``cpp/include/mm/codec.hpp``. The C++ engine's two arms were forced into
byte-identical acceptance during the gate; the two PYTHON arms get the same
treatment here, because the naive->product swap is the measured hot-path optimization and an
optimization that also changes what the client accepts is not a like-for-like comparison.

the Python codec's message-level battery is three chapters, one file each, because a single
1038-line module is against the repo's own files-under-500-lines convention (the same
convention that split C++ files mid-review):

  * this file — which MESSAGE the frame is, and in what order its verdicts are given;
  * ``test_decode_fields.py`` — what each member may hold on the way IN;
  * ``test_encode.py`` — what this client may PRODUCE.

The frame-SHAPE half of the contract — byte policy, grammar, root key policy, resource
caps — is in ``test_preflight.py``, along the seam ``mmclient/_preflight.py`` draws.
"""

import json
import re
from collections.abc import Callable
from typing import Any

import msgspec
import pytest
from golden_support import read_fixture
from protocol_support import TOB, arm, assert_one_wording, mutate, prepend

# --- envelope errors -----------------------------------------------------------------


@arm
def test_unsupported_version_rejects(dec: Callable[[bytes], Any]) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"v":1', b'"v":2'))


def test_both_arms_report_an_unsupported_version_in_the_same_words() -> None:
    """``v`` is the one envelope member with its own REJECT CODE on the C++ side
    (``UNSUPPORTED_VERSION v=2``, not ``Malformed``), and this client says it in the same
    words on both the produce and the consume side."""
    assert_one_wording(mutate(TOB, b'"v":1', b'"v":2'), "unsupported version: v=2")


def test_a_version_outside_uint64_is_malformed_not_unsupported() -> None:
    """The classification both C++ arms give it, and the one the Python client used to
    get wrong.

    ``v`` is excluded from the span pass on the ground that the envelope check owns it —
    so the envelope check must own ALL of it, DOMAIN included. It did not: a 21-digit
    ``v`` was reported as ``unsupported version: v=99999999999999999999`` here while
    ``codec_glaze.cpp`` (``from_chars`` errc) and ``codec_nlohmann.cpp``
    (``is_number_unsigned``) both answer ``malformed: v missing or not an unsigned
    integer``. Same frame, different REJECT CODE across the boundary.
    """
    assert_one_wording(
        mutate(TOB, b'"v":1', b'"v":' + b"9" * 20),
        "malformed: v missing or not an unsigned integer",
    )


@arm
def test_missing_version_rejects(dec: Callable[[bytes], Any]) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"v":1,', b""))


@arm
def test_unknown_tag_rejects(dec: Callable[[bytes], Any]) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b"top_of_book", b"nope"))


@arm
def test_missing_tag_rejects(dec: Callable[[bytes], Any]) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"t":"top_of_book",', b""))


@arm
def test_non_string_tag_rejects(dec: Callable[[bytes], Any]) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"t":"top_of_book"', b'"t":5'))


@arm
def test_escaped_tag_spelling_is_the_same_tag(dec: Callable[[bytes], Any]) -> None:
    """The ACCEPT side of the root key policy's asymmetry, and the only caller of
    ``_decode_string_span``'s escaped branch.

    An escaped member NAME is rejected (``"\\u0074"`` is not an alias for ``"t"``), but an
    escaped tag VALUE is merely another spelling of the same string — and the engine reads
    it that way too: ``codec_nlohmann.cpp`` compares the library-UNESCAPED tag against
    ``kTagNewOrder`` and friends. Pinned so that a later "optimization" comparing the raw
    span bytes cannot silently make the client stricter than the engine.
    """
    assert dec(mutate(TOB, b'"top_of_book"', b'"\\u0074op_of_book"')) == dec(TOB)


@arm
@pytest.mark.parametrize(
    ("spelling", "reported"),
    [
        (b'"\\u006eope"', "nope"),
        (b'"\\u0001nope"', "?nope"),
        (b'"' + b"n" * 40 + b'"', "n" * 32 + "…"),
        (b'"' + b"n" * 32 + b'"', "n" * 32),
        (b'"' + b"n" * 33 + b'"', "n" * 32 + "…"),
        (b'"' + "é".encode() * 16 + b'"', "é" * 16),
        (b'"' + "é".encode() * 17 + b'"', "é" * 16 + "…"),
        (b'"' + "€".encode() * 11 + b'"', "€" * 10 + "…"),
    ],
    ids=[
        "escaped",
        "escaped-control-byte",
        "over-long",
        "at-32-bytes",
        "one-byte-past",
        "multibyte-at-32-bytes",
        "multibyte-past-the-cap",
        "cut-would-split-a-sequence",
    ],
)
def test_unknown_tag_is_reported_sanitized(
    dec: Callable[[bytes], Any], spelling: bytes, reported: str
) -> None:
    """An unknown tag is unescaped first and then SANITIZED for the error text, because
    that text feeds the outbound Reject reason and log lines: control bytes become ``?``
    (CWE-117 forgery) and the peer-sized tag is truncated at ``kMaxTagDetailBytes``
    (CWE-400 amplification). Mirrors ``detail::sanitized_tag``.

    The cap is a BYTE cap, and the last three rows are why that has to be pinned in the
    multibyte cases rather than inferred from the ASCII ones, where bytes and characters are
    the same number. Until the Task-4 gate this side truncated at 32 CHARACTERS: ``"é"*17``
    is 17 characters and 34 bytes, so it came back whole while ``detail::sanitized_tag`` cut
    it to 32 bytes — the reject wording diverged, and the log-amplification bound C++ states
    was not honoured here. ``"€"*11`` is the case a byte cap alone still gets wrong: the cut
    at byte 32 lands inside the eleventh sequence, so it walks back to 30 exactly as the C++
    ``(tag[take] & 0xC0) == 0x80`` loop does, rather than emitting a broken code point.
    """
    with pytest.raises(ValueError) as caught:
        dec(mutate(TOB, b'"top_of_book"', spelling))
    assert str(caught.value) == f"unknown type: {reported}"


@arm
def test_client_tag_is_not_an_engine_message(dec: Callable[[bytes], Any]) -> None:
    """The client decoder accepts engine->client tags only."""
    with pytest.raises(ValueError):
        dec(read_fixture("new_order"))
    with pytest.raises(ValueError):
        dec(read_fixture("cancel_order"))


# --- field typing --------------------------------------------------------------------


@arm
@pytest.mark.parametrize("token", [b'"7"', b"true", b"null", b"[]", b"{}"])
def test_wrong_typed_int_field_rejects(dec: Callable[[bytes], Any], token: bytes) -> None:
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"seq":7', b'"seq":' + token))


@arm
@pytest.mark.parametrize("token", [b"1e3", b"5.0", b"7.0", b"-0.0", b"7E0"])
def test_non_integral_token_for_int_field_rejects(
    dec: Callable[[bytes], Any], token: bytes
) -> None:
    """Integer-typed fields require an INTEGER token — magnitude never decides."""
    with pytest.raises(ValueError):
        dec(mutate(TOB, b'"seq":7', b'"seq":' + token))


# --- forward compatibility -----------------------------------------------------------


@arm
@pytest.mark.parametrize(
    "value",
    [b"123", b"1e309", b"0e0", b'"str"', b"true", b"null", b"[1,2]", b'{"a":1}'],
    ids=["int", "huge", "zero-exp", "string", "bool", "null", "array", "object"],
)
def test_unknown_key_is_ignored(dec: Callable[[bytes], Any], value: bytes) -> None:
    """Additive forward compatibility: unknown values are grammar-checked, never typed."""
    frame = mutate(TOB, b"{", b'{"zzz_unknown":' + value + b",")
    assert dec(frame) == dec(TOB)


@arm
def test_key_order_does_not_matter(dec: Callable[[bytes], Any]) -> None:
    """Canonical order is an ENCODE-side property; decode stays order-insensitive."""
    payload = json.loads(TOB)
    reordered = json.dumps(dict(reversed(list(payload.items()))), separators=(",", ":"))
    assert dec(reordered.encode()) == dec(TOB)


@arm
def test_huge_unknown_integer_is_ignored(dec: Callable[[bytes], Any]) -> None:
    """Magnitude never decides acceptance: a 5000-digit unknown integer is past CPython's
    int-conversion digit limit, so an arm that materialized it would reject a frame the
    engine accepts."""
    frame = prepend(TOB, b'"zzz_unknown":' + b"9" * 5000)
    assert dec(frame) == dec(TOB)


@arm
def test_huge_integer_in_a_known_field_rejects(dec: Callable[[bytes], Any]) -> None:
    """The other half of the rule: a 5000-digit value is fine where it is ignored and
    Malformed where it must fit a ``uint64_t`` — the contract decides, not the digit
    limit that happens to fire first.

    The MESSAGE is asserted, not merely the raise, because that is what separates the two.
    Before the domain verdict moved onto the raw span, this frame died differently in each
    arm and neither answer was the contract's: msgspec raised its own out-of-range
    ``ValidationError`` from inside the typed read, and the naive arm reported "is not an
    integer" — a misdiagnosis, since the token IS an integer, just not one CPython would
    convert.
    """
    out_of_domain = re.escape("top_of_book.seq outside its wire integer domain")
    with pytest.raises(ValueError, match=out_of_domain):
        dec(mutate(TOB, b'"seq":7', b'"seq":' + b"9" * 5000))


# --- error precedence (contract order: parse -> unknown type -> version -> fields) ----
#
# Precedence is a property of the shared preflight, not of whichever library happened to
# consume the frame first — so the POSITION of the offending token must not change the
# verdict. Each case is therefore run with the offender before AND after the `t`/`v`
# members it must outrank, through both arms.


@arm
@pytest.mark.parametrize("first", [True, False], ids=["offender-first", "offender-last"])
def test_parse_error_outranks_unknown_type(dec: Callable[[bytes], Any], first: bool) -> None:
    unknown_tag = mutate(TOB, b"top_of_book", b"nope")
    frame = (
        prepend(unknown_tag, b'"zzz_unknown":07')
        if first
        else mutate(unknown_tag, b'"ask_qty":80', b'"ask_qty":080')
    )
    with pytest.raises(ValueError) as caught:
        dec(frame)
    assert "malformed" in str(caught.value)
    assert "unknown type" not in str(caught.value)
    # A parse-class signal on the product arm too: msgspec's typed read must never have
    # been reached, so this can never be a ValidationError about `$.t`.
    assert not isinstance(caught.value, msgspec.ValidationError)


@arm
@pytest.mark.parametrize("first", [True, False], ids=["version-first", "version-last"])
def test_unknown_type_outranks_unsupported_version(
    dec: Callable[[bytes], Any], first: bool
) -> None:
    unknown_tag = mutate(TOB, b"top_of_book", b"nope")
    frame = (
        prepend(mutate(unknown_tag, b'"v":1,', b""), b'"v":2')
        if first
        else mutate(unknown_tag, b'"v":1', b'"v":2')
    )
    with pytest.raises(ValueError, match="unknown type"):
        dec(frame)


@arm
@pytest.mark.parametrize("first", [True, False], ids=["version-first", "version-last"])
def test_unsupported_version_outranks_field_errors(
    dec: Callable[[bytes], Any], first: bool
) -> None:
    broken_field = mutate(TOB, b',"ask_qty":80', b"")
    frame = (
        prepend(mutate(broken_field, b'"v":1,', b""), b'"v":2')
        if first
        else mutate(broken_field, b'"v":1', b'"v":2')
    )
    with pytest.raises(ValueError, match="unsupported version"):
        dec(frame)


@arm
def test_unsupported_version_outranks_a_field_error_that_precedes_it(
    dec: Callable[[bytes], Any],
) -> None:
    """The version wins even when the bad field is read first — precedence is the
    contract's, not the byte order's."""
    with pytest.raises(ValueError, match="unsupported version"):
        dec(mutate(TOB, b'"v":1,"seq":7', b'"seq":"x","v":2'))


@arm
def test_trailing_garbage_outranks_unsupported_version(dec: Callable[[bytes], Any]) -> None:
    """A smuggled second frame is a parse error however late it appears."""
    with pytest.raises(ValueError) as caught:
        dec(mutate(TOB, b'"v":1', b'"v":2') + b"{}")
    assert "malformed" in str(caught.value)
    assert "unsupported version" not in str(caught.value)
