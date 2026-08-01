// — BenchRecorder unit pins: the m0' attribution guard and
// the first-echo-only rule,
// plus the no-allocation-after-construction and dump-format contracts the benchmark harness
// reads against.
#include "mm/bench_recorder.hpp"

#include "alloc_probe_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

// Self-deleting dump path (telemetry_test_support.hpp's TempPath shape; local because
// this TU needs a .bin suffix and no other server TU dumps recorder files).
struct TempBin {
  TempBin() {
    static std::mt19937_64 gen{std::random_device{}()};
    path = std::filesystem::temp_directory_path() / ("mm_bench_" + std::to_string(gen()) + ".bin");
  }
  ~TempBin() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  std::filesystem::path path;
};

struct Dump {
  std::vector<std::int64_t> svc, m0_m0p, m0p_m3, m0_m3;
  std::uint64_t saturated{0};
  std::uint64_t peak_sessions{0};
};

Dump read_dump(const std::filesystem::path &p) {
  std::ifstream in{p, std::ios::binary};
  REQUIRE(in.is_open());
  std::uint64_t header[6]{};
  in.read(reinterpret_cast<char *>(header), sizeof header);
  Dump d;
  const auto read_stream = [&](std::vector<std::int64_t> &v, std::uint64_t n) {
    v.resize(n);
    in.read(reinterpret_cast<char *>(v.data()), static_cast<std::streamsize>(n * 8));
  };
  read_stream(d.svc, header[0]);
  read_stream(d.m0_m0p, header[1]);
  read_stream(d.m0p_m3, header[2]);
  read_stream(d.m0_m3, header[3]);
  d.saturated = header[4];
  d.peak_sessions = header[5];
  REQUIRE(in.good());
  in.get();
  REQUIRE(in.eof()); // six header words + the four counted streams are the WHOLE file
  return d;
}

// dump() takes an already-open stream. The unit file opens its own sink per call,
// which is the same contract in miniature.
void dump_to(const mm::BenchRecorder &rec, const std::filesystem::path &path,
             std::uint64_t peak_sessions) {
  std::ofstream sink{path, std::ios::binary | std::ios::trunc};
  REQUIRE(sink.is_open());
  rec.dump(sink, peak_sessions);
}

} // namespace

TEST_CASE("bench_recorder: decomposed samples and the first-echo-only rule", "[bench]") {
  mm::BenchRecorder rec{16};
  rec.md_published(1, 1000);
  rec.md_written(1, 1400); // m0 -> m0' = 400 (venue production)
  rec.md_written(1, 9999); // a second session's write of the same tick: not the boundary
  rec.order_for(1, 2000);  // m0' -> m3 = 600, m0 -> m3 = 1000
  rec.order_for(1, 5000);  // later echo: pacing, not reaction — ignored
  rec.svc(77);
  rec.svc(88);

  TempBin out;
  dump_to(rec, out.path, 3);
  const Dump d = read_dump(out.path);
  CHECK(d.svc == std::vector<std::int64_t>{77, 88});
  CHECK(d.m0_m0p == std::vector<std::int64_t>{400});
  CHECK(d.m0p_m3 == std::vector<std::int64_t>{600});
  CHECK(d.m0_m3 == std::vector<std::int64_t>{1000});
  CHECK(d.saturated == 0);
  // The peak-session word round-trips VERBATIM from the caller: the m0->m0' above is the
  // first of two hand-offs of tick 1, so a reader can only tell that minimum from a true
  // single-session boundary by this word. A dump that hardcodes it (0, or 1) is the exact
  // artifact the sixth word exists to prevent, so 3 is asserted, not merely written.
  CHECK(d.peak_sessions == 3);
}

TEST_CASE("bench_recorder: an order for an unknown or evicted md_seq records nothing", "[bench]") {
  mm::BenchRecorder rec{4};
  rec.md_published(1, 100);
  rec.order_for(42, 500); // never published
  // The ring is keyed by md_seq % kCorrelationSlots and is sized INDEPENDENTLY of the
  // sample capacity, so eviction takes the seq one full ring later, not capacity-many
  // publishes: wrapping back onto slot 1 IS the eviction mechanism.
  rec.md_published(static_cast<std::uint64_t>(mm::BenchRecorder::kCorrelationSlots) + 1, 900);
  rec.order_for(1, 900); // slot now belongs to the wrapped seq

  TempBin out;
  dump_to(rec, out.path, 1);
  const Dump d = read_dump(out.path);
  CHECK(d.m0p_m3.empty());
  CHECK(d.m0_m3.empty());
}

