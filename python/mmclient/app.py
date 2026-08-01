"""The client entry point: pick a stack, run it, reconnect once, exit honestly.

`--stack {naive,tuned}` selects the A/B arm; both drive the same `Strategy` and `SessionDriver`.
Exactly ONE retry: a disconnect is a clean slate, but a LOOP would hide a genuinely dead engine.
"""

import argparse
import asyncio
import sys
from collections.abc import Callable, Coroutine
from typing import Any

from mmclient._session import Ending
from mmclient.strategy import Strategy

__all__ = ["build_parser", "main", "run_with_reconnect"]

RECONNECT_DELAY_S = 1.0
# One retry, then out — see the module docstring for why this is not a loop.
MAX_ATTEMPTS = 2
EXIT_DISCONNECTED = 1

RunClient = Callable[..., Coroutine[Any, Any, Ending]]


async def run_with_reconnect(
    run_client: RunClient,
    url: str,
    strategy: Strategy,
    *,
    stop: asyncio.Event,
    delay_s: float = RECONNECT_DELAY_S,
) -> int:
    """One connection, then at most one more. Returns the process exit code.

    Not a loop and not a backoff: a supervisor that cannot tell "engine is down" from "client
    is patient" is worse than one that exits.
    """
    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            ending = await run_client(url, strategy, stop=stop)
        # OSError ONLY — a transport fault, the blip this policy exists for. RuntimeError is a
        # DETERMINISTIC failure (a declined `mm.v1`, a callback bug); retrying it hides the bug.
        except OSError as exc:
            if attempt == MAX_ATTEMPTS or stop.is_set():
                print(f"mm-client: giving up after attempt {attempt}: {exc}", file=sys.stderr)
                return EXIT_DISCONNECTED
            print(f"mm-client: {exc}; one retry in {delay_s:g}s", file=sys.stderr)
            await asyncio.sleep(delay_s)
            continue
        # The RETURNED ENDING, not merely the absence of an exception: both adapters return
        # normally when the peer closes, so a disconnect would otherwise look like a clean stop.
        if ending is not Ending.PEER_GONE:
            return 0
        if attempt == MAX_ATTEMPTS or stop.is_set():
            print(f"mm-client: engine gone after attempt {attempt}", file=sys.stderr)
            return EXIT_DISCONNECTED
        print(f"mm-client: engine closed the session; one retry in {delay_s:g}s", file=sys.stderr)
        await asyncio.sleep(delay_s)
    # UNREACHABLE, and kept so every syntactic path returns: the loop's last iteration always
    # hits `attempt == MAX_ATTEMPTS` and returns from inside.
    return EXIT_DISCONNECTED  # pragma: no cover - the loop returns from inside every arm


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="mm-client", description="mock market-making client")
    p.add_argument("--url", default="ws://127.0.0.1:8765")
    p.add_argument("--stack", choices=("naive", "tuned"), default="tuned")
    p.add_argument("--symbol", default="MOCKUSDT")
    p.add_argument("--qty", type=int, default=100)
    p.add_argument("--max-qty", type=int, default=10_000)
    # Milliseconds on the CLI, nanoseconds inside: the operator thinks in the engine's
    # --interval-ms units, and the strategy compares against perf_counter_ns.
    p.add_argument("--stale-ms", type=int, default=500)
    p.add_argument("--quiet", action="store_true")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    strategy = Strategy(
        symbol=args.symbol,
        qty=args.qty,
        max_qty=args.max_qty,
        stale_ns=args.stale_ms * 1_000_000,
    )

    # Imported HERE, not at module scope: picows and uvloop are the `tuned` extra, so a
    # naive-only install must still be able to run the naive arm.
    if args.stack == "tuned":
        import uvloop

        from mmclient.ws_picows import run_client

        runner: Callable[[Coroutine[Any, Any, int]], int] = uvloop.run
    else:
        from mmclient.ws_naive import run_client

        runner = asyncio.run

    async def _go() -> int:
        stop = asyncio.Event()
        return await run_with_reconnect(run_client, args.url, strategy, stop=stop)

    if not args.quiet:
        print(f"mm-client stack={args.stack} url={args.url} symbol={args.symbol}")
    return runner(_go())


if __name__ == "__main__":  # pragma: no cover - process entry point
    sys.exit(main())
