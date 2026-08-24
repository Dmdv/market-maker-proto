"""Turns raw latency samples into the tables §5.2 asks for, and refuses runs that cannot back
them.

Reads two kinds of artifact: the client's own `.i64` files (raw little-endian int64 nanoseconds,
written by `run_bench.py`) and the engine's `--bench-out` dump (`BenchRecorder::serialize` —
six uint64 header words, then four int64 streams).

THREE RULES THAT ARE NOT NEGOTIABLE HERE, because each one is a way for a measurement tool to
report a flattering number while looking correct:

1. **Percentiles are exact ranks on the sorted data, never interpolated.** An interpolated p99
   is a value no request experienced; publishing it is a claim about a request that did not
   happen. The rule is `ceil(p*n) - 1`, stated once in `rank_index` so the prose in
   BENCHMARK.md and the numbers in the tables cannot drift apart.
2. **A run that misses its sample floor is refused, not summarised.** A thin table reads exactly
   like a full one, and by the time it is in a document nobody can tell.
3. **Coordinated omission is corrected by default.** See `series`: the honest series measures
   from the time a request was DUE, not from the time the client managed to send it.

`main` is a CLI so one run can be inspected by hand; `scripts/run_bench.sh` drives the matrix.
"""

import argparse
import json
import math
import struct
import sys
from array import array
from dataclasses import dataclass
from pathlib import Path

# No cycle: `manifest` imports nothing from this module. `qualifies_as_primary` lives there
# because that is where the run-quality contract is documented; it is APPLIED here because this
# is the only place all three of its inputs exist at once.
from harness.manifest import qualifies_as_primary

__all__ = [
    "EngineDump",
    "Stats",
    "drop_warmup",
    "gaps_over",
    "main",
    "percentiles",
    "rank_index",
    "read_engine_dump",
    "read_i64",
    "series",
]

# The engine's dump header: four sample counts, the refused count, the peak session count.
_HEADER = struct.Struct("<6Q")
# Above this, a run's m0->m0' stream is the minimum across a fan-out rather than the delivery
# boundary (measured 34-79x low at 8 to 64 sessions), so it cannot back an attribution claim.
_MAX_PEAK_SESSIONS = 1
# A quiet second mid-run is a stopped process, not a slow one.
GAP_THRESHOLD_NS = 1_000_000_000


@dataclass(frozen=True)
class Stats:
    """One series, summarised. The fields §5.2 names, and nothing derived."""

    count: int
    min: int
    p50: int
    p90: int
    p99: int
    p99_9: int
    max: int


@dataclass(frozen=True)
class EngineDump:
    """The engine's four streams, plus the two words that make a run self-describing."""

    svc: array[int]
    m0_m0p: array[int]
    m0p_m3: array[int]
    m0_m3: array[int]
    saturated: int
    peak_sessions: int

    @property
    def is_suspect(self) -> bool:
        """A refused sample means the recorder hit capacity, so the streams are TRUNCATED and
        the percentiles describe only the part of the run that fit."""
        return self.saturated > 0


def rank_index(p: float, n: int) -> int:
    """The index of the p-th percentile under the ceil-rank rule, for n samples.

    `ceil(p*n) - 1`, clamped into range. Written once and used everywhere so the rule that
    BENCHMARK.md documents is the rule the tables were computed with.
    """
    if n <= 0:
        msg = "no samples: a percentile of nothing is not zero, it is undefined"
        raise ValueError(msg)
    return min(n - 1, max(0, math.ceil(p * n) - 1))


def percentiles(samples: array[int]) -> Stats:
    """Exact-rank percentiles over a copy of the samples, sorted.

    Copies rather than sorting in place: the caller's array is the raw artifact, and a function
    that reorders its input makes every later read of that array depend on call order.
    """
    n = len(samples)
    if n == 0:
        msg = "no samples: a percentile of nothing is not zero, it is undefined"
        raise ValueError(msg)
    ordered = sorted(samples)
    return Stats(
        count=n,
        min=ordered[0],
        p50=ordered[rank_index(0.5, n)],
        p90=ordered[rank_index(0.9, n)],
        p99=ordered[rank_index(0.99, n)],
        p99_9=ordered[rank_index(0.999, n)],
        max=ordered[-1],
    )


