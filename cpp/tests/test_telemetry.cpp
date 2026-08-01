// Telemetry tests: the SPSC telemetry ring — the ring part of the telemetry
// suite (seam map in telemetry_test_support.hpp: the writer thread, the counters
// snapshot and the JSON emission are cased in test_telemetry_writer.cpp, the writer's
// failure paths in test_telemetry_errors.cpp, the run markers / fallback shapes / drain
// cadence in test_telemetry_wire.cpp; every TU shares the `telemetry:` name prefix and
// `[telemetry]` tag).
//
// What the ring PROMISES, each promise its own falsifiable case: records pop in FIFO
// order with every payload lane intact; a FULL ring drops the INCOMING push — never a
// queued record and never the caller's thread (`try_push` returns false immediately) —
// counts the drop, and leaves the queued records untouched; the drop counter accumulates
// and survives a drain; an empty ring pops nothing and leaves the caller's record
// untouched; capacity rounds UP to a power of two while a zero request and an
// un-roundable one throw at construction (the Outbox's fail-loudly precedent);
// `pending()` reports the consumer's backlog exactly (pushes grow it, pops shrink it,
// refusals leave it untouched); the steady state allocates nothing after construction;
// and — the reason the type exists — a producer thread and a consumer thread exchange
// a million records with no loss, no reorder and no torn lane.
//
// That last case is deliberately the TSan payload: its
// assertions prove ORDER and VALUES, while the happens-before edges the header's
// memory-ordering contract claims are what TSan verifies over the same case — a release
// store downgraded to relaxed is a DATA RACE even when the values happen to survive on
// this hardware. One measured caveat decides WHERE that verification is real
// (bench/probes/tsan_struct_copy_probe.cpp): the slot transfers are struct copies, and
// the dev host's Apple TSan runtime does not race-check memcpy-shaped accesses, so the
// HOST `ctest --preset tsan` enforces the ring's INDEX protocol only. Under the
// authoritative container toolchain (g++/libtsan — where the sanitizer gate's sanitizer matrix
// runs) every downgrade mutant of the contract's edges is reported as a data race on
// this very case and the shipped ordering is clean (stress battery).
#include <catch2/catch_test_macros.hpp>

#include "alloc_probe_support.hpp"
#include "mm/telemetry.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

// Every lane of every record is a DISTINCT function of the sequence number (distinct odd
// multiplier plus the lane index), so a TORN record — lanes from two different writes —
// fails the lane-by-lane comparison even where a single-lane check could pass by
// coincidence, and lane order swaps cannot cancel out.
constexpr std::array<std::uint64_t, 6> kLaneMul = {3, 5, 7, 11, 13, 17};

mm::TelemetryRecord seq_record(std::uint64_t seq) {
  mm::TelemetryRecord rec{.ts_ns = static_cast<std::int64_t>(seq),
                          .kind = static_cast<std::uint32_t>(seq & 1U),
                          .code = static_cast<std::uint32_t>(seq >> 1),
                          .args = {}};
  for (std::size_t lane = 0; lane < std::size(rec.args); ++lane)
    rec.args[lane] = seq * kLaneMul[lane] + lane;
  return rec;
}

// Byte-for-byte comparison against a freshly built expected record: legal because the
// record is trivially copyable with no padding (its fields tile the asserted size
// exactly), and STRICTER than fieldwise checks — any divergence anywhere in the value
// representation fails, which is precisely the tear oracle the stress case wants.
bool matches(const mm::TelemetryRecord &rec, std::uint64_t seq) {
  const mm::TelemetryRecord expected = seq_record(seq);
  return std::memcmp(&rec, &expected, sizeof rec) == 0;
}

} // namespace

