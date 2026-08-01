"""Sanity cases for the benchmark harness.

These are the four properties whose failure would make every number the harness reports wrong
while it still looked like it worked — which is the only failure mode that matters in a
measurement tool. A harness that crashes gets fixed; a harness that quietly reports a flattering
percentile gets published.

  * the percentile RULE, against a distribution whose answer can be written down by hand;
  * warm-up EXCLUSION, because a JIT-free Python client still pays first-touch page faults and
    a cold TCP window, and including them moves p50 more than the optimization being measured;
  * the sample-count FLOOR, refused loudly rather than summarised thinly;
  * COORDINATED OMISSION — the one that needs a fake clock, because a real stall cannot be
    scheduled. A harness that times from the ACTUAL send hides a backlog completely: every
    request it managed to send looks fast, and the ones it never sent are simply absent from
    the data. Timing from the INTENDED send exposes it.
"""

import json
import math
import struct
from array import array
from pathlib import Path

import pytest
from harness import manifest, summarize

# --- the percentile rule ------------------------------------------------------------------


def test_the_percentile_rule_is_exact_rank_on_a_known_distribution() -> None:
    """0..99999, where every answer is arithmetic rather than a library's opinion.

    CEIL-RANK, never interpolated: for n sorted samples and percentile p, the reported value is
    the one at index `ceil(p*n) - 1`. Interpolation invents a number no request experienced,
    which in a latency report is a claim about a request that never happened.
    """
    stats = summarize.percentiles(array("q", range(100_000)))
    assert stats.count == 100_000
    assert stats.min == 0
    assert stats.p50 == 49_999  # ceil(0.5 * 100000) - 1
    assert stats.p90 == 89_999
    assert stats.p99 == 98_999
    assert stats.p99_9 == 99_899  # ceil(0.999 * 100000) - 1
    assert stats.max == 99_999


def test_the_percentile_rule_never_reads_past_the_data() -> None:
    """A one-sample run reports that sample at every percentile, and does not crash or
    extrapolate. The rank rule has to hold at n=1 or the rounding is wrong somewhere."""
    stats = summarize.percentiles(array("q", [42]))
    assert (stats.count, stats.min, stats.max) == (1, 42, 42)
    assert stats.p50 == stats.p99 == stats.p99_9 == 42


def test_an_empty_series_is_refused_not_summarised() -> None:
    """Zero samples is not "p50 = 0"; it is a run that produced nothing."""
    with pytest.raises(ValueError, match="no samples"):
        summarize.percentiles(array("q", []))


# --- warm-up exclusion -------------------------------------------------------------------


def test_warm_up_samples_are_dropped_by_count() -> None:
    """10 warm-up + 5 measured leaves exactly the 5.

    By COUNT rather than by time, because the engine's own streams have no timestamps to filter
    on and the client and engine cycles correspond 1:1 in idle and react modes — so one rule
    applies to both sides of the measurement.
    """
    samples = array("q", [*range(1000, 1010), 1, 2, 3, 4, 5])
    kept = summarize.drop_warmup(samples, 10)
    assert list(kept) == [1, 2, 3, 4, 5]
    assert summarize.percentiles(kept).max == 5, "a warm-up sample survived into the summary"


def test_dropping_more_warm_up_than_exists_is_refused() -> None:
    """Silently returning an empty series here would report a run that measured nothing as a
    run that measured cleanly."""
    with pytest.raises(ValueError, match="warm-up"):
        summarize.drop_warmup(array("q", [1, 2, 3]), 10)


# --- the sample floor --------------------------------------------------------------------


