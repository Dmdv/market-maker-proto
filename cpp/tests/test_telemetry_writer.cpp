// Telemetry tests: the telemetry writer thread, the counters snapshot and the JSON-lines
// emission — the consumer part of the telemetry suite (seam map in
// telemetry_test_support.hpp: the ring is cased in test_telemetry.cpp, the writer's
// failure paths in test_telemetry_errors.cpp, the run markers / fallback shapes / drain
// cadence in test_telemetry_wire.cpp; every TU shares the `telemetry:` name prefix and
// `[telemetry]` tag).
//
// What this half PROMISES, each promise its own falsifiable case: a Counters snapshot
// crosses the ring as TWO self-describing records — totals and watermarks — because a full
// snapshot does not fit one record's payload, and each part emits as its own complete JSON
// line so a ring-full drop of one part loses exactly that part and the writer never
// stitches partial state (the mapping is constexpr, so the case pins it at compile time;
// since the a review wire batch the watermarks part also seats telemetry_pending_hw and
// live_orders, each emitted as its own named key); event records carry their code and
// args, and defined codes have wire names — all four named events are pinned as golden
// writer lines, session_open and session_close deliberately so: their codes (0 and 1) are
// numerically the two counters-part code values, so those two pins are what forces
// emit_line to discriminate on KIND rather than code; the writer drains records to JSON
// lines WHILE RUNNING (not only at shutdown), emitting each record exactly once between
// the open and close run markers (the markers' own shapes are pinned in
// test_telemetry_wire.cpp; here they are counted and skipped); the destructor stops the
// thread promptly (the poll wait is interruptible), joins it, and flushes every record
// still in the ring — the design's "file contains the last record after destruction"; the
// drop count to date rides every totals line, which is the "reported in next snapshot"
// clause of the design's `dropped()` contract made concrete; a record with an undefined
// shape still emits, self-describing, rather than being silently discarded
// (count-every-drop discipline); the shipped default poll (TelemetryWriter::kDefaultPoll)
// is pinned to its exact 10 ms value at compile time, with the runtime
// default-constructed-writer case bounding gross drift; and the output path opens
// TRUNCATING — one file is one run, a successor on the same path starts it over.
//
// Two promises are adversarial by construction: destruction under a producer that NEVER
// stops pushing returns in bounded time — the shutdown drain's debt is cut off at the
// pending count sampled after the stop signal, every record pushed before destruction is
// on disk in order, and the cutoff's excess stays queued for a successor — and the drop
// counter is atomic where it matters, drops generated concurrently with totals emission:
// the tally stays exact, and a non-atomic dropped_ is a data race the tsan preset reports
// on that very case (a SCALAR race, within the host runtime's sight — only struct-copy
// shapes are not, bench/probes/tsan_struct_copy_probe.cpp).
//
// Lines are pinned as EXACT strings, golden-bytes style: the telemetry file is a wire-ish
// surface the server layer's integration tests grep (`hwm_close`, `telemetry_dropped`), so its
// format is contract, not presentation.
#include <catch2/catch_test_macros.hpp>

#include "mm/telemetry.hpp"
#include "telemetry_test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using telemetry_test::lines_of;
using telemetry_test::TempPath;
using telemetry_test::wait_for_lines;

// The leading "ts" value of an emitted line — the sequence stamp the concurrency cases
// key their FIFO/gap-free oracles on (stoll stops at the ',' after the number).
std::int64_t ts_of(const std::string &line) {
  return std::stoll(line.substr(std::string_view{R"({"ts":)"}.size()));
}

// The "telemetry_dropped" stamp of a totals line — its last `:`-prefixed number (stoull
// stops at the closing '}').
std::uint64_t dropped_stamp_of(const std::string &line) {
  return std::stoull(line.substr(line.rfind(':') + 1));
}