TEST_CASE("telemetry: the record is one cache line, trivially copyable, and the ring and "
          "writer are pinned immovable",
          "[telemetry]") {
  STATIC_REQUIRE(sizeof(mm::TelemetryRecord) == 64);
  STATIC_REQUIRE(alignof(mm::TelemetryRecord) == 64);
  STATIC_REQUIRE(std::is_trivially_copyable_v<mm::TelemetryRecord>);
  STATIC_REQUIRE(std::is_standard_layout_v<mm::TelemetryRecord>);
  // The ring's false-sharing contract is layout too, pinned the same way: its three
  // alignas(64) members give the class 64-byte alignment and one full line each (the
  // slot vector's metadata rides dropped_'s line — the header's layout note), so
  // stripping the alignas collapses the size (measured 192 -> 48) and fails HERE at
  // compile time instead of degrading silently to cross-thread line sharing.
  STATIC_REQUIRE(alignof(mm::SpscTelemetryRing) == 64);
  STATIC_REQUIRE(sizeof(mm::SpscTelemetryRing) >= 192);
  // The push/pop slot copies are the noexcept signatures' one real operation each.
  STATIC_REQUIRE(noexcept(std::declval<mm::SpscTelemetryRing &>().try_push(
      std::declval<const mm::TelemetryRecord &>())));
  STATIC_REQUIRE(noexcept(
      std::declval<mm::SpscTelemetryRing &>().try_pop(std::declval<mm::TelemetryRecord &>())));
  STATIC_REQUIRE(noexcept(std::declval<const mm::SpscTelemetryRing &>().dropped()));
  STATIC_REQUIRE(noexcept(std::declval<const mm::SpscTelemetryRing &>().pending()));
  STATIC_REQUIRE(noexcept(std::declval<const mm::SpscTelemetryRing &>().capacity()));
  // Two live threads hold references into each object; a transfer would leave one of them
  // writing into a husk (rationale at the deleted members in telemetry_ring.hpp).
  STATIC_REQUIRE(!std::is_copy_constructible_v<mm::SpscTelemetryRing>);
  STATIC_REQUIRE(!std::is_copy_assignable_v<mm::SpscTelemetryRing>);
  STATIC_REQUIRE(!std::is_move_constructible_v<mm::SpscTelemetryRing>);
  STATIC_REQUIRE(!std::is_move_assignable_v<mm::SpscTelemetryRing>);
  STATIC_REQUIRE(!std::is_copy_constructible_v<mm::TelemetryWriter>);
  STATIC_REQUIRE(!std::is_copy_assignable_v<mm::TelemetryWriter>);
  STATIC_REQUIRE(!std::is_move_constructible_v<mm::TelemetryWriter>);
  STATIC_REQUIRE(!std::is_move_assignable_v<mm::TelemetryWriter>);
  // Counters stays a plain single-writer aggregate: no atomics anywhere in it (rule A1).
  // Trivial copyability alone is VACUOUS as that pin on this host: Apple clang's
  // __is_trivially_copyable reports std::atomic<uint64_t> trivially copyable (measured
  // tc=1 for the atomic and for a struct of them, in defiance of the
  // at-least-one-non-deleted-copy-op requirement), so a struct of atomics would still
  // pass it. Copy-ASSIGNABILITY is decided by real overload resolution — std::atomic
  // deletes its copy assignment — so the assignability pins are what an atomic-membered
  // mutant actually fails to compile at: the plain form is the contract's minimum, and
  // the TRIVIAL form is deliberately the stronger seal on top, additionally outlawing a
  // user-provided operator= that could smuggle synchronization in behind a
  // still-assignable interface.
  STATIC_REQUIRE(std::is_trivially_copyable_v<mm::Counters>);
  STATIC_REQUIRE(std::is_copy_assignable_v<mm::Counters>);
  STATIC_REQUIRE(std::is_trivially_copy_assignable_v<mm::Counters>);
  // Counters layout, pinned against the FINAL field count with the padding stated:
  // eleven 8-byte fields are 88 payload bytes, and the chosen alignas(64) rounds sizeof
  // to 128 (40 tail-padding bytes; the trade and its declined alternative are stated at
  // the struct). A field added without re-deriving this pin fails HERE, not in a layout
  // surprise downstream.
  STATIC_REQUIRE(alignof(mm::Counters) == 64);
  STATIC_REQUIRE(sizeof(mm::Counters) == 128);
}

TEST_CASE("telemetry: records pop in FIFO order with every lane intact", "[telemetry]") {
  mm::SpscTelemetryRing ring{128};
  REQUIRE(ring.pending() == 0);
  for (std::uint64_t seq = 1; seq <= 100; ++seq)
    REQUIRE(ring.try_push(seq_record(seq)));
  REQUIRE(ring.pending() == 100); // the consumer's backlog: exactly the published count
  mm::TelemetryRecord rec{};
  for (std::uint64_t seq = 1; seq <= 100; ++seq) {
    REQUIRE(ring.try_pop(rec));
    REQUIRE(matches(rec, seq));
  }
  REQUIRE(ring.pending() == 0);
  REQUIRE_FALSE(ring.try_pop(rec));
  REQUIRE(ring.dropped() == 0);
}

