"""Historical performance and regression tracking for mm_engine and mmclient.

Maintains an append-only ledger in bench/history/ledger.json and generates
docs/PERFORMANCE_HISTORY.md to track latency metrics (M1, M2, M3) and test
counts across git commits, detecting regressions and improvements automatically.
"""

from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from harness.summarize import Stats, drop_warmup, percentiles, read_engine_dump, read_i64

DEFAULT_LEDGER_PATH = Path("bench/history/ledger.json")
DEFAULT_DOC_PATH = Path("docs/PERFORMANCE_HISTORY.md")
DEFAULT_RESULTS_ROOT = Path("bench/results")

NS_PER_US = 1_000
NS_PER_MS = 1_000_000
MIN_SNAPSHOTS_FOR_COMPARE = 2


@dataclass(frozen=True)
class MetricStats:
    count: int
    min_ns: int
    p50_ns: int
    p90_ns: int
    p99_ns: int
    p99_9_ns: int
    max_ns: int

    @classmethod
    def from_stats(cls, s: Stats) -> MetricStats:
        return cls(
            count=s.count,
            min_ns=s.min,
            p50_ns=s.p50,
            p90_ns=s.p90,
            p99_ns=s.p99,
            p99_9_ns=s.p99_9,
            max_ns=s.max,
        )


@dataclass
class ScenarioRecord:
    m1_rtt: MetricStats | None = None
    m1_actual: MetricStats | None = None
    m2_svc: MetricStats | None = None
    m3_react: MetricStats | None = None
    wall_s: float | None = None


@dataclass
class Snapshot:
    timestamp: str
    git_sha: str
    git_short: str
    git_branch: str
    git_dirty: bool
    description: str
    cpp_tests: int | None
    python_tests: int | None
    python_coverage_pct: float | None
    scenarios: dict[str, dict[str, Any]]


def get_git_info() -> tuple[str, str, str, bool]:
    """Extracts current git commit SHA, short SHA, branch, and dirty status."""
    try:
        sha = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
        short = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip()
        branch = subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], text=True
        ).strip()
        status = subprocess.check_output(["git", "status", "--porcelain"], text=True).strip()
        dirty = len(status) > 0
    except Exception:
        return "unknown", "unknown", "unknown", False
    else:
        return sha, short, branch, dirty


def find_latest_results_dir(results_root: Path) -> Path | None:
    """Finds the most recent benchmark results directory."""
    if not results_root.exists():
        return None
    subdirs = [
        d
        for d in results_root.iterdir()
        if d.is_dir() and not d.name.startswith(".") and d.name != "flame"
    ]
    if not subdirs:
        return None
    return max(subdirs, key=lambda d: d.name)


def _parse_stream_file(file_path: Path, warmup: int) -> MetricStats | None:
    """Parses a single .i64 stream file with warmup dropping."""
    if not file_path.exists():
        return None
    try:
        arr = read_i64(file_path)
        if len(arr) > warmup:
            trimmed = drop_warmup(arr, warmup)
            if trimmed:
                return MetricStats.from_stats(percentiles(trimmed))
    except Exception:
        pass
    return None


def _parse_engine_dump(
    dump_path: Path, warmup: int
) -> tuple[MetricStats | None, MetricStats | None]:
    """Parses engine dump streams for svc (M2) and m0_m3 (M3)."""
    if not dump_path.exists():
        return None, None
    m2_stat: MetricStats | None = None
    m3_stat: MetricStats | None = None
    try:
        dump = read_engine_dump(dump_path)
        if dump.svc and len(dump.svc) > warmup:
            trimmed_svc = drop_warmup(dump.svc, warmup)
            if trimmed_svc:
                m2_stat = MetricStats.from_stats(percentiles(trimmed_svc))
        if dump.m0_m3 and len(dump.m0_m3) > warmup:
            trimmed_m3 = drop_warmup(dump.m0_m3, warmup)
            if trimmed_m3:
                m3_stat = MetricStats.from_stats(percentiles(trimmed_m3))
    except Exception:
        pass
    return m2_stat, m3_stat


