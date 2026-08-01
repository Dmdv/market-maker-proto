// tests, lifecycle/state-machine part (session-lifetime cases live in
// test_engine_session.cpp, fill-rule cases in test_engine_fills.cpp, the allocation
// inventory in test_engine_alloc.cpp; shared helpers in engine_test_support.hpp — split
// under the 500-line file cap). One section per the order-lifecycle spec "Order lifecycle" row
// (new, duplicate ID, reject, cancel, re-quote), plus the Rejected-tombstone and
// constructor-validation contracts, both reject-reason matrices, and the determinism
// byte-pin.
//
// Unit conventions (types.hpp boundary rule): tests speak RAW wire units (px multiple of
// tick_size=5, qty multiple of lot_size=10); the engine converts px to Ticks internally
// and every outbound Fill carries raw units again.
#include "engine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include "mm/codec.hpp"
#include "mm/engine.hpp"
#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace engine_test;

TEST_CASE("engine: new order is acked live", "[engine]") {
  auto eng = primed_engine();

  const auto ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 499990, 100)));
  CHECK(ack.cl_id == "c1");
  CHECK(ack.eng_id == 1);
  CHECK(ack.status == "live");
  // The engine has no clock and NEVER stamps a service time — the Server does, at send time
  // . Asserted here so an engine that started fabricating a latency number would be
  // caught engine-side; the field default-initializes to 0, so this pins the contract, not
  // the assignment.
  CHECK(ack.svc_ns == 0);
  CHECK(eng.live_count(kSessA) == 1);
  CHECK(eng.book().bid_px == 500000); // book() reflects the last on_book
  CHECK(eng.book().md_seq == 1);
}

TEST_CASE("engine: duplicate cl_id rejects, including against a terminal order", "[engine]") {
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 499990, 100)));

  SECTION("duplicate against a live order") {
    require_reject(eng.on_new(kSessA, mk_new("c1", "B", 499985, 100)), mm::RejectCode::DupClOrdId,
                   "c1");
    CHECK(eng.live_count(kSessA) == 1); // state unchanged
  }
  SECTION("duplicate against a TERMINAL order (invariant 4: no resurrection)") {
    require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("c1")));
    require_reject(eng.on_new(kSessA, mk_new("c1", "B", 499985, 100)), mm::RejectCode::DupClOrdId,
                   "c1");
    CHECK(eng.live_count(kSessA) == 0);
  }
  SECTION("a rejected duplicate burns no eng_id") {
    require_reject(eng.on_new(kSessA, mk_new("c1", "B", 499985, 100)), mm::RejectCode::DupClOrdId,
                   "c1");
    const auto ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c2", "B", 499985, 100)));
    CHECK(ack.eng_id == 2);
  }
}

TEST_CASE("engine: validation rejects create no order", "[engine]") {
  auto eng = primed_engine();

  SECTION("off-tick px -> TickSize; the reject leaves a Rejected tombstone (invariant 4)") {
    require_reject(eng.on_new(kSessA, mk_new("c1", "B", 499991, 100)), mm::RejectCode::TickSize,
                   "c1");
    CHECK(eng.live_count(kSessA) == 0); // no LIVE order was created
    // The cl_id is consumed for the session's lifetime: a retry of the rejected cl_id
    // — even corrected — is DupClOrdId, and a cancel of it is AlreadyTerminal (the
    // tombstone is a terminal order, invariant 4 uniform).
    require_reject(eng.on_new(kSessA, mk_new("c1", "B", 499990, 100)), mm::RejectCode::DupClOrdId,
                   "c1");
    require_reject(eng.on_cancel(kSessA, mk_cancel("c1")), mm::RejectCode::AlreadyTerminal, "c1");
    // The tombstone burned no eng_id: the first ACCEPTED order still gets eng_id 1.
    const auto ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c2", "B", 499990, 100)));
    CHECK(ack.eng_id == 1);
  }
  SECTION("QtyLimit reasons distinguish non-positive from over-max (types.hpp contract)") {
    const auto rej_zero =
        require_only<mm::Reject>(eng.on_new(kSessA, mk_new("c1", "B", 499990, 0)));
    CHECK(rej_zero.cl_id == "c1"); // the reject correlates back to its order
    CHECK(rej_zero.code == mm::to_string(mm::RejectCode::QtyLimit));
    CHECK(rej_zero.reason == "qty must be positive");
    const auto rej_max =
        require_only<mm::Reject>(eng.on_new(kSessA, mk_new("c2", "B", 499990, 20000)));
    CHECK(rej_max.cl_id == "c2");
    CHECK(rej_max.code == mm::to_string(mm::RejectCode::QtyLimit));
    CHECK(rej_max.reason == "qty exceeds max_order_qty");
  }
}

