#!/usr/bin/env bash
# One §5.2 matrix pass: both stacks × {idle, react, paced} × N interleaved repeats.
#
# INTERLEAVED A,B,A,B,A,B rather than all-A-then-all-B (record E-3). Running one arm to
# completion and then the other makes the comparison hostage to anything that drifts over the
# pass — thermal state, another process arriving, the OS deciding to index something. Alternating
# puts both arms on both sides of every drift, so a difference that survives is a difference
# between the arms rather than between two halves of an afternoon.
#
# ENGINE RESTARTED PER RUN, on an ephemeral port (F-09). A shared engine would carry one run's
# BenchRecorder state into the next, and a fixed port makes two concurrent passes fight over it.
#
# SIGTERM, NEVER SIGKILL (F-34/F-36). The engine writes its `--bench-out` dump during graceful
# shutdown, so killing it hard destroys the artifact the whole run existed to produce. The script
# signals, then WAITS for the dump to appear before treating the run as finished.
#
# Usage:  scripts/run_bench.sh [repeats] [samples] [warmup]
#   defaults: 3 repeats, 100000 samples, 10000 warm-up (the §5.2 primary floor)
#   MM_ENGINE=path  overrides the binary.
set -euo pipefail

REPEATS="${1:-3}"
SAMPLES="${2:-100000}"
WARMUP="${3:-10000}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="${MM_ENGINE:-$REPO/build/rel/mm_engine}"
# PYTHON, DISCOVERED rather than hardcoded, and overridable. The dev host keeps its venv at
# `.venv`; the authoritative ubuntu:26.04 image builds one at `/opt/venv` (docker/Dockerfile).
# Hardcoding `$REPO/.venv` made this script unrunnable inside the container — which for a
# benchmark is not a cosmetic problem, because the container is where the AUTHORITATIVE numbers
# are supposed to come from. MM_PYTHON wins over both so a reviewer can point it anywhere.
pick_python() {
  if [[ -n "${MM_PYTHON:-}" ]]; then printf '%s' "$MM_PYTHON"; return; fi
  for candidate in "$REPO/.venv/bin/python" /opt/venv/bin/python; do
    [[ -x "$candidate" ]] && { printf '%s' "$candidate"; return; }
  done
  printf '%s' "$REPO/.venv/bin/python"   # the host default, so the error names the usual cause
}
PY="$(pick_python)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$REPO/bench/results/$STAMP"
ENGINE_PID=""
ENGINE_PORT=""
ENGINE_LOG=""
RUNS_OK=0
RUNS_FAILED=0

for required in "$ENGINE" "$PY"; do
  [[ -e "$required" ]] || { echo "FAIL: missing $required" >&2; exit 2; }
done

# ── MATRIX LOCK ──────────────────────────────────────────────────────────────
# Contamination #4: a second matrix started on top of a running one because the
# orphan check greps `pgrep -x mm_engine` on the HOST while engines live inside a
# container (host sees 0). A lockfile is independent of process namespace.
#
# Location: repo-scoped by default (`bench/results/.run_bench.lock`) so two trees
# do not block each other; override with MM_BENCH_LOCK for a host-global lock.
# Contents: pid, start unix time, hostname, repo path — enough to decide STALE.
# Stale = lock exists but the recorded pid is not alive. A crashed run must not
# wedge the benchmark forever; a live holder must refuse a second start.
# Release on EXIT only if we still own the lock (pid match) — never delete a
# successor's lock after a hand-off.
BENCH_LOCK="${MM_BENCH_LOCK:-$REPO/bench/results/.run_bench.lock}"
BENCH_LOCK_OWNED=0
acquire_bench_lock() {
  mkdir -p "$(dirname "$BENCH_LOCK")"
  local holder_pid="" holder_host="" holder_started="" line
  if [[ -f "$BENCH_LOCK" ]]; then
    # Format (one key=value per line): pid=… started=… host=… repo=…
    holder_pid="$(sed -n 's/^pid=//p' "$BENCH_LOCK" 2>/dev/null | head -1)"
    holder_started="$(sed -n 's/^started=//p' "$BENCH_LOCK" 2>/dev/null | head -1)"
    holder_host="$(sed -n 's/^host=//p' "$BENCH_LOCK" 2>/dev/null | head -1)"
    if [[ -n "$holder_pid" ]] && kill -0 "$holder_pid" 2>/dev/null; then
      echo "FAIL: another run_bench matrix holds the lock (pid=$holder_pid host=${holder_host:-?} started=${holder_started:-?})." >&2
      echo "  Lock: $BENCH_LOCK" >&2
      echo "  A second matrix on top of a live one is contamination #4; wait for it to finish." >&2
      echo "  If you are certain the holder is not a matrix, stop it (SIGTERM) and re-run." >&2
      echo "  To knowingly accept (smoke only): MM_BENCH_LOCK= path that does not collide, or remove a proven-stale lock." >&2
      exit 6
    fi
    # Stale: pid dead or unreadable. Steal with a loud note — never wedge.
    echo "WARN: removing stale bench lock (holder pid=${holder_pid:-?} not alive): $BENCH_LOCK" >&2
    rm -f "$BENCH_LOCK"
  fi
  # Write-then-rename would be ideal; bash-portable create is enough for one operator host.
  # Refuse if another process raced us between the stale check and create.
  if ! ( set -o noclobber; {
      echo "pid=$$"
      echo "started=$(date +%s)"
      echo "host=$(hostname 2>/dev/null || echo unknown)"
      echo "repo=$REPO"
    } > "$BENCH_LOCK" ) 2>/dev/null; then
    echo "FAIL: could not acquire bench lock (raced?): $BENCH_LOCK" >&2
    exit 6
  fi
  BENCH_LOCK_OWNED=1
}
release_bench_lock() {
  if [[ "$BENCH_LOCK_OWNED" -eq 1 && -f "$BENCH_LOCK" ]]; then
    local cur
    cur="$(sed -n 's/^pid=//p' "$BENCH_LOCK" 2>/dev/null | head -1)"
    if [[ "$cur" == "$$" ]]; then
      rm -f "$BENCH_LOCK"
    fi
  fi
  BENCH_LOCK_OWNED=0
}
acquire_bench_lock

