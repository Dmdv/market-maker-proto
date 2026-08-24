// Task 3 tests, fill-rule part (lifecycle/state-machine cases live in test_engine.cpp,
// session-lifetime cases in test_engine_session.cpp, the allocation inventory in
// test_engine_alloc.cpp; shared helpers in engine_test_support.hpp — split under the
// 500-line file cap): the §3.3 fill rule (full/partial/exec_id, own-limit price,
// un-rounded quantities), absorbing terminals in the sweep (both sides),
// cancel-of-a-partial, the md_seq idempotency guard + stale_books_ignored counter
// (including a first book carrying md_seq 0), locked/crossed books (A3 F-28), F-30
// exogeneity, and the history-scaling regression for the live-order indexes (perf-tagged:
// hidden from the default suite, registered as its own ctest test — see CMakeLists.txt).
#include "engine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include "mm/engine.hpp"
#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace engine_test;

TEST_CASE("engine: fill rule — full, partial, exec_id monotonic", "[engine]") {
  auto eng = primed_engine();

  SECTION("full fill when the ask moves to/through the resting bid") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    const auto [fill_sess, fill] =
        require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 500000, 80, 2))); // ask <= P
    CHECK(fill_sess == kSessA);
    CHECK(fill.cl_id == "c1");
    CHECK(fill.eng_id == 1);
    CHECK(fill.px == 500000); // the order's own limit price, not the book's
    CHECK(fill.qty == 50);    // min(leaves=50, ask_qty=80)
    CHECK(fill.leaves == 0);
    CHECK(fill.exec_id == 1);
    CHECK(eng.live_count(kSessA) == 0); // Filled is terminal
  }
  SECTION("partial fill leaves the order Live; the second move completes it") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 100)));
    const auto [f1_sess, f1] = require_only<mm::Fill>(
        eng.on_book(mk_book(499990, 100, 499995, 40, 2))); // ask_qty < leaves
    CHECK(f1.px == 500000); // own limit, though the book's ask is 499995 (through-market)
    CHECK(f1.qty == 40);
    CHECK(f1.leaves == 60);
    CHECK(f1.exec_id == 1);
    CHECK(eng.live_count(kSessA) == 1); // still Live

    const auto [f2_sess, f2] =
        require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 499995, 100, 3)));
    CHECK(f2.px == 500000); // own limit again, not the 499995 book price
    CHECK(f2.qty == 60);
    CHECK(f2.leaves == 0);
    CHECK(f2.exec_id == 2); // exec_id increments across fills
    CHECK(eng.live_count(kSessA) == 0);
  }
  SECTION("a partially filled order can still be cancelled; the remainder never fills") {
    // The core MM path — partial fill -> cancel the remainder — pins the
    // cancellability gate: a partial leaves st == Live with leaves != qty, and BOTH
    // cancel entry points must treat it as cancellable, not AlreadyTerminal.
    const auto ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 100)));
    REQUIRE(eng.on_book(mk_book(499990, 100, 499995, 40, 2)).size() == 1); // partial 40
    const auto cxl = require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("c1")));
    CHECK(cxl.cl_id == "c1");
    CHECK(cxl.eng_id == ack.eng_id);
    CHECK(cxl.status == "cancelled");
    CHECK(eng.live_count(kSessA) == 0);
    // The remaining 60 must never fill: a still-crossing book at a fresh md_seq routes
    // nothing.
    CHECK(eng.on_book(mk_book(499990, 100, 499995, 100, 3)).empty());
  }
  SECTION("a partially filled order is cancelled by end_session; the remainder never fills") {
    const auto ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 100)));
    REQUIRE(eng.on_book(mk_book(499990, 100, 499995, 40, 2)).size() == 1); // partial 40
    const auto cxl = require_only<mm::CancelAck>(eng.end_session(kSessA));
    CHECK(cxl.cl_id == "c1");
    CHECK(cxl.eng_id == ack.eng_id);
    CHECK(eng.on_book(mk_book(499990, 100, 499995, 100, 3)).empty());
  }
  SECTION("a resting ask fills when the bid moves to/through it") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "S", 500010, 50)));
    const auto [fill_sess, fill] =
        require_only<mm::Fill>(eng.on_book(mk_book(500010, 30, 500020, 80, 2))); // bid >= P
    CHECK(fill.px == 500010);
    CHECK(fill.qty == 30);
    CHECK(fill.leaves == 20);
  }
  SECTION("bid fill price is the OWN limit when the ask crosses strictly THROUGH it") {
    // Discriminating row: book price != order limit, px asserted.
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b1", "B", 500000, 80)));
    const auto [fill_sess, fill] =
        require_only<mm::Fill>(eng.on_book(mk_book(499980, 100, 499990, 80, 2))); // ask 499990 < P
    CHECK(fill.px == 500000); // own limit, NOT the book's 499990
    CHECK(fill.qty == 80);
    CHECK(fill.leaves == 0);
  }
  SECTION("ask fill price is the OWN limit when the bid crosses strictly THROUGH it") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("s1", "S", 500010, 50)));
    const auto [fill_sess, fill] =
        require_only<mm::Fill>(eng.on_book(mk_book(500020, 60, 500030, 80, 2))); // bid 500020 > P
    CHECK(fill.px == 500010); // own limit, NOT the book's 500020
    CHECK(fill.qty == 50);
    CHECK(fill.leaves == 0);
  }
  SECTION("a corrupt side with px <= 0 but qty > 0 is absent: no cross, no fill") {
    // Defense-in-depth: px is not an absence sentinel in the feed, but a zero px must
    // not cross every resting bid.
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    CHECK(eng.on_book(mk_book(499990, 100, 0, 100, 2)).empty());
    CHECK(eng.live_count(kSessA) == 1);
    // Entry-side mirror: the phantom ask at 0 does not PostOnlyCross a new bid.
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c2", "B", 499995, 50)));
  }
  SECTION("fill qty is un-rounded: min(leaves, top) with no lot flooring") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    // Sub-lot top (5 < lot_size 10) still fills 5 — lot granularity is entry-only.
    const auto [f1_sess, f1] =
        require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 500000, 5, 2)));
    CHECK(f1.qty == 5);
    CHECK(f1.leaves == 45); // a non-lot-multiple remainder stays Live
    CHECK(eng.live_count(kSessA) == 1);
    // Non-lot-multiple top (25) fills min(45, 25) = 25 — never floored to 20.
    const auto [f2_sess, f2] =
        require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 500000, 25, 3)));
    CHECK(f2.qty == 25);
    CHECK(f2.leaves == 20);
    CHECK(eng.live_count(kSessA) == 1);
  }
  SECTION("a vanished opposite side (qty 0) never fills") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    CHECK(eng.on_book(mk_book(499990, 100, 0, 0, 2)).empty());
    CHECK(eng.live_count(kSessA) == 1);
  }
  SECTION("absence is the QTY arm alone: a zero-qty ask at a CROSSING px fills nothing") {
    // Falsifiability follow-up: every other absent-side case in the suite pairs
    // qty == 0 with px == 0, so the px <= 0 arm of the absence rule masked the
    // qty <= 0 arm — a mutant dropping the qty check survived the full suite. Here the
    // phantom ask carries a px that WOULD cross the resting bid; only qty says absent.
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b1", "B", 500000, 50)));
    CHECK(eng.on_book(mk_book(0, 0, 499990, 0, 2)).empty()); // ask 499990 x 0: no fill
    CHECK(eng.live_count(kSessA) == 1);
    // Entry-side mirror (the sweep and the post-only check share the absence rule):
    // the phantom crossing ask does not PostOnlyCross a new bid either.
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b2", "B", 500000, 50)));
  }
  SECTION("absence is the QTY arm alone: a zero-qty bid at a CROSSING px fills nothing") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("s1", "S", 500010, 50)));
    CHECK(eng.on_book(mk_book(500020, 0, 0, 0, 2)).empty()); // bid 500020 x 0: no fill
    CHECK(eng.live_count(kSessA) == 1);
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("s2", "S", 500010, 50)));
  }
}

