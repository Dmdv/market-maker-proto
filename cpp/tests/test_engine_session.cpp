// Task 3 tests, session-lifetime part (lifecycle/state-machine cases live in
// test_engine.cpp, fill-rule cases in test_engine_fills.cpp, the allocation inventory in
// test_engine_alloc.cpp; shared helpers in engine_test_support.hpp — split under the
// 500-line file cap): everything that is about a SESSION rather than a single order —
// per-session cl_id namespacing and cross-session isolation (A2 F-01/F-07), end_session's
// CancelAcks and its destructive reap, the session-key range boundary at UINT64_MAX, and
// the per-session entry cap policy signal.
#include "engine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include "mm/engine.hpp"
#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <cstdint>
#include <limits>

using namespace engine_test;

TEST_CASE("engine: session scoping (F-01/F-07)", "[engine]") {
  auto eng = primed_engine();

  SECTION("cl_id namespaces are per session: same cl_id on two sessions both accepted") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("x", "B", 499990, 100)));
    require_only<mm::OrderAck>(eng.on_new(kSessB, mk_new("x", "B", 499985, 100)));
    CHECK(eng.live_count(kSessA) == 1);
    CHECK(eng.live_count(kSessB) == 1);
  }
  SECTION("a cancel naming another session's cl_id is UnknownClOrdId; the order is untouched") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("only-a", "B", 499990, 100)));
    require_reject(eng.on_cancel(kSessB, mk_cancel("only-a")), mm::RejectCode::UnknownClOrdId,
                   "only-a");
    CHECK(eng.live_count(kSessA) == 1); // A's order still live
  }
  SECTION("end_session(A) cancels only A's live orders; B's stay intact") {
    const auto a1 = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a1", "B", 499990, 100)));
    require_only<mm::OrderAck>(eng.on_new(kSessB, mk_new("b1", "B", 499985, 100)));
    const auto ack = require_only<mm::CancelAck>(eng.end_session(kSessA));
    CHECK(ack.cl_id == "a1");
    CHECK(ack.eng_id == a1.eng_id); // full CancelAck payload, same pin as on_cancel's
    CHECK(ack.status == "cancelled");
    CHECK(eng.live_count(kSessA) == 0);
    CHECK(eng.live_count(kSessB) == 1);
  }
  SECTION("fills route to the owning session") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a1", "B", 500000, 50)));
    require_only<mm::OrderAck>(eng.on_new(kSessB, mk_new("b1", "B", 500000, 50)));
    const auto routed = eng.on_book(mk_book(499990, 100, 500000, 200, 2));
    REQUIRE(routed.size() == 2);
    CHECK(routed[0].session == kSessA); // (session, cl_id) key order within a side
    CHECK(routed[1].session == kSessB);
  }
}

TEST_CASE("engine: end_session emits one CancelAck per LIVE order only", "[engine]") {
  auto eng = primed_engine();

  SECTION("a session with no orders at all is an empty no-op") {
    CHECK(eng.end_session(kSessA).empty());
    CHECK(eng.entry_count(kSessA) == 0);
  }
  SECTION("one live + one terminal -> exactly one CancelAck") {
    const auto live1 =
        require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("live1", "B", 499990, 100)));
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("gone1", "B", 499985, 100)));
    require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("gone1"))); // now terminal

    const auto ack = require_only<mm::CancelAck>(
        eng.end_session(kSessA)); // the terminal order is not re-cancelled
    CHECK(ack.cl_id == "live1");
    CHECK(ack.eng_id == live1.eng_id); // payload pinned, not just the correlation key
    CHECK(ack.status == "cancelled");
    CHECK(eng.live_count(kSessA) == 0);
  }
  SECTION("a live bid AND a live ask -> two acks, in (session, cl_id) key order") {
    // end_session's BATCHING is unfalsifiable with a single live order: acking only the first
    // live entry found, or only one side's orders, passes every other case in this file. The
    // §7 quote loop holds exactly one of each, so this is also the shape the disconnect path
    // actually meets in the demo.
    const auto bid =
        require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a-bid", "B", 499990, 100)));
    const auto ask =
        require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b-ask", "S", 500020, 100)));
    const auto out = eng.end_session(kSessA);
    REQUIRE(out.size() == 2);
    const auto ack_bid = require_at<mm::CancelAck>(out, 0);
    const auto ack_ask = require_at<mm::CancelAck>(out, 1);
    CHECK(ack_bid.cl_id == "a-bid");
    CHECK(ack_bid.eng_id == bid.eng_id);
    CHECK(ack_ask.cl_id == "b-ask");
    CHECK(ack_ask.eng_id == ask.eng_id);
    CHECK(eng.live_count(kSessA) == 0);
    // Both left their live index: a book crossing BOTH of them fills nothing.
    CHECK(eng.on_book(mk_book(700000, 100, 300000, 100, 2)).empty());
  }
  SECTION("a live SELL is cancelled too — the ask side of the disconnect path") {
    // Mirror of the bid-only cases above: both cancel entry points must be pinned on
    // BOTH sides, or an ask-index maintenance mutant survives the suite.
    const auto s1 = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("s1", "S", 500010, 50)));
    const auto ack = require_only<mm::CancelAck>(eng.end_session(kSessA));
    CHECK(ack.cl_id == "s1");
    CHECK(ack.eng_id == s1.eng_id);
    CHECK(ack.status == "cancelled");
    // A book that would cross the cancelled ask fills nothing.
    CHECK(eng.on_book(mk_book(500020, 100, 500030, 80, 2)).empty());
  }
}