TEST_CASE("engine: post-only cross is rejected at entry", "[engine]") {
  auto eng = primed_engine();

  SECTION("bid at/through the ask -> PostOnlyCross") {
    require_reject(eng.on_new(kSessA, mk_new("c1", "B", 500010, 100)),
                   mm::RejectCode::PostOnlyCross, "c1");
    require_reject(eng.on_new(kSessA, mk_new("c2", "B", 500015, 100)),
                   mm::RejectCode::PostOnlyCross, "c2");
    CHECK(eng.live_count(kSessA) == 0);
  }
  SECTION("ask at/through the bid -> PostOnlyCross") {
    require_reject(eng.on_new(kSessA, mk_new("c1", "S", 500000, 100)),
                   mm::RejectCode::PostOnlyCross, "c1");
    require_reject(eng.on_new(kSessA, mk_new("c2", "S", 499995, 100)),
                   mm::RejectCode::PostOnlyCross, "c2");
  }
  SECTION("joining the touch does not cross") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 100)));
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c2", "S", 500010, 100)));
  }
  SECTION("an empty opposite side cannot cross") {
    mm::OrderEngine fresh{spec_instrument()}; // book all zeros: no ask side exists
    require_only<mm::OrderAck>(fresh.on_new(kSessA, mk_new("c1", "B", 500010, 100)));
  }
  SECTION("post_only=false may cross at entry and fills on the NEXT book evaluation") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500010, 100, false)));
    CHECK(eng.live_count(kSessA) == 1);
    const auto [fill_sess, fill] =
        require_only<mm::Fill>(eng.on_book(base_book(2))); // unchanged book, next update
    CHECK(fill.px == 500010); // the order's own limit (maker price semantics)
    CHECK(fill.qty == 80);    // min(leaves=100, ask_qty=80)
    CHECK(fill.leaves == 20);
  }
}

TEST_CASE("engine: max_live_orders is PER SESSION (R-1)", "[engine]") {
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a1", "B", 499990, 100)));
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("a2", "B", 499985, 100)));

  require_reject(eng.on_new(kSessA, mk_new("a3", "B", 499980, 100)), mm::RejectCode::MaxLiveOrders,
                 "a3");
  CHECK(eng.live_count(kSessA) == 2);

  // A valid order on session B while A holds two: limits are independent per connection
  // (the cross-session test depends on this).
  require_only<mm::OrderAck>(eng.on_new(kSessB, mk_new("b1", "B", 499990, 100)));
  CHECK(eng.live_count(kSessB) == 1);
}

