// What do the SPSC telemetry ring's design refusals actually cost or save? The ring
// header (cpp/include/mm/telemetry_ring.hpp) declines three classic optimizations — a
// cached-head producer, a producer-side wake of the writer, and 128-byte index padding —
// and states a push cost and a capacity/poll saturation threshold. Every one of those
// figures is re-taken by a mode of this probe, so none of them has to be believed.
//
// Modes (each prints its provenance and intent; run with no argument for all but
// saturation, which writes a scratch telemetry file):
//   push        ns per L1-hot try_push+try_pop pair, single thread — the producer-side
//               cost a snapshot or event push adds to the owner thread.
//   push-cached two-thread flat-out throughput, shipped ring vs a cached-head variant —
//               what the declined amortization would buy UNDER SATURATION (at the shipped
//               ~1 Hz duty it buys nothing by construction).
//   notify      ns per no-waiter condition_variable::notify_one, and its ratio to the
//               push+pop pair — the floor cost a notifying producer would pay per push;
//               a notify that actually WAKES the parked writer is a futex syscall on
//               top of it.
//   granule     ns per contended cross-core load+store at 64/128/256-byte separation —
//               the coherence-granule calibration behind the header's
//               it-stays-alignas(64) note (Apple M-series granule is 128 bytes,
//               sysctl hw.cachelinesize; amd64 CI is 64).
//   saturation  paced producer against a real TelemetryWriter at kDefaultPoll: peak
//               producer-side pending() and drops per rate — locates the drop onset for
//               the shipped capacity/poll pair and the peak_pending ≈ factor x R x poll
//               drain-lag factor.
//
// Measured (2026-07-29; host = Apple M3, Apple clang 21, arm64;
// container = ubuntu:26.04 g++ 15.2, linux/arm64 on the same silicon; both -O3; figures
// cited by cpp/include/mm/telemetry_ring.hpp — re-take rather than believe):
//   push        ~2.2 ns (host) / ~6.3 ns (container) per L1-hot push+pop pair
//   push-cached +54% (host) / +188% (container) saturation throughput over the shipped
//               ring — and nothing at the shipped ~1 Hz duty
//   notify      no-waiter floor 0.3-1.8x the pair; parked-waiter REAL wake ~2.1 us
//               (host) / ~15.5 us (container) — the path a notifier would take at 1 Hz
//   granule     ~2.5x per contended access at 64B vs 128B separation WHEN the two ends
//               run cross-cluster; scheduler-dependent — flat medians when co-located
//               (macOS offers no honored pinning; the VM adds its own placement layer)
//   saturation  onset of drops between 300k and 350k rec/s at 4096 slots / 10 ms poll
//               on BOTH surfaces; sub-onset peak_pending ≈ 1.25 x R x poll
//
// Build & run:  c++ -std=c++20 -O3 -Icpp/include bench/probes/ring_push_probe.cpp \
//                 -o /tmp/ring_push_probe
//               /tmp/ring_push_probe            # all modes; or one of the mode names
// In the container, same line with g++ (numbers there are the authoritative surface).
#include "mm/telemetry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t ns_since(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

mm::TelemetryRecord probe_record(std::uint64_t seq) {
  return mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq);
}

// Pin the calling thread to one CPU where the platform allows it (Linux — incl. the
// authoritative container). macOS exposes no honored affinity API, which is exactly why
// the granule numbers are scheduler-noisy on the host: whether the two threads land on
// one cluster (shared L2) or two changes the transfer cost measured.
void pin_to_cpu([[maybe_unused]] int cpu) {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#endif
}

// The shipped ring's algorithm with ONE change: the producer caches the last-seen head
// and reloads it only on apparent-full. This is the variant the header DECLINES; the
// push-cached mode prices it against the shipped ring under two-thread saturation.
class CachedHeadRing {
public:
  explicit CachedHeadRing(std::size_t capacity) : slots_(capacity) {}

