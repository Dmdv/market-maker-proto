// Latency-sample recorder for the §5.2 harness (plan Task 7; record A3 + its addendum).
// Header-only, preallocated, and NO allocation ON THE RECORDING PATH: every record method
// is a bounds-checked array write, so the recorder can sit inside the measured windows
// without becoming part of what it measures (pinned on the suite's global operator-new
// probe in cpp/tests/test_bench_recorder.cpp). The one exception is serialize(), which
// materialises the artifact once at SHUTDOWN — after the measured window has closed, so it
// cannot distort what it reports. Stated as a path property rather than a lifetime one
// because the flat "no allocation after construction" it replaces became false the moment
// the dump stopped writing a destination it opened itself.
//
// WHAT THE STREAMS MEAN (the m0' attribution guard, A3 addendum — venue-simulation cost
// must not be misread as engine/transport cost):
//   m0  — the book was APPLIED on the owner thread (md_published, from the feed driver).
//   m0' — the TOB frame was HANDED TO async_write (md_written, from the session write
//         path, immediately before the write): the boundary separating venue PRODUCTION
//         (simulation-side, non-transferable) from DELIVERY.
//   m3  — the FIRST client order echoing that md_seq arrived (order_for): the reaction.
//   svc — e2 - e1 of one command (the engine service window, M2).
// Derived streams recorded: m0->m0' (venue production), m0'->m3 (delivery + client
// reaction — the transferable headline), m0->m3 (total), plus svc.
//
// FIRST-ECHO / FIRST-WRITE RULES: order_for records only the FIRST order per md_seq —
// later echoes are pacing, not reaction (F-01) — and md_written records only the FIRST
// hand-off per md_seq. The second rule carries a PRECONDITION the first does not: the
// first hand-off is the delivery boundary only at ONE session. With N sessions each later
// session's write waits on THAT session's prior async_write, so the recorded m0->m0' is
// the MINIMUM across the fan-out, not the boundary — measured 34-79x low at 8 to 64
// sessions. The dump's sixth header word carries the run's peak session count so the two
// cases are distinguishable from the artifact alone.
//
// SLOTS: a ring keyed by md_seq (index = md_seq % kCorrelationSlots). The §5.2 react run
// holds ~2 orders in flight against a monotonic md_seq, so a slot is correlated long
// before it can be evicted; an order echoing an evicted (or never-published) md_seq
// records nothing. md_seq 0 is the empty-slot sentinel — the Task 7 driver assigns from 1.
// The correlation WINDOW and the sample CAPACITY are independent dimensions and are sized
// by SEPARATE constants: driving both from one parameter cost --bench-out 38.4 MB of
// value-initialized slots before a single sample was taken (measured RSS 2,512 kB without
// the flag, 40,144 kB with it) to hold a window ~600,000x wider than the ~2 in flight.
//
// DUMP FORMAT (the Task 11 harness contract): a fixed header of SIX little-endian
// uint64s — the four sample counts (svc, m0->m0', m0'->m3, m0->m3), then the refused-
// sample count (saturated()), then the run's PEAK CONCURRENT SESSION COUNT — followed by
// the four raw little-endian int64 sample streams in that order, nothing else. The last
// two words are what make a run self-describing: the four counts alone cannot tell "ended
// at capacity" from "ended under it", nor a single-session run from a fan-out run whose
// m0->m0' is the fan-out minimum, and a harness that reads only the file never sees
// stderr. A harness MUST refuse or flag a dump whose peak session count is > 1.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace mm {

class BenchRecorder {
public:
  // SAMPLES. Sized for the §5.2 react run: >= 100k samples plus warm-up headroom (record
  // A3's arithmetic — ≈1k TOB/s for ≈110 s — an order of magnitude inside this default).
  // Drives the four streams' reserve() and nothing else.
  static constexpr std::size_t kDefaultCapacity = 1'200'000;

  // CORRELATION WINDOW, sized independently of the samples. 64Ki slots is ≈65 s of window
  // at the §5.2 1 kHz tick rate against a ~2-in-flight requirement, for 2 MB resident
  // rather than the 38.4 MB that sizing the ring from the sample capacity costs.
  static constexpr std::size_t kCorrelationSlots = 65'536;

  explicit BenchRecorder(std::size_t capacity = kDefaultCapacity) : slots_(kCorrelationSlots) {
    // The ring divides by kCorrelationSlots, so nothing downstream catches a zero here: a
    // zero-capacity recorder reserves nothing, refuses every sample, and dumps four empty
    // streams while still looking live to its caller.
    if (capacity == 0)
      throw std::invalid_argument("BenchRecorder: capacity must be > 0");
    svc_.reserve(capacity);
    m0_m0p_.reserve(capacity);
    m0p_m3_.reserve(capacity);
    m0_m3_.reserve(capacity);
  }

  // The dump belongs to exactly one run's arrays; nothing transfers them.
  BenchRecorder(const BenchRecorder &) = delete;
  BenchRecorder &operator=(const BenchRecorder &) = delete;
  BenchRecorder(BenchRecorder &&) = delete;
  BenchRecorder &operator=(BenchRecorder &&) = delete;