# REFUSE TO MEASURE ON A DIRTY HOST. An engine left over from an earlier pass keeps publishing on
# its feed timer with no client attached, so it competes for exactly the cores this benchmark is
# characterising. That is not a hypothetical: the leak fixed in start_engine left 50 of them on
# the dev host, and every number taken meanwhile was measured against unknown, growing load.
# Checking is cheap and the alternative is silently publishing contaminated percentiles.
# MATCHED ON THE EXECUTABLE NAME (`pgrep -x`), NOT THE COMMAND LINE (`pgrep -f`). `-f` matches any
# process whose full argv MENTIONS the engine — a shell running a script that names it, a
# `docker run` whose in-container command references it, an editor with the path in its title. It
# fired on exactly that: two hits that were a zsh and a docker client, no engine among them.
#
# A guard that cries wolf is worse than no guard. It teaches the next reader that its failures mean
# nothing, and the predictable sequence from there is ignore, then `|| true`, then delete — at which
# point the real orphan it exists to catch sails through. `-x` matches the process NAME exactly,
# which is the thing actually being asked about.
#
# LIMITATION (measured, GROK_INVESTIGATION.md): host `pgrep` cannot see engines inside a
# container. The matrix lock above is what stops contamination #4; this check still catches
# host-side orphans from a non-containerised pass.
if orphans="$(pgrep -x "$(basename "$ENGINE")" 2>/dev/null)" && [[ -n "$orphans" ]]; then
  echo "FAIL: $(printf '%s\n' "$orphans" | wc -l | tr -d ' ') stray $(basename "$ENGINE") process(es) already running." >&2
  echo "  A benchmark shares the host with them, so the numbers would not describe this build." >&2
  echo "  Inspect with: pgrep -xl $(basename "$ENGINE")" >&2
  echo "  Then stop them with SIGTERM (never KILL — TERM lets each write its pending dump)." >&2
  exit 3
fi

