// The SPSC ring half of the in-process telemetry subsystem (plan Task 6.5, record A1):
// the fixed one-cache-line record, its typed discriminants, and the bounded
// single-producer/single-consumer ring the engine owner thread pushes through. Split out
// of mm/telemetry.hpp at gate P4-i1 under the repo's 500-line file cap — that header
// remains the subsystem's umbrella (writer thread, Counters, snapshot/event mapping, and
// the subsystem-level rationale incl. the normative thread-roles/cardinality contract);
// include it, not this file, unless only the ring is needed.
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

// Record discriminants: TelemetryRecord::kind selects the record family and, for
// counters records, TelemetryRecord::code selects the snapshot part. A full Counters
// snapshot does not fit one record's payload, so it crosses the ring as TWO
// self-describing records — totals and watermarks (see snapshot_records below) — each
// emitting as its own complete JSON line: a ring-full drop of one part loses exactly
// that part, and the writer never stitches partial state. Typed enums rather than bare
// integer constants, so a swapped kind/code pair or a raw-integer comparison fails to
// compile at the boundary; the record FIELDS stay std::uint32_t (POD lanes), with the
// casts exactly at that boundary and nowhere else.
enum class TelemetryKind : std::uint32_t { // NOLINT(performance-enum-size) — field's type
  Counters = 0,
  Event = 1,
};
enum class CountersPart : std::uint32_t { // NOLINT(performance-enum-size) — field's type
  Totals = 0,
  Watermarks = 1,
};

// Sparse-event codes (TelemetryRecord::code when kind == TelemetryKind::Event). Task 7
// emits these from the session lifecycle; the writer names them on the wire via
// to_string. The underlying type deliberately IS the record field's type, not the
// smallest that fits the enumerators: the writer casts rec.code back through this enum,
// and a narrower base would make that cast truncate, letting an unknown wide code alias
// a NAMED event in the fallback path (e.g. any code ≡ Reject mod 256 would print as
// "reject").
//
// Event-lane contract — what each args lane carries on the wire. Normative for Task 7's
// emit sites, pinned by its integration tests (PENDING item (s)11):
//   session_open       args[0] = epoch
//   session_close      args[0] = epoch   args[1] = close_code
//   hwm_close          args[0] = epoch   args[1] = report_depth
//   reject             args[0] = epoch   args[1] = RejectCode
//   cmd_in             args[0] = epoch   args[1] = svc_ns   (verbose only)
//   tob_out            args[0] = epoch   args[1] = md_seq   (verbose only)
//   entry_cap_close    args[0] = epoch   args[1] = entry_count at the breach
//   admission_refused  args[0] = the load THIS TIER'S BOUND COMPARED   args[1] = tier
//   upgrade_refused    args[0] = HTTP status   args[1] = refusal reason
//                      (0 = subprotocol, 1 = origin, 2 = admission)
//   engine_fault       args[0] = 0       args[1] = 0
// Lane 0 of admission_refused is a LOAD, not an epoch — the refusal precedes the session
// that would own one. The two tiers compare DIFFERENT quantities by design ((t)14's
// descriptor ceiling is max_sessions sessions PLUS max_sessions pre-upgrade sockets), so
// each emits what its own bound tested rather than one unified figure that would
// desynchronise the event from the check that fired it: in-flight upgrades alone at the
// accept tier (args[1] = 0), sessions + in-flight upgrades at the upgrade tier
// (args[1] = 1). engine_fault likewise carries no epoch: it records that run() unwound
// on an exception, which is a property of the RUN, not of any session.
// The 503 upgrade-tier refusal emits BOTH admission_refused and upgrade_refused, by
// design and not by oversight: upgrade_refused is emitted from the one refusal helper so
// EVERY refusal narrates from a single place (a new refusal reason gets its telemetry for
// free), while admission_refused carries the load figure the bound compared — neither is
// derivable from the other, so dropping either loses information.
// The last two of the first group are the PER-MESSAGE events of record A1: emitted only
// under Task 7's --telemetry-verbose, forced OFF whenever --bench-out is set (per-message
// narration on a measured run would be the distortion §5.2 exists to exclude; the
// manifest records the setting). Additive within v1: new event names join the enum and
// the switch below; the sparse events above are unchanged.
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

