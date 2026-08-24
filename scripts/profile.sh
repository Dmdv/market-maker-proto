#!/usr/bin/env bash
# The §6 profiling exhibit (plan Task 13 Step 2b, record A1): a flamegraph of the engine under a
# real tuned client, captured IN-CONTAINER because that is where the authoritative numbers come
# from and a macOS profile would characterise a different kernel, allocator and scheduler.
#
# WHY THESE EXACT perf ARGUMENTS, since each one is a constraint rather than a preference:
#
#   -e task-clock    A SOFTWARE event. The PMU is not virtualized in Docker Desktop's VM
#                    [measured], so `cycles` — the reflex choice — silently collects nothing.
#                    task-clock is sampled by the kernel timer and works under virtualization.
#   -F 999           Not 1000. A round frequency risks lock-step with a periodic workload; this
#                    engine ticks its feed on a millisecond timer, and sampling at exactly that
#                    rate would alias the tick into or out of every sample.
#   --call-graph dwarf,65528
#                    Frame pointers are omitted at -O3, so `fp` unwinding truncates stacks
#                    inside the very code being profiled. dwarf costs more per sample and is the
#                    only option that produces usable stacks from a release build. The explicit
#                    65528 stack-chunk size is the max perf accepts; the default (8192) is
#                    enough for shallow stacks but truncates deep boost/asio unwind chains.
#   /usr/bin/perf    Invoked DIRECTLY. ubuntu:26.04 ships perf via the `linux-perf` package as a
#                    real binary with no kernel-version wrapper (F-25 probe) — the `perf` on PATH
#                    on some distributions is a shim that refuses to run when the running kernel
#                    does not match the package version, which inside a VM it never does.
#   --privileged     REQUIRED on Docker Desktop, not a convenience. Measured chain of necessity:
#                      1. Under the default workload the engine spends most on-CPU time in the
#                         kernel network path (tcp_sendmsg / softirq / …). task-clock therefore
#                         lands predominantly on [k] samples — that is the real attribution.
#                      2. At perf_event_paranoid=2 (Docker Desktop VM default), /proc/kallsyms
#                         returns zeroed addresses, so every kernel frame becomes [unknown].
#                      3. Lowering paranoid requires writing the VM sysctl. --cap-add PERFMON
#                         alone cannot: /proc/sys is read-only for non-privileged containers.
#                         --cap-add SYS_ADMIN also cannot write that sysctl on Desktop.
#                         Only --privileged makes the write stick (measured).
#                    DEBUGINFOD_URLS is also cleared inside the container: ubuntu:26.04 sets
#                    it to https://debuginfod.ubuntu.com by default, and perf then hangs
#                    indefinitely in buildid-list / report trying to fetch over the network.
#
# The run profiled is TUNED IDLE: the arm the §6 result is claimed for, in the mode with no
# market-data traffic competing for the owner thread, so what dominates the profile is the
# request/response path rather than the feed.
#
# Usage: scripts/profile.sh [output-dir]
#   default output: bench/results/<utc-stamp>/flame/
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${1:-$REPO/bench/results/$STAMP/flame}"
IMAGE="${MM_IMAGE:-mm-engine-verify:aarch64}"
SAMPLES="${PROFILE_SAMPLES:-5000}"
WARMUP="${PROFILE_WARMUP:-2000}"

command -v docker > /dev/null 2>&1 || {
  echo "FATAL: docker is required — the profile must be taken in the pinned image, not on the host." >&2
  exit 2
}

# The image must exist AND carry the current sources, for the same reason verify_linux.sh checks:
# a profile of a stale image describes code nobody is running. Same question, asked here too.
if ! docker image inspect "$IMAGE" > /dev/null 2>&1; then
  echo "FATAL: image $IMAGE not present. Run scripts/verify_linux.sh first (it builds it)." >&2
  exit 2
fi
# FAIL RATHER THAN HASH A PARTIAL TREE. `git ls-files` lists what the INDEX contains, so if a
# tracked file is missing from the working tree — deleted by hand, or by a wrapper that cleaned an
# output directory whose contents happen to be committed — `shasum` errors on it, the pipeline
# still yields a digest built from the remaining files, and the comparison below then reports a
# mismatch whose stated cause ("the image does not carry the working tree") is wrong. Checking
# shasum's own status turns a misleading mismatch into an accurate complaint.
if ! TREE_SHA="$(git -C "$REPO" ls-files -z | xargs -0 shasum | shasum | cut -d' ' -f1)"; then
  echo "FATAL: could not hash the working tree — a tracked file is missing from it." >&2
  echo "  Restore it (git checkout -- <path>) before profiling; a partial hash would" >&2
  echo "  report a stale-image mismatch that is not the real problem." >&2
  exit 2