// Installs a GLOBAL locale whose numpunct groups thousands ("1,234,567") and restores the
// prior global locale on destruction, so a failing assertion cannot leak the grouping into
// later cases. A stream captures the global locale at CONSTRUCTION, which is exactly the
// exposure under test: a writer built inside this guard formats through the grouping
// locale unless its constructor imbues the classic one. The facet pointer is owned by the
// locale it is built into (refcounted by contract), not leaked.
class GroupingLocaleGuard {
public:
  GroupingLocaleGuard() : prior_(std::locale::global(grouping_locale())) {}
  ~GroupingLocaleGuard() { std::locale::global(prior_); }
  GroupingLocaleGuard(const GroupingLocaleGuard &) = delete;
  GroupingLocaleGuard &operator=(const GroupingLocaleGuard &) = delete;
  GroupingLocaleGuard(GroupingLocaleGuard &&) = delete;
  GroupingLocaleGuard &operator=(GroupingLocaleGuard &&) = delete;

private:
  struct GroupingNumpunct : std::numpunct<char> {
    [[nodiscard]] char do_thousands_sep() const override { return ','; }
    [[nodiscard]] std::string do_grouping() const override { return "\3"; }
  };
  static std::locale grouping_locale() {
    return std::locale{std::locale::classic(), new GroupingNumpunct};
  }

  std::locale prior_;
};

// Distinct value per field, so a lane swapped or dropped in the snapshot mapping (or in
// the writer's emission order) cannot produce a passing line by coincidence.
constexpr mm::Counters kCounters{.orders = 11,
                                 .fills = 22,
                                 .rejects = 33,
                                 .cancels = 44,
                                 .conflated = 55,
                                 .outbox_depth_hw = 66,
                                 .send_lag_max_ns = 77,
                                 .sessions = 88,
                                 .telemetry_pending_hw = 99,
                                 .live_orders = 111,
                                 .stale_books_ignored = 122};

constexpr std::string_view kTotalsLine =
    R"({"ts":1234,"orders":11,"fills":22,"rejects":33,"cancels":44,"conflated":55,"sessions":88,"telemetry_dropped":0})";
constexpr std::string_view kWatermarksLine =
    R"({"ts":1234,"outbox_depth_hw":66,"send_lag_max_ns":77,"stale_books_ignored":122,"telemetry_pending_hw":99,"live_orders":111})";

} // namespace

TEST_CASE("telemetry: a counters snapshot crosses the ring as two self-describing records",
          "[telemetry]") {
  // snapshot_records is constexpr (event_record's precedent — nothing in the mapping is
  // runtime-dependent), so the whole field-to-lane assignment is pinned at compile time.
  static constexpr auto records = mm::snapshot_records(kCounters, 1234);

  constexpr const mm::TelemetryRecord &totals = records[0];
  STATIC_REQUIRE(totals.ts_ns == 1234);
  STATIC_REQUIRE(totals.kind == static_cast<std::uint32_t>(mm::TelemetryKind::Counters));
  STATIC_REQUIRE(totals.code == static_cast<std::uint32_t>(mm::CountersPart::Totals));
  STATIC_REQUIRE(totals.args[0] == kCounters.orders);
  STATIC_REQUIRE(totals.args[1] == kCounters.fills);
  STATIC_REQUIRE(totals.args[2] == kCounters.rejects);
  STATIC_REQUIRE(totals.args[3] == kCounters.cancels);
  STATIC_REQUIRE(totals.args[4] == kCounters.conflated);
  STATIC_REQUIRE(totals.args[5] == kCounters.sessions);

  constexpr const mm::TelemetryRecord &watermarks = records[1];
  STATIC_REQUIRE(watermarks.ts_ns == 1234);
  STATIC_REQUIRE(watermarks.kind == static_cast<std::uint32_t>(mm::TelemetryKind::Counters));
  STATIC_REQUIRE(watermarks.code == static_cast<std::uint32_t>(mm::CountersPart::Watermarks));
  STATIC_REQUIRE(watermarks.args[0] == kCounters.outbox_depth_hw);
  STATIC_REQUIRE(watermarks.args[1] == kCounters.send_lag_max_ns);
  STATIC_REQUIRE(watermarks.args[2] == kCounters.stale_books_ignored); // (k)'s seat, taken
  STATIC_REQUIRE(watermarks.args[3] == kCounters.telemetry_pending_hw);
  STATIC_REQUIRE(watermarks.args[4] == kCounters.live_orders);
  // The one still-unseated watermark lane stays a reserved zero — seating it reshapes
  // neither record (the JSON line just gains a key, as args[2]'s seating showed).
  STATIC_REQUIRE(watermarks.args[5] == 0);
}

