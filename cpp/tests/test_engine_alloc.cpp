// tests, allocation part. Pins the engine's measured per-call allocation inventory
// (normative record: the TU comment in cpp/src/engine.cpp) — the quantified exception to the
// plan's zero-allocation constraint and the stated seed for the A/B allocation counter. Until
// now that whole table was documentation only: re-adding a cl_id member to Order, or a lookup
// that materializes a std::string key instead of using the transparent comparator, would break
// every row and the A/B seed with a green suite.
//
// The counting/armed global `operator new` replacement these cases run on is a whole-binary
// property, DEFINED exactly once in alloc_probe.cpp and armed through the RAII helpers in
// alloc_probe_support.hpp.
// In-repo precedent for pinning an allocation contract: test_codec.cpp's encode-buffer-reuse
// case.
//
// The same replacement carries the file's second contract: made to FAIL an allocation rather
// than count it, it pins the STRONG exception guarantee engine.hpp states for EVERY entry
// point — the five injection cases at the end of this file — at least one per entry point,
// with on_new covered twice (the accept path and the first-command counter node) — each
// failing every allocation of the path in turn.
//
// WHY THESE ROWS AND THESE LENGTHS. Exact counts are STDLIB-DEPENDENT wherever a string sits
// near a small-string-optimization cliff: the host's libc++ holds 22 bytes locally, the
// authoritative linux/arm64 container's libstdc++ 15, so an exact-equality pin taken at a
// short cl_id would red the container run. Two kinds of row survive that difference
// and are pinned here:
//   * ZERO rows — no allocation at all, on any stdlib.
//   * rows measured with a cl_id and a reason LONGER THAN BOTH local capacities, where every
//     counted allocation is a container node, a vector, or a string that is unavoidably
//     heap-backed either way. `Reject::code` is deliberately never the discriminator: the
//     wire spellings straddle the container's 15-byte cliff (ALREADY_TERMINAL is 16), which
//     is exactly the sensitivity the engine.cpp inventory warns about.
#include "alloc_probe_support.hpp"
#include "engine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include "mm/engine.hpp"
#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <cstddef>
#include <new>
#include <string>
#include <vector>

using alloc_probe::allocations_in;
using alloc_probe::ThrowOnNthAllocation;

namespace {

// A distinct 64-byte cl_id: past BOTH small-string capacities (libc++ 22, libstdc++ 15) and
// exactly at the codec's cl_id cap, so every cl_id copy below is one heap allocation on
// either standard library. Built per call rather than held in a namespace-scope string — a
// static with a throwing constructor is uncatchable if it fails.
std::string long_id(char tag) { return std::string(63, 'c') + tag; }

// Fails EACH allocation of `command` in turn — the whole path, never one hand-picked point —
// and hands the post-throw engine to `check`. The loop bound is MEASURED on a clean run of
// the same setup and only then compared against the documented `expected_allocations`: two
// literals for one quantity drift apart silently, and the drift always hides the path's LAST
// allocation, which is exactly where every commit boundary below sits (a rollback that
// handled only the earlier ones would survive an unmeasured loop). `setup` runs per
// iteration because each `check` asserts the FULL post-throw state, some of which — a still
// free cl_id, a still-repeatable teardown — is observable only once.
template <class Setup, class Command, class Check>
void inject_at_every_allocation(std::size_t expected_allocations, Setup setup, Command command,
                                Check check) {
  const std::size_t path_allocations = [&] {
    auto eng = setup();
    return allocations_in([&] { command(eng); });
  }();
  REQUIRE(path_allocations == expected_allocations);
  for (std::size_t nth = 1; nth <= path_allocations; ++nth) {
    INFO("std::bad_alloc injected at allocation #" << nth);
    auto eng = setup();
    const auto attempt = [&] {
      const ThrowOnNthAllocation arm{nth};
      return command(eng);
    };
    CHECK_THROWS_AS(attempt(), std::bad_alloc);
    check(eng);
  }
}

} // namespace

using namespace engine_test;

