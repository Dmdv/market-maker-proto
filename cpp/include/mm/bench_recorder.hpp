// Latency-sample recorder for the benchmark harness: every record method is a bounds-checked write
// into preallocated storage, so recording never allocates. serialize() runs at shutdown, once.
//
// STREAMS: m0 = book applied on the owner thread; m0' = TOB frame handed to async_write (the venue
// PRODUCTION/DELIVERY boundary); m3 = first order echoing that md_seq; svc = the service window.
//
// FIRST hand-off and FIRST echo only, per md_seq. Past ONE session the recorded m0->m0' is the
// fan-out MINIMUM rather than the boundary — which is what the dump's peak-session word exposes.
//
// SLOTS: a ring keyed by md_seq (index = md_seq % kCorrelationSlots); md_seq 0 is the empty
// sentinel. The correlation window and the sample capacity are independent dimensions.
//
// DUMP FORMAT (harness contract): SIX little-endian uint64s — sample counts for svc, m0->m0',
// m0'->m3, m0->m3, then refused samples and peak sessions — followed by those four int64 streams.
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
  // SAMPLE CAPACITY: the benchmark react run's >= 100k plus warm-up headroom. Drives the four
  // streams' reserve() and nothing else.
  static constexpr std::size_t kDefaultCapacity = 1'200'000;

  // CORRELATION WINDOW, sized independently of the sample capacity: 64Ki slots is ≈65 s at the
  // benchmark 1 kHz tick rate for 2 MB resident, against a ~2-in-flight requirement.
  static constexpr std::size_t kCorrelationSlots = 65'536;

  explicit BenchRecorder(std::size_t capacity = kDefaultCapacity) : slots_(kCorrelationSlots) {
    // Nothing downstream catches a zero: such a recorder refuses every sample and dumps four
    // empty streams while still looking live to its caller.
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

  // The shutdown artifact, in the format above. It returns BYTES and names no destination: the
  // engine writes through a descriptor bound at startup, so a symlink swap cannot redirect it.
  //
  // peak_sessions is the run's peak concurrent session count, tracked by ServerImpl. Deliberately
  // NOT defaulted: a default would let a caller omit the dimension the sixth header word exposes.
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

  // Stream convenience over serialize(), for callers that own an open stream (the unit test,
  // which exercises the FORMAT). The engine does not use it: it writes through its descriptor.
  void dump(std::ostream &out, std::uint64_t peak_sessions) const {
    const std::vector<char> bytes = serialize(peak_sessions);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out)
      throw std::runtime_error("BenchRecorder: write to the bench dump failed");
  }

  // Samples refused because a stream was at capacity — zero on any honest run, but counted
  // rather than silently dropped.
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

  // Capacity-guarded: reserve() ran at construction, so a push_back below capacity cannot
  // reallocate — the noexcept is genuine; the analyzer only sees push_back's signature.
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