# REFUSE TO MEASURE ON A BUSY HOST (pre-flight). The orphan-engine check above only knows
# about host-visible engines. Concurrent work still matters:
#   * Run 2: the project's own test suite mid-matrix → a 63 ms stall.
#   * Run 3: a grok agent building images; paced_naive R2 alone had 108 samples >5 ms
#     while wall time was IDENTICAL to siblings (110.002 s) — pre-flight load was the
#     only host signal elevated (8.46).
#   * EXP burn_1x1 (GROK_INVESTIGATION.md): one foreign container at ~1 core
#     (container_cpu_sum ≈ 112%, host load ≈ 4.1 < ncpu/4=8) moved paced_tuned p99.9
#     from ~180 µs to 12.8 ms on ONE of three peers. Load did NOT fire; container CPU did.
#
# PRE-FLIGHT IS NECESSARY BUT NOT SUFFICIENT. Docker Desktop's VM also produces
# multi-millisecond stalls with no elevated load and no foreign CPU (quiet-host matrix
# with idle_tuned_R1 wall 11.0 s vs siblings 9.4 s). Those are caught AFTER the fact by
# scripts/bench_peer_audit.py (wall + >5 ms peer rules), not here.
#
# TWO PRE-FLIGHT SIGNALS, both overridable, both loud when overridden:
#   1. Container CPU IMPACT (docker stats), NOT container COUNT.
#      Threshold CONTAINER_CPU_BUSY_PCT — MEASURED (exp series in
#      scripts/investigate_artifacts/, GROK_INVESTIGATION.md §thresholds):
#        * Quiet permanent infra (k8s staging + CI proxy + idle buildx): 16–20% of one core.
#        * Half-core synthetic burner alone: ≈50% of one core (docker stats).
#        * One-core synthetic burner: ≈100–115% total; produced severe tail damage.
#        * Four-core burner: ≈400%+; load also rose above ncpu/4.
#      Set to 50 (half a core of foreign container work): quiet infra still has ~3× margin;
#      a full-core concurrent burner is refused with margin against stats under-report.
#      The previous 100 was a single-core ceiling with ZERO margin against the burn_1x1
#      contamination we measured. Units: docker stats % of one core; sum across containers.
#      Probe BOUND (timeout). Failure/timeout = UNKNOWN, never "fine".
#      Skipped when WE are inside a container (authoritative matrix path).
#   2. 1-minute load average vs ncpu/4.
#      Kept at ncpu/4: Run-3 contamination was 8.46 on 32 cores; quiet developer hosts with
#      permanent staging sit at load 3–6 and must still be able to pass. Strengthening to
#      ncpu/8 (=4 on this box) would false-positive on a normal staging-cluster machine.
#      EMPIRICALLY a weak predictor of tail quality for container CPU burn (burn_1x1
#      contaminated at load 4.1). Kept as a coarse "machine on fire" ceiling only.
# Load is read from `uptime` (portable macOS + Linux).
#
# Overrides: BENCH_ALLOW_CONTAINERS=1 / BENCH_ALLOW_LOAD=1 / BENCH_ALLOW_PEER_AUDIT=1.
# Each prints a banner so an overridden pass cannot be quietly published as primary-table.
in_container() {
  [[ -f /.dockerenv ]] && return 0
  # cgroup v1/v2: docker, containerd, kubepods, podman (libpod). Host cgroup is typically just "/".
  [[ -r /proc/1/cgroup ]] || return 1
  grep -qE 'docker|containerd|kubepods|libpod|lxc|podman' /proc/1/cgroup 2>/dev/null
}

read_ncpu() {
  # getconf is POSIX and works on macOS + Linux; fall through for odd environments.
  local n
  n="$(getconf _NPROCESSORS_ONLN 2>/dev/null)" || n=""
  [[ -n "$n" && "$n" -gt 0 ]] 2>/dev/null && { printf '%s' "$n"; return; }
  n="$(sysctl -n hw.ncpu 2>/dev/null)" || n=""
  [[ -n "$n" && "$n" -gt 0 ]] 2>/dev/null && { printf '%s' "$n"; return; }
  n="$(nproc 2>/dev/null)" || n=""
  [[ -n "$n" && "$n" -gt 0 ]] 2>/dev/null && { printf '%s' "$n"; return; }
  printf '1'
}

read_loadavg_1m() {
  # macOS: "load averages: 1.23 4.56 7.89"
  # Linux: "load average: 1.23, 4.56, 7.89"
  # Strip commas so the first token is always a bare float.
  uptime | sed -n 's/.*load average[s]*: *//p' | tr -d ',' | awk '{print $1}'
}

# Bound a command that can hang (docker stats on a wedged daemon). Prefer GNU timeout /
# gtimeout; fall back to perl alarm (present on macOS system perl and most Linux images).
# Returns the command's exit status; 124 mirrors timeout(1) for the alarm path.
# Uses `cmd && return 0 || return $?` so a non-zero child does not trip set -e (a failing
# command in an &&/|| list is exempt) and so we never toggle the caller's errexit flag.
run_bounded() {
  local secs="$1"
  shift
  local rc=0
  if command -v timeout >/dev/null 2>&1; then
    timeout "$secs" "$@" && return 0 || return $?
  elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout "$secs" "$@" && return 0 || return $?
  elif command -v perl >/dev/null 2>&1; then
    # perl alarm: 0 on success, 142 (128+SIGALRM) on fire — map alarm to 124 like timeout(1).
    perl -e 'alarm shift; exec @ARGV' "$secs" "$@" && return 0 || rc=$?
    # 142 = 128 + 14 (SIGALRM) on most unices; also accept 14 if wait status is bare.
    if [[ "$rc" -eq 142 || "$rc" -eq 14 ]]; then
      return 124
    fi
    return "$rc"
  else
    # No bound available — still probe; a hang is worse UX than a missing timeout helper.
    "$@" && return 0 || return $?
  fi
}