TEST_CASE("engine: the max_live cap is SHARED across the two sides", "[engine]") {
  // One bid plus one ask exhaust a cap of 2: the limit counts a SESSION's live orders, not a
  // side's (live_count sums both live indexes). The bid-only cases above cannot tell the two
  // readings apart, and a per-side cap would quietly let a session hold 2x max_live_orders —
  // the testing-spec quote loop holds exactly one of each, so it would never notice either.
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b1", "B", 499990, 100)));
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("s1", "S", 500020, 100)));
  CHECK(eng.live_count(kSessA) == 2);

  require_reject(eng.on_new(kSessA, mk_new("b2", "B", 499985, 100)), mm::RejectCode::MaxLiveOrders,
                 "b2");
  require_reject(eng.on_new(kSessA, mk_new("s2", "S", 500030, 100)), mm::RejectCode::MaxLiveOrders,
                 "s2");
  // Cancelling the ASK frees the shared slot a BID can then take.
  require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("s1")));
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("b3", "B", 499985, 100)));
  CHECK(eng.live_count(kSessA) == 2);
}

TEST_CASE("engine: a cancel frees a max_live slot — the testing-spec re-quote transition",
          "[engine]") {
  // The MM loop is quote -> cancel -> re-quote: a cancel MUST free its max_live_orders
  // slot or the whole seven-step demo stalls after two orders.
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("q1", "B", 499990, 100)));
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("q2", "B", 499985, 100)));
  require_reject(eng.on_new(kSessA, mk_new("q3", "B", 499980, 100)), mm::RejectCode::MaxLiveOrders,
                 "q3");

  require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("q1")));
  CHECK(eng.live_count(kSessA) == 1);
  // The MaxLiveOrders reject left a tombstone (invariant 4): even with a slot now
  // free, retrying q3 is DupClOrdId — the replacement quote needs a fresh cl_id.
  require_reject(eng.on_new(kSessA, mk_new("q3", "B", 499980, 100)), mm::RejectCode::DupClOrdId,
                 "q3");
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("q4", "B", 499990, 100)));
  CHECK(eng.live_count(kSessA) == 2);
}

TEST_CASE("engine: on_new check-order precedence is pinned pairwise", "[engine]") {
  // Each row makes TWO checks fire on the same command and asserts which wins —
  // reordering the documented duplicate -> validate -> max-live -> post-only chain
  // (engine.hpp contract) would flip a client-visible reject code. Probe cl_id is "x";
  // px 499991/500011 are off-tick (tick 5), 500010/500011 cross the base_book ask.
  struct Row {
    const char *label;
    bool consume_cl_id; // pre-consume "x" so the probe is a duplicate
    bool fill_to_max;   // occupy both max_live_orders slots first
    std::int64_t px;
    mm::RejectCode expect;
  };
  const Row rows[] = {
      {.label = "duplicate beats validation (off-tick px)",
       .consume_cl_id = true,
       .fill_to_max = false,
       .px = 499991,
       .expect = mm::RejectCode::DupClOrdId},
      {.label = "validation beats max-live (off-tick px at the cap)",
       .consume_cl_id = false,
       .fill_to_max = true,
       .px = 499991,
       .expect = mm::RejectCode::TickSize},
      {.label = "max-live beats post-only (crossing on-tick px at the cap)",
       .consume_cl_id = false,
       .fill_to_max = true,
       .px = 500010,
       .expect = mm::RejectCode::MaxLiveOrders},
      {.label = "validation beats post-only (off-tick crossing px)",
       .consume_cl_id = false,
       .fill_to_max = false,
       .px = 500011,
       .expect = mm::RejectCode::TickSize},
  };
  for (const Row &row : rows) {
    INFO(row.label);
    auto eng = primed_engine();
    if (row.consume_cl_id)
      require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("x", "B", 499990, 100)));
    if (row.fill_to_max) {
      require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("f1", "B", 499990, 100)));
      require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("f2", "B", 499985, 100)));
    }
    require_reject(eng.on_new(kSessA, mk_new("x", "B", row.px, 100)), row.expect, "x");
  }
}

