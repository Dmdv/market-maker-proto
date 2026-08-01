"""Fixtures shared by the two strategy suites.

The suite is split along a behavioural seam — ``test_strategy.py`` holds the per-message
decisions (quoting, the never-cross belt, reports) and ``test_strategy_session.py`` holds
what spans a whole connection (the two counters, lifecycle, determinism) — because one file
carrying both went past the repository's 500-line convention. Same seam and same reason as
the codec and engine suites.
"""

from mmclient.protocol import CancelOrder, NewOrder, Tob
from mmclient.strategy import Cmd, SendCmd, Strategy

SYMBOL = "MOCKUSDT"


def strategy(*, qty: int = 5, max_qty: int = 10, stale_ns: int = 1_000_000_000) -> Strategy:
    s = Strategy(symbol=SYMBOL, qty=qty, max_qty=max_qty, stale_ns=stale_ns)
    s.on_connect(1)
    return s


def tob(seq: int, md_seq: int, bid_px: int = 100, ask_px: int = 110, *, epoch: int = 1) -> Tob:
    return Tob(
        v=1,
        seq=seq,
        epoch=epoch,
        md_seq=md_seq,
        symbol=SYMBOL,
        bid_px=bid_px,
        bid_qty=50,
        ask_px=ask_px,
        ask_qty=50,
    )


def sent(cmds: list[Cmd]) -> list[NewOrder | CancelOrder]:
    """The messages, in order. Fails loudly if a StopQuoting is hiding in the list."""
    out: list[NewOrder | CancelOrder] = []
    for cmd in cmds:
        assert isinstance(cmd, SendCmd), f"expected only sends, got {cmd!r}"
        out.append(cmd.msg)
    return out