def drop_warmup(samples: array[int], warmup: int) -> array[int]:
    """Drop the first `warmup` samples BY COUNT.

    By count rather than by elapsed time because the engine's streams carry no timestamps to
    filter on, and client and engine cycles correspond 1:1 in idle and react modes — so one rule
    covers both sides of the measurement (audit F-27).

    Refuses to drop more than exists: returning an empty series would report a run that measured
    nothing as a run that measured cleanly.
    """
    if warmup < 0:
        msg = f"warm-up count must not be negative, got {warmup}"
        raise ValueError(msg)
    if warmup >= len(samples):
        msg = (
            f"warm-up ({warmup}) consumes the whole series ({len(samples)} samples): "
            "the run measured nothing"
        )
        raise ValueError(msg)
    return samples[warmup:]


def series(
    intended: array[int], sent: array[int], done: array[int], count: int
) -> tuple[array[int], array[int], array[int]]:
    """(co_corrected, actual, lag), in nanoseconds.

    THE COORDINATED-OMISSION CORRECTION, and why the harness reports both series rather than
    picking one. Measured from the ACTUAL send, a client that stalls and then catches up reports
    only its service time: every request it managed to send looks fast, and the requests it
    never sent during the stall are simply absent from the data. The queueing delay is real and
    was paid by whoever was waiting — it just happened before the timer started.

    Measured from the INTENDED send, that delay lands in the tail where a user would have felt
    it. `co_corrected` is therefore the honest series and the one the primary tables use;
    `actual` is reported beside it precisely so the gap between them is visible rather than
    argued about. `lag` (`sent - intended`) is the queue proxy: it is what the correction is
    made of.

    Takes the probe's three PREALLOCATED stamp buffers and the number of cycles actually recorded,
    rather than a list of per-cycle objects. The buffers are sized for the whole run up front
    (§4.4: zero allocation on the hot path), so they are longer than `count` whenever a run ends
    early — everything past `count` is the zero fill and must not reach a percentile.
    """
    if not 0 <= count <= min(len(intended), len(sent), len(done)):
        msg = f"count {count} is outside the recorded stamps"
        raise ValueError(msg)
    co_corrected = array("q", bytes(8 * count))
    actual = array("q", bytes(8 * count))
    lag = array("q", bytes(8 * count))
    for i in range(count):
        intended_i, sent_i, done_i = intended[i], sent[i], done[i]
        co_corrected[i] = done_i - intended_i
        actual[i] = done_i - sent_i
        lag[i] = sent_i - intended_i
    return co_corrected, actual, lag


def gaps_over(sent: array[int], count: int, threshold_ns: int = GAP_THRESHOLD_NS) -> bool:
    """Whether any two consecutive sends are further apart than `threshold_ns`.

    A second of silence mid-run is a stopped process — a laptop that slept, a container that was
    throttled, a GC pause of a kind Python does not have. FLAGGED rather than dropped: the run
    is evidence of something, just not of latency (F-34).

    `count` bounds the scan for the same reason as in `series`: the tail of the preallocated
    buffer is zeros, and a zero send stamp beside a real one is a gap of the whole run's length.
    """
    return any(sent[i + 1] - sent[i] > threshold_ns for i in range(max(0, count - 1)))


def _le_i64(raw: bytes) -> array[int]:
    """Little-endian int64s from raw bytes, on any host.

    `array("q").frombytes()` is NATIVE-endian, but both artifact formats are defined as
    little-endian — the engine's dump says so explicitly and refuses to compile on a big-endian
    producer, and the header beside those streams is already read with an explicit `<6Q`. So the
    header was endian-correct while the samples next to it were not: move a dump to a big-endian
    reader and every latency comes back byte-swapped, silently and enormously wrong rather than
    obviously broken.

    No host this project runs on is big-endian, so this is correctness for a case that has not
    occurred rather than a bug being fixed — which is exactly why it is worth eight lines now
    instead of a debugging session later.
    """
    out = array("q")
    out.frombytes(raw)
    if sys.byteorder != "little":
        out.byteswap()
    return out


