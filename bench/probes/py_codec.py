#!/usr/bin/env python3
"""Re-runnable probe behind every absolute figure `mmclient/protocol.py` quotes.

Design-time probe, min of 7 x N iterations, never a deliverable number — the deliverable
latency matrix is Task 13's `bench/results/`, produced by `scripts/run_bench.sh` under the
pinned container. What this answers is narrower and is what the module's comments claim:
the codec's own cost on the canonical frame, the split between the shared preflight and
each arm's library work, the worst case the ONE remaining bound — the transport's 64 KiB
cap — permits, and the accept-set identity of the `_TOKEN` string production against the
per-character form it replaced.

The worst-case block is the reason this file must keep being run rather than quoted. The
Task-4 gate removed the scan-token budget, the per-token byte charge and the escape cap as
accept-set divergences from `cpp/src/frame_preflight.cpp` (PENDING_AMENDMENTS (p)7), so the
frames that block measures are no longer boundary cases of a cap this client chose: they are
simply the most expensive things the wire can carry, and their cost is a fact about pure
Python rather than a number anyone picked.

Run:  .venv/bin/python bench/probes/py_codec.py
      .venv/bin/python bench/probes/py_codec.py --fuzz     (adds the ~40 s differential)

Every figure is printed with the provenance line below it, so a stale comment in the
module can always be checked against a fresh run rather than believed.
"""

import argparse
import platform
import random
import re
import subprocess
import sys
import time
from collections.abc import Callable
from itertools import product
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "python"))

import json  # noqa: E402

import msgspec  # noqa: E402

from mmclient import _preflight, _rules, protocol  # noqa: E402

TOB = (REPO / "tests" / "golden" / "tob.json").read_bytes().strip()
NEW_ORDER = (REPO / "tests" / "golden" / "new_order.json").read_bytes().strip()
CAP = _preflight._MAX_FRAME_BYTES

REPEATS = 7