  bool try_push(const mm::TelemetryRecord &rec) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ == slots_.size()) {
      cached_head_ = head_.load(std::memory_order_acquire); // reload only on apparent-full
      if (tail - cached_head_ == slots_.size())
        return false;
    }
    slots_[static_cast<std::size_t>(tail) & (slots_.size() - 1)] = rec;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(mm::TelemetryRecord &out) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    if (tail == head)
      return false;
    out = slots_[static_cast<std::size_t>(head) & (slots_.size() - 1)];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

private:
  alignas(64) std::atomic<std::uint64_t> head_{0};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
  alignas(64) std::uint64_t cached_head_{0}; // producer-only: the second index invariant
  std::vector<mm::TelemetryRecord> slots_;
};

void mode_push() {
  constexpr std::uint64_t kPairs = 20'000'000;
  mm::SpscTelemetryRing ring{4096};
  mm::TelemetryRecord out{};
  std::uint64_t popped = 0;
  const auto start = Clock::now();
  for (std::uint64_t seq = 0; seq < kPairs; ++seq) {
    (void)ring.try_push(probe_record(seq));
    popped += ring.try_pop(out) ? 1 : 0; // consume the verdict: the pop cannot be elided
  }
  const double ns_per_pair = static_cast<double>(ns_since(start)) / static_cast<double>(kPairs);
  std::printf("push: %.2f ns per L1-hot try_push+try_pop pair (%llu pairs, popped=%llu)\n",
              ns_per_pair, static_cast<unsigned long long>(kPairs),
              static_cast<unsigned long long>(popped));
}

template <class Ring> double throughput_mrec_per_s(Ring &ring, std::uint64_t records) {
  std::atomic<bool> go{false};
  std::thread consumer{[&ring, &go, records] {
    go.store(true, std::memory_order_release);
    mm::TelemetryRecord out{};
    for (std::uint64_t got = 0; got < records;) {
      if (ring.try_pop(out))
        ++got;
    }
  }};
  while (!go.load(std::memory_order_acquire)) {
  }
  const auto start = Clock::now();
  for (std::uint64_t seq = 0; seq < records; ++seq) {
    const mm::TelemetryRecord rec = probe_record(seq);
    while (!ring.try_push(rec)) {
    }
  }
  consumer.join();
  const auto elapsed = ns_since(start);
  return static_cast<double>(records) * 1e3 / static_cast<double>(elapsed);
}

void mode_push_cached() {
  constexpr std::uint64_t kRecords = 20'000'000;
  double shipped = 0;
  double cached = 0;
  for (int rep = 0; rep < 3; ++rep) { // best-of-3: flat-out A/B is scheduler-noisy
    mm::SpscTelemetryRing plain_ring{4096};
    shipped = std::max(shipped, throughput_mrec_per_s(plain_ring, kRecords));
    CachedHeadRing cached_ring{4096};
    cached = std::max(cached, throughput_mrec_per_s(cached_ring, kRecords));
  }
  std::printf("push-cached: shipped %.1f Mrec/s vs cached-head %.1f Mrec/s under two-thread "
              "saturation => the declined amortization buys %+.0f%% there (and nothing at 1 Hz)\n",
              shipped, cached, (cached / shipped - 1.0) * 100.0);
}