def read_i64(path: Path) -> array[int]:
    """One raw sample file. Refuses a size that is not a whole number of int64s — a partial
    tail means the writer died mid-flush, and the samples after the cut are not there."""
    raw = path.read_bytes()
    if len(raw) % 8 != 0:
        msg = f"{path.name}: {len(raw)} bytes is not a whole number of int64 samples (truncated)"
        raise ValueError(msg)
    return _le_i64(raw)


def read_engine_dump(path: Path) -> EngineDump:
    """The engine's `--bench-out` artifact, validated against its own documented contract.

    Refuses a fan-out run outright: with more than one session the recorded m0->m0' is the
    MINIMUM across the fan-out rather than the delivery boundary, so it cannot back the
    attribution decomposition BENCHMARK.md is built on. The recorder's header comment requires a
    reader to refuse or flag this, and refusing is the safer half — a flag in a log is not read
    by whoever later quotes the table.
    """
    raw = path.read_bytes()
    if len(raw) < _HEADER.size:
        msg = f"{path.name}: shorter than the {_HEADER.size}-byte header (truncated)"
        raise ValueError(msg)
    n_svc, n_m0_m0p, n_m0p_m3, n_m0_m3, saturated, peak = _HEADER.unpack_from(raw)
    if peak > _MAX_PEAK_SESSIONS:
        msg = (
            f"{path.name}: peak session count {peak} > {_MAX_PEAK_SESSIONS}; m0->m0' is the "
            "fan-out minimum here, not the delivery boundary, so this run cannot back an "
            "attribution table"
        )
        raise ValueError(msg)

    counts = (n_svc, n_m0_m0p, n_m0p_m3, n_m0_m3)
    expected = _HEADER.size + sum(counts) * 8
    if len(raw) != expected:
        msg = (
            f"{path.name}: header promises {sum(counts)} samples ({expected} bytes) but the file "
            f"is {len(raw)} bytes — truncated dump"
        )
        raise ValueError(msg)

    streams: list[array[int]] = []
    offset = _HEADER.size
    for count in counts:
        stream = array("q")
        stream = _le_i64(raw[offset : offset + count * 8])
        streams.append(stream)
        offset += count * 8
    svc, m0_m0p, m0p_m3, m0_m3 = streams
    return EngineDump(
        svc=svc,
        m0_m0p=m0_m0p,
        m0p_m3=m0p_m3,
        m0_m3=m0_m3,
        saturated=saturated,
        peak_sessions=peak,
    )


def _table(name: str, stats: Stats, *, markdown: bool) -> str:
    fields = (
        ("count", stats.count),
        ("min", stats.min),
        ("p50", stats.p50),
        ("p90", stats.p90),
        ("p99", stats.p99),
        ("p99.9", stats.p99_9),
        ("max", stats.max),
    )
    if markdown:
        head = "| series | " + " | ".join(k for k, _ in fields) + " |"
        rule = "|---" * (len(fields) + 1) + "|"
        row = f"| {name} | " + " | ".join(str(v) for _, v in fields) + " |"
        return f"{head}\n{rule}\n{row}"
    body = "  ".join(f"{k}={v}" for k, v in fields)
    return f"{name}: {body}"


def _engine_tables(dump: EngineDump, *, warmup: int, mode: str, markdown: bool) -> list[str]:
    """The engine-side tables, with M3 suppressed outside react mode.

    Idle echoes one stale `md_seq` for the whole run and paced measures its own pacing phase, so
    an M3 table from either is a number about the harness rather than about the system — which is
    exactly the shape of claim §5.2 exists to prevent.
    """
    tables: list[str] = []
    if len(dump.svc):
        tables.append(
            _table("svc (M2)", percentiles(drop_warmup(dump.svc, warmup)), markdown=markdown)
        )
    m3_streams = (
        ("m0->m0' (venue production)", dump.m0_m0p),
        ("m0'->m3 (delivery + reaction)", dump.m0p_m3),
        ("m0->m3 (total)", dump.m0_m3),
    )
    if mode != "react":
        if any(len(s) for _, s in m3_streams):
            print(
                f"summarize: M3 streams present but mode={mode}; suppressed as semantically "
                "invalid (M3 tables come only from react runs)",
                file=sys.stderr,
            )
        return tables
    for label, stream in m3_streams:
        if len(stream):
            tables.append(
                _table(label, percentiles(drop_warmup(stream, warmup)), markdown=markdown)
            )
    return tables