TEST_CASE("engine: on_book allocates NOTHING in its steady state", "[engine]") {
  // The strongest row in the inventory and the one the benchmark/the A/B 100k-sample benchmarks
  // lean on hardest: the M3 path is a no-fill sweep, and its empty result vector never allocates.
  // Stdlib-independent by construction — zero is zero.
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new(long_id('a'), "B", 400000, 100)));
  const auto fresh = mk_book(500000, 100, 500010, 80, 2);
  const auto replay = mk_book(500000, 100, 500010, 80, 2);
  std::vector<mm::Routed> routed;

  const auto sweep_allocs = allocations_in([&] { routed = eng.on_book(fresh); });
  CHECK(routed.empty());
  CHECK(sweep_allocs == 0);
  // An IGNORED stale md_seq is likewise free — the guard returns before any work.
  const auto stale_allocs = allocations_in([&] { routed = eng.on_book(replay); });
  CHECK(routed.empty());
  CHECK(stale_allocs == 0);
  CHECK(eng.stale_books_ignored() == 1);
}

TEST_CASE("engine: the command paths allocate exactly their inventoried sources", "[engine]") {
  // Each row is measured on a session that has ALREADY sent a command, so the once-per-session
  // entries_by_session_ node is not in the figure — the declared STEADY-STATE basis of the
  // inventory, which is what the A/B counter must be seeded from.
  auto eng = primed_engine();
  const auto warm = mk_new(long_id('w'), "B", 400000, 100);
  require_only<mm::OrderAck>(eng.on_new(kSessA, warm));
  std::vector<mm::OutMsg> out;

  SECTION("accept: orders_ node + key cl_id + ack cl_id + live-index node + result vector") {
    const auto order = mk_new(long_id('a'), "B", 400000, 100);
    const auto allocs = allocations_in([&] { out = eng.on_new(kSessA, order); });
    require_only<mm::OrderAck>(out);
    CHECK(allocs == 5);
  }
  SECTION("tombstone reject: the same node + key, plus the Reject's cl_id and reason") {
    // Off-tick px. Its reason ("px is not a multiple of tick_size", 32 bytes) is past both
    // local capacities, so the heap-reason variant of the row is the stdlib-independent one.
    const auto order = mk_new(long_id('r'), "B", 499991, 100);
    const auto allocs = allocations_in([&] { out = eng.on_new(kSessA, order); });
    require_reject(out, mm::RejectCode::TickSize, order.cl_id);
    CHECK(allocs == 5);
  }
  SECTION("DupClOrdId returns EARLY: no tombstone node, no key string") {
    // Two fewer than the tombstone row above — that difference IS the assertion. A duplicate
    // that started storing a second entry would also break the entry-cap accounting.
    const auto order = mk_new(long_id('w'), "B", 400000, 100); // same id as `warm`
    const auto before = eng.entry_count(kSessA);
    const auto allocs = allocations_in([&] { out = eng.on_new(kSessA, order); });
    require_reject(out, mm::RejectCode::DupClOrdId, order.cl_id);
    CHECK(allocs == 3);
    CHECK(eng.entry_count(kSessA) == before);
  }
  SECTION("cancel of a live order: ack cl_id + result vector, and the LOOKUP is free") {
    // The transparent (session, cl_id) comparator means find() probes with a string_view pair
    // and materializes no key. A comparator change that dropped is_transparent would add
    // exactly one allocation here and nowhere else in the suite.
    const auto order = mk_new(long_id('c'), "B", 400000, 100);
    require_only<mm::OrderAck>(eng.on_new(kSessA, order));
    const auto cancel = mk_cancel(order.cl_id);
    const auto allocs = allocations_in([&] { out = eng.on_cancel(kSessA, cancel); });
    require_only<mm::CancelAck>(out);
    CHECK(allocs == 2);
  }
  SECTION("cancel of an unknown cl_id: reject cl_id + reason + result vector") {
    const auto cancel = mk_cancel(long_id('u'));
    const auto allocs = allocations_in([&] { out = eng.on_cancel(kSessA, cancel); });
    require_reject(out, mm::RejectCode::UnknownClOrdId, cancel.cl_id);
    CHECK(allocs == 3);
  }
  SECTION("a session's FIRST command costs one MORE than the steady-state row") {
    // The rows above are all measured steady-state, and that basis is exactly what the
    // engine.cpp inventory DECLARES ("a session's FIRST command adds +1 to either on_new
    // row") and what the A/B counter is seeded from. Without this section the +1 is
    // unfalsifiable: moving or dropping the once-per-session entries_by_session_ node
    // would invalidate the declared basis with a green suite. Measured on kSessB, which
    // has sent nothing — the warm-up above touches kSessA only. Stdlib-independent for the
    // same reason the rows it offsets are: the extra allocation is a std::map node, not a
    // string.
    SECTION("accept: the accept row + the entries_by_session_ node") {
      const auto order = mk_new(long_id('a'), "B", 400000, 100);
      const auto allocs = allocations_in([&] { out = eng.on_new(kSessB, order); });
      require_only<mm::OrderAck>(out);
      CHECK(allocs == 6);
    }
    SECTION("tombstone reject: the increment runs on the reject path too") {
      const auto order = mk_new(long_id('r'), "B", 499991, 100);
      const auto allocs = allocations_in([&] { out = eng.on_new(kSessB, order); });
      require_reject(out, mm::RejectCode::TickSize, order.cl_id);
      CHECK(allocs == 6);
    }
  }
  SECTION("a one-fill sweep: both vectors' growth + the Fill's cl_id copy") {
    // THREE, not two: a sweep that fills also grows the STAGING vector that holds the batch
    // until every allocation of it has succeeded (engine.cpp on_book — the price of the
    // sweep's strong exception guarantee). It is charged to filling sweeps only, which is
    // why the steady-state no-fill row above is still exactly zero.
    const auto order = mk_new(long_id('f'), "B", 500000, 50);
    require_only<mm::OrderAck>(eng.on_new(kSessA, order));
    const auto crossing = mk_book(499990, 100, 500000, 80, 2);
    std::vector<mm::Routed> routed;
    const auto allocs = allocations_in([&] { routed = eng.on_book(crossing); });
    require_only<mm::Fill>(routed);
    CHECK(allocs == 3);
  }
}