def parse_result_run(run_prefix: Path, warmup: int = 500) -> ScenarioRecord:
    """Parses .i64 and .engine.bench files for a scenario run prefix."""
    m1_rtt = _parse_stream_file(run_prefix.with_suffix(".rtt.i64"), warmup)
    m1_act = _parse_stream_file(run_prefix.with_suffix(".actual.i64"), warmup)
    m2_svc, m3_react = _parse_engine_dump(run_prefix.with_suffix(".engine.bench"), warmup)

    return ScenarioRecord(
        m1_rtt=m1_rtt,
        m1_actual=m1_act,
        m2_svc=m2_svc,
        m3_react=m3_react,
    )


def collect_snapshot(
    results_dir: Path,
    description: str = "",
    cpp_tests: int | None = None,
    python_tests: int | None = None,
    coverage_pct: float | None = None,
    warmup: int = 500,
) -> Snapshot:
    """Collects benchmark and test metrics into a structured Snapshot."""
    sha, short, branch, dirty = get_git_info()
    now_iso = datetime.datetime.now(datetime.UTC).isoformat()

    manifest_files = list(results_dir.glob("*.manifest.json"))
    scenarios: dict[str, dict[str, Any]] = {}

    for mf in manifest_files:
        scenario_name = mf.name.replace(".manifest.json", "")
        run_prefix = results_dir / scenario_name
        rec = parse_result_run(run_prefix, warmup=warmup)
        scenarios[scenario_name] = asdict(rec)

    return Snapshot(
        timestamp=now_iso,
        git_sha=sha,
        git_short=short,
        git_branch=branch,
        git_dirty=dirty,
        description=description,
        cpp_tests=cpp_tests,
        python_tests=python_tests,
        python_coverage_pct=coverage_pct,
        scenarios=scenarios,
    )


def load_ledger(ledger_path: Path) -> list[Snapshot]:
    """Loads historical snapshots from ledger file."""
    if not ledger_path.exists():
        return []
    try:
        with ledger_path.open("r", encoding="utf-8") as f:
            data = json.load(f)
            return [Snapshot(**item) for item in data]
    except Exception:
        return []


def save_ledger(ledger: list[Snapshot], ledger_path: Path) -> None:
    """Saves snapshots to ledger file."""
    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    with ledger_path.open("w", encoding="utf-8") as f:
        json.dump([asdict(s) for s in ledger], f, indent=2)


def format_ns(ns: int | None) -> str:
    """Formats nanoseconds into human readable string (ns or us)."""
    if ns is None:
        return "-"
    if ns < NS_PER_US:
        return f"{ns}ns"
    if ns < NS_PER_MS:
        return f"{ns / NS_PER_US:.1f}µs"
    return f"{ns / NS_PER_MS:.2f}ms"