def _sibling_manifest(rtt: Path) -> Path:
    """The manifest `run_bench` wrote beside this `.rtt.i64`, by construction of its `--out`."""
    name = rtt.name
    stem = name[: -len(".rtt.i64")] if name.endswith(".rtt.i64") else rtt.stem
    return rtt.with_name(f"{stem}.manifest.json")


def _primary_verdict(
    *, manifest_path: Path, saturated: int | None, markdown: bool
) -> tuple[str, bool | None]:
    """Apply the §5.2 primary-table gate and render it as a block.

    Returns `(block, qualified)`, where `qualified` is None when the verdict CANNOT be reached
    because an input is missing. That third state matters: "not shown to qualify" and "shown not
    to qualify" are different claims, and collapsing them into a bare False would let a missing
    artifact read as a failed run — or worse, a missing artifact read as a pass.

    The inputs arrive from three places and no earlier stage can see all of them: `rejects` and
    `gaps` from the client's manifest, `saturated` from the engine dump written after the client
    exited. That split is why this gate went unapplied until now.
    """
    unknown: list[str] = []
    rejects: int | None = None
    gaps: bool | None = None

    try:
        meta = json.loads(manifest_path.read_text())
    except OSError, ValueError:
        unknown.append(f"no readable manifest at {manifest_path.name} (rejects, gaps)")
    else:
        raw_rejects, raw_gaps = meta.get("rejects"), meta.get("gaps")
        if isinstance(raw_rejects, int):
            rejects = raw_rejects
        else:
            unknown.append("manifest has no `rejects`")
        if isinstance(raw_gaps, bool):
            gaps = raw_gaps
        else:
            # Manifests written before `gaps` was recorded. Say so rather than defaulting to
            # False, which would silently upgrade an unknown into a pass.
            unknown.append("manifest has no `gaps` (written before it was recorded)")

    if saturated is None:
        unknown.append("no --engine dump given (saturated)")

    rows = [
        ("rejects", "unknown" if rejects is None else str(rejects), "0"),
        ("engine recorder refused", "unknown" if saturated is None else str(saturated), "0"),
        ("send-stream gaps", "unknown" if gaps is None else str(gaps), "False"),
    ]

    if rejects is None or gaps is None or saturated is None:
        verdict, qualified = "INDETERMINATE — " + "; ".join(unknown), None
    elif qualifies_as_primary(rejects=rejects, saturated=saturated, gaps=gaps):
        verdict, qualified = "PRIMARY — this run may back a primary table", True
    else:
        verdict, qualified = "NOT PRIMARY — secondary/probe only", False

    if markdown:
        body = "\n".join(f"| {name} | {value} | {want} |" for name, value, want in rows)
        return (
            "**run quality (§5.2 primary-table gate)**\n\n"
            "| input | value | required |\n| --- | --- | --- |\n" + body + f"\n\n**{verdict}**"
        ), qualified
    body = "\n".join(f"  {name:<24} {value:<10} (required: {want})" for name, value, want in rows)
    return f"run quality (§5.2 primary-table gate)\n{body}\n  => {verdict}", qualified


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="summarize", description="§5.2 latency tables")
    p.add_argument("--rtt", type=Path, required=True, help="client .rtt.i64 (CO-corrected)")
    p.add_argument("--actual", type=Path, help="client .actual.i64, reported beside the honest one")
    p.add_argument("--lag", type=Path, help="client .lag.i64 (the queue proxy)")
    p.add_argument("--engine", type=Path, help="engine --bench-out dump")
    p.add_argument(
        "--manifest",
        type=Path,
        help="run manifest; defaults to the --rtt path with .manifest.json in place of .rtt.i64",
    )
    p.add_argument("--warmup", type=int, default=0, help="samples to drop from the head")
    p.add_argument(
        "--require-samples",
        type=int,
        default=0,
        help="refuse the run below this many measured samples (§5.2 primary floor: 100000)",
    )
    p.add_argument("--md", action="store_true", help="emit markdown tables")
    p.add_argument(
        "--rate",
        type=float,
        default=0.0,
        help="the paced rate the run ASKED for; enables the saturation check",
    )
    p.add_argument(
        "--mode",
        choices=("idle", "paced", "react"),
        default="idle",
        help="suppresses M3 tables outside react mode, where they are not meaningful",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    """Returns a process exit code. Non-zero means the run does not support a table.

    Every refusal prints to STDERR and prints no table at all. A summary emitted next to a
    warning gets quoted without the warning.
    """
    args = build_parser().parse_args(argv)
    try:
        measured = drop_warmup(read_i64(args.rtt), args.warmup)
    except ValueError as exc:
        print(f"summarize: {exc}", file=sys.stderr)
        return 2

    if len(measured) < args.require_samples:
        print(
            f"summarize: {len(measured)} measured samples is below the required "
            f"{args.require_samples}; this run cannot back a table",
            file=sys.stderr,
        )
        return 1

    out: list[str] = [_table("rtt (CO-corrected)", percentiles(measured), markdown=args.md)]
    lag_stats: Stats | None = None
    for label, path in (("rtt (actual send)", args.actual), ("send lag", args.lag)):
        if path is not None:
            samples = drop_warmup(read_i64(path), args.warmup)
            stats = percentiles(samples)
            if label == "send lag":
                lag_stats = stats
            out.append(_table(label, stats, markdown=args.md))

    # SATURATION, which is a different failure from a slow system and must not be reported as
    # one. If the median send is already more than one whole period late, the client never
    # sustained the rate it was asked for — so the actual-send series describes only the requests
    # it MANAGED to send, and its percentiles are a statement about the harness rather than about
    # the system. Measured directly: a 10 kHz probe whose round trip is ~112 us is serially
    # capped near 8.9 kHz, and its actual-send p50 read a healthy 112 us while it was 10.7
    # SECONDS behind schedule. A run in that state can be published as a labelled saturation
    # probe; it cannot answer a percentile question.
    if args.rate > 0 and lag_stats is not None:
        period_ns = int(1_000_000_000 / args.rate)
        if lag_stats.p50 > period_ns:
            behind = lag_stats.p50 / 1e9
            warning = (
                f"SATURATED: the median send was {behind:.3f}s late against a "
                f"{period_ns} ns period — the client did not sustain {args.rate:g}/s, so this "
                "run is a saturation probe and NOT primary-table material"
            )
            # In the OUTPUT, not only on stderr: a warning that does not travel with the table
            # is a warning nobody sees once the table is pasted somewhere else.
            out.append(f"**{warning}**" if args.md else f"!! {warning}")
            print(f"summarize: {warning}", file=sys.stderr)
            if args.require_samples:
                # The tables are still printed — the run IS evidence, just not of latency — and
                # the exit code refuses the primary-table claim the caller made by asking for a
                # sample floor.
                print("\n\n".join(out))
                return 1

    saturated: int | None = None
    if args.engine is not None:
        try:
            dump = read_engine_dump(args.engine)
        except ValueError as exc:
            print(f"summarize: {exc}", file=sys.stderr)
            return 2
        saturated = dump.saturated
        if dump.is_suspect:
            print(
                f"summarize: the engine refused {dump.saturated} samples — its streams are "
                "truncated and describe only the part of the run that fit",
                file=sys.stderr,
            )
            return 1
        out.extend(_engine_tables(dump, warmup=args.warmup, mode=args.mode, markdown=args.md))

    verdict_line, qualified = _primary_verdict(
        manifest_path=args.manifest or _sibling_manifest(args.rtt),
        saturated=saturated,
        markdown=args.md,
    )
    out.append(verdict_line)

    # `--require-samples` IS the caller's primary-table claim (it is how the §5.2 floor gets
    # asserted), so a run that fails the gate must not exit 0 under it. Without this the verdict
    # would be one more line of prose in an output nobody diffs.
    #
    # One exit rather than two: the tables print on both paths, so duplicating the print to get a
    # second `return` only invites the two copies to drift.
    refused = bool(args.require_samples) and qualified is False
    if refused:
        print(
            "summarize: this run does not qualify as a primary table (see the run-quality block)",
            file=sys.stderr,
        )
    print("\n\n".join(out))
    return 1 if refused else 0


if __name__ == "__main__":  # pragma: no cover - process entry point
    sys.exit(main())