// Wire names for the telemetry JSON lines (same discipline as types.hpp's reject-code
// spellings: one normative mapping, pinned by test). Backstop: an out-of-range value cast
// into TelemetryEvent has NO name — the empty view tells the writer to emit the numeric
// self-describing form instead of inventing a spelling; every in-range enumerator returns
// from the exhaustive switch (-Wswitch flags an appended one).
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

// Fixed one-cache-line POD record. alignas keeps every slot of the ring's storage on its
// own line (the array would otherwise straddle two lines per slot at typical allocator
// alignment), so a producer-side slot write and a consumer-side slot read of NEIGHBORING
// slots never share a line.
struct alignas(64) TelemetryRecord {
  std::int64_t ts_ns;    // steady_clock at emit
  std::uint32_t kind;    // TelemetryKind, cast at this POD boundary
  std::uint32_t code;    // counters: CountersPart; event: TelemetryEvent
  std::uint64_t args[6]; // payload: snapshot lanes (see snapshot_records) or event args
};
static_assert(sizeof(TelemetryRecord) == 64);
static_assert(std::is_trivially_copyable_v<TelemetryRecord>,
              "the push/pop slot copies are the noexcept signatures' one real operation");

// Single producer (engine owner thread), single consumer (writer thread).
//
// Memory-ordering contract (verified by the threaded stress test, and by TSan over it —
// enforceable only under a runtime that race-checks struct-copy accesses; see the test
// TU's note and bench/probes/tsan_struct_copy_probe.cpp):
//   try_push: head_.load(acquire) for the space check, write the slot,
//             tail_.store(release).
//   try_pop:  tail_.load(acquire), read the slot, head_.store(release).
// The producer's release on tail_ pairs with the consumer's acquire: a consumer that
// observes the advanced tail therefore observes the slot bytes it publishes. The
// consumer's release on head_ pairs with the producer's acquire: a producer that observes
// the advanced head knows the reader is DONE with the freed slot before overwriting it —
// that second edge is the wraparound half a torn read would slip through. Each side loads
// its OWN index relaxed (it is that index's only writer). Indices increase monotonically
// and never mask-wrap in the uint64 space (full is `tail - head == capacity`), so
// capacity slots are all usable and index reuse is out of arithmetic reach.
//
// Three DESIGN choices on top of that contract, each with its price stated (figures from
// bench/probes/ring_push_probe.cpp, 2026-07-29, dev host + container — the build/run
// lines are in its header; re-take them rather than believe them):
//   - The producer RE-READS head_ on every push. The classic cached-head amortization
//     (cache the last-seen head, reload only on apparent-full) pays only when pushes
//     arrive back-to-back — measured +29% to +94% across runs (host) and +188%
//     (container) two-thread SATURATION throughput; flat-out A/B is scheduler-noisy,
//     so the spread is the report, and the invariant is that it buys nothing at this
//     ring's ~1 Hz duty — and it buys that saturation gain with a SECOND index
//     invariant (a cache that may lag reality) every future reader must re-verify.
//     Declined.
//   - Push cost itself: measured ~2.2 ns (host) / ~6.3 ns (container) per L1-hot
//     push+pop pair, so the producer-side cost of a snapshot is noise against the 1 Hz
//     cadence that triggers it.
//   - The producer never NOTIFIES the writer. The no-waiter notify_one FLOOR is
//     pair-priced (0.3-1.8x measured), but that floor is not the observed path: at this
//     duty the writer is PARKED when a push arrives, so a notifying producer takes a
//     real wake — a futex/ulock syscall measuring ~2.1 us (host) / ~15.5 us (container)
//     — three orders of magnitude over the push it decorates, spent to shave latency
//     off telemetry nobody reads at that latency. TelemetryWriter::kDefaultPoll
//     (mm/telemetry.hpp) is the wake-free alternative.
class SpscTelemetryRing {
public:
  // Capacity and TelemetryWriter::kDefaultPoll are a COUPLED saturation contract, not
  // independent knobs: the producer-side backlog peaks near 1.25 x R x poll for a
  // sustained record rate R (drain-lag factor measured by ring_push_probe's saturation
  // mode, 2026-07-29), so 4096 slots against the 10 ms poll put the measured onset of
  // drops between 300k and 350k records/s — same bracket on the dev host and
  // in-container, orders of magnitude above the shipped 1 Hz snapshot + sparse-event
  // duty. Changing either constant moves that threshold: re-take the probe, or read the
  // shipped telemetry_pending_hw watermark live, rather than scaling the margin by eye.
  static constexpr std::size_t kDefaultCapacity = 4096;

