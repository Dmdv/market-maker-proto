"""The client entry point: pick a stack, run it, reconnect once, exit honestly.

`--stack {naive,tuned}` selects which arm of the §6 measurement runs. Both drive the same
`Strategy` through the same `SessionDriver`; the flag changes the socket library, the codec
and the event loop, and nothing else.

RECONNECT POLICY, and why it is exactly one attempt. A disconnect means the engine retired
our epoch and cancelled every order we had resting (cancel-on-disconnect), so there is no
state to reconcile — a reconnect is a clean slate by construction, which is why `on_connect`
wipes the local picture rather than trying to re-establish it. One retry covers the case the
policy is for: a transport blip. A retry LOOP would turn a genuinely dead engine into a
client that looks alive forever, so the second failure exits non-zero and lets whatever
supervises the process decide.
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

    The retry is not a loop and not a backoff: this client is a measurement harness and a
    demo, and a supervisor that cannot tell "engine is down" from "client is patient" is
    worse than one that exits.
    """
    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            ending = await run_client(url, strategy, stop=stop)
        # OSError ONLY — a transport fault, which is the blip this policy exists for. RuntimeError
        # used to be caught here too, and that is how a DETERMINISTIC failure got retried: a
        # server that declines `mm.v1` raises RuntimeError from the handshake check, and dialling
        # it a second time asks the same server the same question and gets the same answer, one
        # second later. A programming error inside a callback raises RuntimeError as well, and
        # retrying that hid the bug behind a second run instead of surfacing it.
        except OSError as exc:
            if attempt == MAX_ATTEMPTS or stop.is_set():
                print(f"mm-client: giving up after attempt {attempt}: {exc}", file=sys.stderr)
                return EXIT_DISCONNECTED
            print(f"mm-client: {exc}; one retry in {delay_s:g}s", file=sys.stderr)
            await asyncio.sleep(delay_s)
            continue
        # The RETURNED ENDING, not merely the absence of an exception. Both adapters return
        # normally when the peer closes — that is what a close handshake IS — so this policy
        # was previously unreachable: a disconnect looked exactly like a clean stop and the
        # client exited 0 on an engine that had gone away. That is the failure the retry exists
        # for, and it was the one case that could not trigger it.
        if ending is not Ending.PEER_GONE:
            return 0
        if attempt == MAX_ATTEMPTS or stop.is_set():
            print(f"mm-client: engine gone after attempt {attempt}", file=sys.stderr)
            return EXIT_DISCONNECTED
        print(f"mm-client: engine closed the session; one retry in {delay_s:g}s", file=sys.stderr)
        await asyncio.sleep(delay_s)
    # UNREACHABLE, and kept so every syntactic path returns: the loop's last iteration always
    # hits `attempt == MAX_ATTEMPTS` and returns from inside. The pragma was here before this
    # function was rewritten for `Ending` and I dropped it in the rewrite, which is what took
    # the module off 100% — a coverage floor is only a floor if a genuine gap and a genuine
    # impossibility are labelled differently.
    return EXIT_DISCONNECTED  # pragma: no cover - the loop returns from inside every arm


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="mm-client", description="mock market-making client")
    p.add_argument("--url", default="ws://127.0.0.1:8765")
    p.add_argument("--stack", choices=("naive", "tuned"), default="tuned")
    p.add_argument("--symbol", default="MOCKUSDT")
    p.add_argument("--qty", type=int, default=100)
    p.add_argument("--max-qty", type=int, default=10_000)
    # Milliseconds on the CLI, nanoseconds inside: the operator thinks in the units the
    # engine's --interval-ms uses, and the strategy compares against perf_counter_ns.
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