void mode_notify() {
  // The pair cost re-measured in-run so the printed ratio is one run's arithmetic. The
  // pop verdicts are ACCUMULATED, not discarded: a discarded loop with no observable
  // effect measures 0.00 ns because -O3 deletes it whole (observed).
  constexpr std::uint64_t kPairs = 20'000'000;
  mm::SpscTelemetryRing ring{4096};
  mm::TelemetryRecord out{};
  std::uint64_t popped = 0;
  auto start = Clock::now();
  for (std::uint64_t seq = 0; seq < kPairs; ++seq) {
    (void)ring.try_push(probe_record(seq));
    popped += ring.try_pop(out) ? 1 : 0;
  }
  const double pair_ns = static_cast<double>(ns_since(start)) / static_cast<double>(kPairs);
  if (popped == 0) // consume `popped` so the loop stays observable
    std::printf("notify: pair loop popped nothing — measurement invalid\n");

  constexpr std::uint64_t kNotifies = 20'000'000;
  std::condition_variable cv;
  start = Clock::now();
  for (std::uint64_t i = 0; i < kNotifies; ++i)
    cv.notify_one(); // no waiter: the FLOOR cost a notifying producer pays per push
  const double notify_ns = static_cast<double>(ns_since(start)) / static_cast<double>(kNotifies);
  std::printf("notify: %.2f ns per no-waiter notify_one = %.1fx the %.2f ns push+pop pair\n",
              notify_ns, notify_ns / pair_ns, pair_ns);

  // The REAL wake: at the shipped duty (~1 Hz pushes vs a 10 ms poll) the writer is
  // PARKED when a push arrives, so a notifying producer would take this path, not the
  // floor above. Measured as a cv ping-pong with a parked partner — each half-round is
  // one wake-with-waiter, i.e. a futex/ulock syscall plus the wakeup latency.
  constexpr std::uint64_t kRounds = 100'000;
  std::mutex pp_mutex;
  std::condition_variable pp_cv;
  bool ball_theirs = false;
  std::thread partner{[&] {
    for (std::uint64_t i = 0; i < kRounds; ++i) {
      std::unique_lock<std::mutex> lock{pp_mutex};
      pp_cv.wait(lock, [&] { return ball_theirs; });
      ball_theirs = false;
      pp_cv.notify_one();
    }
  }};
  start = Clock::now();
  for (std::uint64_t i = 0; i < kRounds; ++i) {
    {
      const std::scoped_lock lock{pp_mutex};
      ball_theirs = true;
    }
    pp_cv.notify_one();
    std::unique_lock<std::mutex> lock{pp_mutex};
    pp_cv.wait(lock, [&] { return !ball_theirs; });
  }
  const double wake_ns = static_cast<double>(ns_since(start)) / static_cast<double>(kRounds) / 2.0;
  partner.join();
  std::printf("notify: %.0f ns per parked-waiter wake (half a cv ping-pong round) = %.0fx "
              "the push+pop pair — the path a notifying producer would take at 1 Hz duty\n",
              wake_ns, wake_ns / pair_ns);
}