TEST_CASE("telemetry: event records carry their code and args, and defined codes have wire names",
          "[telemetry]") {
  constexpr auto rec = mm::event_record(mm::TelemetryEvent::HwmClose, 5678, 9, 10);
  STATIC_REQUIRE(rec.ts_ns == 5678);
  STATIC_REQUIRE(rec.kind == static_cast<std::uint32_t>(mm::TelemetryKind::Event));
  STATIC_REQUIRE(rec.code == static_cast<std::uint32_t>(mm::TelemetryEvent::HwmClose));
  STATIC_REQUIRE(rec.args[0] == 9);
  STATIC_REQUIRE(rec.args[1] == 10);
  STATIC_REQUIRE(rec.args[2] == 0);
  STATIC_REQUIRE(rec.args[3] == 0);
  STATIC_REQUIRE(rec.args[4] == 0);
  STATIC_REQUIRE(rec.args[5] == 0);

  STATIC_REQUIRE(mm::to_string(mm::TelemetryEvent::SessionOpen) == "session_open");
  STATIC_REQUIRE(mm::to_string(mm::TelemetryEvent::SessionClose) == "session_close");
  STATIC_REQUIRE(mm::to_string(mm::TelemetryEvent::HwmClose) == "hwm_close");
  STATIC_REQUIRE(mm::to_string(mm::TelemetryEvent::Reject) == "reject");
  STATIC_REQUIRE(mm::to_string(mm::TelemetryEvent::CmdIn) == "cmd_in");
  STATIC_REQUIRE(mm::to_string(mm::TelemetryEvent::TobOut) == "tob_out");
  STATIC_REQUIRE(mm::to_string(mm::TelemetryEvent::AdmissionRefused) == "admission_refused");
  // An out-of-range value has NO name — the empty view is the writer's signal to fall back
  // to the numeric self-describing form instead of inventing a wire spelling.
  STATIC_REQUIRE(mm::to_string(static_cast<mm::TelemetryEvent>(999)).empty());
}

TEST_CASE("telemetry: the writer drains records to JSON lines while running", "[telemetry]") {
  const TempPath out{"live"};
  mm::SpscTelemetryRing ring;
  {
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    for (const mm::TelemetryRecord &rec : mm::snapshot_records(kCounters, 1234))
      REQUIRE(ring.try_push(rec));
    REQUIRE(ring.try_push(mm::event_record(mm::TelemetryEvent::HwmClose, 5678, 9)));
    // session_open and session_close, deliberately beside the counters lines: their codes
    // (0 and 1) are numerically the two CountersPart values (Totals and Watermarks), so
    // these two golden lines are what forces emit_line to discriminate on KIND — under a
    // TelemetryKind::Counters == TelemetryKind::Event aliasing mutant each would emit as a
    // fabricated counters line and red here, while every other case in the suite stayed
    // green.
    REQUIRE(ring.try_push(mm::event_record(mm::TelemetryEvent::SessionOpen, 1111, 42)));
    REQUIRE(ring.try_push(mm::event_record(mm::TelemetryEvent::SessionClose, 2222, 42, 7)));
    // Asserted BEFORE destruction: this is the LIVE drain, not the shutdown flush. The
    // open marker is the file's first line, so the five records make six.
    REQUIRE(wait_for_lines(out.path(), 6).size() == 6);
  }
  const auto lines = lines_of(out.path());
  // Shutdown re-emitted nothing: the open marker, each record exactly once, the close
  // marker (marker shapes are pinned in test_telemetry_wire.cpp — counted here).
  REQUIRE(lines.size() == 7);
  CHECK(lines[1] == kTotalsLine);
  CHECK(lines[2] == kWatermarksLine);
  CHECK(lines[3] == R"({"ts":5678,"event":"hwm_close","args":[9,0,0,0,0,0]})");
  CHECK(lines[4] == R"({"ts":1111,"event":"session_open","args":[42,0,0,0,0,0]})");
  CHECK(lines[5] == R"({"ts":2222,"event":"session_close","args":[42,7,0,0,0,0]})");
}