def compare_snapshots(
    base: Snapshot, current: Snapshot, threshold_pct: float = 5.0
) -> tuple[bool, str]:
    """Compares two snapshots and reports regressions or improvements."""
    lines: list[str] = []
    lines.append(
        f"=== Performance Comparison: {base.git_short} -> {current.git_short} "
        f"({current.description or 'Latest'}) ==="
    )
    has_regression = False

    common_scenarios = sorted(set(base.scenarios.keys()) & set(current.scenarios.keys()))
    if not common_scenarios:
        return False, "No common scenarios found between snapshots."

    for sc in common_scenarios:
        b_sc = base.scenarios[sc]
        c_sc = current.scenarios[sc]
        lines.append(f"\nScenario: {sc}")
        lines.append(
            f"  {'Metric':<18} | {'Base p50':<10} | {'Curr p50':<10} | {'Delta p50':<12} | "
            f"{'Base p99':<10} | {'Curr p99':<10} | {'Delta p99':<12} | Status"
        )
        lines.append("  " + "-" * 95)

        for key, label in [
            ("m1_rtt", "M1 RTT (CO)"),
            ("m1_actual", "M1 Actual"),
            ("m2_svc", "M2 Service"),
            ("m3_react", "M3 Reaction"),
        ]:
            b_m = b_sc.get(key)
            c_m = c_sc.get(key)
            if not b_m or not c_m:
                continue

            bp50 = b_m["p50_ns"]
            cp50 = c_m["p50_ns"]
            bp99 = b_m["p99_ns"]
            cp99 = c_m["p99_ns"]

            d_p50 = cp50 - bp50
            pct_p50 = (d_p50 / bp50 * 100.0) if bp50 > 0 else 0.0
            d_p99 = cp99 - bp99
            pct_p99 = (d_p99 / bp99 * 100.0) if bp99 > 0 else 0.0

            status = "PASS"
            if pct_p50 > threshold_pct or pct_p99 > threshold_pct:
                status = "REGRESSION"
                has_regression = True
            elif pct_p50 < -threshold_pct:
                status = "FASTER"

            lines.append(
                f"  {label:<18} | {format_ns(bp50):<10} | {format_ns(cp50):<10} | "
                f"{pct_p50:+6.1f}% ({format_ns(d_p50)}) | "
                f"{format_ns(bp99):<10} | {format_ns(cp99):<10} | "
                f"{pct_p99:+6.1f}% ({format_ns(d_p99)}) | {status}"
            )

    return has_regression, "\n".join(lines)