TEST_CASE("engine: cancel transitions", "[engine]") {
  auto eng = primed_engine();

  SECTION("cancel live -> CancelAck, terminal, live_count drops") {
    const auto ack = require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 499990, 100)));
    const auto cxl = require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("c1")));
    CHECK(cxl.cl_id == "c1");
    CHECK(cxl.eng_id == ack.eng_id);
    CHECK(cxl.status == "cancelled");
    CHECK(eng.live_count(kSessA) == 0);
    // Terminal is absorbing (invariant 1): a second cancel is AlreadyTerminal.
    require_reject(eng.on_cancel(kSessA, mk_cancel("c1")), mm::RejectCode::AlreadyTerminal, "c1");
  }
  SECTION("cancel unknown -> UnknownClOrdId") {
    require_reject(eng.on_cancel(kSessA, mk_cancel("nope")), mm::RejectCode::UnknownClOrdId,
                   "nope");
  }
  SECTION("cancel filled -> AlreadyTerminal (invariant 3: cancel/fill race, engine side)") {
    require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 500000, 50)));
    REQUIRE(eng.on_book(mk_book(499990, 100, 499995, 80, 2)).size() == 1); // fills c1
    require_reject(eng.on_cancel(kSessA, mk_cancel("c1")), mm::RejectCode::AlreadyTerminal, "c1");
    CHECK(eng.live_count(kSessA) == 0);
  }
}

TEST_CASE("engine: constructor rejects a nonsensical configuration", "[engine]") {
  // The sweep and entry conversion divide/multiply by tick/lot; an invalid Instrument
  // must fail loudly at construction, never reach signed-division UB.
  mm::Instrument bad_lot = spec_instrument();
  bad_lot.lot_size = 0;
  CHECK_THROWS_AS(mm::OrderEngine{bad_lot}, std::invalid_argument);
  mm::Instrument neg_lot = spec_instrument();
  neg_lot.lot_size = -1;
  CHECK_THROWS_AS(mm::OrderEngine{neg_lot}, std::invalid_argument);
  mm::Instrument bad_tick = spec_instrument();
  bad_tick.tick_size = 0;
  CHECK_THROWS_AS(mm::OrderEngine{bad_tick}, std::invalid_argument);
  // Same fail-loudly policy: a negative max_live_orders would silently reject every
  // order, and max_session_entries == 0 would flag entry_cap_breached for a session
  // that has never sent anything (an instant 1008 close under the obligation,
  // the limitations backlog).
  mm::Instrument neg_live = spec_instrument();
  neg_live.max_live_orders = -3;
  CHECK_THROWS_AS(mm::OrderEngine{neg_live}, std::invalid_argument);
  // Same silently-reject-every-order policy: max_order_qty <= 0 makes every order
  // QtyLimit (types.hpp validate_order folds qty <= 0 and qty > max), and
  // min_price > max_price puts every px outside the range -> PriceRange.
  mm::Instrument bad_qty = spec_instrument();
  bad_qty.max_order_qty = 0;
  CHECK_THROWS_AS(mm::OrderEngine{bad_qty}, std::invalid_argument);
  mm::Instrument inverted_range = spec_instrument();
  inverted_range.min_price = inverted_range.max_price + 1;
  CHECK_THROWS_AS(mm::OrderEngine{inverted_range}, std::invalid_argument);
  CHECK_THROWS_AS((mm::OrderEngine{spec_instrument(), /*max_session_entries=*/0}),
                  std::invalid_argument);
}

