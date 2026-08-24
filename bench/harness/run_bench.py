"""Drives one benchmark run and writes its raw samples.

THE PROBE IS STRATEGY-SHAPED, and that is the central design decision here. Both shipped arms
expose `run_client(url, strategy, ...)` and only ever call four methods on that object —
`on_connect`, `on_disconnect`, `on_timer`, `on_report`. `_Probe` implements exactly those, so a
benchmark run goes through the SAME session loop, the SAME envelope stamping and the SAME codec
as a production run, with only the decision logic swapped. The alternative — a transport loop
living in the harness — would be a second copy of the thing whose difference §6 is measuring,
and two hand-copied loops diverge. That is the argument `_session.py` was built on, and it
applies with more force to the code that produces the numbers.

WHAT THE THREE MODES MEASURE, and why they are not interchangeable:

* **idle (M1)** — one command in flight, alternating far-from-touch post-only NewOrder and
  CancelOrder against a deterministic `cl_id` sequence. Nothing ever fills, so every command
  gets exactly one report and the cycle is a clean request/response. This is the client-observable
  round trip with no queueing anywhere.
* **paced** — commands are DUE at `t_i = t_start + i/rate`, whether or not the client manages to
  send them then. Both series are recorded (see `summarize.series`) so coordinated omission is
  visible rather than argued about, plus `send_lag` as the queue proxy.
* **react (M3)** — on every inbound book, cancel the previous probe order and send one echoing
  that book's `md_seq`. This is the ONLY mode whose engine-side `BenchRecorder` streams mean
  anything: idle echoes one stale `md_seq` forever and paced measures its own pacing phase.

THE MESSAGE MIX NEVER TRIPS `max_live_orders{2}` (audit F-07). Every mode keeps at most two
commands outstanding by construction, and a mode that would need a third skips the slot and
counts it as lag instead. A single reject disqualifies a run from the primary tables, so the
pattern has to be safe by design rather than safe in practice.

Prices sit far from the touch and every order is post-only, so nothing can fill: a fill would
change the outstanding count mid-run and make the mix non-deterministic.
"""

import argparse
import asyncio
import json
import os
import sys
import time
from array import array
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from harness import manifest
from harness.summarize import gaps_over, series
from mmclient.protocol import CancelOrder, NewOrder
from mmclient.strategy import SendCmd

__all__ = ["build_parser", "main", "pacing_tick_s"]

# Far from any touch demo.feed or bench_paced.feed produces, and post-only, so a probe order can
# never fill. A fill would retire an order the probe still believes is live and change the
# outstanding count mid-run.
#
# BOTH VALUES MUST SATISFY THE INSTRUMENT'S GRID (`mm::types.hpp`): px is a multiple of
# `tick_size` (5) and qty a multiple of `lot_size` (10). A first real run used qty=1 and every
# order came back `reject LOT_SIZE`, which — before the reject handling below existed — simply
# hung: the probe waited for an ack that was never coming.
_BID_PX = 1_000  # 200 ticks; far below any bid the scenarios produce
_QTY = 10  # exactly one lot
_SYMBOL = "MOCKUSDT"

# PACING RESOLUTION, derived from the rate rather than fixed. The probe can only send when it is
# woken, so the wake interval is a floor on how precisely it can hit a slot — and a fixed 0.5 ms
# tick put ~0.4 ms of the harness's own granularity into `send_lag` at 1 kHz, which is lag the
# system never caused. One tenth of a period keeps that contribution to ~5% of the interval.
#
# Bounded below at 20 us because the wake itself costs something: past that the loop spends more
# time waking than working, and the jitter it is trying to remove comes back as scheduler noise.
# LAG BELOW THIS RESOLUTION IS NOT A MEASUREMENT — BENCHMARK.md must say so.
_PACING_TICK_FRACTION = 0.1
_PACING_TICK_FLOOR_S = 0.000_02
_PACING_TICK_CEILING_S = 0.001


def pacing_tick_s(rate_hz: float) -> float:
    """The wake interval for a paced run at `rate_hz`, and the floor on its lag resolution."""
    if rate_hz <= 0:
        return _PACING_TICK_CEILING_S
    period = 1.0 / rate_hz
    return min(_PACING_TICK_CEILING_S, max(_PACING_TICK_FLOOR_S, period * _PACING_TICK_FRACTION))


