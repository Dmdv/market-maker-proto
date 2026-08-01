// Does this toolchain's TSan race-check STRUCT-COPY (memcpy-shaped) accesses, or only
// scalar loads/stores? Decides where the SPSC telemetry ring's memory-ordering contract
// (cpp/include/mm/telemetry_ring.hpp) is actually ENFORCEABLE by TSan: the ring's slot
// transfers are 64-byte trivially-copyable struct copies, so a TSan runtime that skips
// memcpy-shaped accesses cannot report a release->relaxed downgrade on the publish path
// no matter how long the stress case runs.
//
// Three modes; each prints its provenance and intent. The reader's verdict is whether a
// "WARNING: ThreadSanitizer: data race" report appears where the mode says one should:
//   relaxed-scalar  producer publishes a plain uint64 via a RELAXED store — a data race
//                   by the C++ model; ANY working TSan must report it (harness control).
//   relaxed-copy    identical race, but both payload accesses are whole-struct copies —
//                   the ring's slot-transfer shape. A report here means struct copies are
//                   race-checked; silence means the runtime is BLIND to this shape.
//   release-copy    correct release publish — no runtime may report (false-positive
//                   control).
//
// Measured, battery (2026-07-28), same flags both toolchains
// (-std=c++20 -O1 -g -fsanitize=thread):
//   Apple clang 21.0.0 arm64 (macOS dev host): relaxed-scalar REPORTED;
//     relaxed-copy SILENT (also silent at -O0, where the copy is a literal memcpy call);
//     release-copy silent. => host TSan cannot see the ring's slot transfers; on this
//     toolchain the tsan preset verifies the ring's INDEX protocol only.
//   ubuntu:26.04 g++ 15.2 libtsan (authoritative container): all four ring
//     memory-ordering downgrade mutants REPORTED as data races on the million-record
//     stress case (a dedicated stress battery covers this). => the container
//     sanitizer matrix is where the ring's ordering contract is enforced.
//
// Build & run (host):      c++ -std=c++20 -O1 -g -fsanitize=thread \
//                            bench/probes/tsan_struct_copy_probe.cpp -o /tmp/tsan_probe
//                          /tmp/tsan_probe relaxed-scalar   # etc.
// In the container, same line with g++.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

struct alignas(64) Rec { // the telemetry record's shape: one cache line, POD
  std::int64_t ts_ns;
  std::uint32_t kind, code;
  std::uint64_t args[6];
};
static_assert(sizeof(Rec) == 64);

Rec payload;                  // non-atomic
std::uint64_t scalar_payload; // non-atomic
std::atomic<std::uint64_t> flag{0};

} // namespace

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "relaxed-copy";
  const bool scalar = std::strcmp(mode, "relaxed-scalar") == 0;
  const bool release = std::strcmp(mode, "release-copy") == 0;

  std::printf("tsan_struct_copy_probe mode=%s compiler=\"%s\" sizeof(Rec)=%zu\n", mode, __VERSION__,
              sizeof(Rec));
  std::printf("expect a ThreadSanitizer data-race report: %s\n",
              release  ? "NO (correct release publish)"
              : scalar ? "YES from any working TSan (scalar-access control)"
                       : "YES iff this runtime race-checks struct-copy/memcpy accesses");

  std::thread producer{[scalar, release] {
    if (scalar) {
      scalar_payload = 42;
    } else {
      Rec rec{.ts_ns = 42, .kind = 1, .code = 2, .args = {1, 2, 3, 4, 5, 6}};
      payload = rec; // whole-struct write: the ring's slot-install shape
    }
    flag.store(1, release ? std::memory_order_release : std::memory_order_relaxed);
  }};

  while (flag.load(std::memory_order_acquire) != 1) {
  }
  long long seen = 0;
  if (scalar) {
    seen = static_cast<long long>(scalar_payload);
  } else {
    Rec rec{};
    rec = payload; // whole-struct read: the ring's slot-extract shape
    seen = rec.ts_ns;
  }
  producer.join();
  std::printf("payload seen=%lld (the value is irrelevant; the report above is the "
              "probe's answer)\n",
              seen);
  return 0;
}