TEST_CASE("engine: the book is EXOGENOUS (F-30) — fills never consume displayed qty", "[engine]") {
  // Two resting bids of 50 against a displayed ask top of 50: BOTH fill their full
  // min(leaves, top) = 50, so total filled (100) EXCEEDS the displayed 50. Intentional,
  // not a bug: the TOB feed is an exogenous signal the engine never depletes (F-30) —
  // each resting order is evaluated against the full displayed top independently.
  // The bids are accepted in REVERSE lexicographic order ("b2" before "b1") so the
  // per-fill assertions below pin (session, cl_id) KEY order, not insertion order, and
  // the explicit exec_id 1/2 pins per-fill uniqueness WITHIN one sweep — a sweep
  // stamping every fill with the same exec_id would corrupt any client ledger keyed
  // on it.
  auto eng = primed_engine();
  const auto b2_ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b2", "B", 500000, 50)));
  const auto b1_ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b1", "B", 500000, 50)));

  const auto routed = eng.on_book(mk_book(499990, 100, 500000, 50, 2));
  REQUIRE(routed.size() == 2);
  const auto first = require_at<mm::Fill>(routed, 0);
  const auto second = require_at<mm::Fill>(routed, 1);
  CHECK(first.cl_id == "b1"); // key order, though "b2" was accepted first
  // Fill.eng_id is checked against the ACK's handle, not a literal: this is the only case in
  // the suite with two distinct live orders filling, so it is the only place a constant
  // `fill.eng_id = 1` can be told apart from the real order handle (same pattern the cancel
  // paths already use for CancelAck.eng_id).
  CHECK(first.eng_id == b1_ack.eng_id);
  CHECK(first.exec_id == 1);
  CHECK(first.qty == 50); // full top for EACH order — 100 filled vs 50 displayed
  CHECK(first.leaves == 0);
  CHECK(second.cl_id == "b2");
  CHECK(second.eng_id == b2_ack.eng_id);
  CHECK(second.exec_id == 2); // unique within the sweep, not just across sweeps
  CHECK(second.qty == 50);
  CHECK(second.leaves == 0);
}