@dataclass
class _Probe:
    """A strategy-shaped benchmark probe. Records three timestamps per completed command.

    Holds no socket and no library: like the strategy it stands in for, it is handed `now_ns`
    and returns commands, which is what lets one probe drive both arms unchanged.

    THE SAMPLE BUFFERS ARE PREALLOCATED, and the recording path only assigns into them. The
    design record is explicit — "samples land in a preallocated array — zero allocation on the hot
    path, since the harness must not perturb what it measures" (03-system-design-spec.md §4.4) —
    and this originally violated it twice per ACK: a growable `list.append` plus a fresh dataclass
    instance. Both sit inside the exact window being timed, so the allocator and the GC were
    contributing to the tail this harness exists to characterise, at 110k samples per run.

    Three parallel `array("q")` rather than one array of records, because that is what the
    analysis wants and what the `.i64` files already are: writing a run out becomes a `tobytes()`
    of a buffer that was filled in place.
    """

    mode: str
    total: int  # warm-up + measured; the run stops here
    rate_hz: float
    # Sized in __post_init__ from `total`; a default_factory cannot see another field's value.
    intended_ns: array[int] = field(default_factory=lambda: array("q"))
    sent_ns: array[int] = field(default_factory=lambda: array("q"))
    done_ns: array[int] = field(default_factory=lambda: array("q"))
    count: int = 0
    rejects: int = 0
    reject_reason: str = ""
    epoch: int = 0

    _next_id: int = 0
    _pending_id: str | None = None
    _pending_intended_ns: int = 0
    _pending_sent_ns: int = 0
    _cancel_phase: bool = False
    # The cl_id of the order currently RESTING on the engine, in react mode. A far-from-touch
    # post-only order never fills, so only an explicit cancel removes it.
    _live_id: str | None = None
    _start_ns: int = 0
    _issued: int = 0
    _md_seq: int = 0
    _done: asyncio.Event = field(default_factory=asyncio.Event)

    def __post_init__(self) -> None:
        if self.total <= 0:
            msg = f"a run needs at least one cycle, got total={self.total}"
            raise ValueError(msg)
        # `bytes(8 * n)` is n zeroed int64s: one allocation per series, before the clock starts.
        for name in ("intended_ns", "sent_ns", "done_ns"):
            if not getattr(self, name):
                setattr(self, name, array("q", bytes(8 * self.total)))

    # --- the strategy-shaped surface -----------------------------------------------------

    def on_connect(self, epoch: int) -> list[Any]:
        """A fresh epoch is a fresh run: the engine cancelled anything the last one left."""
        self.epoch = epoch
        self._pending_id = None
        self._cancel_phase = False
        self._start_ns = time.perf_counter_ns()
        return []

    def on_disconnect(self) -> list[Any]:
        self._pending_id = None
        self._done.set()
        return []

    def on_timer(self, now_ns: int) -> list[Any]:
        """Drives idle and paced. React is driven by inbound books instead, so it does nothing
        here — a timer-driven send in react mode would add a second, unpaced producer.

        NOTHING IS ISSUED BEFORE AN EPOCH EXISTS. `SessionDriver` adopts the engine's epoch from
        the FIRST inbound message and stamps every outbound command with it, so a command
        emitted before that message trips the driver's own assertion — measured on the first
        real run of this harness, as an `AssertionError` with no message and zero samples. The
        timer fires as soon as the socket is up, which is strictly before the first book.
        """
        if self.epoch == 0 or self.finished or self.mode == "react":
            return []
        if self.mode == "paced":
            return self._paced(now_ns)
        return [] if self._pending_id is not None else self._issue(now_ns, intended_ns=now_ns)

    def on_report(self, msg: Any, now_ns: int) -> list[Any]:
        """One inbound message, routed by kind.

        Split into three small handlers rather than one branch tree: each kind ends the cycle
        differently — a reject abandons it, a book may start one, an ack closes one — and the
        routing is the only thing they share.
        """
        kind = type(msg).__name__
        if kind == "Reject":
            return self._on_reject(msg)
        if kind == "Tob":
            return self._on_book(msg)
        return self._on_ack(msg, kind)

    def _on_reject(self, msg: Any) -> list[Any]:
        """A reject already disqualifies this run from the primary tables (F-07), so there is
        nothing to gain by continuing — and the outstanding slot must be RELEASED either way.
        Left set, the probe waited for an ack that was never coming and the run died on its
        wall-clock timeout with no diagnosis; measured, on the first real run."""
        self.rejects += 1
        self.reject_reason = f"{getattr(msg, 'code', '?')}: {getattr(msg, 'reason', '')}"
        self._pending_id = None
        self._done.set()
        return []

    def _on_book(self, msg: Any) -> list[Any]:
        """A book. React reacts to it; the other modes only note its `md_seq` so their orders
        carry a plausible one."""
        self._md_seq = int(msg.md_seq)
        return self._react(time.perf_counter_ns()) if self.mode == "react" else []

    def _on_ack(self, msg: Any, kind: str) -> list[Any]:
        """The ack of the command being timed closes its cycle.

        The cycle is closed with a stamp taken HERE, which is after decode — so the recorded
        window is the full client-observable one (decide -> encode -> send -> engine -> receive
        -> decode -> observe) rather than one that quietly excludes the decode the tuned arm
        exists to make faster.
        """
        cl_id = getattr(msg, "cl_id", None)
        if cl_id is None or cl_id != self._pending_id:
            return []  # a report for something we are not timing
        done_ns = time.perf_counter_ns()
        # THREE INDEXED STORES INTO PREALLOCATED BUFFERS — no append, no object construction. This
        # is inside the timed window, so anything that allocates here is measured as latency.
        i = self.count
        self.intended_ns[i] = self._pending_intended_ns
        self.sent_ns[i] = self._pending_sent_ns
        self.done_ns[i] = done_ns
        self.count = i + 1
        # An acked NewOrder is now RESTING and must be cancelled before the next one, or the
        # engine's live set grows a slot per book. A CancelAck retires one instead.
        if kind == "OrderAck":
            self._live_id = cl_id
        self._pending_id = None
        if self.finished:
            self._done.set()
            return []
        # STRICTLY PAIRED new -> cancel, in BOTH idle and paced (F-07). The phase flips on every
        # completed cycle, so an accepted order is always retired by the next command and the
        # engine's live set never exceeds one. Toggled only for idle at first, which left paced
        # sending news forever — every accepted far-from-touch order RESTS (it cannot fill), so
        # the third one was refused `MAX_LIVE_ORDERS` and the run died after two cycles.
        # React does its own pairing in `_react` and does not use this flag.
        if self.mode in ("idle", "paced"):
            self._cancel_phase = not self._cancel_phase
        if self.mode == "idle":
            return self._issue(done_ns, intended_ns=done_ns)
        # PACED ISSUES HERE TOO when its next slot is already due, instead of waiting for the
        # next timer tick. The schedule says when a send is DUE; the ack is the earliest moment
        # one is ALLOWED (never more than one outstanding); so the send belongs at the later of
        # the two. Left to the timer alone, the rate was capped by TICK GRANULARITY rather than
        # by the transport — a 1 kHz run measured 28 ms of accumulating lag against a round trip
        # of only ~110 us, and a harness that reports its own pacing granularity as the system's
        # queueing delay is worse than no harness.
        if self.mode == "paced":
            return self._paced(done_ns)
        return []

    # --- helpers -------------------------------------------------------------------------

    @property
    def finished(self) -> bool:
        return self.count >= self.total

    async def wait(self, timeout: float) -> bool:
        """Wait for the run to stop, and report whether it actually FINISHED.

        `_done` means "stop waiting", which a disconnect also means — so returning `True`
        because the event fired reported a session that died after zero cycles as a completed
        run, and the harness then wrote three empty sample files and a manifest describing them.
        The success condition is `finished`, and it is checked rather than inferred.
        """
        try:
            await asyncio.wait_for(self._done.wait(), timeout)
        except TimeoutError:
            return False
        return self.finished

    def _issue(self, sent_ns: int, *, intended_ns: int) -> list[Any]:
        self._pending_intended_ns = intended_ns
        self._pending_sent_ns = sent_ns
        self._issued += 1
        if self._cancel_phase and self._next_id:
            cl_id = f"P-{self._next_id}"
            self._pending_id = cl_id
            return [SendCmd(CancelOrder(v=1, seq=0, epoch=self.epoch, cl_id=cl_id))]
        self._next_id += 1
        cl_id = f"P-{self._next_id}"
        self._pending_id = cl_id
        return [
            SendCmd(
                NewOrder(
                    v=1,
                    seq=0,
                    epoch=self.epoch,
                    md_seq=self._md_seq,
                    cl_id=cl_id,
                    symbol=_SYMBOL,
                    side="B",
                    px=_BID_PX,
                    qty=_QTY,
                    post_only=True,
                )
            )
        ]

    def _paced(self, now_ns: int) -> list[Any]:
        """Send the slot that is DUE, if any, and never more than one outstanding.

        A slot whose predecessor has not been acked is SKIPPED rather than stacked: stacking
        would put a third command in flight and risk the engine's `max_live_orders{2}`, which
        would disqualify the whole run. The skipped time still shows up, because the next send's
        lag is measured against its own intended slot.
        """
        if self._pending_id is not None:
            return []
        period_ns = int(1_000_000_000 / self.rate_hz)
        intended = self._start_ns + self._issued * period_ns
        if now_ns < intended:
            return []
        return self._issue(now_ns, intended_ns=intended)

    def _react(self, now_ns: int) -> list[Any]:
        """CANCEL the resting probe order, then send one echoing this book's `md_seq`.

        The cancel is not optional, and leaving it out is not a simplification. An accepted
        far-from-touch order RESTS: it never fills, so nothing retires it, and one order per book
        means the engine's live set grows by one per tick until `max_live_orders{2}` refuses the
        third — measured on the first real react run, which managed exactly two cycles before
        `reject MAX_LIVE_ORDERS`. An earlier version of this docstring argued the cancel was the
        risky choice; the opposite is true.

        The live count peaks at TWO by construction and cannot exceed it: the resting order is
        still live while its cancel is in flight, and the replacement is at most pending beside
        it. That is exactly the engine's limit, which is why the pair is emitted together rather
        than the new order being sent first.

        Only the NewOrder is timed — it is the one carrying the `md_seq` the engine correlates
        against — so the cancel is tracked for the outstanding count and nothing else.
        """
        if self._pending_id is not None or self.finished or self.epoch == 0:
            return []
        cmds: list[Any] = []
        if self._live_id is not None:
            cmds.append(SendCmd(CancelOrder(v=1, seq=0, epoch=self.epoch, cl_id=self._live_id)))
            self._live_id = None
        return cmds + self._issue(now_ns, intended_ns=now_ns)