void mode_granule() {
  // Load-then-store per side — the RING's own access shape (each end loads an index it
  // then stores past; the drop counter is the same load+store ): the
  // load pulls the granule in, the store takes it exclusive, so sharing shows as
  // ping-pong. Two wrong instruments, observed flat and rejected: plain relaxed STORES
  // (store buffer absorbs them in bursts; 0.28 vs 0.27 ns) and relaxed fetch_add (this
  // silicon executes contended LSE atomics as far atomics, no line transfer; 1.90 vs
  // 1.92 ns).
  // Median of three barrier-synced reps per separation: one rep is 15-50 ms, short
  // enough that a single scheduler blip (the hammer descheduled mid-measure) once
  // produced a flat, uncontended-looking row — the median rejects that outlier.
  alignas(512) static char buffer[512];
  std::memset(buffer, 0, sizeof buffer);
  for (const std::size_t separation : {std::size_t{64}, std::size_t{128}, std::size_t{256}}) {
    std::array<double, 3> reps{};
    for (double &rep : reps) {
      auto *mine = new (buffer) std::atomic<std::uint64_t>{0};
      auto *theirs = new (buffer + separation) std::atomic<std::uint64_t>{0};
      std::atomic<bool> stop{false};
      std::atomic<bool> running{false};
      std::thread hammer{[theirs, &stop, &running] {
        pin_to_cpu(1);
        running.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed))
          theirs->store(theirs->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      }};
      pin_to_cpu(0);
      while (!running.load(std::memory_order_acquire)) {
      }
      constexpr std::uint64_t kOps = 50'000'000;
      for (std::uint64_t i = 0; i < kOps / 10; ++i) // warmup: both sides contending
        mine->store(mine->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      const auto start = Clock::now();
      for (std::uint64_t i = 0; i < kOps; ++i)
        mine->store(mine->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      rep = static_cast<double>(ns_since(start)) / static_cast<double>(kOps);
      stop.store(true, std::memory_order_relaxed);
      hammer.join();
    }
    std::sort(reps.begin(), reps.end());
    std::printf("granule: %3zu-byte separation: %.2f ns per contended load+store (median of 3)\n",
                separation, reps[1]);
  }
  std::printf("granule: a 64B row slower than the 128B row is the granule showing; FLAT rows "
              "mean the scheduler co-located the two ends (no honored pinning on macOS; the "
              "container VM adds its own layer) — the architectural fact is sysctl "
              "hw.cachelinesize = 128 on this silicon\n");
}

void mode_saturation(const std::filesystem::path &out_path) {
  // Fire-and-forget pacing, exactly the engine's stance: no retry, a full ring drops.
  // Each rate runs long enough to cross several poll intervals.
  constexpr std::int64_t kRunNs = 400'000'000;
  mm::SpscTelemetryRing ring; // kDefaultCapacity — the pair under calibration
  mm::TelemetryWriter writer{ring, out_path};
  const double poll_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(mm::TelemetryWriter::kDefaultPoll)
          .count();
  std::printf("saturation: capacity=%zu poll=%.0fms run=%.1fs per rate\n", ring.capacity(),
              poll_s * 1e3, static_cast<double>(kRunNs) / 1e9);
  for (const double rate : {50e3, 100e3, 200e3, 250e3, 300e3, 350e3, 400e3, 500e3}) {
    const std::uint64_t drops_before = ring.dropped();
    std::size_t peak_pending = 0;
    const auto start = Clock::now();
    for (std::uint64_t seq = 0;; ++seq) {
      const auto due = start + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                   static_cast<double>(seq) * 1e9 / rate));
      while (Clock::now() < due) {
      }
      if (ns_since(start) > kRunNs)
        break;
      (void)ring.try_push(probe_record(seq));
      peak_pending = std::max(peak_pending, ring.pending()); // producer-side: safe overestimate
    }
    while (ring.pending() > 0) // drain between rates so each row starts empty
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const std::uint64_t drops = ring.dropped() - drops_before;
    std::printf("saturation: rate=%6.0fk/s peak_pending=%6zu drops=%8llu factor=%.2f\n", rate / 1e3,
                peak_pending, static_cast<unsigned long long>(drops),
                static_cast<double>(peak_pending) / (rate * poll_s));
  }
  std::printf("saturation: onset = first rate with drops>0; sub-onset factor = "
              "peak_pending / (R x poll)\n");
}

} // namespace

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "all";
  std::printf("ring_push_probe mode=%s compiler=\"%s\" sizeof(TelemetryRecord)=%zu\n", mode,
              __VERSION__, sizeof(mm::TelemetryRecord));
  const bool all = std::strcmp(mode, "all") == 0;
  if (all || std::strcmp(mode, "push") == 0)
    mode_push();
  if (all || std::strcmp(mode, "push-cached") == 0)
    mode_push_cached();
  if (all || std::strcmp(mode, "notify") == 0)
    mode_notify();
  if (all || std::strcmp(mode, "granule") == 0)
    mode_granule();
  if (all || std::strcmp(mode, "saturation") == 0) {
    const std::filesystem::path out =
        argc > 2 ? argv[2] : std::filesystem::temp_directory_path() / "ring_push_probe_sat.jsonl";
    mode_saturation(out);
    std::error_code ec;
    std::filesystem::remove(out, ec);
  }
  return 0;
}
