#!/usr/bin/env bash
# Substantiates the `requires-python = ">=3.11"` claim in python/pyproject.toml.
#
# WHY THIS EXISTS AS A SCRIPT rather than a sentence in a README: the floor is a PACKAGING claim,
# and a packaging claim nobody executes is indistinguishable from a guess. The plan (Task 12
# Step 4) asks for it to be run on a real 3.11 interpreter; run once by hand it would rot the
# first time someone used a 3.12+ idiom in the shipped package.
#
# WHAT IS AND IS NOT CLAIMED, precisely — the distinction cost real time to establish:
#
#   * THE SHIPPED PACKAGE (`python/mmclient`) runs on 3.11. That is what `requires-python`
#     governs: what a consumer installs. Verified below by EXERCISING it — encode, decode, and a
#     real two-sided quoting decision — not merely importing it, because an import proves only
#     that the syntax parses.
#
#   * THE TEST SUITE AND DEV TOOLING REQUIRE 3.14, and deliberately. Two independent reasons:
#       - `python/tests/protocol_support.py` uses PEP 695 type parameters (`def f[Msg](...)`),
#         which is 3.12+;
#       - `ruff format` with `target-version = "py314"` REWRITES `except (A, B):` into PEP 758's
#         `except A, B:` — measured: parenthesising by hand and re-running the formatter reverts
#         it, while `ruff check --fix` leaves it alone. So the formatter actively enforces 3.14
#         syntax in every file it touches, tests and bench tooling included.
#
# Those two facts are why this script tests the PACKAGE on 3.11 and does not try to run the suite
# there. Claiming ">=3.11" for the whole repository would be false; claiming it for the package
# is true, and this is the evidence.
#
# Usage: scripts/check_python_floor.sh          (needs docker; skips loudly without it)
set -euo pipefail

FLOOR_IMAGE="${FLOOR_IMAGE:-python:3.11-slim}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

command -v docker > /dev/null 2>&1 || {
  echo "FATAL: docker is required to test the floor on a real $FLOOR_IMAGE interpreter." >&2
  echo "  The floor is a claim about an interpreter this host does not have; asserting it" >&2
  echo "  from the 3.14 venv would prove nothing." >&2
  exit 2
}

echo "--- python floor: exercising the shipped package on $FLOOR_IMAGE"
# Mounted READ-ONLY and copied inside, so an editable install cannot write egg-info into the
# repo — the container is a probe, not a build step.
docker run --rm -v "$REPO":/src:ro "$FLOOR_IMAGE" bash -lc '
  set -euo pipefail
  cp -r /src/python /tmp/py
  cd /tmp/py
  # No extras: the floor claim covers the package and its runtime dependency set, and the
  # `tuned` extra (picows/uvloop/msgspec) is explicitly a 3.14 measurement concern.
  pip install -q -e . > /dev/null
  python --version
  python - <<PYEOF
from mmclient import protocol
from mmclient.protocol import NewOrder
from mmclient.strategy import Strategy

# 1. Encode through the naive arm (stdlib json only — no compiled dependency).
order = NewOrder(v=1, seq=1, epoch=1, md_seq=2, cl_id="B-1", symbol="MOCKUSDT",
                 side="B", px=500000, qty=10, post_only=True)
raw = protocol.encode_naive(order)
assert raw.startswith(b"{\"t\":\"new_order\""), raw[:48]

# 2. Decode an engine frame.
tob = (b"{\"t\":\"top_of_book\",\"v\":1,\"seq\":1,\"epoch\":1,\"md_seq\":1,"
       b"\"symbol\":\"MOCKUSDT\",\"bid_px\":500000,\"bid_qty\":100,"
       b"\"ask_px\":500010,\"ask_qty\":80}")
book = protocol.decode_naive(tob)
assert book.bid_px == 500000 and book.ask_px == 500010

# 3. Make a real decision: the whole point is that the DECISION CORE runs here, since that is
#    what a consumer of this package would use.
strategy = Strategy(symbol="MOCKUSDT", qty=10, max_qty=100, stale_ns=10**9)
strategy.on_connect(1)
cmds = strategy.on_tob(book, now_ns=0)
assert [c.msg.side for c in cmds] == ["B", "S"], cmds
assert [c.msg.px for c in cmds] == [500000, 500010], cmds
print("floor OK: encode, decode, and a two-sided quote at the touch")
PYEOF
'
echo "python floor: ALL GREEN (package runs on ${FLOOR_IMAGE#python:}; the SUITE requires 3.14 — see this script's header)"
