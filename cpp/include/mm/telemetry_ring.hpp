// The SPSC ring half of the in-process telemetry subsystem: the fixed one-cache-line record, its
// typed discriminants, and the bounded ring. Include mm/telemetry.hpp unless only this is needed.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mm {

// TelemetryRecord::kind selects the record family; for counters records, code selects the
// snapshot part, which crosses the ring as TWO records so a ring-full drop loses one part only.
enum class TelemetryKind : std::uint32_t { // NOLINT(performance-enum-size) — field's type
  Counters = 0,
  Event = 1,
};
enum class CountersPart : std::uint32_t { // NOLINT(performance-enum-size) — field's type
  Totals = 0,
  Watermarks = 1,
};

// Sparse-event codes (TelemetryRecord::code when kind == Event); the args lanes carry per-event
// payload. The base type IS the record field's: narrowing it would alias unknown codes to names.
enum class TelemetryEvent : std::uint32_t { // NOLINT(performance-enum-size)
  SessionOpen = 0,
  SessionClose = 1,
  HwmClose = 2,
  Reject = 3,
  CmdIn = 4,
  TobOut = 5,
  AdmissionRefused = 6,
  EntryCapClose = 7,
  UpgradeRefused = 8,
  EngineFault = 9,
};

// Wire names for the telemetry JSON lines: one normative mapping, pinned by test. An
// out-of-range value has no name, so the writer emits the numeric self-describing form.
[[nodiscard]] constexpr std::string_view to_string(TelemetryEvent event) noexcept {
  switch (event) {
  case TelemetryEvent::SessionOpen:
    return "session_open";
  case TelemetryEvent::SessionClose:
    return "session_close";
  case TelemetryEvent::HwmClose:
    return "hwm_close";
  case TelemetryEvent::Reject:
    return "reject";
  case TelemetryEvent::CmdIn:
    return "cmd_in";
  case TelemetryEvent::TobOut:
    return "tob_out";
  case TelemetryEvent::AdmissionRefused:
    return "admission_refused";
  case TelemetryEvent::EntryCapClose:
    return "entry_cap_close";
  case TelemetryEvent::UpgradeRefused:
    return "upgrade_refused";
  case TelemetryEvent::EngineFault:
    return "engine_fault";
  }
  return {};
}

// Fixed one-cache-line POD record. alignas keeps every ring slot on its own line, so a
// producer's slot write and a consumer's read of neighboring slots never share one.
struct alignas(64) TelemetryRecord {
  std::int64_t ts_ns;    // steady_clock at emit
  std::uint32_t kind;    // TelemetryKind, cast at this POD boundary
  std::uint32_t code;    // counters: CountersPart; event: TelemetryEvent
  std::uint64_t args[6]; // payload: snapshot lanes (see snapshot_records) or event args
};
static_assert(sizeof(TelemetryRecord) == 64);
static_assert(std::is_trivially_copyable_v<TelemetryRecord>,
              "the push/pop slot copies are the noexcept signatures' one real operation");

// Single producer (engine owner thread), single consumer (writer thread). tail_
// release/acquire publishes the slot bytes; head_ release/acquire frees the slot before reuse.
class SpscTelemetryRing {
public:
  // Capacity and TelemetryWriter::kDefaultPoll are a coupled saturation contract: producer-side
  // backlog peaks near 1.25 x rate x poll, so changing either moves the onset of drops.
  static constexpr std::size_t kDefaultCapacity = 4096;

  // Capacity is rounded UP to the next power of two (mask indexing) and fixed for the object's
  // lifetime. Throws std::invalid_argument on zero (every push would drop) or on overflow.
  explicit SpscTelemetryRing(std::size_t min_capacity = kDefaultCapacity)
      : slots_(checked_capacity(min_capacity)) {}

  // Both ends hold references across threads; a transfer would leave one of them writing
  // into a husk, and two owners would each believe they exclusively own an index.
  SpscTelemetryRing(const SpscTelemetryRing &) = delete;
  SpscTelemetryRing &operator=(const SpscTelemetryRing &) = delete;
  SpscTelemetryRing(SpscTelemetryRing &&) = delete;
  SpscTelemetryRing &operator=(SpscTelemetryRing &&) = delete;

  // NEVER blocks and never overwrites: full ⇒ write nothing, count the drop, return false. The
  // verdict is discardable by design — the drop is already counted where dropped() reports it.
  bool try_push(const TelemetryRecord &rec) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    if (tail - head == slots_.size()) {
      // Single writer: an RMW would buy nothing here, but this leans on the single-producer
      // contract — a second producer would LOSE increments, not merely contend on them.
      dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      return false;
    }
    slots_[static_cast<std::size_t>(tail) & (slots_.size() - 1)] = rec;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Empty ⇒ returns false and leaves `out` untouched.
  [[nodiscard]] bool try_pop(TelemetryRecord &out) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    if (tail == head)
      return false;
    out = slots_[static_cast<std::size_t>(head) & (slots_.size() - 1)];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Pushes refused because the ring was full, for the object's lifetime. Producer-incremented,
  // relaxed read from any thread; the writer stamps it into every totals line it emits.
  [[nodiscard]] std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

  // Published but not yet consumed. EXACT on the consumer (head_ is its own), a safe
  // overestimate on the producer, off-contract elsewhere: a stale tail_ underflows it.
  [[nodiscard]] std::size_t pending() const noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(tail - head);
  }

  // The rounded, fixed slot count — every slot usable. Public so the rounding and the
  // no-reallocation-after-construction contracts are testable from outside.
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
  static std::size_t checked_capacity(std::size_t min_capacity) {
    if (min_capacity == 0)
      throw std::invalid_argument("SpscTelemetryRing: capacity must be > 0");
    if (min_capacity > SIZE_MAX / 2 + 1)
      throw std::invalid_argument("SpscTelemetryRing: capacity rounds past size_t");
    return std::bit_ceil(min_capacity);
  }

  // Each index on its own cache line so the producer's tail stores never invalidate the
  // consumer's head line and vice versa; the slot vector's metadata shares dropped_'s line.
  alignas(64) std::atomic<std::uint64_t> head_{0}; // consumer index — store(release) by consumer
  alignas(64) std::atomic<std::uint64_t> tail_{0}; // producer index — store(release) by producer
  alignas(64) std::atomic<std::uint64_t> dropped_{0};
  std::vector<TelemetryRecord> slots_; // pow2 capacity, mask indexing; never reallocated
};

static_assert(!std::is_copy_constructible_v<SpscTelemetryRing> &&
              !std::is_copy_assignable_v<SpscTelemetryRing> &&
              !std::is_move_constructible_v<SpscTelemetryRing> &&
              !std::is_move_assignable_v<SpscTelemetryRing>);

} // namespace mm