TEST_CASE("engine: on_new walks the full safety-control validation reject matrix", "[engine]") {
  // Engine-path coverage of every validate_order code (types.hpp covers the function
  // directly, this pins the on_new wiring).
  auto eng = primed_engine();
  struct Row {
    const char *label;
    const char *cl_id;
    const char *symbol;
    const char *side;
    std::int64_t px, qty;
    mm::RejectCode code;
    const char *reason;
  };
  const Row rows[] = {
      {.label = "unknown symbol",
       .cl_id = "r1",
       .symbol = "OTHER",
       .side = "B",
       .px = 499990,
       .qty = 100,
       .code = mm::RejectCode::UnknownSymbol,
       .reason = "unknown symbol"},
      {.label = "bad side",
       .cl_id = "r2",
       .symbol = "MOCKUSDT",
       .side = "X",
       .px = 499990,
       .qty = 100,
       .code = mm::RejectCode::BadSide,
       .reason = "side must be \"B\" or \"S\""},
      {.label = "off-tick px",
       .cl_id = "r3",
       .symbol = "MOCKUSDT",
       .side = "B",
       .px = 499991,
       .qty = 100,
       .code = mm::RejectCode::TickSize,
       .reason = "px is not a multiple of tick_size"},
      {.label = "off-lot qty",
       .cl_id = "r4",
       .symbol = "MOCKUSDT",
       .side = "B",
       .px = 499990,
       .qty = 15,
       .code = mm::RejectCode::LotSize,
       .reason = "qty is not a multiple of lot_size"},
      {.label = "non-positive qty",
       .cl_id = "r5",
       .symbol = "MOCKUSDT",
       .side = "B",
       .px = 499990,
       .qty = 0,
       .code = mm::RejectCode::QtyLimit,
       .reason = "qty must be positive"},
      {.label = "over-max qty",
       .cl_id = "r6",
       .symbol = "MOCKUSDT",
       .side = "B",
       .px = 499990,
       .qty = 20000,
       .code = mm::RejectCode::QtyLimit,
       .reason = "qty exceeds max_order_qty"},
      {.label = "out-of-range px",
       .cl_id = "r7",
       .symbol = "MOCKUSDT",
       .side = "B",
       .px = 15'000'000,
       .qty = 100,
       .code = mm::RejectCode::PriceRange,
       .reason = "px outside [min_price, max_price]"},
  };
  for (const Row &row : rows) {
    INFO(row.label);
    mm::NewOrder order = mk_new(row.cl_id, row.side, row.px, row.qty);
    order.symbol = row.symbol;
    const auto rej = require_only<mm::Reject>(eng.on_new(kSessA, order));
    CHECK(rej.cl_id == row.cl_id); // the client's correlation key on every reject path
    CHECK(rej.code == mm::to_string(row.code));
    // Reason pinned row-by-row: validation_reason carries NO default label, so
    // -Wswitch guards an appended enumerator at compile time, and these assertions
    // catch a silent degradation of the client-visible reason to the raw wire
    // spelling at run time regardless.
    CHECK(rej.reason == row.reason);
  }
  CHECK(eng.live_count(kSessA) == 0);
  // Each reject left a tombstone: any retry of a consumed cl_id is DupClOrdId.
  require_reject(eng.on_new(kSessA, mk_new("r4", "B", 499990, 100)), mm::RejectCode::DupClOrdId,
                 "r4");
}

TEST_CASE("engine: the five engine-originated rejects pin their reason too", "[engine]") {
  // Mirror of the validation matrix above, for the codes on_new/on_cancel raise
  // THEMSELVES rather than via validate_order. Same rationale, which applies verbatim here:
  // Reject.reason is the client's only human-readable explanation, and asserting the code
  // alone leaves it free to degrade silently (blanking any of the five reasons was a mutant
  // the suite could not see). The strings are additionally load-bearing for the allocation
  // inventory, which classifies each reason by small-string fit.
  auto eng = primed_engine();
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c1", "B", 499990, 100)));
  require_reject(eng.on_new(kSessA, mk_new("c1", "B", 499985, 100)), mm::RejectCode::DupClOrdId,
                 "c1", "cl_id already used on this session");
  require_only<mm::OrderAck>(eng.on_new(kSessA, mk_new("c2", "B", 499985, 100)));
  require_reject(eng.on_new(kSessA, mk_new("c3", "B", 499980, 100)), mm::RejectCode::MaxLiveOrders,
                 "c3", "session already holds max_live_orders live orders");
  require_reject(eng.on_new(kSessB, mk_new("x", "B", 500010, 100)), mm::RejectCode::PostOnlyCross,
                 "x", "post-only order would cross the book");
  require_reject(eng.on_cancel(kSessA, mk_cancel("nope")), mm::RejectCode::UnknownClOrdId, "nope",
                 "unknown cl_id on this session");
  require_only<mm::CancelAck>(eng.on_cancel(kSessA, mk_cancel("c1")));
  require_reject(eng.on_cancel(kSessA, mk_cancel("c1")), mm::RejectCode::AlreadyTerminal, "c1",
                 "order is already terminal");
}

