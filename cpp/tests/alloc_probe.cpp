// The allocation probe's ONE definition site: the state alloc_probe_support.hpp declares
// `extern`, plus the global `operator new`/`operator delete` replacement for the ENTIRE test
// binary. Replacing operator new is a whole-binary property and belongs in exactly one TU
// (CMakeLists.txt records the rationale); the TUs that USE the probe (test_engine_alloc.cpp,
// test_outbox_alloc.cpp) arm it through the support header's RAII helpers and define nothing
// of it themselves.
#include "alloc_probe_support.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace alloc_probe {

std::size_t g_new_calls = 0;
std::size_t g_throw_at = 0;
bool g_counting = false;

} // namespace alloc_probe

// Replacing the throwing scalar operator new/delete pair is enough: operator new[] and the
// nothrow forms are specified to route through them, and the ALIGNED forms are a
// library-provided, internally consistent pair we deliberately leave alone
// (alloc_probe_support.hpp pins the one type-alignment assumption that exemption rests on).
// Both replacements go through malloc/free, so ASan still sees (and instruments) every
// allocation.
void *operator new(std::size_t size) {
  if (alloc_probe::g_counting) {
    ++alloc_probe::g_new_calls;
    if (alloc_probe::g_throw_at != 0 && alloc_probe::g_new_calls == alloc_probe::g_throw_at) {
      // One-shot: stack unwinding allocates (the exception object aside, Catch2's own
      // machinery does), and a probe that re-armed itself would fail the unwind path
      // instead of the path under test.
      alloc_probe::g_throw_at = 0;
      throw std::bad_alloc{};
    }
  }
  void *p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr)
    throw std::bad_alloc{};
  return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
