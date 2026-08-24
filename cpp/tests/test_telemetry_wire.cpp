// Task 6.5 tests: the wire-shape edges of the telemetry stream — the fourth TU of the
// telemetry suite, split out at gate P4-i1 under the 500-line file cap (seam map in
// telemetry_test_support.hpp: the ring is cased in test_telemetry.cpp, the writer's
// happy-path JSON contract in test_telemetry_writer.cpp, its failure paths in
// test_telemetry_errors.cpp; every TU shares the `telemetry:` name prefix and
// `[telemetry]` tag).
//
// What this TU PROMISES, each promise its own falsifiable case: a run's stream is
// bracketed by an OPEN marker (constructor: wall anchor, ring capacity, poll, drops to
// date) and a CLOSE marker (writer thread, post-final-drain: exact leftovers, drops,
// stream health) — a zero-push run emits EXACTLY that pair, so the markers are pinned
// here shape-by-shape where no record line can hide a miscount; an undefined code on
// EITHER kind falls back to the self-describing form — an unknown CountersPart never
// borrows the totals or watermarks shape (that discriminant is what makes a future
// third counters part a safe intermediate state), and an event code that would alias a
// named event under a narrower enum base (259 ≡ Reject mod 256) emits numerically,
// which together with the underlying-type pins is what keeps the enum's uint32_t base
// load-bearing; and a BUSY ring drains back-to-back rather than at the poll cadence —
// the run loop's busy-path `continue` is the liveness half of the poll contract (the
// poll bounds only IDLE latency), pinned by arithmetic a cadence mutant cannot make.
#include <catch2/catch_test_macros.hpp>

#include "mm/telemetry.hpp"
#include "telemetry_test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>

namespace {

using namespace std::chrono_literals;
using telemetry_test::lines_of;
using telemetry_test::TempPath;
using telemetry_test::wait_for_lines;

} // namespace

TEST_CASE("telemetry: a zero-push run emits exactly the open and close markers", "[telemetry]") {
  const TempPath out{"markers"};
  mm::SpscTelemetryRing ring{8};
  {
    mm::TelemetryWriter writer{ring, out.path(), 5ms};
  }
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() == 2);
  // Open marker: two runtime stamps bracket a pinned tail. ts is steady_clock like every
  // other line; wall_ns is the file's one wall-clock ANCHOR — for correlating with
  // outside logs, never an operand against any ts (the clock rule, stated in the
  // header) — so the assertions pin the keys' presence and order, not their values.
  CHECK(lines[0].rfind(R"({"ts":)", 0) == 0);
  CHECK(lines[0].find(R"(,"wall_ns":)") != std::string::npos);
  CHECK(lines[0].ends_with(R"(,"ring_capacity":8,"poll_ms":5,"dropped_at_open":0})"));
  // Close marker, written by the WRITER thread after its final drain: pending_left is
  // the exact leftover count (zero — nothing was pushed), dropped the lifetime tally,
  // stream_ok the failure latch's negation.
  CHECK(lines[1].rfind(R"({"ts":)", 0) == 0);
  CHECK(lines[1].ends_with(R"(,"pending_left":0,"dropped":0,"stream_ok":true})"));
}

TEST_CASE("telemetry: undefined codes on either kind fall back self-describing", "[telemetry]") {
  // The enum surfaces ride uint32_t record fields; pinned so a narrowed base cannot
  // sneak in (types.hpp's underlying_type convention). The 259 probe below is the
  // runtime half of the same contract: 259 ≡ Reject (3) mod 256, so a uint8_t-based
  // TelemetryEvent plus a static_cast at the POD boundary would alias it onto "reject"
  // and red the golden fallback line.
  STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<mm::TelemetryEvent>, std::uint32_t>);
  STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<mm::TelemetryKind>, std::uint32_t>);
  STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<mm::CountersPart>, std::uint32_t>);

  const TempPath out{"fallback"};
  mm::SpscTelemetryRing ring;
  {
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    // A counters record whose part does not exist: the writer must not let it borrow
    // the totals or watermarks shape — the code discriminant, not just the kind, picks
    // those branches. This is what makes a future third CountersPart a safe (never
    // silent, never mislabeled) intermediate state while its named emission lands.
    REQUIRE(ring.try_push(
        mm::TelemetryRecord{.ts_ns = 7,
                            .kind = static_cast<std::uint32_t>(mm::TelemetryKind::Counters),
                            .code = 2,
                            .args = {1, 2, 3, 4, 5, 6}}));
    REQUIRE(ring.try_push(mm::event_record(static_cast<mm::TelemetryEvent>(259), 8, 1)));
    REQUIRE(wait_for_lines(out.path(), 3).size() == 3); // open marker + the two fallbacks
  }
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() == 4);
  CHECK(lines[1] == R"({"ts":7,"kind":0,"code":2,"args":[1,2,3,4,5,6]})");
  CHECK(lines[2] == R"({"ts":8,"kind":1,"code":259,"args":[1,0,0,0,0,0]})");
}

TEST_CASE("telemetry: a busy ring drains back-to-back, not at the poll cadence", "[telemetry]") {
  // The run loop's busy-path `continue`, pinned by construction rather than by racing:
  // capacity 8 and a 100 ms poll make the cadence mutant's ceiling arithmetic — a writer
  // that waits out the poll between batches moves at most 8 records per 100 ms, so a
  // thousand records need >= 12.5 s and the 5 s deadline is unreachable for it — while
  // the shipped busy path re-batches immediately and clears the same thousand in well
  // under a second. The deadline here IS the oracle, which is why this wait passes a
  // tight budget instead of the suite's generous default.
  const TempPath out{"busy"};
  mm::SpscTelemetryRing ring{8};
  std::atomic<bool> cancel{false};
  {
    mm::TelemetryWriter writer{ring, out.path(), 100ms};
    std::thread producer{[&ring, &cancel] {
      for (std::uint64_t seq = 0; seq < 1000; ++seq) {
        const mm::TelemetryRecord rec =
            mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq);
        while (!ring.try_push(rec)) {
          if (cancel.load(std::memory_order_relaxed))
            return;
          std::this_thread::yield();
        }
      }
    }};
    const auto lines = wait_for_lines(out.path(), 1001, 5s); // open marker + the records
    cancel.store(true, std::memory_order_relaxed);
    producer.join(); // joined before the REQUIRE: a red must still tear down cleanly
    REQUIRE(lines.size() >= 1001);
  }
}
