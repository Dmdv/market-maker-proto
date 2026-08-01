// Shared allocation probe for the suite's allocation test TUs. Three
// consumer TUs arm it:
//   test_engine_alloc.cpp — the engine's measured allocation inventory + the strong
//                           exception guarantee, one injection case per entry point
//   test_outbox_alloc.cpp — the Outbox's allocation cases //   test_telemetry.cpp    — the ring's
//   allocation-free steady state ; its armed
//                           region is scoped by what this probe CAN see — the ring's slot
//                           storage is over-aligned (alignas(64) records) and allocates
//                           through the unintercepted ALIGNED new, so the no-reallocation
//                           duty there is carried by the capacity() pin, not the counter.
//                           Forward consequence of the same mechanics: alignas(64)
//                           mm::Counters gives any type embedding it (the server layer's Server)
//                           alignof 64, so a heap-allocated Server routes around this
//                           probe too — scope its armed regions accordingly, or add
//                           aligned interceptors first.
// The ONE-TU constraint (CMakeLists.txt) is about the global `operator new` REPLACEMENT — a
// whole-binary property — not about the cases that arm it: the replacement and this header's
// probe state are DEFINED exactly once, in alloc_probe.cpp; the state is declared `extern`
// here so the RAII/counting helpers below can live next to the cases that use them.
#pragma once

#include "mm/protocol.hpp"

#include <cstddef>

// The replacement TU (alloc_probe.cpp) intercepts the ordinary scalar new/delete pair and
// deliberately leaves the ALIGNED forms alone — sound only while the message type whose
// container nodes the armed cases count takes the ordinary route. Pinned here, in the one
// header both consumer TUs include, so the exemption cannot rot silently (hardgate-fix-2):
static_assert(alignof(mm::OutMsg) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
              "the allocation probe intercepts ordinary new/delete only; an over-aligned "
              "OutMsg alternative would route node allocation around it — add aligned "
              "interceptors before accepting one");

namespace alloc_probe {

// Single-threaded probe state: Catch2 runs cases serially and only the region between
// arm/disarm is counted. `g_throw_at` is the second use of the same counter: 0 leaves the
// probe purely observational, N makes the Nth COUNTED allocation fail instead of succeed,
// which is what turns a header's STRONG exception guarantee into something a test can
// falsify.
extern std::size_t g_new_calls;
extern std::size_t g_throw_at;
extern bool g_counting;

// Arms the counting operator new over the region it guards, and (for nth != 0) makes the
// Nth allocation of that region fail. RAII, and that is the whole point: the bad_alloc
// unwinds through this destructor BEFORE it reaches Catch2's handler, so the probe is always
// disarmed by the time the assertion machinery starts allocating.
class ThrowOnNthAllocation {
public:
  explicit ThrowOnNthAllocation(std::size_t nth) {
    g_new_calls = 0;
    g_throw_at = nth;
    g_counting = true;
  }
  ~ThrowOnNthAllocation() {
    g_counting = false;
    g_throw_at = 0;
  }
  ThrowOnNthAllocation(const ThrowOnNthAllocation &) = delete;
  ThrowOnNthAllocation &operator=(const ThrowOnNthAllocation &) = delete;
};

// Counts the allocations performed by `body`. The rule every call site follows: build every
// fixture the body needs OUTSIDE the counted region; if the body's result must be inspected,
// capture it into storage declared outside the region and assert after it returns. Labelled
// example (the engine TU): its command objects and long identifiers are prebuilt, and each
// entry point's returned report batch is move-assigned into an outside vector — the move
// steals the buffer the counted call already allocated, so the assertions add nothing to the
// measured figure.
//
// Arming goes through the same RAII class as the throwing probe, with nth = 0 (purely
// observational): a `body()` that throws — the paths measured by these TUs demonstrably can,
// which is the subject of the injection cases — must not leave the counter armed for the
// rest of the binary, silently absorbing Catch2's reporting allocations into the next
// measurement.
template <class F> std::size_t allocations_in(F &&body) {
  const ThrowOnNthAllocation counting{0};
  body();
  return g_new_calls;
}

} // namespace alloc_probe