TEST_CASE("engine: a failed allocation ANYWHERE on the accept path changes nothing", "[engine]") {
  // engine.hpp declares on_new STRONGLY exception-safe and engine.cpp names the divergence
  // that guarantee prevents: an order Live in orders_ but absent from the live index, hence
  // invisible to on_book and live_count while still cancellable through orders_. The
  // rollback that prevents it (the try/catch around the live-index insert, plus the ordering
  // that keeps ++entries and ++next_eng_id_ after the last throwing operation) was
  // unfalsifiable until now: deleting it, or hoisting either increment above the insert,
  // left every other case in the suite green. The suite already owns the global operator new
  // (alloc_probe.cpp), so arming it to FAIL an allocation is the cheapest available pin.
  //
  // Every allocation of the accept path is injected in turn, so the loop covers the whole
  // path rather than the single interesting allocation, and a rollback that only handled the
  // last one would still have to survive the ones before it (see inject_at_every_allocation
  // on why the bound is measured rather than hand-copied).
  const auto warm = mk_new(long_id('w'), "B", 400000, 100);
  // Priced ABOVE `warm` so the crossing book at the end of each iteration reaches this order
  // and only this one — the fill is what proves the LIVE INDEX was restored.
  const auto probe = mk_new(long_id('p'), "B", 400010, 100);
  inject_at_every_allocation(
      /*expected_allocations=*/5,
      [&] {
        auto eng = primed_engine();
        require_only<mm::OrderAck>(eng.on_new(kSessA, warm)); // steady state, as the rows above
        REQUIRE(eng.live_count(kSessA) == 1);
        REQUIRE(eng.entry_count(kSessA) == 1);
        return eng;
      },
      [&](mm::OrderEngine &eng) { return eng.on_new(kSessA, probe); },
      [&](mm::OrderEngine &eng) {
        // 1-2. Neither view of the order world moved: no half-inserted live order, no
        // tombstone.
        CHECK(eng.live_count(kSessA) == 1);
        CHECK(eng.entry_count(kSessA) == 1);
        // 3. The cl_id was NOT consumed — a partially applied accept would answer DupClOrdId
        //    here and strand that id for the session's lifetime (invariant 4).
        // 4. The eng_id the failed call did not burn is the one this accept carries.
        const auto ack = require_only<mm::OrderAck>(eng.on_new(kSessA, probe));
        CHECK(ack.cl_id == probe.cl_id);
        CHECK(ack.eng_id == 2);
        CHECK(eng.live_count(kSessA) == 2);
        CHECK(eng.entry_count(kSessA) == 2);
        // The recovered order is reachable from the LIVE INDEX too, not just orders_: a
        // sweep that crosses it must produce its fill. This is the assertion that separates
        // a genuine rollback from an orders_-only one.
        const auto [sess, fill] =
            require_only<mm::Fill>(eng.on_book(mk_book(400000, 100, 400010, 100, 2)));
        CHECK(sess == kSessA);
        CHECK(fill.eng_id == ack.eng_id);
      });
}