def provenance() -> str:
    try:
        sha = subprocess.run(
            ["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except OSError, subprocess.CalledProcessError:  # pragma: no cover - probe convenience
        sha = "unknown"
    return (
        f"provenance: min of {REPEATS} x N iterations | "
        f"Python {platform.python_version()} | msgspec {msgspec.__version__} | "
        f"{platform.system()} {platform.machine()} | repo {sha}"
    )


def bench(label: str, fn: Callable[[Any], Any], arg: Any, n: int) -> float:
    """Microseconds per call, min of REPEATS batches of n."""
    for _ in range(min(n, 100)):
        fn(arg)
    best = float("inf")
    for _ in range(REPEATS):
        start = time.perf_counter_ns()
        for _ in range(n):
            fn(arg)
        best = min(best, (time.perf_counter_ns() - start) / n)
    micros = best / 1000.0
    print(f"  {label:<46s} {micros:9.2f} us   (n={n})")
    return micros


def padded(total: int, escape: bool) -> bytes:
    """The canonical frame grown to exactly `total` bytes with one unknown string."""
    lead = b'{"zzz_unknown":"' + (b"\\n" if escape else b"")
    tail = b'",' + TOB[1:]
    frame = lead + b"x" * (total - len(lead) - len(tail)) + tail
    assert len(frame) == total
    return frame


def token_heavy(elements: int) -> bytes:
    return b'{"zzz_unknown":[' + b"1," * elements + b"0]," + TOB[1:]


def dense_escapes(count: int) -> bytes:
    return b'{"zzz_unknown":"' + b"\\n" * count + b'","' + TOB[2:]


# Decode cost has three axes — TOKENS, escapes and BYTES — and with only the byte cap left
# to bound any of them, the maximum is whichever axis the 64 KiB budget buys most of. Every
# shape below is therefore built AT the cap by `at_cap` rather than at a boundary chosen by
# this client, and `cost_load` prints what each one actually loaded, so which axis dominates
# is read off a run instead of argued.
def escaped_padded_tokens(elements: int, escapes_each: int, pad: int) -> bytes:
    """`elements` PADDED string tokens, each carrying `escapes_each` escapes.

    All three axes at once. Each element is ``3 + 2*escapes_each + pad`` bytes including its
    separator, so the element count spends the token axis and the padding spends the byte
    cap underneath it — the shape that used to be the maximum when a token budget capped the
    element count at 489, kept so the same frame family can still be compared across gates.
    """
    element = b'"' + b"\\n" * escapes_each + b"x" * pad + b'"'
    return b'{"zzz_unknown":[' + b",".join([element] * elements) + b"]," + TOB[1:]


def at_cap(build: Callable[[int], bytes], unit: int) -> bytes:
    """`build(n)` at the largest `n` that still fits the transport cap.

    The counts are DERIVED from the cap rather than written down, so this block cannot go
    stale against a fixture whose length changes: `unit` is the bytes one more element or
    escape costs, and `build(0)` is the frame's fixed overhead.
    """
    frame = build((CAP - len(build(0))) // unit)
    assert len(frame) <= CAP
    return frame


def cost_load(frame: bytes) -> str:
    """The three axes as absolute counts — there are no caps left to state them against."""
    tokens, pos = 0, 0
    while (match := _preflight._TOKEN.match(frame, pos)) is not None:
        tokens += 1
        pos = match.end()
    escapes = frame.count(b"\\")
    return f"{tokens} tokens, {escapes} escapes, {len(frame)}/{CAP} bytes"


# The per-character string production `_TOKEN` used to carry, kept here as the differential
# oracle: the possessive-run form that replaced it must accept exactly the same language.
_TOKEN_PER_CHAR = re.compile(
    rb"[ \t\n\r]*(?:"
    rb'(?P<str>"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*")'
    rb"|(?P<num>-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)"
    rb"|(?P<lit>true|false|null)"
    rb"|(?P<punct>[{}\[\]:,])"
    rb")"
)

_ALPHABET = [
    b'"',
    b"\\",
    b"\\\\",
    b'\\"',
    b"\\n",
    b"\\u0041",
    b"\\uD83D",
    b"\\x",
    b"a",
    b"\xc3\xa9",
    b"\x00",
    b"\x1f",
    b" ",
    b"\t",
    b"/",
    b"\\/",
    b"\\b",
    b"\\uZZZZ",
    b"\\u00",
    b"{",
    b"}",
    b"[",
    b"]",
    b":",
    b",",
    b"0",
    b"-0",
    b"07",
    b"1e3",
    b"true",
    b"null",
    b"NaN",
    b".7",
]
_NARROW = [b'"', b"\\", b"a", b"n", b"u", b"0", b"\x01", b"/"]


def verdict(rx: re.Pattern[bytes], frame: bytes, pos: int) -> Any:
    match = rx.match(frame, pos)
    if match is None:
        return None
    kind = match.lastgroup
    assert kind is not None  # every alternative of both patterns is a named group
    return kind, match.span(kind)


def token_accept_set_differential(cases: int) -> int:
    """0 differences is the claim `_preflight._TOKEN`'s comment makes."""
    diffs = 0
    rng = random.Random(20260728)
    for _ in range(cases):
        frame = b"".join(rng.choice(_ALPHABET) for _ in range(rng.randrange(1, 9)))
        for pos in (0, min(1, len(frame))):
            if verdict(_TOKEN_PER_CHAR, frame, pos) != verdict(_preflight._TOKEN, frame, pos):
                diffs += 1
    exhaustive = 0
    for length in range(1, 6):
        for combo in product(_NARROW, repeat=length):
            frame = b"".join(combo)
            exhaustive += 1
            if verdict(_TOKEN_PER_CHAR, frame, 0) != verdict(_preflight._TOKEN, frame, 0):
                diffs += 1
    print(f"  random cases {cases}, exhaustive cases {exhaustive}: {diffs} differences")
    return diffs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fuzz", action="store_true", help="run the _TOKEN differential too")
    args = parser.parse_args()

    print(provenance())
    bare_msgspec = msgspec.json.Decoder(protocol.EngineMsg).decode

    print("\ncanonical frame (tests/golden/tob.json), decode:")
    bench("bare msgspec Decoder.decode (no contract)", bare_msgspec, TOB, 20_000)
    bench("bare json.loads (no contract)", json.loads, TOB, 20_000)
    bench("_check_frame_bytes (byte policy)", _preflight._check_frame_bytes, TOB, 20_000)
    bench("_scan (flat-frame fast path)", _preflight._scan, TOB, 20_000)
    bench("_scan_general (the scan of record)", _preflight._scan_general, TOB, 20_000)
    bench("_prepare (whole shared preflight)", protocol._prepare, TOB, 20_000)
    bench("decode (product arm, full contract)", protocol.decode, TOB, 20_000)
    bench("decode_naive (baseline arm, full contract)", protocol.decode_naive, TOB, 20_000)

    print("\ncanonical frame (tests/golden/new_order.json), encode:")
    msg = protocol.NewOrder(
        v=1,
        seq=3,
        epoch=2,
        md_seq=41,
        cl_id="C-1",
        symbol="MOCKUSDT",
        side="B",
        px=499995,
        qty=100,
    )
    assert protocol.encode(msg) == NEW_ORDER
    bench(
        "_check_client_message (dispatch + three passes)",
        lambda m: _rules._check_client_message(m, protocol._CLIENT_RULES),
        msg,
        20_000,
    )
    bench("encode (product arm)", protocol.encode, msg, 20_000)
    bench("encode_naive (baseline arm)", protocol.encode_naive, msg, 20_000)

    bytes_only = padded(CAP, False)
    bytes_escaped = padded(CAP, True)
    tokens_at_cap = at_cap(token_heavy, 2)
    escapes_at_cap = at_cap(dense_escapes, 2)
    all_three_axes = at_cap(lambda n: escaped_padded_tokens(n, 2, 121), 128)

    print("\nworst case the 64 KiB transport cap permits (all of these are ACCEPTED frames):")
    print("  -- one axis at a time --")
    bench("64 KiB, one unknown string, no escape", protocol.decode, bytes_only, 200)
    bench("64 KiB, one unknown string, one escape", protocol.decode, bytes_escaped, 200)
    bench("64 KiB of scalar tokens", protocol.decode, tokens_at_cap, 5)
    bench("64 KiB of escapes", protocol.decode, escapes_at_cap, 20)
    print("  -- all three axes --")
    bench("64 KiB of padded escaped tokens", protocol.decode, all_three_axes, 20)
    print("  -- the same four on the naive arm --")
    bench("64 KiB no escape, naive arm", protocol.decode_naive, bytes_only, 200)
    bench("64 KiB one escape, naive arm", protocol.decode_naive, bytes_escaped, 200)
    bench("64 KiB of scalar tokens, naive arm", protocol.decode_naive, tokens_at_cap, 5)
    bench("64 KiB of escapes, naive arm", protocol.decode_naive, escapes_at_cap, 20)
    bench("64 KiB padded escaped tokens, naive arm", protocol.decode_naive, all_three_axes, 20)
    for label, frame in (
        ("bytes only", bytes_only),
        ("tokens", tokens_at_cap),
        ("escapes", escapes_at_cap),
        ("all three", all_three_axes),
    ):
        print(f"  cost load, {label:<20s} {cost_load(frame)}")

    if args.fuzz:
        print("\n_TOKEN accept-set differential (possessive-run form vs per-character form):")
        if token_accept_set_differential(400_000):
            print("FAIL: the two forms accept different languages")
            return 1
    print(f"\n{provenance()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