TEST_CASE("engine: terminal states are absorbing in the fill sweep", "[engine]") {
  auto eng = primed_engine();

  SECTION("a cancelled order never fills, even when the book crosses it") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("c1")));
    CHECK(eng.on_book(mk_book(499990, 100, 499995, 80, 2)).empty()); // crossing, no fill
    CHECK(eng.live_count(kSessA) == 0);
    require_reject(eng.on_cancel(kSessA, mk_cancel("c1")), mm::RejectCode::AlreadyTerminal, "c1");
  }
  SECTION("a cancelled SELL never fills — the ask side of the live-index maintenance") {
    // Ask-side mirror of the case above: on_cancel must remove the order from the ASK
    // index, and the sweep trusts the index — a mutant erasing from the bid index on a
    // sell cancel would leave a phantom ask that fills on the next crossing book.
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("s1", "S", 500010, 50)));
    require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("s1")));
    CHECK(eng.live_count(kSessA) == 0);
    CHECK(eng.on_book(mk_book(500020, 100, 500030, 80, 2)).empty()); // bid > P: no fill
  }
  SECTION("a filled order never fills again; the no-op sweep consumes no exec_id") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    const auto [f1_sess, f1] =
        require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 499995, 80, 2)));
    CHECK(f1.leaves == 0);
    CHECK(f1.exec_id == 1);
    // Still-crossing book at a NEW md_seq: the Filled order is absorbing — no re-fill.
    CHECK(eng.on_book(mk_book(499990, 100, 499995, 80, 3)).empty());
    CHECK(eng.live_count(kSessA) == 0);
    // exec_id was untouched by the no-op sweep: the next real fill gets 2.
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c2", "B", 500000, 50, false)));
    const auto [f2_sess, f2] =
        require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 499995, 80, 4)));
    CHECK(f2.exec_id == 2);
  }
}

TEST_CASE("engine: md_seq guard — repeated or rewound books are ignored", "[engine]") {
  // The fill sweep runs exactly once per TOB update; a redelivered or rewound md_seq
  // neither fills nor installs the stale book — and each drop is COUNTED
  // (stale_books_ignored), never silent: the feed is monotonic by construction, so
  // any nonzero count in production is a producer regression Task 7 must surface.
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 100)));

  const auto first = eng.on_book(mk_book(499990, 100, 499995, 40, 2)); // partial 40
  REQUIRE(first.size() == 1);
  CHECK(eng.stale_books_ignored() == 0); // real updates are never counted as drops

  // Replay of md_seq 2: NO phantom second fill, and the drop is counted.
  CHECK(eng.on_book(mk_book(499990, 100, 499995, 40, 2)).empty());
  CHECK(eng.stale_books_ignored() == 1);
  // Rewind to md_seq 1: ignored (counted), and the stale book is NOT installed.
  CHECK(eng.on_book(base_book(1)).empty());
  CHECK(eng.stale_books_ignored() == 2);
  CHECK(eng.book().md_seq == 2);
  CHECK(eng.book().ask_qty == 40);

  // The next real update completes the order with the TRUE remaining 60 — proof that
  // the replay/rewind above never touched leaves.
  const auto [f2_sess, f2] =
      require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 499995, 100, 3)));
  CHECK(f2.qty == 60);
  CHECK(f2.leaves == 0);
}