TEST_CASE("engine: determinism — same commands, byte-identical reports", "[engine]") {
  // The acceptance list names five conditions the state must stay deterministic under: rejects,
  // fills, cancels, STALE DATA and disconnect. The script drives all five — the replayed and
  // rewound books below are the stale-data arm, and this task is the one that introduced
  // engine behavior for it (the md_seq guard and its counter).
  struct Run {
    std::string bytes;
    std::size_t messages{};
    std::size_t fills{};
    std::uint64_t stale{};
  };
  const auto run_script = [] {
    Run run;
    mm::OrderEngine eng{spec_instrument()};
    const auto codec = mm::make_codec(mm::CodecKind::Tuned);
    std::string buf;
    const auto emit = [&](const mm::OutMsg &msg) {
      codec->encode(msg, buf);
      run.bytes += buf;
      run.bytes += '\n';
      ++run.messages;
      run.fills += std::holds_alternative<mm::Fill>(msg) ? 1u : 0u;
    };
    for (const auto &routed : eng.on_book(base_book()))
      emit(routed.msg);
    for (const auto &msg : eng.on_new(kSessA, mk_new("c1", "B", 500000, 100)))
      emit(msg);
    for (const auto &msg : eng.on_new(kSessA, mk_new("c2", "S", 500010, 50)))
      emit(msg);
    for (const auto &msg : eng.on_new(kSessB, mk_new("c1", "B", 499995, 100)))
      emit(msg);
    for (const auto &msg : eng.on_new(kSessA, mk_new("c1", "B", 499990, 100)))
      emit(msg); // DupClOrdId
    for (const auto &routed : eng.on_book(mk_book(499990, 100, 499995, 40, 2)))
      emit(routed.msg); // partial fill of c1@A, fills c1@B
    // STALE DATA arm: a redelivered md_seq 2 and a rewind to md_seq 1. Both must emit
    // nothing, leave `leaves` untouched and count one drop each — so the byte-pin covers a
    // condition the acceptance list names and the counter it feeds.
    for (const auto &routed : eng.on_book(mk_book(499990, 100, 499995, 40, 2)))
      emit(routed.msg);
    for (const auto &routed : eng.on_book(base_book(1)))
      emit(routed.msg);
    for (const auto &msg : eng.on_cancel(kSessA, mk_cancel("c2")))
      emit(msg);
    for (const auto &routed : eng.on_book(mk_book(499990, 100, 499990, 200, 3)))
      emit(routed.msg); // completes c1@A, fills c1@B
    for (const auto &msg : eng.end_session(kSessB))
      emit(msg);
    run.stale = eng.stale_books_ignored();
    return run;
  };
  const Run first = run_script();
  const Run second = run_script();
  CHECK(first.bytes == second.bytes);
  CHECK_FALSE(first.bytes.empty());
  // Byte-identity alone cannot tell "identical because deterministic" from "identical
  // because the interesting reports stopped being produced" — a mutant that never crosses
  // makes both runs empty and still passes. Pin the SHAPE of the stream as well: 3 acks +
  // 1 DupClOrdId reject + 1 CancelAck + 4 fills, with 2 stale books dropped.
  CHECK(first.messages == 9);
  CHECK(first.fills == 4);
  CHECK(first.stale == 2);
  CHECK(second.messages == first.messages);
  CHECK(second.fills == first.fills);
  CHECK(second.stale == first.stale);
}
