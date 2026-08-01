"""Shared fixture table for the cross-language golden tests.

Mirrors the C++ side's ``cpp/tests/codec_test_support.hpp``: one place that knows where
``tests/golden/`` lives, how a fixture file is read, and what each fixture's decoded
value is. Both Python test modules import it, so a fixture value is stated exactly once
on this side of the boundary — the same discipline the C++ suite applies on its side.
"""

from pathlib import Path
from typing import Final

from mmclient.protocol import (
    CancelAck,
    CancelOrder,
    ClientMsg,
    EngineMsg,
    Fill,
    NewOrder,
    OrderAck,
    Reject,
    Tob,
)

# python/tests/golden_support.py -> python/tests -> python -> repo root
GOLDEN_DIR: Final = Path(__file__).resolve().parents[2] / "tests" / "golden"


def read_fixture(name: str) -> bytes:
    """Fixture bytes with surrounding whitespace stripped.

    Fixtures are single-line JSON with a trailing newline; the byte comparisons pin the
    canonical wire form, which carries no whitespace at all (C++ ``trim_ws``).
    """
    return (GOLDEN_DIR / f"{name}.json").read_bytes().strip()


# Engine -> client. These are what the Python client DECODES, so every one is exercised
# through both arms.
ENGINE_FIXTURES: Final[dict[str, EngineMsg]] = {
    "tob": Tob(
        v=1,
        seq=7,
        epoch=2,
        md_seq=41,
        symbol="MOCKUSDT",
        bid_px=500000,
        bid_qty=100,
        ask_px=500010,
        ask_qty=80,
    ),
    "order_ack": OrderAck(
        v=1, seq=8, epoch=2, cl_id="C-1", eng_id=1001, status="live", svc_ns=18500
    ),
    "cancel_ack": CancelAck(v=1, seq=9, epoch=2, cl_id="C-1", eng_id=1001, status="cancelled"),
    "reject": Reject(
        v=1,
        seq=10,
        epoch=2,
        cl_id="",
        code="TICK_SIZE",
        reason="px not a multiple of tick_size",
    ),
    "fill": Fill(
        v=1,
        seq=11,
        epoch=2,
        cl_id="C-1",
        eng_id=1001,
        px=499995,
        qty=100,
        leaves=0,
        exec_id=501,
    ),
}

# Client -> engine. These are what the Python client ENCODES; the byte-identity test is
# the one that actually pins C++/Python wire compatibility in this direction.
CLIENT_FIXTURES: Final[dict[str, ClientMsg]] = {
    "new_order": NewOrder(
        v=1,
        seq=3,
        epoch=2,
        md_seq=41,
        cl_id="C-1",
        symbol="MOCKUSDT",
        side="B",
        px=499995,
        qty=100,
        post_only=True,
    ),
    "cancel_order": CancelOrder(v=1, seq=4, epoch=2, cl_id="C-1"),
}

ALL_FIXTURES: Final[dict[str, EngineMsg | ClientMsg]] = {
    **ENGINE_FIXTURES,
    **CLIENT_FIXTURES,
}