TEST_CASE("engine: a failed FIRST command of a session leaves no counter node", "[engine]") {
  // The case above measures a session that has ALREADY sent a command, so the once-per-session
  // entries_by_session_ node is allocated before the injected call and NO injection can reach
  // it. On a session's first command that node is in the path: reserved before the commit (so
  // the ++ that records the entry cannot throw) and, without a rollback, left behind
  // zero-valued when a later allocation fails. entry_count cannot see it — 0 reads the same as
  // absent — so the falsifier is the RETRY's allocation count: a leaked node makes the retry a
  // steady-state command (5) instead of a first command (6).
  const auto probe_first_command = [](const mm::NewOrder &order, auto require_report) {
    inject_at_every_allocation(
        /*expected_allocations=*/6, [] { return primed_engine(); }, // kSessA has sent nothing
        [&](mm::OrderEngine &eng) { return eng.on_new(kSessA, order); },
        [&](mm::OrderEngine &eng) {
          CHECK(eng.entry_count(kSessA) == 0);
          CHECK(eng.live_count(kSessA) == 0);
          std::vector<mm::OutMsg> out;
          const auto retry = allocations_in([&] { out = eng.on_new(kSessA, order); });
          require_report(out);
          CHECK(retry == 6); // a FIRST command again: the leaked node would make this 5
          CHECK(eng.entry_count(kSessA) == 1);
        });
  };
  SECTION("accept path") {
    const auto order = mk_new(long_id('a'), "B", 400000, 100);
    probe_first_command(order, [&](const std::vector<mm::OutMsg> &out) {
      const auto ack = require_only<mm::OrderAck>(out);
      CHECK(ack.cl_id == order.cl_id);
      CHECK(ack.eng_id == 1); // no failed attempt burned one
    });
  }
  SECTION("reject path: the counter node is reserved there too") {
    const auto order = mk_new(long_id('r'), "B", 499991, 100); // off-tick -> tombstone
    probe_first_command(order, [&](const std::vector<mm::OutMsg> &out) {
      require_reject(out, mm::RejectCode::TickSize, order.cl_id);
    });
  }
}

TEST_CASE("engine: a failed allocation in on_cancel keeps the cancel retryable", "[engine]") {
  // Committing the cancellation before its CancelAck exists is unrecoverable in a way no
  // other entry point is: the order is terminal, so the RETRY answers AlreadyTerminal and the
  // client can never be told about a cancellation that did happen. The retry — not the
  // post-throw state alone — is therefore the discriminating assertion.
  const auto order = mk_new(long_id('c'), "B", 400000, 100);
  const auto cancel = mk_cancel(order.cl_id);
  inject_at_every_allocation(
      /*expected_allocations=*/2,
      [&] {
        auto eng = primed_engine();
        require_only<mm::OrderAck>(eng.on_new(kSessA, order));
        return eng;
      },
      [&](mm::OrderEngine &eng) { return eng.on_cancel(kSessA, cancel); },
      [&](mm::OrderEngine &eng) {
        CHECK(eng.live_count(kSessA) == 1); // still Live: the cancel did not happen
        const auto ack = require_only<mm::CancelAck>(eng.on_cancel(kSessA, cancel));
        CHECK(ack.cl_id == cancel.cl_id);
        CHECK(ack.status == "cancelled");
        CHECK(eng.live_count(kSessA) == 0);
      });
}