def _cpu_pct(before: float | None, after: float | None, wall_s: float) -> float | None:
    """CPU utilisation over the run as a percentage of one core, or None if it was not measurable.

    None rather than 0.0 when procfs is absent: a benchmark that reports 0% CPU for a process that
    was certainly busy is worse than one that says it did not look.
    """
    if before is None or after is None or wall_s <= 0:
        return None
    return round(100.0 * (after - before) / wall_s, 1)


def _write_i64(path: Path, samples: array[int]) -> None:
    """Raw little-endian int64, the same shape the engine dumps.

    Takes the `array` straight from `series` and writes its buffer — no intermediate copy, since
    the samples were already in the right layout the moment they were recorded.
    """
    path.write_bytes(samples.tobytes())


async def _drive(args: argparse.Namespace) -> int:
    # EACH DIMENSION, not just their sum. `total > 0` alone admits warmup=-1 with
    # samples=100000: the run completes, the wrapper calls it green, and only `summarize` — which
    # refuses a negative warm-up — later discovers the artifacts cannot back a table. A benchmark
    # that produces unusable output and reports success is the failure mode this file exists to
    # avoid, so the refusal happens before a single sample is taken.
    if args.warmup < 0:
        print(f"run_bench: warm-up must not be negative, got {args.warmup}", file=sys.stderr)
        return 2
    if args.samples <= 0:
        print(f"run_bench: need at least one measured sample, got {args.samples}", file=sys.stderr)
        return 2
    total = args.warmup + args.samples
    probe = _Probe(mode=args.mode, total=total, rate_hz=args.rate)

    # DEFERRED, exactly as `app.py` defers them: picows and uvloop are the `tuned` extra, so a
    # naive-only install must still be able to run the naive arm of the measurement.
    if args.stack == "tuned":
        from mmclient.ws_picows import run_client  # noqa: PLC0415
    else:
        from mmclient.ws_naive import run_client  # noqa: PLC0415

    tick = pacing_tick_s(args.rate) if args.mode == "paced" else 0.001
    stop = asyncio.Event()

    # E-5's CPU evidence, bracketed around the WHOLE run (warm-up included, which the manifest's
    # `wall_s` makes explicit). Both processes are sampled: the saturation question is which side
    # runs out of headroom first, and a client at 100% with an idle engine is a completely
    # different finding from the reverse.
    cpu0_client = manifest.cpu_seconds(os.getpid())
    cpu0_engine = manifest.cpu_seconds(args.engine_pid) if args.engine_pid else None
    wall0 = time.perf_counter()

    task = asyncio.create_task(
        run_client(args.url, probe, stop=stop, timer_interval_s=tick)  # type: ignore[arg-type]
    )
    completed = await probe.wait(args.timeout)
    stop.set()
    try:
        await asyncio.wait_for(task, 10)
    except Exception as exc:
        # BROAD ON PURPOSE, and reported rather than swallowed: the samples already collected are
        # the artifact, and losing a completed run because the shutdown path raised would be the
        # worse outcome. (Written as `(TimeoutError, Exception)` first, which collapses to
        # `Exception` anyway — the same redundant-tuple mistake this gate caught in a test.)
        print(f"run_bench: client ended with {type(exc).__name__}: {exc}", file=sys.stderr)

    if not completed:
        why = (
            f"the engine rejected a probe command ({probe.reject_reason})"
            if probe.rejects
            else f"the run did not finish within {args.timeout}s"
        )
        print(
            f"run_bench: only {probe.count} of {total} cycles completed — {why}",
            file=sys.stderr,
        )
        return 1

    wall_s = time.perf_counter() - wall0
    co_corrected, actual, lag = series(probe.intended_ns, probe.sent_ns, probe.done_ns, probe.count)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    _write_i64(out.with_suffix(".rtt.i64"), co_corrected)
    _write_i64(out.with_suffix(".actual.i64"), actual)
    _write_i64(out.with_suffix(".lag.i64"), lag)

    meta = manifest.capture(
        stack=args.stack,
        mode=args.mode,
        rate_hz=args.rate,
        warmup=args.warmup,
        samples=args.samples,
        interval_ms=args.interval_ms,
        url=args.url,
        repeat_tag=args.repeat_tag,
        rejects=probe.rejects,
        # The client's own send stream, checked HERE because this is the only place the cycles
        # exist — the `.i64` files it writes are already reduced to three flat series, so nothing
        # downstream can recover the inter-send spacing this predicate needs.
        gaps=gaps_over(probe.sent_ns, probe.count),
        engine=Path(args.engine) if args.engine else None,
        engine_codec=args.engine_codec,
        image_digest=os.environ.get("MM_IMAGE_DIGEST", ""),
        client_cpu_pct=_cpu_pct(cpu0_client, manifest.cpu_seconds(os.getpid()), wall_s),
        engine_cpu_pct=_cpu_pct(
            cpu0_engine,
            manifest.cpu_seconds(args.engine_pid) if args.engine_pid else None,
            wall_s,
        ),
        wall_s=round(wall_s, 3),
    )
    out.with_suffix(".manifest.json").write_text(json.dumps(meta, indent=2) + "\n")

    print(
        f"run_bench: {probe.count} cycles ({args.warmup} warm-up + {args.samples} "
        f"measured), rejects={probe.rejects}, out={out}"
    )
    if probe.rejects:
        print(
            f"run_bench: {probe.rejects} rejects — this run does NOT qualify as a primary "
            "table (F-07)",
            file=sys.stderr,
        )
        return 1
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="run_bench", description="§5.2 benchmark run")
    p.add_argument("--mode", choices=("idle", "paced", "react"), required=True)
    p.add_argument("--stack", choices=("naive", "tuned"), required=True)
    p.add_argument("--url", required=True)
    p.add_argument("--out", required=True, help="path prefix; .rtt.i64/.lag.i64/.manifest.json")
    p.add_argument("--rate", type=float, default=1000.0, help="paced mode sends per second")
    p.add_argument("--warmup", type=int, default=10_000)
    p.add_argument("--samples", type=int, default=100_000)
    p.add_argument("--repeat-tag", default="A1")
    p.add_argument("--interval-ms", type=int, default=1, help="the engine's feed cadence")
    p.add_argument("--engine", default="", help="engine binary, for its --version in the manifest")
    p.add_argument(
        "--engine-pid",
        type=int,
        default=0,
        help="the engine's pid, so E-5 can report its CPU%% alongside the client's",
    )
    p.add_argument(
        "--engine-codec",
        default="",
        choices=("", "naive", "tuned"),
        help="the engine's --codec for this arm; recorded so the artifact names the C++ leg too",
    )
    p.add_argument("--timeout", type=float, default=600.0, help="wall-clock bound on the run")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.stack == "tuned":
        import uvloop  # noqa: PLC0415 - the tuned extra; see _drive

        return uvloop.run(_drive(args))
    return asyncio.run(_drive(args))


if __name__ == "__main__":  # pragma: no cover - process entry point
    sys.exit(main())