TEST_CASE("engine: end_session ENDS the session lifetime — entries are reaped", "[engine]") {
  // Retention is scoped to the SESSION's lifetime, and end_session (the
  // cancel-on-disconnect hook) is the end of it — the whole (session, cl_id) range is
  // reclaimed, or dead sessions' orders would stay resident for the process lifetime.
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a1", "B", 499990, 100)));
  require_reject(eng.on_new(kSessA, mk_new("r1", "B", 499991, 100)), mm::RejectCode::TickSize,
                 "r1");
  require_only<mm::OrderAck>(eng.on_new(kSessB, mk_new("b1", "B", 499985, 100)));
  CHECK(eng.entry_count(kSessA) == 2); // one live + one tombstone

  const auto out = eng.end_session(kSessA);
  REQUIRE(out.size() == 1);            // one live -> one CancelAck; the tombstone gets none
  CHECK(eng.entry_count(kSessA) == 0); // live AND tombstone reaped
  CHECK(eng.live_count(kSessA) == 0);

  // In production the epoch never recurs (D6), so the reaped range is unaddressable;
  // reusing the numeric key here therefore models a FRESH session: previously consumed
  // cl_ids — accepted and tombstoned alike — are free again, and a cancel of a reaped
  // cl_id is UnknownClOrdId (the entry is GONE, not terminal). This destruction of the
  // cl_id history is exactly why the call is named end_session, and why it must never
  // be invoked on a still-connected session.
  require_reject(eng.on_cancel(kSessA, mk_cancel("a1")), mm::RejectCode::UnknownClOrdId, "a1");
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a1", "B", 499990, 100)));
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("r1", "B", 499990, 100)));
  // Session B was untouched by A's reap.
  CHECK(eng.live_count(kSessB) == 1);
  CHECK(eng.entry_count(kSessB) == 1);
}

TEST_CASE("engine: a session key at UINT64_MAX addresses only its own orders", "[engine]") {
  // The session range is deliberately lower_bound + a forward walk, NOT
  // upper_bound(session_floor(session + 1)): the latter OVERFLOWS at UINT64_MAX and silently
  // widens the range to the whole container. Without a key at the boundary the engine.hpp
  // paragraph explaining that is unfalsifiable — a "simplification" to upper_bound would pass
  // the suite while making end_session(UINT64_MAX) reap every session's orders and route
  // their CancelAcks to the wrong peer.
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a1", "B", 499990, 100)));
  require_only<mm::OrderAck>(eng.on_new(kMax, mk_new("m1", "B", 499985, 100)));
  CHECK(eng.live_count(kMax) == 1);
  CHECK(eng.entry_count(kMax) == 1);

  const auto ack = require_only<mm::CancelAck>(eng.end_session(kMax)); // exactly one, not two
  CHECK(ack.cl_id == "m1");
  CHECK(eng.entry_count(kMax) == 0);
  CHECK(eng.live_count(kSessA) == 1); // A untouched by the top-of-range reap
  CHECK(eng.entry_count(kSessA) == 1);
}

TEST_CASE("engine: the per-session entry cap is a policy signal", "[engine]") {
  // Tombstones are NOT gated by max_live_orders, so invalid orders mint entries at
  // frame rate; the cap bounds that growth. The engine keeps recording past the cap
  // (invariant 4 holds until the session dies) — the Server closes 1008 on breach
  // (engine.hpp contract; the Task 7 obligation is queued as PENDING_AMENDMENTS (i)).
  auto eng = primed_engine(/*max_session_entries=*/3);
  CHECK_FALSE(eng.entry_cap_breached(kSessA));

  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a1", "B", 499990, 100)));
  require_reject(eng.on_new(kSessA, mk_new("r1", "B", 499991, 100)), mm::RejectCode::TickSize,
                 "r1");
  CHECK_FALSE(eng.entry_cap_breached(kSessA)); // 2 of 3
  require_reject(eng.on_new(kSessA, mk_new("r2", "B", 499991, 100)), mm::RejectCode::TickSize,
                 "r2");
  CHECK(eng.entry_count(kSessA) == 3);
  CHECK(eng.entry_cap_breached(kSessA));
  CHECK_FALSE(eng.entry_cap_breached(kSessB)); // per session, like every other limit

  // Recording continues past the cap: invariant 4 must hold until the policy close.
  require_reject(eng.on_new(kSessA, mk_new("r3", "B", 499991, 100)), mm::RejectCode::TickSize,
                 "r3");
  CHECK(eng.entry_count(kSessA) == 4);
  // A duplicate reject consumes NO new entry (the early return precedes the tombstone).
  require_reject(eng.on_new(kSessA, mk_new("r1", "B", 499990, 100)), mm::RejectCode::DupClOrdId,
                 "r1");
  CHECK(eng.entry_count(kSessA) == 4);

  // The policy close's cancel-on-disconnect reaps the session and clears the condition.
  REQUIRE(eng.end_session(kSessA).size() == 1);
  CHECK_FALSE(eng.entry_cap_breached(kSessA));
}
