#!/usr/bin/env python3
"""Post-hoc peer audit for a finished §5.2 matrix directory.

Pre-flight cannot see Docker Desktop VM stalls that arrive mid-matrix (idle_tuned_R1
with wall 11.0 s vs siblings 9.4 s; paced_naive_R2 with 108 samples >5 ms vs 16–30
while wall was identical). This script reads manifests + `.rtt.i64` and refuses a
matrix when any primary (R*) peer is disturbed relative to its siblings.

Rules (measured on bench/results/20260730T051524Z + synthetic peer checks; see
GROK_INVESTIGATION.md):

  WALL:  wall_s > median(peer walls) * (1 + WALL_FRAC)
         WALL_FRAC=0.10. Clean idle wall CV ~0.7%; contaminated idle was +17%.
         Clean react wall CV ~5–7%; 10% leaves margin. Paced walls are fixed by
         the rate generator — this rule is silent there (by design).

  TAIL:  n_samples_above_5ms > max(ABSOLUTE_FLOOR, TAIL_MULT * median(peer counts))
         AND excess over that median >= ABSOLUTE_FLOOR.
         ABSOLUTE_FLOOR=20, TAIL_MULT=2.0.
         Catches paced_naive_R2 (108 vs med 30) without flagging siblings or
         clean paced_tuned (all zeros).

Exit 0 if clean (or <2 peers in a group — cannot audit). Exit 1 if any flag.
Prints a machine-readable summary line and a human table on stderr.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from array import array
from collections import defaultdict
from pathlib import Path

WALL_FRAC = 0.10
TAIL_MULT = 2.0
ABSOLUTE_FLOOR = 20
TAIL_US = 5000.0


def read_i64(path: Path) -> array:
    raw = path.read_bytes()
    n = len(raw) // 8
    a: array = array("q")
    a.frombytes(raw[: n * 8])
    return a


def n_above(samples: array, warmup: int, thr_us: float) -> int:
    thr_ns = int(thr_us * 1000)
    s = samples[warmup:]
    return sum(1 for v in s if v > thr_ns)


def n_windows(samples: array, warmup: int, thr_us: float, window: int = 1000) -> int:
    thr_ns = int(thr_us * 1000)
    s = samples[warmup:]
    hits = {i // window for i, v in enumerate(s) if v > thr_ns}
    return len(hits)


def median(xs: list[float]) -> float:
    if not xs:
        return 0.0
    ys = sorted(xs)
    return ys[len(ys) // 2]


def load_primaries(out: Path, warmup: int) -> list[dict]:
    rows = []
    for man_path in sorted(out.glob("*.manifest.json")):
        name = man_path.name[: -len(".manifest.json")]
        m = json.loads(man_path.read_text())
        tag = str(m.get("repeat_tag") or "")
        if not tag.startswith("R"):
            continue
        rtt = out / f"{name}.rtt.i64"
        above = windows = None
        if rtt.is_file():
            s = read_i64(rtt)
            above = n_above(s, warmup, TAIL_US)
            windows = n_windows(s, warmup, TAIL_US)
        rows.append(
            {
                "name": name,
                "mode": m.get("mode") or name.split("_")[0],
                "stack": m.get("stack") or "",
                "tag": tag,
                "wall_s": m.get("wall_s"),
                "n_above_5ms": above,
                "n_windows_5ms": windows,
            }
        )
    return rows


def audit(rows: list[dict]) -> list[dict]:
    groups: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        groups[f"{r['mode']}_{r['stack']}"].append(r)

    flags: list[dict] = []
    for key, peers in sorted(groups.items()):
        if len(peers) < 2:
            continue
        walls = [p["wall_s"] for p in peers if p["wall_s"] is not None]
        med_wall = median(walls) if walls else None
        aboves = [p["n_above_5ms"] for p in peers if p["n_above_5ms"] is not None]
        med_above = median([float(x) for x in aboves]) if aboves else None

        for p in peers:
            reasons = []
            if (
                med_wall is not None
                and p["wall_s"] is not None
                and med_wall > 0
                and p["wall_s"] > med_wall * (1.0 + WALL_FRAC)
            ):
                rel = (p["wall_s"] - med_wall) / med_wall
                reasons.append(
                    f"wall_s={p['wall_s']:.3f}s is {rel*100:.1f}% above peer median "
                    f"{med_wall:.3f}s (thr {WALL_FRAC*100:.0f}%)"
                )
            if (
                med_above is not None
                and p["n_above_5ms"] is not None
            ):
                thr = max(float(ABSOLUTE_FLOOR), TAIL_MULT * med_above)
                excess = p["n_above_5ms"] - med_above
                if p["n_above_5ms"] > thr and excess >= ABSOLUTE_FLOOR:
                    reasons.append(
                        f"n_above_5ms={p['n_above_5ms']} exceeds thr={thr:.0f} "
                        f"(2× peer median {med_above:.0f}, floor {ABSOLUTE_FLOOR}); "
                        f"windows={p['n_windows_5ms']}"
                    )
            if reasons:
                flags.append({"group": key, "name": p["name"], "reasons": reasons, "peer": p})
    return flags


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--warmup", type=int, default=10000)
    ap.add_argument(
        "--allow",
        action="store_true",
        help="report flags but exit 0 (BENCH_ALLOW_PEER_AUDIT=1 path)",
    )
    args = ap.parse_args()
    if not args.out_dir.is_dir():
        print(f"FAIL: peer audit: not a directory: {args.out_dir}", file=sys.stderr)
        return 2

    rows = load_primaries(args.out_dir, args.warmup)
    flags = audit(rows)

    # Always print a compact table for the log.
    print(f"peer-audit: groups_primaries={len(rows)} flags={len(flags)}", file=sys.stderr)
    by = defaultdict(list)
    for r in rows:
        by[f"{r['mode']}_{r['stack']}"].append(r)
    for key, peers in sorted(by.items()):
        for p in peers:
            print(
                f"  {p['name']:24s} wall={p['wall_s']!s:>8} "
                f">5ms={p['n_above_5ms']!s:>5} win5={p['n_windows_5ms']!s:>3}",
                file=sys.stderr,
            )

    if not flags:
        print("peer-audit: CLEAN — no disturbed primary peers", file=sys.stderr)
        return 0

    print("peer-audit: DISTURBED RUN(S) detected:", file=sys.stderr)
    for f in flags:
        for reason in f["reasons"]:
            print(f"  FAIL {f['name']}: {reason}", file=sys.stderr)
    print(
        "  A disturbed peer's p99.9 is not comparable to its siblings; "
        "do not publish this matrix as primary-table material.",
        file=sys.stderr,
    )
    if args.allow:
        print(
            "  BENCH_ALLOW_PEER_AUDIT: accepting contamination (smoke only).",
            file=sys.stderr,
        )
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