fi
IMAGE_SHA="$(docker inspect --format '{{index .Config.Labels "mm.tree_sha"}}' "$IMAGE" 2>/dev/null || true)"
if [[ "$TREE_SHA" != "$IMAGE_SHA" ]]; then
  echo "FATAL: $IMAGE does not carry the working tree." >&2
  echo "  working tree: $TREE_SHA" >&2
  echo "  image label : ${IMAGE_SHA:-<absent>}" >&2
  echo "  A profile of a stale image describes code you are not running. Rebuild first." >&2
  exit 1
fi

mkdir -p "$OUT"
echo "--- profiling the tuned idle arm in $IMAGE (out: $OUT)"

NAME="mm-profile-$$"
cleanup() { docker rm -f "$NAME" > /dev/null 2>&1 || true; }
trap cleanup EXIT

# CPU/memory bounded for the same reason the asan build is (see ASAN_BUILD_JOBS): an unbounded
# container has twice preceded a wedged daemon on this host.
# --privileged: see header comment. PERFMON alone leaves kallsyms addresses zeroed.
docker run -d --name "$NAME" --cpus 6 --memory 8g --privileged \
  -e DEBUGINFOD_URLS= \
  -v "$OUT":/out "$IMAGE" bash -lc '
  set -euo pipefail
  cd /work
  # Prevent debuginfod network hangs during report/script (ubuntu:26.04 default URL).
  export DEBUGINFOD_URLS=
  test -x /usr/bin/perf || { echo "FATAL: /usr/bin/perf absent (linux-perf package)" >&2; exit 2; }
  /usr/bin/perf --version

  # Unhide kernel addresses so [k] samples symbolise. Without this, /proc/kallsyms returns
  # zeros and every kernel frame is [unknown] — which is the majority of this workload.
  sysctl -w kernel.perf_event_paranoid=-1 >/tmp/sysctl.log 2>&1 || true
  sysctl -w kernel.kptr_restrict=0 >>/tmp/sysctl.log 2>&1 || true
  echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid) kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict)"
  # Fail closed if kallsyms is still zeroed: the rest of the run would produce an unusable profile.
  kallsyms_sample="$(awk "\$1 != \"0000000000000000\" { print \$1; exit }" /proc/kallsyms || true)"
  if [ -z "$kallsyms_sample" ]; then
    echo "FATAL: /proc/kallsyms addresses are zeroed — kernel frames cannot be symbolised." >&2
    echo "  sysctl log:" >&2
    cat /tmp/sysctl.log >&2 || true
    echo "  This container needs --privileged on Docker Desktop so it can lower" \
         "kernel.perf_event_paranoid (measured: PERFMON/SYS_ADMIN alone cannot)." >&2
    exit 1
  fi
  echo "kallsyms sample address=$kallsyms_sample"

  # The engine on an ephemeral port, feed-silent so the profile is the ORDER path.
  ./build/rel/mm_engine --feed bench/scenarios/bench_idle.feed --port 0 --codec tuned \
      --interval-ms 1 --telemetry-out /tmp/prof.jsonl > /tmp/engine.log 2>&1 &
  ENGINE_PID=$!
  PORT=""
  for _ in $(seq 200); do
    PORT="$(sed -n "s/^mm_engine .* listening port=\([0-9]*\).*/\1/p" /tmp/engine.log | head -1)"
    [ -n "$PORT" ] && break
    sleep 0.05
  done
  [ -n "$PORT" ] || { echo "FATAL: engine never reported a port" >&2; cat /tmp/engine.log >&2; exit 1; }
  echo "engine pid=$ENGINE_PID port=$PORT"

  # perf attached to the ENGINE, while a real tuned client drives it. Profiling the engine rather
  # than the client because §6 attributes the C++ side separately from the Python side.
  # NO TRAILING COMMAND. This read `-- sleep 1`, which bounds the recording to one second — and
  # that second elapses while the engine is still idle waiting for the client to connect. With
  # `-e task-clock` an idle process emits NO samples (the counter only advances while the task is
  # on CPU), so the run produced a perf.data with nothing in it and a 0-byte `perf script`. The
  # trailing command also contradicted the `kill -INT` below, which is what actually stops it:
  # perf without a command records until interrupted, which is the shape this script wants.
  # (Verified in-image: `-p PID -- sleep 1` against a BUSY process collects ~17k samples, so the
  # mechanism is the idle window, not the flag combination.)
  /usr/bin/perf record -F 999 -e task-clock -g --call-graph dwarf,65528 \
      -o /tmp/perf.data -p "$ENGINE_PID" > /tmp/perf_start.log 2>&1 &
  PERF_PID=$!
  sleep 0.3

  # THE CLIENT MUST SUCCEED. This carried `|| true`, which meant a run where the client failed to
  # connect at all still produced a perf.data — of an engine sitting idle — and the script went on
  # to call it a profile of the order path. A flamegraph of the wrong workload is worse than no
  # flamegraph, because it reads as evidence.
  PYTHONPATH=/work/bench /opt/venv/bin/python -B -m harness.run_bench \
      --mode idle --stack tuned --url "ws://127.0.0.1:$PORT" \
      --out /tmp/prof_run --warmup '"$WARMUP"' --samples '"$SAMPLES"' \
      --timeout 300 --engine ./build/rel/mm_engine > /tmp/run.log 2>&1
  run_rc=$?
  tail -2 /tmp/run.log
  [ $run_rc -eq 0 ] || { echo "FATAL: the benchmark client failed (rc=$run_rc); the profile would describe an idle engine" >&2; exit 1; }

  # perf must be stopped politely: SIGKILL leaves perf.data without its final mmap records and
  # `perf report` then resolves no symbols at all.
  kill -INT "$PERF_PID" 2>/dev/null || true
  wait "$PERF_PID" 2>/dev/null || true
  kill -INT "$ENGINE_PID" 2>/dev/null || true
  wait "$ENGINE_PID" 2>/dev/null || true

  test -s /tmp/perf.data || { echo "FATAL: perf.data is empty — perf collected nothing" >&2; exit 1; }

  # THREE artifacts: the human-readable report, the RAW per-sample stacks, and genuinely
  # collapsed stacks (the plan asks for "perf report --stdio + collapsed stacks").
  #
  # `perf script` output is NOT collapsed — it is one indented frame per line per sample, and the
  # earlier comment here called it collapsed stacks, which would have sent a reader to feed the
  # wrong format into a flamegraph renderer. Collapsing is the fold into
  # `root;child;leaf <count>` lines, done below rather than deferred to a FlameGraph checkout the
  # image does not contain.
  # STDERR IS NOT SUPPRESSED. Both of these carried `2>/dev/null || true`, and when `perf script`
  # then produced a 0-byte file the reason was unrecoverable — the one line that would have said
  # why had been discarded. A tool that fails silently in a script whose whole job is to produce
  # evidence is the worst possible combination.
  /usr/bin/perf report --stdio --no-children -i /tmp/perf.data > /out/perf_report.txt || {
    echo "FATAL: perf report failed" >&2; exit 1; }
  /usr/bin/perf script -i /tmp/perf.data > /out/perf_script.txt || {
    echo "FATAL: perf script failed (see the error above)" >&2; exit 1; }
  test -s /out/perf_script.txt || { echo "FATAL: perf script produced nothing to collapse" >&2; exit 1; }

  # The collapser is written to a FILE and then fed the samples. It was `python - < samples <<EOF`,
  # which is two stdin redirections: the heredoc wins, so python read its PROGRAM from the heredoc
  # and then found stdin already exhausted — it emitted nothing, correctly, for a reason that
  # looked exactly like "perf produced no stacks".
  cat > /tmp/collapse.py <<"PYEOF"