TEST_CASE("engine: a forward md_seq GAP is a real update, not a drop", "[engine]") {
  // The guard is "<= the last PROCESSED md_seq", and that is the engine's ONLY md_seq
  // consumption (engine.hpp: no gap detection, no reordering — those belong to the protocol
  // layer). A jump forward is the shape a coalesced or dropped feed update takes, and it must
  // install and sweep like any other real update; without this case an "expected == last + 1"
  // check would pass the whole suite while making the engine deaf for the rest of the
  // connection after a single feed hiccup.
  auto eng = primed_engine(); // base_book at md_seq 1
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
  CHECK(eng.on_book(mk_book(499990, 100, 500010, 80, 2)).empty()); // ask above P: no fill yet

  const auto [fill_sess, fill] =
      require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 500000, 80, 9))); // 2 -> 9
  CHECK(fill.qty == 50);
  CHECK(eng.book().md_seq == 9);
  CHECK(eng.stale_books_ignored() == 0); // a gap is not a drop and is never counted as one
  // The guard follows the jump rather than the skipped numbers: 8 is now stale.
  CHECK(eng.on_book(mk_book(499990, 100, 500000, 80, 8)).empty());
  CHECK(eng.stale_books_ignored() == 1);
}

TEST_CASE("engine: a FIRST book carrying md_seq 0 is installed and sweeps", "[engine]") {
  // The pre-first-book state is tracked explicitly (book_seen_), not by an md_seq==0
  // sentinel — a real first update with md_seq 0 must not be silently dropped, or its
  // fills vanish and the post-only entry check stays blind. md_seq is assigned by the
  // Task 5 feed / Task 7 server; the engine does not assume it starts at 1.
  mm::OrderEngine eng{spec_instrument()};
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));

  const auto [fill_sess, fill] =
      require_only<mm::Fill>(eng.on_book(mk_book(499990, 100, 500000, 80, 0))); // md_seq 0 crosses
  CHECK(fill.qty == 50);
  CHECK(eng.book().md_seq == 0);
  CHECK(eng.book().ask_px == 500000); // installed, not dropped

  // From the second book on the guard is armed: a REPEAT of md_seq 0 is ignored and
  // counted.
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c2", "B", 499995, 50)));
  CHECK(eng.on_book(mk_book(499990, 100, 499995, 80, 0)).empty());
  CHECK(eng.stale_books_ignored() == 1);
  // And the installed md_seq-0 book drives the post-only entry check.
  require_reject(eng.on_new(kSessA, mk_new("c3", "B", 500000, 50)), mm::RejectCode::PostOnlyCross,
                 "c3");
}

TEST_CASE("engine: locked and crossed books (F-28)", "[engine]") {
  mm::OrderEngine eng{spec_instrument()};

  SECTION("locked book: entry at the lock price crosses; a resting order at it fills") {
    REQUIRE(eng.on_book(base_book()).empty());
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    // Locked at 500000: ask <= P fills c1.
    REQUIRE(eng.on_book(mk_book(500000, 100, 500000, 80, 2)).size() == 1);
    require_reject(eng.on_new(kSessA, mk_new("c2", "B", 500000, 50)), mm::RejectCode::PostOnlyCross,
                   "c2");
    require_reject(eng.on_new(kSessA, mk_new("c3", "S", 500000, 50)), mm::RejectCode::PostOnlyCross,
                   "c3");
  }
  SECTION("crossed book fills resting bids BEFORE resting asks (documented tie-break)") {
    REQUIRE(eng.on_book(base_book()).empty());
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("bid1", "B", 500000, 50)));
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("ask1", "S", 500010, 50)));
    // Crossed feed: bid 500020 >= ask1's 500010 AND ask 499990 <= bid1's 500000.
    const auto routed = eng.on_book(mk_book(500020, 100, 499990, 100, 2));
    REQUIRE(routed.size() == 2);
    const auto first = require_at<mm::Fill>(routed, 0);
    const auto second = require_at<mm::Fill>(routed, 1);
    CHECK(first.cl_id == "bid1");
    CHECK(first.exec_id == 1); // unique per fill WITHIN the sweep, bids first
    CHECK(second.cl_id == "ask1");
    CHECK(second.exec_id == 2);
  }
}