# docker stats CPU% is "% of one core". Sum across all running containers.
# Threshold 50: measured quiet infra 16–20%; 1-core burner ~112% wrecked p99.9
# (scripts/investigate_artifacts/exp_burn_1x1). See GROK_INVESTIGATION.md.
CONTAINER_CPU_BUSY_PCT=50
# Probe budget: docker stats --no-stream samples once; healthy daemon is sub-second to a few
# seconds. 15s is well above healthy, well below "wedged forever".
CONTAINER_STATS_TIMEOUT_S=15

BUSY_HOST_WARNED=0
busy_host_banner() {
  # Printed once, into the run's own stdout (not only stderr), so it lands in any captured log.
  if [[ "$BUSY_HOST_WARNED" -eq 0 ]]; then
    BUSY_HOST_WARNED=1
    printf '\n'
    printf '!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n'
    printf '!!  BENCH_ALLOW_* OVERRIDE ACTIVE — NUMBERS ARE NOT PRIMARY-TABLE     !!\n'
    printf '!!  MATERIAL. Do not publish this run into the §5.2 matrix.           !!\n'
    printf '!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n'
    printf '\n'
  fi
}

# container_probe: quiet | busy | unknown | skipped
#   quiet   — stats succeeded; sum <= threshold (including zero containers)
#   busy    — stats succeeded; sum > threshold
#   unknown — docker missing from PATH is "skipped"; stats fail/timeout is unknown
#   skipped — we are inside a container, or no docker binary
container_probe="skipped"
container_n=0
container_cpu_sum="0.00"
container_stats_out=""

if ! in_container; then
  if command -v docker >/dev/null 2>&1; then
    # THREE STATES. A failed/timed-out probe is UNKNOWN, never "zero impact" / fine.
    # (A false-positive gate is worse than no gate — so is a false-negative from a dead probe.)
    stats_rc=0
    set +e
    container_stats_out="$(run_bounded "$CONTAINER_STATS_TIMEOUT_S" \
      docker stats --no-stream --format $'{{.Name}}\t{{.CPUPerc}}' 2>/dev/null)"
    stats_rc=$?
    set -e
    if [[ "$stats_rc" -eq 0 ]]; then
      # Parse Name\tCPUPerc lines; CPUPerc looks like "12.34%" or "0.00%".
      # awk owns the float math; bash cannot compare floats.
      read -r container_n container_cpu_sum <<EOF