import collections, re, sys

# perf script emits one sample as a header line, then indented frames leaf-first, then a blank.
# Collapsed format wants root-first, semicolon-joined, with identical stacks aggregated.
frame_re = re.compile(r"^\s+\S+\s+(.+?)(?:\+0x[0-9a-f]+)?\s+\((.*)\)\s*$")
folded, stack = collections.Counter(), []
def flush():
    if stack:
        folded[";".join(reversed(stack))] += 1
    stack.clear()
for line in sys.stdin:
    if not line.strip():
        flush()
        continue
    m = frame_re.match(line)
    if m:
        name = m.group(1).strip() or "[unknown]"
        stack.append(name.replace(";", ":"))
flush()
for key, count in sorted(folded.items(), key=lambda kv: -kv[1]):
    print(key, count)
PYEOF
  /opt/venv/bin/python /tmp/collapse.py < /out/perf_script.txt > /out/perf_collapsed.txt
  test -s /out/perf_collapsed.txt || { echo "FATAL: the collapse produced no folded stacks" >&2; exit 1; }

  # SYMBOLS MUST ACTUALLY RESOLVE. A non-empty file is not a usable profile: a first green run
  # produced 1819 frame lines of which 1819 were `[unknown]`, and the gate passed it because it
  # only asked whether bytes existed. A flamegraph of `[unknown];[unknown];...` attributes nothing,
  # and shipping one as the profiling exhibit would be worse than shipping none — it reads as
  # evidence. This is the same false-positive-gate failure this script has now hit twice, so the
  # check is on the CONTENT the artifact is supposed to carry.
  #
  # Majority, not "not all unknown": with kallsyms zeroed the report is still mixed with a few
  # userspace leaf symbols (vdso/clock_gettime) while 99% of frames stay [unknown]. Requiring a
  # clear majority of frames to be real names catches that case without weakening the all-unknown
  # failure the previous gate already caught.
  total_frames=$(grep -c "^[[:space:]]" /out/perf_script.txt || true)
  unknown_frames=$(grep -c "\[unknown\]" /out/perf_script.txt || true)
  echo "  frames=$total_frames unknown=$unknown_frames"
  if [ "$total_frames" -eq 0 ]; then
    echo "FATAL: perf script produced zero frame lines — nothing to attribute." >&2
    exit 1
  fi
  # unknown * 2 >= total  <=>  unknown fraction >= 50%
  if [ $((unknown_frames * 2)) -ge "$total_frames" ]; then
    echo "FATAL: $unknown_frames of $total_frames frames are [unknown] (>=50%) — profile not symbolised." >&2
    echo "  perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo ?)," \
         "kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null || echo ?)" >&2
    echo "  kallsyms sample: $(awk "\$1 != \"0000000000000000\" { print \$1; exit }" /proc/kallsyms 2>/dev/null || echo zeroed)" >&2
    echo "  A profile without a majority of resolvable symbols cannot support an attribution claim." >&2
    exit 1
  fi
  # Spot-check: collapsed stacks should contain at least one non-unknown name.
  if ! grep -qv "\[unknown\]" /out/perf_collapsed.txt; then
    echo "FATAL: perf_collapsed.txt has no resolved symbol names." >&2
    exit 1
  fi
  cp /tmp/perf.data /out/perf.data 2>/dev/null || true
  wc -l /out/perf_report.txt /out/perf_script.txt /out/perf_collapsed.txt | sed "s/^/  /"
  echo "PROFILE_OK"