  // Capacity is rounded UP to the next power of two (mask indexing) and fixed for the
  // object's lifetime; the slot storage is the type's only allocation. Throws
  // std::invalid_argument on a zero request (a ring that can hold nothing would count
  // every push as a drop — same fail-loudly policy as the Outbox's zero mark) and on one
  // whose rounding cannot be represented.
  explicit SpscTelemetryRing(std::size_t min_capacity = kDefaultCapacity)
      : slots_(checked_capacity(min_capacity)) {}

  // Both ends hold references across threads; a transfer would leave one of them writing
  // into a husk, and two owners would each believe they exclusively own an index.
  SpscTelemetryRing(const SpscTelemetryRing &) = delete;
  SpscTelemetryRing &operator=(const SpscTelemetryRing &) = delete;
  SpscTelemetryRing(SpscTelemetryRing &&) = delete;
  SpscTelemetryRing &operator=(SpscTelemetryRing &&) = delete;

  // NEVER blocks and never overwrites: full ⇒ write nothing, count the drop, return
  // false. The verdict is deliberately discardable — fire-and-forget is the intended
  // producer stance, because the drop is already counted where the next snapshot line
  // reports it (dropped()); losing telemetry must never stall the path it observes.
  bool try_push(const TelemetryRecord &rec) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    if (tail - head == slots_.size()) {
      // Single writer: an RMW would buy nothing and cost an atomic read-modify-write —
      // same rule A1 as Counters. NB this leans on the single-producer contract harder
      // than the fetch_add it replaced (a second producer would now LOSE increments, not
      // merely contend on them) — one reason the THREAD ROLES cardinality above is
      // normative, not advisory.
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

  // Pushes refused because the ring was full, accumulated for the object's lifetime
  // (count-every-drop discipline). Producer-incremented; relaxed read from any thread —
  // the writer stamps the value to date into every totals line it emits, which is how a
  // drop becomes visible in the "next snapshot".
  [[nodiscard]] std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

  // Records published but not yet consumed at the instant of the call. Per-thread
  // contract, and it is three-way, not two-way:
  //   CONSUMER (same thread as try_pop) — EXACT: head_ is the caller's own index and the
  //     value can only grow under its feet (tail_ only advances). This is the writer's
  //     batch budget: the finite cutoff that keeps every drain bounded (see
  //     TelemetryWriter::drain).
  //   PRODUCER — safe OVERESTIMATE: tail_ is its own index (latest by definition), and
  //     head_ only ever advances, so a stale head_ read can only under-count consumption —
  //     the subtraction never underflows.
  //   any OTHER thread — do not read it there: neither index is its own, so a FRESH head_
  //     can pair with a STALE tail_ (per-variable coherence orders neither load against
  //     the other variable's writes) and the subtraction underflows to ~2^64. Deliberately
  //     NOT clamped: no third thread is in the contract, and a clamp would dress an
  //     out-of-contract read as a plausible number.
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
  // consumer's head line and vice versa. Calibration for that "line": the authoritative
  // surface is ubuntu:26.04 linux/arm64 ON the M3 host, whose coherence granule is 128
  // bytes (sysctl hw.cachelinesize; the container VM runs on the same silicon), so
  // 64-byte-separated indices can still share a granule there — a cross-cluster
  // co-scheduled run measures that sharing at ~2.5x per contended access, though the
  // contrast is scheduler-dependent and flat when the OS co-locates the two ends
  // (bench/probes/ring_push_probe.cpp granule mode records both observations and the
  // two instruments it rejected) — while the amd64 CI surface isolates fully at 64.
  // Repadding to 128 was DECLINED: at this ring's ~1 Hz duty the residual sharing is
  // unobservable, and it would break the alignof==64 layout pin in test_telemetry.cpp.
  // The slot vector's metadata shares dropped_'s line: read-mostly beside a counter
  // written only on the full path, which is already the degraded regime.
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