TEST_CASE("telemetry: a grouping global locale cannot reshape the emitted numbers", "[telemetry]") {
  // The constructor's imbue-classic pin (telemetry.hpp): the lines are a machine-read
  // surface, and out_ is CONSTRUCTED while the grouping locale is global, so without the
  // imbue every number past the grouping threshold would emit grouped ("1,234,567") —
  // structurally invalid JSON on the surface the server layer's integration tests parse. Both
  // stamps sit past that threshold, so the mutant cannot pass by small-number coincidence.
  const TempPath out{"locale"};
  mm::SpscTelemetryRing ring;
  {
    const GroupingLocaleGuard grouping;
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    REQUIRE(ring.try_push(mm::event_record(mm::TelemetryEvent::Reject, 1234567, 7654321)));
    REQUIRE(wait_for_lines(out.path(), 2).size() == 2); // the open marker plus the record
  } // writer destroyed inside the guard's scope: the shutdown flush ran under the locale too
  // The probe formats through the GLOBAL locale, so it reds if the guard's restore is
  // ever deleted — the writer's own imbue cannot mask it.
  std::ostringstream probe;
  probe << 1234567U;
  CHECK(probe.str() == "1234567");
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() == 3);
  // The open marker is CONSTRUCTOR-written — the earliest line the imbue must already
  // cover — and its capacity stamp sits past the grouping threshold ("4,096" under the
  // mutant), so the marker pins the ctor ordering (imbue before first write) too.
  CHECK(lines[0].find(R"("ring_capacity":4096)") != std::string::npos);
  CHECK(lines[1] == R"({"ts":1234567,"event":"reject","args":[7654321,0,0,0,0,0]})");
}

TEST_CASE("telemetry: the default poll drains a quiet ring promptly", "[telemetry]") {
  // The VALUE pin: the design ships exactly 10 ms. The runtime half below bounds GROSS
  // drift only — its wait deadline is 10 s, so a default drifted to (say) 1000 ms would
  // still pass it; any smaller drift is this line's catch, at compile time.
  STATIC_REQUIRE(mm::TelemetryWriter::kDefaultPoll == std::chrono::milliseconds(10));
  // The behavioral half — the one case that constructs the writer WITHOUT a poll
  // argument, because does and the shipped default is therefore the live-drain
  // latency of an idle engine. The record is pushed only after the thread has passed its
  // startup drain into the timed wait (the dtor case's 50ms pattern), so the line below
  // can appear only via a poll WAKE: a default drifted past the wait deadline reds the
  // bounded wait here, where every case that passes 1ms explicitly would stay green.
  const TempPath out{"default_poll"};
  mm::SpscTelemetryRing ring;
  {
    mm::TelemetryWriter writer{ring, out.path()};
    std::this_thread::sleep_for(50ms);
    REQUIRE(ring.try_push(mm::event_record(mm::TelemetryEvent::Reject, 1, 1)));
    // Two lines = the ctor-written open marker plus the record; only the RECORD needs
    // the poll wake, so the wait is deadline-bounded, not sleep-and-hope.
    REQUIRE(wait_for_lines(out.path(), 2).size() == 2);
  }
}