' > /dev/null 2>&1

# Three-state wait: a failed probe is UNKNOWN, never completion.
# 40 MINUTES, not 10. `perf script` on a dwarf-unwound capture is minutes of work on its own —
# dwarf unwinding walks CFI for every frame of every sample — and the previous 10-minute bound
# expired WHILE it was running. The cleanup trap then removed the container, leaving a complete
# perf_report.txt beside a 0-byte perf_script.txt: a killed job that looked exactly like a failed
# one. The three-state probe below distinguishes daemon trouble from completion; this bound only
# has to be longer than the work.
fails=0
for _ in $(seq 1 240); do
  state="$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" && rc=0 || rc=1
  if [[ $rc -eq 0 && "$state" == "false" ]]; then break; fi
  if [[ $rc -ne 0 ]]; then
    fails=$((fails + 1))
    [[ $fails -ge 8 ]] && { echo "FATAL: daemon degraded — not a finished profile" >&2; exit 4; }
  else
    fails=0
  fi
  sleep 10
done

docker logs "$NAME" 2>&1 | tail -20
if ! docker logs "$NAME" 2>&1 | grep -q PROFILE_OK; then
  echo "FATAL: the profile did not complete (no PROFILE_OK marker)." >&2
  exit 1
fi
echo "profile: ALL GREEN — artifacts in $OUT"
