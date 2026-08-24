// Shared support for the Task 3 engine test TUs (test_engine.cpp outgrew the 500-line
// file cap; split along behavioral seams, mirroring the Task 2
// codec_test_support.hpp remedy):
//   test_engine.cpp         — lifecycle/state-machine cases: new, duplicate, validation,
//                             post-only entry, max-live + re-quote, cancel, constructor
//                             guards, the two reject-reason matrices, determinism
//   test_engine_session.cpp — session-lifetime cases: cross-session isolation, end_session
//                             acks + reap, the UINT64_MAX key boundary, the entry cap
//   test_engine_fills.cpp   — fill rule, absorbing terminals, md_seq guard, locked and
//                             crossed books, F-30 exogeneity, history-scaling regression
//                             (perf-tagged; see CMakeLists.txt)
//   test_engine_alloc.cpp   — the measured per-call allocation inventory and on_new's strong
//                             exception guarantee (arms the whole-binary global operator new
//                             probe defined once in alloc_probe.cpp)
// Everything here is `inline` in a named namespace: the two session ids, the spec
// instrument, command/book builders and the report-shape helpers the TUs above share.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include "mm/engine.hpp"
#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace engine_test {

inline constexpr std::uint64_t kSessA = 1;
inline constexpr std::uint64_t kSessB = 2;

inline mm::Instrument spec_instrument() {
  return mm::Instrument{.symbol = "MOCKUSDT"}; // defaults: tick 5, lot 10, max_live 2
}

inline mm::NewOrder mk_new(std::string cl_id, std::string side, std::int64_t px, std::int64_t qty,
                           bool post_only = true) {
  mm::NewOrder o;
  o.cl_id = std::move(cl_id);
  o.symbol = "MOCKUSDT";
  o.side = std::move(side);
  o.px = px;
  o.qty = qty;
  o.post_only = post_only;
  return o;
}

inline mm::CancelOrder mk_cancel(std::string cl_id) {
  mm::CancelOrder c;
  c.cl_id = std::move(cl_id);
  return c;
}

inline mm::Book mk_book(std::int64_t bid_px, std::int64_t bid_qty, std::int64_t ask_px,
                        std::int64_t ask_qty, std::uint64_t md_seq) {
  return mm::Book{
      .bid_px = bid_px, .bid_qty = bid_qty, .ask_px = ask_px, .ask_qty = ask_qty, .md_seq = md_seq};
}

// The demo-scenario resting book: bid 500000x100 / ask 500010x80.
inline mm::Book base_book(std::uint64_t md_seq = 1) {
  return mk_book(500000, 100, 500010, 80, md_seq);
}

// Almost every case starts from the same place: the demo resting book installed, and the
// sweep that installs it routing nothing (there are no live orders yet). Spelled as a named
// factory so the priming reads as SETUP — the bare `REQUIRE(eng.on_book(base_book()).empty())`
// it replaces looked like an emptiness assertion with its real intent stated nowhere.
inline mm::OrderEngine
primed_engine(std::size_t max_session_entries = mm::OrderEngine::kDefaultMaxSessionEntries) {
  mm::OrderEngine eng{spec_instrument(), max_session_entries};
  REQUIRE(eng.on_book(base_book()).empty());
  return eng;
}

// Returns by VALUE: the argument is usually a temporary vector, and a reference into it
// would dangle past the enclosing full-expression. Call sites must bind the result by
// value too (`const auto ack = ...`), so the call-site spelling matches this contract
// instead of silently depending on it.
template <class T> T require_only(const std::vector<mm::OutMsg> &out) {
  REQUIRE(out.size() == 1);
  const T *msg = std::get_if<T>(&out.front());
  REQUIRE(msg != nullptr);
  return *msg;
}

// Pins the by-value return category: an obvious-looking "optimization" to
// `const T &require_only(...)` would make every call site bind into a destroyed
// temporary vector (lifetime extension does not apply through a returned reference),
// with only ASan likely to notice.
static_assert(!std::is_reference_v<decltype(require_only<mm::OrderAck>(
                  std::declval<const std::vector<mm::OutMsg> &>()))>,
              "require_only must return by value: its argument is usually a temporary");

// on_book's return type. Same by-value contract and rationale as the OutMsg overload above;
// returns the routed session ALONGSIDE the message, because routing is half of what on_book
// promises — call sites bind `const auto [sess, fill] = ...`.
template <class T> std::pair<std::uint64_t, T> require_only(const std::vector<mm::Routed> &out) {
  REQUIRE(out.size() == 1);
  const T *msg = std::get_if<T>(&out.front().msg);
  REQUIRE(msg != nullptr);
  return {out.front().session, *msg};
}

// Indexed form for the MULTI-report command batches require_only cannot serve (end_session
// acking a live bid AND a live ask): same by-value contract, same null check.
template <class T> T require_at(const std::vector<mm::OutMsg> &out, std::size_t index) {
  REQUIRE(out.size() > index);
  const T *msg = std::get_if<T>(&out[index]);
  REQUIRE(msg != nullptr);
  return *msg;
}

// Indexed form for the MULTI-report sweeps require_only cannot serve (a crossed book, two
// resting orders against one displayed top): same by-value contract, same null check.
template <class T> T require_at(const std::vector<mm::Routed> &out, std::size_t index) {
  REQUIRE(out.size() > index);
  const T *msg = std::get_if<T>(&out[index].msg);
  REQUIRE(msg != nullptr);
  return *msg;
}

// Every reject assertion pins code AND cl_id: Reject.cl_id is the field the client
// uses to correlate a reject back to its pending order, so a helper that checked only
// the code would leave the correlation field unfalsifiable suite-wide. `reason` is
// optional because most call sites are asserting a transition, not the client-visible
// explanation — but where the explanation IS the subject (the reject-reason matrices in
// test_engine.cpp) passing it here is what makes the string falsifiable.
inline void require_reject(const std::vector<mm::OutMsg> &out, mm::RejectCode code,
                           std::string_view cl_id,
                           std::optional<std::string_view> reason = std::nullopt) {
  const auto rej = require_only<mm::Reject>(out);
  CHECK(rej.code == mm::to_string(code));
  CHECK(rej.cl_id == cl_id);
  if (reason)
    CHECK(rej.reason == *reason);
}

} // namespace engine_test