TEST_CASE("engine: a failed allocation in the sweep leaves the book replayable", "[engine]") {
  // The sweep is the worst place to commit early: installing the book makes the retry of that
  // same md_seq STALE, so fills lost to the failure could never be replayed. Two crossing
  // orders, so the injection also covers a half-applied MULTI-fill sweep — the first fill
  // committed, the second lost — which a per-fill guarantee would not catch.
  const auto bid_a = mk_new(long_id('a'), "B", 500000, 50);
  const auto bid_b = mk_new(long_id('b'), "B", 500000, 50);
  const auto crossing = mk_book(499990, 100, 500000, 80, 2);
  inject_at_every_allocation(
      /*expected_allocations=*/6,
      [&] {
        auto eng = primed_engine(); // base_book is md_seq 1: `crossing` is a real update
        require_only<mm::OrderAck>(eng.on_new(kSessA, bid_a));
        require_only<mm::OrderAck>(eng.on_new(kSessA, bid_b));
        return eng;
      },
      [&](mm::OrderEngine &eng) { return eng.on_book(crossing); },
      [&](mm::OrderEngine &eng) {
        // The candidate book was never installed, so the SAME book is still a fresh update
        // rather than a redelivery — and both orders are still Live and unfilled.
        CHECK(eng.book().md_seq == 1);
        CHECK(eng.stale_books_ignored() == 0);
        CHECK(eng.live_count(kSessA) == 2);
        const auto routed = eng.on_book(crossing);
        REQUIRE(routed.size() == 2); // the FULL batch, not the remainder of a partial sweep
        const auto first = require_at<mm::Fill>(routed, 0);
        const auto second = require_at<mm::Fill>(routed, 1);
        CHECK(first.qty == 50);  // no leaves were stolen by the failed sweep
        CHECK(second.qty == 50); // ... on either order (the book is exogenous)
        CHECK(first.leaves == 0);
        CHECK(second.leaves == 0);
        CHECK(first.exec_id == 1); // and no exec_id was burned by it
        CHECK(second.exec_id == 2);
        CHECK(eng.live_count(kSessA) == 0);
      });
}

TEST_CASE("engine: a failed allocation in end_session keeps the teardown repeatable", "[engine]") {
  // Cancelling as it acked left orders Cancelled but still INDEXED when the batch failed
  // mid-flight: the next on_book would FILL a cancelled order (terminal states are
  // absorbing), and the teardown could not be repeated — a retry skips them and emits
  // nothing. One bid and one ask, so both live indexes are covered.
  const auto bid = mk_new(long_id('b'), "B", 400000, 100);
  const auto ask = mk_new(long_id('s'), "S", 600000, 100);
  inject_at_every_allocation(
      /*expected_allocations=*/4,
      [&] {
        auto eng = primed_engine();
        require_only<mm::OrderAck>(eng.on_new(kSessA, bid));
        require_only<mm::OrderAck>(eng.on_new(kSessA, ask));
        return eng;
      },
      [](mm::OrderEngine &eng) { return eng.end_session(kSessA); },
      [](mm::OrderEngine &eng) {
        // Nothing was cancelled and nothing was reaped: the session is exactly as it was.
        CHECK(eng.live_count(kSessA) == 2);
        CHECK(eng.entry_count(kSessA) == 2);
        // And the teardown is still available IN FULL — BOTH acks. This is the discriminating
        // assertion: cancelling as it acked leaves the orders terminal but still indexed, so
        // the retry skips them and emits NOTHING while a crossing book would still fill them.
        const auto out = eng.end_session(kSessA);
        REQUIRE(out.size() == 2);
        CHECK(eng.live_count(kSessA) == 0);
        CHECK(eng.entry_count(kSessA) == 0);
        // Reaped from both live indexes: a book crossing both orders is now a no-op.
        CHECK(eng.on_book(mk_book(700000, 100, 300000, 100, 2)).empty());
      });
}