TEST_CASE("telemetry: the writer opens its path truncating — one file is one run", "[telemetry]") {
  // The file half of the successor contract, decided and pinned: leftover RECORDS stay in
  // the ring for a successor draining the same ring, but a writer (re)opening a PATH
  // starts that file over — a reader never has to split one file into runs. An ios::app
  // drift would silently prepend stale lines (a predecessor's, or a crashed run's) to
  // every file this writer emits.
  const TempPath out{"truncate"};
  {
    std::ofstream pre{out.path()};
    pre << "{\"stale\":1}\n{\"stale\":2}\n";
  }
  mm::SpscTelemetryRing ring;
  {
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    REQUIRE(ring.try_push(mm::event_record(mm::TelemetryEvent::Reject, 1, 1)));
    REQUIRE(wait_for_lines(out.path(), 2).size() == 2); // open marker + record
  }
  const auto lines = lines_of(out.path());
  // The pre-seeded lines are GONE, not prepended to: open marker, the record, close
  // marker — an append mutant would make five with the stale pair leading.
  REQUIRE(lines.size() == 3);
  CHECK(lines[1] == R"({"ts":1,"event":"reject","args":[1,0,0,0,0,0]})");
}

TEST_CASE("telemetry: the destructor stops promptly, joins, and flushes the tail", "[telemetry]") {
  const TempPath out{"dtor"};
  mm::SpscTelemetryRing ring;
  const auto start = std::chrono::steady_clock::now();
  {
    // A poll long enough that no timed wake can plausibly fire inside this scope: the
    // records pushed below are reachable ONLY by the destructor's post-stop drain.
    mm::TelemetryWriter writer{ring, out.path(), 10min};
    std::this_thread::sleep_for(50ms); // let the thread pass its startup drain into the wait
    for (std::uint64_t seq = 1; seq <= 5; ++seq)
      REQUIRE(ring.try_push(
          mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq)));
  }
  // This bound asserts PROMPTNESS — the destructor interrupted the 10-minute wait — not
  // hang protection, which is the ctest-level TIMEOUT property's job (CMakeLists.txt).
  // 5 s is ~15x this case's slowest measured run (0.34 s, tsan preset, 2026-07-29;
  // re-take: ctest --preset tsan -R "destructor stops promptly" and read its time), so
  // it cannot flake on a loaded host yet still reds a destructor that sleeps out even a
  // fraction of the poll.
  REQUIRE(std::chrono::steady_clock::now() - start < 5s);

  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() == 7); // open marker, the five records in order, close marker
  CHECK(lines[5] == R"({"ts":5,"event":"reject","args":[5,0,0,0,0,0]})");
}