$(printf '%s\n' "$container_stats_out" | awk -F'\t' '
  {
    cpu = $NF
    gsub(/%/, "", cpu)
    if (cpu ~ /^[0-9]+(\.[0-9]+)?$/) { s += cpu + 0; n++ }
  }
  END { printf "%d %.2f\n", n + 0, s + 0 }
')
EOF
      if awk -v sum="$container_cpu_sum" -v thr="$CONTAINER_CPU_BUSY_PCT" \
          'BEGIN { exit !(sum > thr) }'; then
        container_probe="busy"
        if [[ "${BENCH_ALLOW_CONTAINERS:-}" == "1" ]]; then
          busy_host_banner
          echo "WARN: container CPU impact ${container_cpu_sum}% of one core across ${container_n} container(s) exceeds threshold ${CONTAINER_CPU_BUSY_PCT}%; BENCH_ALLOW_CONTAINERS=1 accepts the contamination." >&2
          echo "  Inspect with: docker stats --no-stream" >&2
        else
          echo "FAIL: container CPU impact ${container_cpu_sum}% of one core across ${container_n} container(s) exceeds threshold ${CONTAINER_CPU_BUSY_PCT}%." >&2
          echo "  A latency matrix shares cores with whatever they are doing; numbers would not describe this build." >&2
          echo "  Top consumers:" >&2
          printf '%s\n' "$container_stats_out" \
            | awk -F'\t' '{ cpu=$NF; gsub(/%/,"",cpu); printf "%8s  %s\n", $NF, $1 }' \
            | sort -k1 -gr \
            | head -n 5 \
            | sed 's/^/    /' >&2
          echo "  Inspect with: docker stats --no-stream" >&2
          echo "  Stop the hot work (or re-run inside the authoritative bench container — container check is skipped there)." >&2
          echo "  To knowingly accept contamination (smoke only): BENCH_ALLOW_CONTAINERS=1 $0 $*" >&2
          exit 4
        fi
      else
        container_probe="quiet"
      fi
    else
      container_probe="unknown"
      if [[ "$stats_rc" -eq 124 ]]; then
        echo "WARN: container CPU probe timed out after ${CONTAINER_STATS_TIMEOUT_S}s (docker stats hang?) — UNKNOWN, not treated as quiet." >&2
      else
        echo "WARN: container CPU probe failed (docker stats rc=${stats_rc}) — UNKNOWN, not treated as quiet." >&2
      fi
      echo "  Inspect with: docker stats --no-stream; docker info" >&2
      echo "  Proceeding: a dead probe must not look like 'zero impact', but it also must not hard-block every run when the daemon is merely unhappy." >&2
    fi
  fi
fi

ncpu="$(read_ncpu)"
load1="$(read_loadavg_1m)"
load_thr=""
if [[ -n "$load1" && -n "$ncpu" ]]; then
  load_thr="$(awk -v ncpu="$ncpu" 'BEGIN { printf "%.2f", ncpu / 4.0 }')"
  # ncpu/4: see the block comment. awk does the float compare; bash cannot.
  if awk -v load="$load1" -v ncpu="$ncpu" 'BEGIN { exit !(load > ncpu / 4.0) }'; then
    if [[ "${BENCH_ALLOW_LOAD:-}" == "1" ]]; then
      busy_host_banner
      echo "WARN: 1-min load average $load1 exceeds threshold $load_thr (ncpu/4, ncpu=$ncpu); BENCH_ALLOW_LOAD=1 accepts the contamination." >&2
      echo "  Inspect with: uptime; top" >&2
    else
      echo "FAIL: 1-min load average $load1 exceeds threshold $load_thr (ncpu/4 of $ncpu cores)." >&2
      echo "  A latency matrix needs headroom; this host is too busy for trustworthy percentiles." >&2
      echo "  Inspect with: uptime; top / Activity Monitor" >&2
      echo "  Wait for load to drop below $load_thr, stop competing work, then re-run." >&2
      echo "  To knowingly accept contamination (smoke only): BENCH_ALLOW_LOAD=1 $0 $*" >&2
      exit 5
    fi
  fi
fi

# Host state always lands in the run log when preflight passes (or is overridden through).
# Answers "was this run clean?" without re-deriving from per-process CPU across repeats.
# For unknown/skipped, do not print a numeric cpu_sum of 0.00 — that reads as "fine".
case "$container_probe" in
  quiet|busy) _cpu_disp="${container_cpu_sum}% (n=${container_n})" ;;
  unknown)    _cpu_disp="UNKNOWN" ;;
  *)          _cpu_disp="skipped" ;;
esac
echo "host-preflight: containers=${container_probe} cpu=${_cpu_disp} thr=${CONTAINER_CPU_BUSY_PCT}% | load1=${load1:-?} ncpu=${ncpu} thr_load=${load_thr:-?} | overrides containers=${BENCH_ALLOW_CONTAINERS:-0} load=${BENCH_ALLOW_LOAD:-0}" >&2

mkdir -p "$OUT"

# The engine is stopped on EVERY exit path. An orphan holds its telemetry file and its dump, and
# the next run would fail for a reason that has nothing to do with the code under test.
# The matrix lock is released only if we still own it (see release_bench_lock).
cleanup() {
  if [[ -n "$ENGINE_PID" ]] && kill -0 "$ENGINE_PID" 2>/dev/null; then
    kill -TERM "$ENGINE_PID" 2>/dev/null || true
    for _ in $(seq 100); do kill -0 "$ENGINE_PID" 2>/dev/null || break; sleep 0.1; done
    # KILL only as the last resort, and it forfeits the dump — which is why the wait above is
    # ten seconds rather than one.
    kill -KILL "$ENGINE_PID" 2>/dev/null || true
  fi
  release_bench_lock
}
trap cleanup EXIT

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }

# Starts an engine and publishes BOTH the pid and the port it bound through GLOBALS.
#
# WHY GLOBALS AND NOT AN ECHOED PORT (F-41, and it cost the whole first matrix). This function
# used to `echo "$port"` and be called as `port="$(start_engine ...)"`. Command substitution runs
# its body in a SUBSHELL, so `ENGINE_PID=$!` was assigned in a child and the parent's ENGINE_PID
# stayed empty forever. Everything downstream that guards on it then became a no-op:
#
#   * `stop_engine_and_wait_for_dump` opens with `[[ -n "$ENGINE_PID" ]] || return 0`, so it
#     returned IMMEDIATELY — the engine was never signalled, never shut down gracefully, and
#     therefore never wrote its `--bench-out` dump. Every dump in every run was 0 bytes, which
#     is to say the §5.1 M3 measurement did not exist while appearing to.
#   * it also returned before its own "no bench dump" warning, so the failure was silent.
#   * the `cleanup` EXIT trap was dead for the same reason, so every engine LEAKED. Each one
#     keeps publishing on its `--interval-ms` timer with no client attached, so a pass of N runs
#     ends with N engines competing for the cores the benchmark is measuring — measured: 50 live
#     engines on the dev host, and a 22-run matrix whose last runs shared 8 cores with ~21
#     orphans. The interleaving still protected the WITHIN-PAIR direction, but the absolute
#     numbers and the run-to-run spread were contaminated in run order.
#
# `--port 0` and read the port back: no probe-then-bind window for two concurrent passes to both
# win.
start_engine() {
  local feed="$1" interval="$2" loop_flag="$3" bench_out="$4" telemetry="$5" codec="$6"
  ENGINE_LOG="$(mktemp -t mm_bench_engine.XXXXXX)"
  # shellcheck disable=SC2086  # loop_flag is deliberately word-split: it is "" or "--loop"
  "$ENGINE" --feed "$feed" --port 0 --codec "$codec" --interval-ms "$interval" \
            --telemetry-out "$telemetry" --bench-out "$bench_out" $loop_flag \
            > "$ENGINE_LOG" 2>&1 &
  ENGINE_PID=$!
  ENGINE_PORT=""
  for _ in $(seq 200); do
    ENGINE_PORT="$(sed -n 's/^mm_engine .* listening port=\([0-9]*\).*/\1/p' "$ENGINE_LOG" | head -1)"
    [[ -n "$ENGINE_PORT" ]] && break
    sleep 0.05
  done
  [[ -n "$ENGINE_PORT" ]] || {
    echo "FAIL: engine never reported a port" >&2
    cat "$ENGINE_LOG" >&2
    exit 1
  }
  # THE ARM IS VERIFIED FROM THE ENGINE'S OWN STARTUP LINE, not assumed from the flag we passed.
  # `--codec tuned` was hardcoded here for both arms, so the "naive" arm was
  # websockets+asyncio+stdlib-json against a GLAZE engine — three of the four ratified components
  # swapped and the C++ leg silently held at the tuned one. The manifest recorded `stack` but not
  # the engine's codec, so nothing in the artifact set could contradict it. Reading the arm back
  # from the process makes that class of mismatch impossible rather than merely unlikely.
  local seen
  seen="$(sed -n 's/^mm_engine .* codec=\([a-z]*\) .*/\1/p' "$ENGINE_LOG" | head -1)"
  [[ "$seen" == "$codec" ]] || {
    echo "FAIL: asked the engine for --codec $codec but it reports codec=${seen:-<none>}" >&2
    cat "$ENGINE_LOG" >&2
    exit 1
  }
}

# Signals the engine and waits for its dump, so a run is not called finished before its artifact
# exists. A dump that is still growing is worse than a missing one: it looks complete.
#
# A MISSING DUMP NOW FAILS THE RUN (nonzero return) rather than printing a note. §5.1 requires M3,
# which lives only in this artifact, so a run without one has not produced the measurement it was
# asked for. The previous `note` was unreachable anyway (see start_engine), and even reachable it
# would have been the wrong severity: 22 warnings scrolled past under a closing `runs_ok=22`
# banner is indistinguishable from success.
stop_engine_and_wait_for_dump() {
  local dump="$1"
  [[ -n "$ENGINE_PID" ]] || { note "BUG: stop called with no engine pid"; return 1; }
  kill -TERM "$ENGINE_PID" 2>/dev/null || true
  for _ in $(seq 100); do kill -0 "$ENGINE_PID" 2>/dev/null || break; sleep 0.1; done
  # KILL only if it ignored TERM; that forfeits the dump, which the size check below then catches.
  kill -KILL "$ENGINE_PID" 2>/dev/null || true
  ENGINE_PID=""
  local last=-1 now=0
  for _ in $(seq 50); do
    now=$( [[ -f "$dump" ]] && wc -c < "$dump" || echo 0 )
    [[ "$now" -gt 0 && "$now" -eq "$last" ]] && return 0
    last="$now"
    sleep 0.1
  done
  note "FAIL: no bench dump at $dump — M3 was not measured for this run"
  return 1
}