  // Book applied: opens the md_seq's slot (unconditionally — a fresh publish owns it).
  void md_published(std::uint64_t md_seq, std::int64_t m0_ns) noexcept {
    if (md_seq == 0)
      return;
    slots_[static_cast<std::size_t>(md_seq % slots_.size())] =
        Slot{.md_seq = md_seq, .m0_ns = m0_ns, .m0p_ns = 0, .m3_taken = false};
  }

  // TOB frame handed to async_write (first hand-off only — see the header rules).
  void md_written(std::uint64_t md_seq, std::int64_t m0p_ns) noexcept {
    Slot *slot = find(md_seq);
    if (slot == nullptr || slot->m0p_ns != 0)
      return;
    slot->m0p_ns = m0p_ns;
    append(m0_m0p_, m0p_ns - slot->m0_ns);
  }

  // First order echoing md_seq arrived (its e1). Later echoes are ignored.
  void order_for(std::uint64_t md_seq, std::int64_t m3_ns) noexcept {
    Slot *slot = find(md_seq);
    if (slot == nullptr || slot->m3_taken)
      return;
    slot->m3_taken = true;
    if (slot->m0p_ns != 0)
      append(m0p_m3_, m3_ns - slot->m0p_ns);
    append(m0_m3_, m3_ns - slot->m0_ns);
  }

  void svc(std::int64_t e2_minus_e1_ns) noexcept { append(svc_, e2_minus_e1_ns); }

  // The shutdown artifact, materialised in memory in the format documented above.
  //
  // It returns BYTES and names no destination, which is the point: the engine holds its
  // bench output as a file DESCRIPTOR bound at startup and writes through that, so the
  // identity checked when the run began is the identity written when it ends. A recorder
  // that opened — or even named — its own output would put a pathname back on the shutdown
  // path, where re-resolving it is exactly the hazard the binding removes (a symlink or hard
  // link swapped in mid-run would redirect a truncating write onto the telemetry artifact).
  //
  // peak_sessions is the run's PEAK concurrent session count, tracked as a watermark by
  // ServerImpl: the recorder has no session notion and must not grow one. The parameter is
  // deliberately NOT defaulted — a default would let a caller silently omit the very
  // dimension the sixth header word exists to expose.
  //
  // This is the one allocation the recorder makes after construction. It is shutdown-sized
  // and happens once, after the measured window has closed, so it cannot distort what it
  // reports — see the recording-path qualifier in the file header.
  [[nodiscard]] std::vector<char> serialize(std::uint64_t peak_sessions) const {
    static_assert(std::endian::native == std::endian::little,
                  "the dump format is little-endian by direct memory write; a big-endian "
                  "port must add byte swaps here");
    const std::uint64_t header[6] = {
        svc_.size(), m0_m0p_.size(), m0p_m3_.size(), m0_m3_.size(), saturated_, peak_sessions,
    };
    std::vector<char> bytes;
    bytes.reserve(sizeof header + (svc_.size() + m0_m0p_.size() + m0p_m3_.size() + m0_m3_.size()) *
                                      sizeof(std::int64_t));
    const auto append_bytes = [&bytes](const void *src, std::size_t n) {
      const auto *p = static_cast<const char *>(src);
      bytes.insert(bytes.end(), p, p + n);
    };
    append_bytes(header, sizeof header);
    for (const auto *stream : {&svc_, &m0_m0p_, &m0p_m3_, &m0_m3_})
      append_bytes(stream->data(), stream->size() * sizeof(std::int64_t));
    return bytes;
  }

  // Stream convenience over serialize(), for callers that legitimately own an open stream —
  // the unit file, which is testing the FORMAT rather than the engine's binding discipline.
  // The engine does not use this: it writes through its bound descriptor.
  void dump(std::ostream &out, std::uint64_t peak_sessions) const {
    const std::vector<char> bytes = serialize(peak_sessions);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out)
      throw std::runtime_error("BenchRecorder: write to the bench dump failed");
  }

  // Samples refused because a stream was at capacity — zero on any honest run (the
  // capacity is sized an order of magnitude past the §5.2 floor); the count-every-drop
  // discipline still wants the refusals countable rather than silent.
  [[nodiscard]] std::uint64_t saturated() const noexcept { return saturated_; }

private:
  struct Slot {
    std::uint64_t md_seq{0}; // 0 = empty (the driver assigns md_seq from 1)
    std::int64_t m0_ns{0};
    std::int64_t m0p_ns{0}; // 0 = not yet handed to a write
    bool m3_taken{false};
  };

  Slot *find(std::uint64_t md_seq) noexcept {
    if (md_seq == 0)
      return nullptr;
    Slot &slot = slots_[static_cast<std::size_t>(md_seq % slots_.size())];
    return slot.md_seq == md_seq ? &slot : nullptr;
  }

  // Capacity-guarded append: reserve() ran at construction, so push_back below capacity
  // cannot reallocate (and an int64 copy cannot throw) — the noexcept is genuine; the
  // analyzer only sees push_back's unconditional signature, hence the suppression.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  void append(std::vector<std::int64_t> &stream, std::int64_t sample) noexcept {
    if (stream.size() == stream.capacity()) {
      ++saturated_;
      return;
    }
    stream.push_back(sample);
  }

  std::vector<Slot> slots_;
  std::vector<std::int64_t> svc_, m0_m0p_, m0p_m3_, m0_m3_;
  std::uint64_t saturated_{0};
};

} // namespace mm
