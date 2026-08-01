"""Shared frame constructors and assertions for the two codec test modules.

The C++ analogue is ``cpp/tests/codec_test_support.hpp``: the helpers that build a mutated
frame and state a rejection live in ONE place, so the four chapters that use them can be
read separately without any of them restating them — ``test_preflight.py`` (what a frame
must LOOK like: byte policy, grammar, key policy, resource caps), ``test_protocol.py``
(which MESSAGE it is, and its verdict precedence), ``test_decode_fields.py`` (what each
member may hold on the way in) and ``test_encode.py`` (what this client may produce).

The frame CONSTRUCTORS carry a pin of their own — see :func:`pin_scan_agreement`.
"""

from collections.abc import Callable
from typing import Any, Literal

import msgspec
import pytest
from golden_support import read_fixture

from mmclient import _preflight
from mmclient.protocol import NewOrder, decode, decode_naive, encode, encode_naive

TOB = read_fixture("tob")

# Both arms are exercised through one parametrization; msgspec's DecodeError and
# ValidationError both derive from ValueError, so ValueError is the common contract.
ARMS: list[Callable[[bytes], Any]] = [decode, decode_naive]
arm = pytest.mark.parametrize("dec", ARMS, ids=["tuned", "naive"])

ENCODERS = pytest.mark.parametrize("enc", [encode, encode_naive], ids=["tuned", "naive"])


def scan_verdict(scan: Callable[[bytes], Any], frame: bytes) -> Any:
    """The scan's answer as a comparable value: its spans, or its rejection text."""
    try:
        return scan(frame)
    except ValueError as exc:
        return str(exc)


def pin_scan_agreement(frame: bytes) -> bytes:
    """Fast path vs the scan of record, on EVERY frame the helpers below build.

    ``_preflight._scan`` walks a flat, escape-free object with one regex per member and
    delegates everything else to ``_preflight._scan_general``, which is the authority (~4x
    the cost). An accelerator that accepted one frame the authority would reject — or
    produced one different span — would be an acceptance differential inside a single
    arm, so the two are pinned frame by frame.

    The pin lives in the CONSTRUCTOR rather than in a list of frames, because a list only
    covers what someone remembered to add to it: an earlier revision pinned 18 hand-picked
    frames while the number-grammar, NaN/Infinity, key-cap, integer-domain and precedence
    cases these modules also build went unpinned. Here, a case added by a later task is
    covered by writing it.
    """
    assert scan_verdict(_preflight._scan, frame) == scan_verdict(_preflight._scan_general, frame), (
        f"fast path and the scan of record disagree on {frame[:120]!r}"
    )
    return frame


def mutate(frame: bytes, old: bytes, new: bytes) -> bytes:
    assert frame.count(old) == 1, f"mutation anchor {old!r} is not unique"
    return pin_scan_agreement(frame.replace(old, new))


def prepend(frame: bytes, member: bytes) -> bytes:
    """Insert a member as the FIRST one, ahead of the fixture's `t` and `v`."""
    return mutate(frame, b"{", b"{" + member + b",")


def assert_one_wording(frame: bytes, expected: str) -> None:
    """Both arms reject ``frame`` — with the SAME text, and with the module's own.

    The C++ arms return one ``DecodeError.detail`` per rejection reason so that a log line
    cannot depend on which codec the benchmark flag picked (``codec.hpp``). This is where
    Python holds that line, and it is load-bearing beyond operability: it is the stated
    justification for the project-wide ``TRY003`` suppression in ``pyproject.toml``.
    """
    with pytest.raises(ValueError) as tuned:
        decode(frame)
    with pytest.raises(ValueError) as naive:
        decode_naive(frame)
    assert str(tuned.value) == str(naive.value) == expected


class UnpolicedString(msgspec.Struct, tag="unpoliced", tag_field="t"):
    """A schema whose string member is neither capped nor named as engine free text.

    Both totality guards are pinned against it — the client-side one in ``test_encode.py``
    and the engine-side one in ``test_decode_fields.py`` — because both raise on the same
    shape: a string member that no policy table covers.
    """

    v: Literal[1]
    note: str


def both_arms_decode[Msg](cls: type[Msg], frame: bytes) -> tuple[Msg, Msg]:
    """Both arms' decode of ``frame``, narrowed to the Struct the tag selects.

    ``decode`` returns the ``EngineMsg`` union, so a member read needs the narrowing — and
    asserting the concrete TYPE is part of the claim anyway: the two arms must agree on
    which Struct a frame is, not merely on accepting it.
    """
    tuned, naive = decode(frame), decode_naive(frame)
    assert isinstance(tuned, cls)
    assert isinstance(naive, cls)
    return tuned, naive


def new_order(**overrides: Any) -> NewOrder:
    fields: dict[str, Any] = {
        "v": 1,
        "seq": 1,
        "epoch": 1,
        "md_seq": 1,
        "cl_id": "C-1",
        "symbol": "MOCKUSDT",
        "side": "B",
        "px": 5,
        "qty": 10,
    }
    return NewOrder(**(fields | overrides))


__all__ = [
    "ARMS",
    "ENCODERS",
    "TOB",
    "UnpolicedString",
    "arm",
    "assert_one_wording",
    "both_arms_decode",
    "mutate",
    "new_order",
    "pin_scan_agreement",
    "prepend",
    "scan_verdict",
]