# A dump that is non-empty is not the same as a dump that is USABLE. The wait above only proves
# bytes arrived and stopped arriving, so a dump truncated mid-write — the engine killed partway,
# a full disk — stabilises at a plausible size and passes. The failure then surfaces much later,
# inside `summarize`, while THIS script has already printed "ALL GREEN (22 runs, each with client
# series AND an engine dump)". That is the overclaiming shape this file has already been bitten by
# once, so the check is on whether the artifact PARSES.
#
# Validated with the harness's own reader rather than a size rule reimplemented here: the dump
# format (a six-word header, then four int64 streams whose lengths the header declares) has exactly
# one authoritative parser, and a second copy of it would be free to drift.
dump_is_parseable() {
  local dump="$1"
  PYTHONPATH="$REPO/bench" "$PY" -B -c '
import sys
from pathlib import Path
from harness.summarize import read_engine_dump
try:
    d = read_engine_dump(Path(sys.argv[1]))
except ValueError as exc:
    print(f"unparseable: {exc}", file=sys.stderr)
    sys.exit(1)
# SATURATION FAILS THE RUN. `refused > 0` means the recorder hit capacity and DROPPED samples,
# so the streams describe only the part of the run that fit -- `summarize` refuses such a dump
# outright (EngineDump.is_suspect). Printing the count and returning success meant a saturated
# run counted toward RUNS_OK and reached the closing "ALL GREEN", and the contradiction only
# surfaced later when someone tried to summarise it. (A fan-out dump needs no check here:
# read_engine_dump itself refuses peak_sessions > 1, so this call already raises on it.)
if d.is_suspect:
    print(f"saturated: the recorder refused {d.saturated} sample(s) -- streams are truncated",
          file=sys.stderr)
    sys.exit(1)
print(f"svc={len(d.svc)} m0_m0p={len(d.m0_m0p)} m0p_m3={len(d.m0p_m3)} m0_m3={len(d.m0_m3)} "
      f"refused={d.saturated} peak_sessions={d.peak_sessions}")
' "$dump"
}

one_run() {
  local stack="$1" mode="$2" tag="$3" rate="$4"
  local name="${mode}_${stack}_${tag}"
  local dump="$OUT/$name.engine.bench"
  local telemetry="$OUT/$name.telemetry.jsonl"
  local feed interval loop_flag

  case "$mode" in
    # The idle feed opens a 3 s connect window, publishes ONE book, then goes silent for an
    # hour: M1 is measured with no market-data work on the owner thread at all (record A3).
    idle)  feed="$REPO/bench/scenarios/bench_idle.feed";  interval=1; loop_flag="" ;;
    # react and paced both need a book on every tick, which is what bench_paced.feed under
    # --loop delivers at --interval-ms 1 (≈1k TOB/s).
    *)     feed="$REPO/bench/scenarios/bench_paced.feed"; interval=1; loop_flag="--loop" ;;
  esac

  # THE ENGINE'S CODEC IS THE ARM, exactly as the decision record ratifies it: the §6 swap is
  # `websockets + asyncio + stdlib json + nlohmann` -> `picows + uvloop + msgspec + glaze` on the
  # SAME BINARY via the codec flag (02-decision-record.md). The C++ leg therefore has to move with
  # the Python leg; holding it at one codec measures three of the four components.
  local codec="$stack"

  # Called DIRECTLY, not through `$( )`: the pid and port arrive in globals, because a subshell
  # would strand ENGINE_PID and leak the engine (see start_engine).
  start_engine "$feed" "$interval" "$loop_flag" "$dump" "$telemetry" "$codec"
  note "$name: engine on 127.0.0.1:$ENGINE_PORT (pid $ENGINE_PID, codec=$codec)"

  local rc=0
  PYTHONPATH="$REPO/bench" "$PY" -B -m harness.run_bench \
      --mode "$mode" --stack "$stack" --url "ws://127.0.0.1:$ENGINE_PORT" \
      --out "$OUT/$name" --rate "$rate" --warmup "$WARMUP" --samples "$SAMPLES" \
      --repeat-tag "$tag" --interval-ms "$interval" --engine "$ENGINE" \
      --engine-codec "$codec" --engine-pid "$ENGINE_PID" --timeout 900 || rc=$?
  # The dump is part of the run's deliverable, so its absence fails the run exactly as a client
  # error does — M3 comes from nowhere else.
  stop_engine_and_wait_for_dump "$dump" || rc=$(( rc == 0 ? 90 : rc ))
  if [[ "$rc" -eq 0 ]]; then
    local shape
    if shape="$(dump_is_parseable "$dump")"; then
      note "$name: dump OK — $shape"
    else
      note "$name: FAIL — the bench dump does not parse; M2/M3 are unusable for this run"
      rc=91
    fi
  fi
  if [[ "$rc" -ne 0 ]]; then
    note "$name: FAILED (rc=$rc) — excluded from the tables"
    RUNS_FAILED=$(( RUNS_FAILED + 1 ))
  else
    RUNS_OK=$(( RUNS_OK + 1 ))
  fi
  return 0  # a failed run is recorded and skipped, never fatal to the pass
}