def generate_markdown_history(ledger: list[Snapshot], doc_path: Path) -> None:
    """Generates a comprehensive Markdown history document."""
    lines: list[str] = [
        "# Historical Performance & Regression Ledger",
        "",
        "This document records benchmark latency trends (M1, M2, M3) and test suite metrics "
        "across Git commits and architectural iterations.",
        "",
        "## 1. Summary of Benchmarked Milestones",
        "",
        "| Date (UTC) | Commit | Branch | Description | C++ Tests | Py Tests | Py Cover | "
        "M1 RTT p50 (Tuned) | M2 Svc p50 (Tuned) | M3 React p50 (Tuned) |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]

    for s in ledger:
        tuned_idle = s.scenarios.get("idle_tuned_R1", {})
        tuned_react = s.scenarios.get("react_tuned_R1", {})

        m1_p50 = format_ns(
            tuned_idle.get("m1_rtt", {}).get("p50_ns") if tuned_idle.get("m1_rtt") else None
        )
        m2_p50 = format_ns(
            tuned_idle.get("m2_svc", {}).get("p50_ns") if tuned_idle.get("m2_svc") else None
        )
        m3_p50 = format_ns(
            tuned_react.get("m3_react", {}).get("p50_ns") if tuned_react.get("m3_react") else None
        )

        cov_str = f"{s.python_coverage_pct:.1f}%" if s.python_coverage_pct is not None else "-"
        cpp_str = str(s.cpp_tests) if s.cpp_tests is not None else "-"
        py_str = str(s.python_tests) if s.python_tests is not None else "-"

        lines.append(
            f"| {s.timestamp[:10]} | `{s.git_short}` | `{s.git_branch}` | "
            f"{s.description or '-'} | {cpp_str} | {py_str} | {cov_str} | "
            f"{m1_p50} | {m2_p50} | {m3_p50} |"
        )

    lines.append("")
    lines.append("## 2. Regression Protection Gate Guidelines")
    lines.append(
        "- **M1 (ACK RTT p50):** Alert if $p50 > +5\\%$ over baseline without justification."
    )
    lines.append("- **M2 (Engine Service Time p50):** Hard limit $< 1.5\\,\\mu\\text{s}$.")
    lines.append(
        "- **Python Coverage:** Hard ratchet $\\ge 100\\%$ branch coverage enforced by CI."
    )
    lines.append("- **C++ Test Pass Rate:** 100% required across all 4 build presets.")
    lines.append("")

    doc_path.parent.mkdir(parents=True, exist_ok=True)
    with doc_path.open("w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Historical Performance and Regression Tracker.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # Record command
    p_rec = subparsers.add_parser("record", help="Record a new benchmark snapshot into history.")
    p_rec.add_argument("--results-dir", type=Path, default=None, help="Results directory.")
    p_rec.add_argument("--description", type=str, default="", help="Note / description.")
    p_rec.add_argument("--cpp-tests", type=int, default=179, help="Total C++ tests passed.")
    p_rec.add_argument("--python-tests", type=int, default=580, help="Total Python tests passed.")
    p_rec.add_argument(
        "--coverage-pct", type=float, default=100.0, help="Python branch coverage percentage."
    )
    p_rec.add_argument("--warmup", type=int, default=500, help="Warmup sample count to discard.")
    p_rec.add_argument(
        "--ledger", type=Path, default=DEFAULT_LEDGER_PATH, help="Path to ledger file."
    )
    p_rec.add_argument("--doc", type=Path, default=DEFAULT_DOC_PATH, help="Path to Markdown doc.")

    # Compare command
    p_comp = subparsers.add_parser("compare", help="Compare latest run against baseline.")
    p_comp.add_argument(
        "--ledger", type=Path, default=DEFAULT_LEDGER_PATH, help="Path to ledger file."
    )
    p_comp.add_argument(
        "--threshold", type=float, default=5.0, help="Regression percentage threshold."
    )
    p_comp.add_argument(
        "--fail-on-regression", action="store_true", help="Exit code 1 on regression."
    )

    # Report command
    p_rep = subparsers.add_parser("report", help="Regenerate docs/PERFORMANCE_HISTORY.md.")
    p_rep.add_argument(
        "--ledger", type=Path, default=DEFAULT_LEDGER_PATH, help="Path to ledger file."
    )
    p_rep.add_argument("--doc", type=Path, default=DEFAULT_DOC_PATH, help="Path to Markdown doc.")

    args = parser.parse_args()

    if args.command == "record":
        res_dir = args.results_dir or find_latest_results_dir(DEFAULT_RESULTS_ROOT)
        if not res_dir or not res_dir.exists():
            print(f"FAIL: no valid results in {DEFAULT_RESULTS_ROOT}", file=sys.stderr)
            sys.exit(1)

        snapshot = collect_snapshot(
            results_dir=res_dir,
            description=args.description,
            cpp_tests=args.cpp_tests,
            python_tests=args.python_tests,
            coverage_pct=args.coverage_pct,
            warmup=args.warmup,
        )

        ledger = load_ledger(args.ledger)
        ledger.append(snapshot)
        save_ledger(ledger, args.ledger)
        generate_markdown_history(ledger, args.doc)
        print(f"Recorded snapshot {snapshot.git_short} ({len(snapshot.scenarios)} scenarios).")
        print(f"Saved to {args.ledger} and updated {args.doc}.")

    elif args.command == "compare":
        ledger = load_ledger(args.ledger)
        if len(ledger) < MIN_SNAPSHOTS_FOR_COMPARE:
            print("Need at least 2 snapshots in ledger to compare.", file=sys.stderr)
            sys.exit(0)

        base = ledger[-2]
        current = ledger[-1]
        has_regr, report = compare_snapshots(base, current, threshold_pct=args.threshold)
        print(report)

        if has_regr and args.fail_on_regression:
            print("\nFAIL: Performance regression detected!", file=sys.stderr)
            sys.exit(1)

    elif args.command == "report":
        ledger = load_ledger(args.ledger)
        generate_markdown_history(ledger, args.doc)
        print(f"Updated {args.doc} ({len(ledger)} snapshots).")


if __name__ == "__main__":
    main()