TEST_CASE("telemetry: a full ring drops the push, counts it, and its queued records survive",
          "[telemetry]") {
  mm::SpscTelemetryRing ring{8};
  REQUIRE(ring.capacity() == 8);
  for (std::uint64_t seq = 1; seq <= 8; ++seq)
    REQUIRE(ring.try_push(seq_record(seq)));

  REQUIRE_FALSE(ring.try_push(seq_record(9)));
  REQUIRE(ring.dropped() == 1);
  REQUIRE_FALSE(ring.try_push(seq_record(10)));
  REQUIRE(ring.dropped() == 2);
  REQUIRE(ring.pending() == 8); // a refused push published nothing: the backlog is untouched

  // The dropped pushes wrote NOTHING: every pre-overflow record pops back intact.
  mm::TelemetryRecord rec{};
  for (std::uint64_t seq = 1; seq <= 8; ++seq) {
    REQUIRE(ring.try_pop(rec));
    REQUIRE(matches(rec, seq));
  }
  REQUIRE_FALSE(ring.try_pop(rec));

  SECTION("the drop counter accumulates: a drain and new traffic never reset it") {
    REQUIRE(ring.dropped() == 2);
    REQUIRE(ring.try_push(seq_record(11)));
    REQUIRE(ring.dropped() == 2);
  }
  SECTION("a drained ring accepts a full round again") {
    for (std::uint64_t seq = 21; seq <= 28; ++seq)
      REQUIRE(ring.try_push(seq_record(seq)));
    for (std::uint64_t seq = 21; seq <= 28; ++seq) {
      REQUIRE(ring.try_pop(rec));
      REQUIRE(matches(rec, seq));
    }
  }
}

TEST_CASE("telemetry: an empty ring pops nothing and leaves the caller's record untouched",
          "[telemetry]") {
  mm::SpscTelemetryRing ring;
  mm::TelemetryRecord rec{.ts_ns = -1, .kind = 77, .code = 88, .args = {1, 2, 3, 4, 5, 6}};
  REQUIRE_FALSE(ring.try_pop(rec));
  CHECK(rec.ts_ns == -1);
  CHECK(rec.kind == 77);
  CHECK(rec.code == 88);
  for (std::size_t lane = 0; lane < std::size(rec.args); ++lane)
    CHECK(rec.args[lane] == lane + 1);
  // Failed pops are not drops: only a refused PUSH loses a record.
  REQUIRE(ring.dropped() == 0);
}

TEST_CASE("telemetry: capacity rounds up to a power of two and zero is rejected", "[telemetry]") {
  CHECK(mm::SpscTelemetryRing{1}.capacity() == 1);
  CHECK(mm::SpscTelemetryRing{3}.capacity() == 4);
  CHECK(mm::SpscTelemetryRing{5}.capacity() == 8);
  CHECK(mm::SpscTelemetryRing{8}.capacity() == 8);
  CHECK(mm::SpscTelemetryRing{1023}.capacity() == 1024);
  CHECK(mm::SpscTelemetryRing{}.capacity() == 4096); // the design's default
  CHECK(mm::SpscTelemetryRing{}.capacity() == mm::SpscTelemetryRing::kDefaultCapacity);
  REQUIRE_THROWS_AS(mm::SpscTelemetryRing{0}, std::invalid_argument);
  // The other rounding edge: a request past bit_ceil's representable range must throw the
  // same type, not violate bit_ceil's precondition and silently build a 1-slot ring.
  // Deliberately NOT probed at SIZE_MAX/2+1: that value rounds representably to 2^63 and
  // then legitimately throws std::length_error from the vector allocation instead.
  REQUIRE_THROWS_AS(mm::SpscTelemetryRing{SIZE_MAX}, std::invalid_argument);

  SECTION("the rounded capacity is fully usable: a five-slot request holds exactly eight") {
    mm::SpscTelemetryRing ring{5};
    for (std::uint64_t seq = 1; seq <= 8; ++seq)
      REQUIRE(ring.try_push(seq_record(seq)));
    REQUIRE_FALSE(ring.try_push(seq_record(9)));
    REQUIRE(ring.dropped() == 1);
  }
  SECTION("a one-slot ring is legal and obeys the same full/empty rules") {
    mm::SpscTelemetryRing ring{1};
    REQUIRE(ring.try_push(seq_record(1)));
    REQUIRE_FALSE(ring.try_push(seq_record(2)));
    mm::TelemetryRecord rec{};
    REQUIRE(ring.try_pop(rec));
    REQUIRE(matches(rec, 1));
    REQUIRE_FALSE(ring.try_pop(rec));
    REQUIRE(ring.try_push(seq_record(3)));
  }
}