TEST_CASE("bench_recorder: the correlation ring is wider than the sample capacity", "[bench]") {
  // The DECOUPLING itself, in positive form. The eviction case above cannot see it: at
  // capacity 4 the wrapped seq it publishes is kCorrelationSlots + 1, and 65'537 % 4 == 1
  // is the same slot the capacity-sized ring would have evicted — so reverting
  // slots_(kCorrelationSlots) to slots_(capacity) leaves that case green and the 38.4 MB
  // regression invisible. Here the recorder holds 4 SAMPLES and is offered five publishes:
  // a ring sized from the capacity wraps on the fifth (5 % 4 == 1) and evicts md_seq 1, so
  // the order below correlates ONLY while the two dimensions are sized separately.
  mm::BenchRecorder rec{4};
  rec.md_published(1, 100);
  for (std::uint64_t seq = 2; seq <= 5; ++seq)
    rec.md_published(seq, 200);
  rec.order_for(1, 900);

  TempBin out;
  dump_to(rec, out.path, 1);
  const Dump d = read_dump(out.path);
  CHECK(d.m0_m3 == std::vector<std::int64_t>{800}); // still correlated, four publishes later
}

TEST_CASE("bench_recorder: the RECORDING path allocates nothing, saturates at capacity",
          "[bench]") {
  mm::BenchRecorder rec{64};
  // Storms past the sample capacity on the armed probe: the recorder must saturate,
  // never grow — a realloc inside the measured M-window would be a measurement artifact.
  const std::size_t allocs = alloc_probe::allocations_in([&] {
    for (std::uint64_t seq = 1; seq <= 500; ++seq) {
      rec.md_published(seq, static_cast<std::int64_t>(seq));
      rec.md_written(seq, static_cast<std::int64_t>(seq) + 1);
      rec.order_for(seq, static_cast<std::int64_t>(seq) + 2);
      rec.svc(1);
    }
  });
  CHECK(allocs == 0);
  TempBin out;
  dump_to(rec, out.path, 1);
  const Dump d = read_dump(out.path);
  // Saturated at capacity: exactly the first `cap` samples of each stream survive.
  CHECK(d.svc.size() == 64);
  CHECK(d.m0_m0p.size() == 64);
  CHECK(d.m0p_m3.size() == 64);
  CHECK(d.m0_m3.size() == 64);
  // ...and every refusal is COUNTED in the dump header: a stream at exactly capacity is
  // indistinguishable from one that happened to end there without the fifth word. Four
  // streams, 500 samples offered to each, 64 taken — the rest refused.
  CHECK(rec.saturated() == 4 * (500 - 64));
  CHECK(d.saturated == 4 * (500 - 64));
}

TEST_CASE("bench_recorder: an order without a prior write still records m0->m3 only", "[bench]") {
  // m0' is the delivery boundary; a reaction can land before any TOB hand-off is
  // stamped (slow write loop, first session still arming). m0->m3 is the total window
  // and is recorded whenever m0 exists; m0'->m3 is only defined once m0' is. Every other
  // case in this file publishes then writes then orders, so a mutant that (a) gates
  // m0->m3 on m0' being set, or (b) appends m0'->m3 with a zero m0', both stay green.
  mm::BenchRecorder rec{8};
  rec.md_published(1, 1000);
  rec.order_for(1, 2500); // no md_written

  TempBin out;
  dump_to(rec, out.path, 1);
  const Dump d = read_dump(out.path);
  CHECK(d.m0_m0p.empty());
  CHECK(d.m0p_m3.empty());                           // no delivery stamp: no delivery leg
  CHECK(d.m0_m3 == std::vector<std::int64_t>{1500}); // total still defined
}

TEST_CASE("bench_recorder: a zero capacity is refused at construction", "[bench]") {
  // Capacity does not divide the ring, so nothing else catches a zero: it would reserve
  // nothing, refuse every sample, and dump four empty streams from a recorder that looks
  // live. The throw is the only observable — no happy-path case constructs with zero.
  CHECK_THROWS_AS(mm::BenchRecorder{0}, std::invalid_argument);
}
