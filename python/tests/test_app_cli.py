"""The CLI surface: what an operator can ask for, and what the process returns.

`app.py` is the only module in `mmclient` whose stdout IS its interface, and it is what the
demo drives. Its exit code carries the one distinction a supervisor needs — the
engine was reachable, or it was not — so the reconnect policy's arithmetic is pinned here
rather than left to the demo to discover.
"""

import asyncio

import pytest

from mmclient._session import Ending
from mmclient.app import build_parser, main, run_with_reconnect
from mmclient.strategy import Strategy


def _strategy() -> Strategy:
    return Strategy(symbol="MOCKUSDT", qty=5, max_qty=10, stale_ns=10**9)


def test_the_defaults_are_the_documented_ones() -> None:
    """This flag surface is published, and the benchmark harness reads it; a default that
    drifts silently changes what the demo and the benchmark actually ran."""
    args = build_parser().parse_args([])
    assert args.stack == "tuned"  # the measured arm is the default; naive is opt-in
    assert args.url == "ws://127.0.0.1:8765"
    assert (args.symbol, args.qty, args.max_qty, args.stale_ms) == ("MOCKUSDT", 100, 10_000, 500)
    assert args.quiet is False


def test_every_documented_flag_parses() -> None:
    args = build_parser().parse_args(
        [
            "--url",
            "ws://h:1",
            "--stack",
            "naive",
            "--symbol",
            "X",
            "--qty",
            "7",
            "--max-qty",
            "9",
            "--stale-ms",
            "250",
            "--quiet",
        ]
    )
    assert (args.url, args.stack, args.symbol) == ("ws://h:1", "naive", "X")
    assert (args.qty, args.max_qty, args.stale_ms, args.quiet) == (7, 9, 250, True)


def test_an_unknown_stack_is_refused() -> None:
    """`choices` rather than a free string: a typo must fail at the CLI, not silently pick
    an arm and report its numbers under the other one's name."""
    with pytest.raises(SystemExit):
        build_parser().parse_args(["--stack", "picows"])


def test_stale_ms_reaches_the_strategy_as_nanoseconds(monkeypatch: pytest.MonkeyPatch) -> None:
    """The operator thinks in the milliseconds the engine's --interval-ms uses; the strategy
    compares against perf_counter_ns. A missing conversion is a stale timer a million times
    too patient, which is exactly the failure the timer exists to prevent."""
    seen: dict[str, object] = {}

    async def fake_run(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        seen["stale_ns"] = strategy.stale_ns
        seen["symbol"] = strategy.symbol
        return Ending.STOPPED

    monkeypatch.setattr("mmclient.ws_naive.run_client", fake_run)
    rc = main(["--stack", "naive", "--stale-ms", "250", "--symbol", "ZZZ", "--quiet"])
    assert rc == 0
    assert seen == {"stale_ns": 250_000_000, "symbol": "ZZZ"}


def test_the_banner_is_suppressed_by_quiet(
    capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    """Via monkeypatch, which UNDOES itself. This case used to assign the module attribute
    directly, so `mmclient.ws_naive.run_client` stayed replaced by a stub for the whole
    session — every later test that touched the real naive client got the stub instead, and
    the first one to do so failed with a signature error from a test file it never imported.
    A test that mutates global state and does not restore it is a landmine for whatever runs
    next, and the sibling case above already had this right."""

    async def fake_run(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        return Ending.STOPPED

    monkeypatch.setattr("mmclient.ws_naive.run_client", fake_run)
    main(["--stack", "naive", "--quiet"])
    assert capsys.readouterr().out == ""
    main(["--stack", "naive"])
    assert "mm-client stack=naive" in capsys.readouterr().out


async def test_a_clean_run_exits_zero_without_retrying() -> None:
    attempts = 0

    async def ok(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        nonlocal attempts
        attempts += 1
        return Ending.STOPPED

    rc = await run_with_reconnect(ok, "ws://x", _strategy(), stop=asyncio.Event(), delay_s=0.01)
    assert (attempts, rc) == (1, 0)


async def test_a_stop_requested_mid_retry_does_not_retry_again() -> None:
    """An operator who asked to stop while the first attempt was failing gets a stop, not a
    patient client working through its retry budget."""
    attempts = 0
    stop = asyncio.Event()

    async def fails(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        nonlocal attempts
        attempts += 1
        stop.set()
        msg = "connection refused"
        raise OSError(msg)

    rc = await run_with_reconnect(fails, "ws://x", _strategy(), stop=stop, delay_s=0.01)
    assert (attempts, rc) == (1, 1)


def test_the_tuned_arm_selects_uvloop_and_picows(monkeypatch: pytest.MonkeyPatch) -> None:
    """The default arm, and the one every measured run uses. Its imports are DEFERRED so a
    naive-only install still works — which also means nothing else in the suite executes
    that branch, and a broken tuned selection would surface first in the benchmark."""
    chosen: dict[str, object] = {}

    async def fake_run(url: str, strategy: Strategy, *, stop: asyncio.Event) -> Ending:
        chosen["ran"] = True
        return Ending.STOPPED

    def fake_uvloop_run(coro: object) -> int:
        chosen["runner"] = "uvloop"
        return asyncio.run(coro)  # type: ignore[arg-type]

    import uvloop

    import mmclient.ws_picows

    monkeypatch.setattr(mmclient.ws_picows, "run_client", fake_run)
    monkeypatch.setattr(uvloop, "run", fake_uvloop_run)
    rc = main(["--stack", "tuned", "--quiet"])
    assert rc == 0
    assert chosen == {"runner": "uvloop", "ran": True}