TEST_CASE("telemetry: the steady state allocates nothing after construction", "[telemetry]") {
  // Construction owns the type's ONLY allocation (the slot storage). The steady-state
  // paths — push, pop, full-drop, empty-pop, wraparound — must never touch the heap:
  // pinned by the suite's global operator-new counter AND by the capacity pin, which also
  // catches the regrowth route the counter cannot see (over-aligned slot storage allocates
  // through aligned new, which alloc_probe.cpp deliberately leaves unintercepted).
  mm::SpscTelemetryRing ring{64};
  const std::size_t cap_before = ring.capacity();
  mm::TelemetryRecord rec{};
  const std::size_t allocs = alloc_probe::allocations_in([&ring, &rec] {
    for (std::uint64_t seq = 0; seq < 10'000; ++seq) {
      (void)ring.try_push(seq_record(seq)); // fills, then keeps refusing while full
      if (seq % 3 == 0)
        (void)ring.try_pop(rec); // uneven drain: full, empty and wrap boundaries all cross
    }
    while (ring.try_pop(rec)) {
    }
  });
  REQUIRE(allocs == 0);
  REQUIRE(ring.capacity() == cap_before);
  REQUIRE(ring.dropped() > 0); // the full path really ran inside the counted region
}

TEST_CASE("telemetry: a million records cross the threads in order, none torn", "[telemetry]") {
  // The TSan payload . The producer RETRIES refused pushes —
  // the ring itself never blocks; the retry loop is this test's, standing in for records
  // the engine would instead let drop — so every record must arrive, in order, with every
  // lane intact. dropped() is left unasserted here BY CONTRACT: it counts REFUSALS (each
  // full-ring attempt increments it, the telemetry subsystem), so under retries it tallies
  // momentary fullness, not lost records — loss-freedom is exactly `received == kRecords` in order.
  constexpr std::uint64_t kRecords = 1'000'000;
  mm::SpscTelemetryRing ring{1024};
  const std::size_t cap_before = ring.capacity();

  // The retry loop honors a cancellation flag: when the consumer's deadline below fires,
  // the producer may be mid-retry against a full ring nobody will ever drain again, and
  // an unconditional join would turn the safety valve into the very hang it exists to
  // prevent — the timeout path must CANCEL, then join.
  std::atomic<bool> cancel{false};
  std::thread producer{[&ring, &cancel] {
    for (std::uint64_t seq = 0; seq < kRecords; ++seq) {
      const mm::TelemetryRecord rec = seq_record(seq);
      while (!ring.try_push(rec)) {
        if (cancel.load(std::memory_order_relaxed))
          return;
        std::this_thread::yield();
      }
    }
  }};

  // No Catch2 assertion inside the hot loop (assertions allocate and serialize the run):
  // mismatches are counted and asserted once after the join. The deadline is a safety
  // valve so a record-LOSING mutant fails the case instead of hanging the suite.
  std::uint64_t received = 0;
  std::uint64_t out_of_order_or_torn = 0;
  mm::TelemetryRecord rec{};
  auto last_progress = std::chrono::steady_clock::now();
  while (received < kRecords) {
    if (!ring.try_pop(rec)) {
      if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(30)) {
        cancel.store(true, std::memory_order_relaxed);
        break;
      }
      std::this_thread::yield();
      continue;
    }
    if (!matches(rec, received))
      ++out_of_order_or_torn;
    ++received;
    last_progress = std::chrono::steady_clock::now();
  }
  producer.join();

  REQUIRE(out_of_order_or_torn == 0);
  REQUIRE(received == kRecords);
  REQUIRE_FALSE(ring.try_pop(rec));
  REQUIRE(ring.capacity() == cap_before); // no reallocation across the stress run
}