TEST_CASE("telemetry: destruction under a sustained producer is bounded and keeps every "
          "record pushed before it",
          "[telemetry]") {
  // The hang this case exists to make impossible: an unbounded shutdown drain lets a
  // producer that never stops pushing keep the consumer inside drain() forever, so the
  // destructor's join never returns. Contract under test: the destructor returns in
  // BOUNDED time regardless of the producer; the file holds — in FIFO order, gap-free —
  // at least every record pushed before destruction began; and the pushes the shutdown
  // cutoff excused stay QUEUED in the ring, a successor's inheritance rather than a loss.
  //
  // The ring is deliberately LARGE (4 MiB for the case's lifetime): it is the pressure
  // vessel that makes the unbounded-drain mutant's hang deterministic. The producer
  // refills any freed slot within microseconds, so for a drain-until-empty to exit, the
  // consumer must clear the ENTIRE backlog inside one contiguous producer absence — with
  // 64 Ki records that is ~100 ms of uninterrupted starvation of a ready thread, beyond
  // any plausible scheduler hiccup (a 256-slot ring measurably let the mutant slip out
  // through exactly such a window). The bounded-drain contract is indifferent to the
  // size: its shutdown debt is the cutoff's pending count whatever the capacity.
  const TempPath out{"dtor_load"};
  mm::SpscTelemetryRing ring{65536};
  std::atomic<bool> cancel{false};
  std::atomic<std::uint64_t> pushed{0};
  std::optional<mm::TelemetryWriter> writer;
  writer.emplace(ring, out.path(), 1ms);

  // Seq-stamped events pushed with retry, so `pushed` counts PUBLISHED records exactly.
  // The producer never stops on its own; only the cancel flag ends it (checked every
  // retry — the suite's stuck-producer discipline, same shape as the 1M stress case).
  std::thread producer{[&ring, &cancel, &pushed] {
    for (std::uint64_t seq = 0; !cancel.load(std::memory_order_relaxed); ++seq) {
      const mm::TelemetryRecord rec =
          mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq);
      while (!ring.try_push(rec)) {
        if (cancel.load(std::memory_order_relaxed))
          return;
        std::this_thread::yield();
      }
      pushed.fetch_add(1, std::memory_order_release);
    }
  }};

  std::this_thread::sleep_for(100ms); // so destruction lands mid-traffic, never at idle
  const std::uint64_t pushed_before = pushed.load(std::memory_order_acquire);
  const auto dtor_start = std::chrono::steady_clock::now();
  writer.reset(); // ~TelemetryWriter with the producer still pushing at full rate
  const auto dtor_elapsed = std::chrono::steady_clock::now() - dtor_start;
  cancel.store(true, std::memory_order_relaxed);
  producer.join();
  REQUIRE(pushed_before > 0); // the destruction really was under load

  // Bounded: the final drain owes at most its cutoff's pending records (<= capacity),
  // never the producer's future — sub-second on the dev preset, low seconds when a
  // sanitizer slows every emit. 30 s bounds the pathology and is still unambiguous
  // against the unbounded-drain shape, which never returns at all.
  REQUIRE(dtor_elapsed < 30s);

  // FIFO and gap-free: the producer's seqs are consecutive from 0 and nothing published
  // may be skipped, so record line i (the span between the open and close markers, whose
  // runtime stamps would wreck a seq oracle) must carry ts == i-1...
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() >= pushed_before + 2); // both markers plus at least every pre-dtor push
  std::size_t misplaced_lines = 0;            // counted, asserted once — not one REQUIRE per line
  for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
    if (ts_of(lines[i]) != static_cast<std::int64_t>(i - 1))
      ++misplaced_lines;
  }
  REQUIRE(misplaced_lines == 0);

  // ...and every publish the writer did NOT emit is still queued, continuing that exact
  // sequence: emitted + queued account for every push — none lost, none duplicated.
  std::uint64_t next_seq = lines.size() - 2;
  std::uint64_t misplaced_leftovers = 0;
  mm::TelemetryRecord rec{};
  while (ring.try_pop(rec)) {
    if (std::cmp_not_equal(rec.ts_ns, next_seq))
      ++misplaced_leftovers;
    ++next_seq;
  }
  REQUIRE(misplaced_leftovers == 0);
  REQUIRE(next_seq == pushed.load());
}