say "§5.2 matrix — $REPEATS repeats, $WARMUP warm-up + $SAMPLES samples, out=$OUT"
note "engine: $($ENGINE --version | head -1) / $($ENGINE --version | sed -n '2p')"

# PRIMARY tables. Interleaved per repeat so the two arms share every drift.
for r in $(seq 1 "$REPEATS"); do
  for mode in idle react paced; do
    for stack in naive tuned; do
      one_run "$stack" "$mode" "$(printf 'R%d' "$r")" 1000
    done
  done
done

# SECONDARY probes, labelled non-primary in every table (F-10): they answer "do queues build",
# not "what is p99". One repeat each — a percentile is not being claimed.
say "Secondary probes (non-primary: saturation behaviour, not percentiles)"
for stack in naive tuned; do
  for rate in 5000 10000; do
    SAMPLES_SAVE="$SAMPLES"; SAMPLES=20000
    one_run "$stack" "paced" "P${rate}" "$rate"
    SAMPLES="$SAMPLES_SAVE"
  done
done

# ONE AFFIRMATIVE CLOSING LINE, and a nonzero exit if any run failed. The first matrix reported
# `runs_ok=22 runs_failed=0` from a wrapper that counted only the client's exit status, while all
# 22 engine dumps were empty — the tally has to be computed from the same predicate the tables
# depend on, or it is just a comforting banner.
say "Matrix finished — ok=$RUNS_OK failed=$RUNS_FAILED — $OUT"
if [[ "$RUNS_FAILED" -ne 0 ]]; then
  echo "FAIL: $RUNS_FAILED run(s) did not produce a complete artifact set; the tables are incomplete." >&2
  exit 1
fi

# POST-HOC PEER AUDIT. Pre-flight cannot see mid-matrix Docker Desktop stalls or a
# paced contamination that leaves wall time unchanged (contamination #3: 108 samples
# >5 ms, wall identical). scripts/bench_peer_audit.py compares primary (R*) peers on
# wall_s and n_above_5ms. Thresholds measured on the authoritative matrix + controlled
# burns — see GROK_INVESTIGATION.md. Does not rewrite artifacts; refuses to call the
# matrix primary-table clean.
say "Post-hoc peer audit (wall + >5ms cluster vs siblings)"
PEER_RC=0
PEER_ARGS=()
if [[ "${BENCH_ALLOW_PEER_AUDIT:-}" == "1" ]]; then
  busy_host_banner
  PEER_ARGS+=(--allow)
fi
# Empty PEER_ARGS must expand to zero words (not one empty word) under set -u.
if [[ ${#PEER_ARGS[@]} -gt 0 ]]; then
  if ! "$PY" -B "$REPO/scripts/bench_peer_audit.py" \
      "$OUT" --warmup "$WARMUP" "${PEER_ARGS[@]}"; then
    PEER_RC=1
  fi
else
  if ! "$PY" -B "$REPO/scripts/bench_peer_audit.py" \
      "$OUT" --warmup "$WARMUP"; then
    PEER_RC=1
  fi
fi
if [[ "$PEER_RC" -ne 0 ]]; then
  echo "FAIL: peer audit found disturbed primary repeat(s); matrix is not primary-table material." >&2
  echo "  Artifacts retained at $OUT for inspection. Re-run on a quieter host, or smoke with BENCH_ALLOW_PEER_AUDIT=1." >&2
  exit 1
fi

note "bench matrix: ALL GREEN ($RUNS_OK runs, each with client series AND an engine dump; peer audit clean)"
note "summarise one run with:"
note "  PYTHONPATH=$REPO/bench $PY -m harness.summarize \\"
note "    --rtt $OUT/<name>.rtt.i64 --actual $OUT/<name>.actual.i64 \\"
note "    --lag $OUT/<name>.lag.i64 --engine $OUT/<name>.engine.bench \\"
note "    --warmup $WARMUP --require-samples $SAMPLES --mode <mode> --md"