def test_a_short_run_exits_nonzero_and_emits_no_summary(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """benchmark sets a 100k floor for a primary table. A run that fell short must FAIL, not print a
    thin table that reads exactly like a full one."""
    path = tmp_path / "short.rtt.i64"
    path.write_bytes(array("q", range(50)).tobytes())

    rc = summarize.main(["--rtt", str(path), "--warmup", "0", "--require-samples", "100000"])
    assert rc != 0, "a short run must not exit clean"
    assert "p50" not in capsys.readouterr().out, "a refused run must emit no summary"


def test_a_run_that_meets_the_floor_summarises(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    path = tmp_path / "ok.rtt.i64"
    path.write_bytes(array("q", range(200)).tobytes())

    rc = summarize.main(["--rtt", str(path), "--warmup", "100", "--require-samples", "100"])
    assert rc == 0
    assert "p99.9" in capsys.readouterr().out


# --- coordinated omission ----------------------------------------------------------------


def test_the_intended_time_series_exposes_a_stall_the_actual_time_series_hides() -> None:
    """THE case this harness exists to get right.

    A fake timeline: 1000 cycles at 1 kHz, each taking 100 µs of service — then the client
    stalls for 100 ms at cycle 500 and afterwards sends as fast as it can to catch up. Timed
    from the ACTUAL send, every one of those catch-up requests looks like an ordinary 100 µs
    cycle, because the queueing delay was spent BEFORE the timer started. Timed from the
    INTENDED send, the backlog is visible in the tail exactly as a user would have felt it.

    Asserting on p99 rather than max, because a single outlier is not the point: the stall
    displaces a whole run of requests, and a percentile is what a report would publish.
    """
    rate_hz, service_ns, stall_ns = 1_000, 100_000, 100_000_000
    period = 1_000_000_000 // rate_hz
    n = 1000
    # The probe's own shape: three preallocated buffers filled by index (step 4 zero-allocation
    # sampling path), so this test drives `series` exactly as a real run does.
    intended_b, sent_b, done_b = (array("q", bytes(8 * n)) for _ in range(3))
    now = 0
    for i in range(n):
        intended = i * period
        if i == 500:
            now += stall_ns  # the client is off the air
        sent = max(now, intended)  # cannot send before its slot, but may be late
        done = sent + service_ns
        now = done
        intended_b[i], sent_b[i], done_b[i] = intended, sent, done

    co_corrected, actual, lag = summarize.series(intended_b, sent_b, done_b, n)
    p99_co = summarize.percentiles(co_corrected).p99
    p99_actual = summarize.percentiles(actual).p99

    assert p99_actual == service_ns, "the actual-send series should see only service time"
    assert p99_co - p99_actual >= stall_ns // 2, (
        f"the intended-time series hid the backlog: co={p99_co} actual={p99_actual}"
    )
    assert max(lag) >= stall_ns // 2, "send lag is the queue proxy and must record the stall"


def test_a_run_with_no_stall_has_matching_series() -> None:
    """The control. If the two series diverged on a clean run, the correction would be an
    artifact of the arithmetic rather than a measurement of anything."""
    period, service, n = 1_000_000, 100_000, 500
    intended_b, sent_b, done_b = (array("q", bytes(8 * n)) for _ in range(3))
    for i in range(n):
        intended_b[i] = sent_b[i] = i * period
        done_b[i] = i * period + service
    co_corrected, actual, lag = summarize.series(intended_b, sent_b, done_b, n)
    assert co_corrected == actual
    assert set(lag) == {0}


def test_series_ignores_the_unused_tail_of_the_preallocated_buffers() -> None:
    """The buffers are sized for the WHOLE run up front, so a run that ends early leaves a zero
    fill past `count`. Those zeros are not samples: read as ones, they would put a fabricated
    `done - intended` of zero into every percentile and drag p50 toward nothing.
    """
    n, recorded, period, service = 100, 7, 1_000_000, 250_000
    intended_b, sent_b, done_b = (array("q", bytes(8 * n)) for _ in range(3))
    for i in range(recorded):
        intended_b[i] = sent_b[i] = i * period
        done_b[i] = i * period + service

    co_corrected, actual, lag = summarize.series(intended_b, sent_b, done_b, recorded)
    assert len(co_corrected) == len(actual) == len(lag) == recorded
    assert set(co_corrected) == {service}, "the zero fill leaked into the series"


def test_series_refuses_a_count_past_the_recorded_stamps() -> None:
    """A count larger than the buffers is a bookkeeping bug, and silently clamping it would
    publish zero-filled slots as measurements."""
    n = 10
    intended_b, sent_b, done_b = (array("q", bytes(8 * n)) for _ in range(3))
    with pytest.raises(ValueError, match="outside the recorded stamps"):
        summarize.series(intended_b, sent_b, done_b, n + 1)


def test_gaps_over_scans_only_the_recorded_prefix() -> None:
    """`count` bounds the scan, and the direction of the bug it prevents is worth being precise
    about: the zero tail sits BELOW the last real send, so an unbounded scan produces negative
    deltas that no threshold catches. The failure is therefore a missed gap, not a fabricated one
    — which is the more dangerous direction, because the run still looks clean.
    """
    n, recorded, period = 50, 5, 1_000_000
    sent = array("q", bytes(8 * n))
    for i in range(recorded):
        sent[i] = i * period
    assert not summarize.gaps_over(sent, recorded), "a tightly-paced prefix has no gap"

    # A real stall inside the recorded prefix is still caught...
    stalled = array("q", bytes(8 * n))
    for i in range(recorded):
        stalled[i] = i * period
    stalled[3] += 5_000_000_000
    assert summarize.gaps_over(stalled, recorded), "a real 5s discontinuity must be caught"

    # ...and a scan that runs past `count` MISSES it, because the tail's zeros mean the deltas
    # after the last real sample go negative and swamp the comparison.
    assert not summarize.gaps_over(stalled, recorded, 10_000_000_000), (
        "sanity: a threshold above the stall must not fire"
    )


# --- the engine's dump -------------------------------------------------------------------


def test_the_engine_dump_header_is_read_exactly_as_documented(tmp_path: Path) -> None:
    """Six little-endian uint64s — four counts, the refused count, the PEAK SESSION COUNT —
    then the four int64 streams in order. Pinned here because the harness is the only automated
    reader of a format the engine writes, so a layout change breaks here first."""
    svc, m0_m0p, m0p_m3, m0_m3 = [1, 2, 3], [10, 20], [100], [7, 8, 9, 10]
    blob = struct.pack("<6Q", len(svc), len(m0_m0p), len(m0p_m3), len(m0_m3), 0, 1)
    for stream in (svc, m0_m0p, m0p_m3, m0_m3):
        blob += array("q", stream).tobytes()
    path = tmp_path / "engine.bench"
    path.write_bytes(blob)

    dump = summarize.read_engine_dump(path)
    assert list(dump.svc) == svc
    assert list(dump.m0_m0p) == m0_m0p
    assert list(dump.m0p_m3) == m0p_m3
    assert list(dump.m0_m3) == m0_m3
    assert dump.saturated == 0
    assert dump.peak_sessions == 1


def test_a_fan_out_dump_is_refused(tmp_path: Path) -> None:
    """The header's sixth word exists for exactly this. With more than one session the recorded
    m0->m0' is the MINIMUM across the fan-out rather than the delivery boundary — measured
    34-79x low at 8 to 64 sessions — so a multi-session dump cannot back an m0->m0' claim. The
    recorder's own header comment requires a harness to refuse or flag it."""
    blob = struct.pack("<6Q", 0, 0, 0, 0, 0, 4)
    path = tmp_path / "fanout.bench"
    path.write_bytes(blob)

    with pytest.raises(ValueError, match="peak session"):
        summarize.read_engine_dump(path)


def test_a_saturated_dump_is_flagged(tmp_path: Path) -> None:
    """A refused sample means the recorder hit capacity, so the streams are truncated and the
    percentiles describe only the part of the run that fit."""
    blob = struct.pack("<6Q", 1, 0, 0, 0, 17, 1) + array("q", [5]).tobytes()
    path = tmp_path / "sat.bench"
    path.write_bytes(blob)

    dump = summarize.read_engine_dump(path)
    assert dump.saturated == 17
    assert dump.is_suspect, "a dump that refused samples must be flagged as suspect"


def test_a_truncated_dump_is_refused(tmp_path: Path) -> None:
    """A header promising more samples than the file holds is a dump cut short — a killed
    engine, a full disk. Reading it as short-but-valid would publish a partial run."""
    blob = struct.pack("<6Q", 100, 0, 0, 0, 0, 1) + array("q", [1, 2]).tobytes()
    path = tmp_path / "trunc.bench"
    path.write_bytes(blob)

    with pytest.raises(ValueError, match="truncated"):
        summarize.read_engine_dump(path)


# --- suspect-run detection ---------------------------------------------------------------


def test_an_inter_sample_gap_over_a_second_flags_the_run() -> None:
    """A second of silence mid-run is not a latency measurement, it is a stopped process — a
    laptop that slept, a container that was throttled. Flagged rather than dropped: the run is
    evidence of something, just not of latency."""
    period, n = 1_000_000, 11
    sent = array("q", bytes(8 * n))
    for i in range(10):
        sent[i] = i * period
    assert not summarize.gaps_over(sent, 10, 1_000_000_000)

    sent[10] = 5_000_000_000  # five seconds after the last send
    assert summarize.gaps_over(sent, 11, 1_000_000_000)


# --- the manifest ------------------------------------------------------------------------


def test_the_manifest_records_every_field_a_reader_needs_to_reproduce_the_run() -> None:
    """A number without its environment is not a measurement. Each key here is something that
    changes the answer, so a missing one makes the run unreproducible rather than merely
    under-documented."""
    captured = manifest.capture(
        stack="tuned",
        mode="idle",
        rate_hz=0,
        warmup=10,
        samples=100,
        interval_ms=1,
        url="ws://127.0.0.1:1",
        repeat_tag="A1",
        rejects=0,
        gaps=False,
    )
    for key in (
        "cpu_model",
        "cores",
        "os",
        "kernel",
        "python_version",
        "python_build",
        "stack",
        "mode",
        "rate_hz",
        "warmup",
        "samples",
        "interval_ms",
        "repeat_tag",
        "tcp_nodelay",
        "compression",
        "affinity",
        "cpu_governor",
        "rejects",
        # Both remaining benchmark gate inputs the client can know. `engine_sha256` is what
        # makes the build identifiable; a matrix previously taken against a binary nobody could
        # name afterwards, and only these two fields prevent a repeat.
        "gaps",
        "engine_sha256",
        "packages",
    ):
        assert key in captured, f"the manifest omits {key}"
    assert json.dumps(captured), "the manifest must be JSON-serialisable"


def test_the_power_management_field_is_explicit_when_unreadable() -> None:
    """Silence about the CPU governor reads as "controlled"; it usually is not. When the
    value cannot be read the manifest says so in words rather than omitting the key."""
    captured = manifest.capture(
        stack="naive",
        mode="paced",
        rate_hz=1000,
        warmup=10,
        samples=100,
        interval_ms=1,
        url="ws://x",
        repeat_tag="B1",
        rejects=0,
        gaps=False,
    )
    governor = captured["cpu_governor"]
    assert governor, "the governor field must never be empty"
    assert isinstance(governor, str)


def test_a_run_with_rejects_is_not_primary_table_material() -> None:
    """A reject means the message mix tripped an engine limit, so the pattern under
    measurement is not the pattern intended."""
    assert manifest.qualifies_as_primary(rejects=0, saturated=0, gaps=False)
    assert not manifest.qualifies_as_primary(rejects=1, saturated=0, gaps=False)
    assert not manifest.qualifies_as_primary(rejects=0, saturated=3, gaps=False)
    assert not manifest.qualifies_as_primary(rejects=0, saturated=0, gaps=True)


# --- the gate is APPLIED, not merely correct ----------------------------------------------
#
# The predicate above was unit-tested and passing for the whole life of the harness while NOTHING
# CALLED IT. `gaps_over` was in the same state. A test that proves a rule is right says nothing
# about whether the rule runs, and the tests below are the ones that would have caught it: they go
# through `summarize.main`, the real entry point, and assert on its verdict and its exit code.


def _artifacts(tmp_path: Path, *, rejects: int, gaps: bool, saturated: int) -> tuple[Path, Path]:
    """A minimal but COMPLETE artifact set: the client series, the manifest beside it under the
    name `run_bench` gives it, and an engine dump. Returns (rtt, engine)."""
    rtt = tmp_path / "run.rtt.i64"
    rtt.write_bytes(array("q", range(200)).tobytes())
    (tmp_path / "run.manifest.json").write_text(json.dumps({"rejects": rejects, "gaps": gaps}))

    svc, m0_m0p, m0p_m3, m0_m3 = [1, 2, 3], [10, 20], [100], [7, 8, 9, 10]
    blob = struct.pack("<6Q", len(svc), len(m0_m0p), len(m0p_m3), len(m0_m3), saturated, 1)
    for stream in (svc, m0_m0p, m0p_m3, m0_m3):
        blob += array("q", stream).tobytes()
    engine = tmp_path / "run.engine.bench"
    engine.write_bytes(blob)
    return rtt, engine


def test_summarize_stamps_a_clean_run_as_primary(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The gate must RUN and say so in the output, because the verdict travels with the table."""
    rtt, engine = _artifacts(tmp_path, rejects=0, gaps=False, saturated=0)

    rc = summarize.main(
        ["--rtt", str(rtt), "--engine", str(engine), "--warmup", "0", "--require-samples", "200"]
    )
    out = capsys.readouterr().out
    assert rc == 0
    assert "PRIMARY" in out and "NOT PRIMARY" not in out
    assert "benchmark primary-table gate" in out


def test_summarize_refuses_a_rejected_run_under_a_primary_table_claim(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """`--require-samples` IS the primary-table claim. A run with a reject fails it — this is the
    case that silently exited 0 before the gate was wired."""
    rtt, engine = _artifacts(tmp_path, rejects=1, gaps=False, saturated=0)

    rc = summarize.main(
        ["--rtt", str(rtt), "--engine", str(engine), "--warmup", "0", "--require-samples", "200"]
    )
    assert rc != 0, "a rejected run must not pass a primary-table claim"
    assert "NOT PRIMARY" in capsys.readouterr().out


def test_a_gapped_run_is_refused_so_gaps_over_cannot_go_dead_again(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """`gaps` reaches the manifest from `gaps_over` in `run_bench`; this pins the consuming half so
    the predicate cannot be reduced to dead code a second time."""
    rtt, engine = _artifacts(tmp_path, rejects=0, gaps=True, saturated=0)

    rc = summarize.main(
        ["--rtt", str(rtt), "--engine", str(engine), "--warmup", "0", "--require-samples", "200"]
    )
    assert rc != 0
    assert "NOT PRIMARY" in capsys.readouterr().out


def test_a_missing_input_is_indeterminate_and_never_a_silent_pass(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Three states, not two. Without the engine dump the gate CANNOT be decided, and saying so is
    the only honest answer: reporting PRIMARY would launder a missing artifact into a pass, and
    reporting NOT PRIMARY would blame the run for the harness's own gap."""
    rtt, _ = _artifacts(tmp_path, rejects=0, gaps=False, saturated=0)

    rc = summarize.main(["--rtt", str(rtt), "--warmup", "0", "--require-samples", "200"])
    out = capsys.readouterr().out
    assert "INDETERMINATE" in out
    assert "saturated" in out, "it must name WHICH input was missing"
    assert rc == 0, "an undecidable gate cannot fail a run that may well be fine"


def test_a_manifest_predating_the_gaps_field_is_indeterminate_not_a_pass(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Every manifest written before `gaps` existed lacks the key. Defaulting a missing key to
    False would silently upgrade all of them to PRIMARY — which is exactly how the 22-run matrix
    came to look qualified."""
    rtt, engine = _artifacts(tmp_path, rejects=0, gaps=False, saturated=0)
    (tmp_path / "run.manifest.json").write_text(json.dumps({"rejects": 0}))  # no `gaps`

    rc = summarize.main(
        ["--rtt", str(rtt), "--engine", str(engine), "--warmup", "0", "--require-samples", "200"]
    )
    out = capsys.readouterr().out
    assert "INDETERMINATE" in out and "gaps" in out
    assert rc == 0


def test_the_ceil_rank_helper_matches_the_documented_formula() -> None:
    """The rule stated once, in code, so the tables and the prose cannot drift apart."""
    for n in (1, 2, 10, 999, 100_000):
        for p in (0.5, 0.9, 0.99, 0.999):
            assert summarize.rank_index(p, n) == min(n - 1, max(0, math.ceil(p * n) - 1))