TEST_CASE("telemetry: the drop count stays exact while drops race the writer's totals lines",
          "[telemetry]") {
  // Rule under test: dropped_ is ATOMIC — the one counter two threads touch directly
  // (the producer increments it on every refusal, the writer reads it inside every
  // totals emit). The pre-existing cases generated drops only while the writer was not
  // yet running, so a plain-uint64 mutant stayed green suite-wide; here the increments
  // and the reads overlap BY CONSTRUCTION, which makes that mutant a data race the tsan
  // preset reports on this very case (a SCALAR race, within the host runtime's sight —
  // only struct-copy shapes are not, bench/probes/tsan_struct_copy_probe.cpp), while the
  // REQUIREs pin the exact final tally under every preset.
  const TempPath out{"drop_race"};
  mm::SpscTelemetryRing ring{1}; // one slot: almost every push in the storm is a drop
  const auto records = mm::snapshot_records(kCounters, 1234);
  const mm::TelemetryRecord &totals = records[0];
  std::uint64_t attempts = 0;
  {
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    // Keep the drop storm running until eight totals lines exist past the open marker,
    // so at least seven emissions read dropped() while this thread was still hammering
    // it — overlap by construction, not by sleep-and-hope. The deadline only bounds a
    // broken writer (which then fails the >= 10 REQUIRE below) — it can never hang the
    // suite.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (lines_of(out.path()).size() < 9 && std::chrono::steady_clock::now() < deadline) {
      for (int burst = 0; burst < 64; ++burst) {
        (void)ring.try_push(totals);
        ++attempts;
      }
    }
  } // this thread stopped pushing BEFORE destruction, so the final drain owed everything
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() >= 10); // open marker + the eight-plus totals lines + close marker
  mm::TelemetryRecord rec{};
  REQUIRE_FALSE(ring.try_pop(rec)); // drained empty ⇒ the accounting below is complete
  // Every attempt either became a totals line (the span between the markers — neither
  // marker consumed a push) or was counted as a drop — exactly one of the two, so the
  // lifetime tally closes to the push arithmetic with no slack for a lost or
  // double-counted increment.
  REQUIRE(ring.dropped() == attempts - (lines.size() - 2));
  // The per-line stamps are one thread's reads of one monotonically increasing counter:
  // non-decreasing across the record span, never past the final tally. The markers stay
  // out of the scan — their trailing :-number is a different field entirely (the open
  // marker's dropped_at_open, the close marker's non-numeric stream_ok).
  std::uint64_t prev_stamp = 0;
  std::size_t non_monotone = 0;
  for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
    const std::uint64_t stamp = dropped_stamp_of(lines[i]);
    if (stamp < prev_stamp)
      ++non_monotone;
    prev_stamp = stamp;
  }
  REQUIRE(non_monotone == 0);
  REQUIRE(prev_stamp <= ring.dropped());
}

TEST_CASE("telemetry: drops to date ride the next totals line", "[telemetry]") {
  const TempPath out{"dropped"};
  mm::SpscTelemetryRing ring{1};
  const auto records = mm::snapshot_records(kCounters, 1234);
  REQUIRE(ring.try_push(records[0]));       // fills the one-slot ring
  REQUIRE_FALSE(ring.try_push(records[1])); // dropped — the count the emitted line must carry
  {
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    REQUIRE(wait_for_lines(out.path(), 2).size() == 2); // open marker + the totals line
  }
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() == 3);
  // Same totals line as kTotalsLine except the drop count, read from the ring at emit
  // time: the record was pushed BEFORE the drop and still reports it. The open marker
  // ahead of it carries the same fact as "dropped_at_open":1 — the drop predates the
  // writer, and both stamps agree.
  CHECK(lines[0].find(R"("dropped_at_open":1)") != std::string::npos);
  CHECK(
      lines[1] ==
      R"({"ts":1234,"orders":11,"fills":22,"rejects":33,"cancels":44,"conflated":55,"sessions":88,"telemetry_dropped":1})");
}

TEST_CASE("telemetry: a record with an undefined shape still emits, self-describing",
          "[telemetry]") {
  const TempPath out{"unknown"};
  mm::SpscTelemetryRing ring;
  {
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    REQUIRE(ring.try_push(
        mm::TelemetryRecord{.ts_ns = 7, .kind = 9, .code = 3, .args = {1, 2, 3, 4, 5, 6}}));
    REQUIRE(ring.try_push(mm::event_record(static_cast<mm::TelemetryEvent>(99), 8, 1)));
    REQUIRE(wait_for_lines(out.path(), 3).size() == 3); // open marker + the two fallbacks
  }
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() == 4);
  CHECK(lines[1] == R"({"ts":7,"kind":9,"code":3,"args":[1,2,3,4,5,6]})");
  // An event code without a wire name emits numerically rather than inventing one (the
  // adversarial fallback edges — an undefined COUNTERS part and the enum-base
  // truncation probe — are cased in test_telemetry_wire.cpp).
  CHECK(lines[2] == R"({"ts":8,"kind":1,"code":99,"args":[1,0,0,0,0,0]})");
}