// History-scaling regression guards. The `[.]` tag HIDES this case from
// catch_discover_tests — it is the suite's only wall-clock-bounded case, and a
// scheduler preemption on a loaded CI host must never red a CORRECTNESS gate (a red
// that trains reviewers to wave "flakes" through). It is NOT unenforced: CMakeLists.txt
// registers it as its own ctest test (label `perf`, run via `ctest --preset perf`),
// and the Task 12/13 obligation to run that preset is queued in PENDING_AMENDMENTS.
//
// Deliberately NOT tagged `[engine]`, the tag every correctness engine case carries:
// `[.]` only suppresses the UNFILTERED run, so a targeted `./mm_tests "[engine]"` in a
// Debug or sanitizer tree would otherwise execute these Release-calibrated bounds at -O0
// through a path neither the preset label filter nor the Release-only registration covers.
// `[perf]` is the sole selector that reaches this case.
TEST_CASE("engine: hot-path cost does not scale with retained history", "[.][perf]") {
  constexpr std::size_t kHistory = 20'000;
  // Each configuration is timed kReps times and the MINIMA are compared. A single timing of
  // each is not a measurement: the two configurations differ in working-set size, so the
  // deep one is systematically more exposed to deschedules and TLB refills, and a
  // single-shot ratio conflates that footprint sensitivity with the algorithmic history
  // sensitivity the test exists to guard (measured: 40% failure rate under heavy CPU
  // contention, always with the deep run inflated 5-16x while the base stayed flat). The
  // minimum over repetitions is the standard robust estimator of unperturbed cost, and it
  // removes that bias — under the same contention the min-of-N ratio held at 0.88-1.10.
  constexpr int kReps = 7;
  const auto min_ns = [](auto &&time_one, std::size_t history) {
    auto best = time_one(history);
    for (int rep = 1; rep < kReps; ++rep)
      best = std::min(best, time_one(history));
    return best;
  };

  SECTION("full command cycle: catches a return of the O(N) history walk") {
    // With kHistory terminal entries retained on the session, on_new/on_book/on_cancel
    // must never LINEARLY walk retained history. The timed loop carries NO Catch2
    // assertions (they would swamp the engine cost and hide an O(N) regression behind
    // common-mode overhead); results are counted inside and asserted after.
    // Calibration: the pre-index linear walk cost ~240 us per cycle at 20k history ->
    // ~480 ms over kCycles, far over this bound; the indexed engine measures ~0.67 ms at
    // history 0 and ~0.83 ms at 20k (min-of-kReps, dev host macOS arm64, rel -O3 -DNDEBUG
    // — the perf preset's flavor; TIME_LOG.md row 3 records 0.685/0.826 for the same
    // change). Re-measure and update these two figures whenever the command path moves.
    // This bound is deliberately NOT sensitive to the residual O(log n) tree descent
    // on the on_new/on_cancel map lookups (a documented, accepted cost — engine.hpp
    // "Complexity"): the map work inside the cycle dominates and masks it. The
    // sweep-only section below is the history-blindness pin.
    constexpr int kCycles = 2'000;
    const auto run_ns = [](std::size_t history) {
      auto eng = primed_engine();
      for (std::size_t i = 0; i < history; ++i) {
        // Off-tick px -> Rejected tombstone: the cheapest way to mint retained
        // terminals.
        const std::string cl_id = "t" + std::to_string(i);
        require_reject(eng.on_new(kSessA, mk_new(cl_id, "B", 499991, 100)),
                       mm::RejectCode::TickSize, cl_id);
      }
      // The whole premise is that this history EXISTS. If a later change stopped retaining
      // tombstones or reaped them early, `deep` would collapse onto `base`, both bounds
      // below would pass, and the case would report green while testing nothing.
      REQUIRE(eng.entry_count(kSessA) == history);
      std::uint64_t md_seq = 2;
      std::size_t acks = 0, empty_sweeps = 0, cancel_acks = 0;
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < kCycles; ++i) {
        // One live resting bid far below the ask: swept every update, never fills.
        const std::string cl_id = "live" + std::to_string(i);
        acks += eng.on_new(kSessA, mk_new(cl_id, "B", 400000, 100)).size();
        empty_sweeps += eng.on_book(mk_book(500000, 100, 500010, 80, md_seq++)).empty() ? 1 : 0;
        cancel_acks += eng.on_cancel(kSessA, mk_cancel(cl_id)).size();
      }
      const auto t1 = std::chrono::steady_clock::now();
      REQUIRE(acks == static_cast<std::size_t>(kCycles));
      REQUIRE(empty_sweeps == static_cast<std::size_t>(kCycles));
      REQUIRE(cancel_acks == static_cast<std::size_t>(kCycles));
      return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    };
    const auto base = min_ns(run_ns, 0);
    const auto deep = min_ns(run_ns, kHistory);
    INFO("base_ns=" << base << " deep_ns=" << deep);
    // Tightened with the minima: the previous +10 ms additive slack was ~8x the whole
    // measured deep run, so the multiplicative term could never bind and only a >12x
    // regression was catchable. 3x + 1 ms still clears the ~0.67 ms / ~0.83 ms measurement
    // above by a wide margin while making the ratio the thing that actually fails.
    CHECK(deep < base * 3 + 1'000'000);
  }

  SECTION("sweep only: on_book is HISTORY-BLIND (the M3 path)") {
    // The genuine history-blindness pin for the fill sweep: on_book ALONE in the timed
    // region — no on_new/on_cancel map work to mask it. The live index stores orders_
    // iterators, so the no-fill sweep must cost the same at 0 and 20k retained
    // entries (measured over kSweeps: ~0.80 ms vs ~0.80 ms — ratio ~1.0; min-of-kReps,
    // dev host macOS arm64, rel -O3 -DNDEBUG, the perf preset's flavor, agreeing with the
    // 0.799/0.795 TIME_LOG.md row 3 records for the same change. Only the RATIO is a
    // contract: the MAGNITUDES track host load, and repeat batches on this same tree have
    // read as high as ~0.94 ms with the ratio still ~1.0 — which is why the bound below is
    // multiplicative and why an absolute figure here is calibration, never an assertion); a
    // regression that re-introduces a per-swept-order descent into orders_ (~14 extra
    // key compares at 20k history, ~10x the ~5 ns rel sweep) blows the 3x ratio at
    // kSweeps volume.
    constexpr int kSweeps = 200'000;
    const auto sweep_ns = [](std::size_t history) {
      auto eng = primed_engine();
      for (std::size_t i = 0; i < history; ++i) {
        const std::string cl_id = "t" + std::to_string(i);
        require_reject(eng.on_new(kSessA, mk_new(cl_id, "B", 499991, 100)),
                       mm::RejectCode::TickSize, cl_id);
      }
      // One live resting bid far below the ask: swept every update, never fills.
      require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("resting", "B", 400000, 100)));
      // Same premise check as the cycle section: the retained history must be there (+1 for
      // the resting order just accepted), or "history-blind" is measured against nothing.
      REQUIRE(eng.entry_count(kSessA) == history + 1);
      std::uint64_t md_seq = 2;
      std::size_t empty_sweeps = 0;
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < kSweeps; ++i)
        empty_sweeps += eng.on_book(mk_book(500000, 100, 500010, 80, md_seq++)).empty() ? 1 : 0;
      const auto t1 = std::chrono::steady_clock::now();
      REQUIRE(empty_sweeps == static_cast<std::size_t>(kSweeps));
      return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    };
    const auto base = min_ns(sweep_ns, 0);
    const auto deep = min_ns(sweep_ns, kHistory);
    INFO("sweep_base_ns=" << base << " sweep_deep_ns=" << deep);
    CHECK(deep < base * 2 + 500'000); // 2x + 0.5 ms: the minima measure a ratio of ~1.0
  }
}
