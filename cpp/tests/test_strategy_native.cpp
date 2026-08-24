// Unit tests for NativeMarketMaker and Thread Affinity (C++20).
#include <catch2/catch_test_macros.hpp>

#include "mm/affinity.hpp"
#include "mm/strategy_native.hpp"

using namespace mm;

TEST_CASE("affinity: cpu pinning and page locking", "[affinity][ull]") {
  // Pinning to CPU core 0 (or affinity tag on macOS)
  bool affinity_set = set_thread_affinity(0);
  CHECK(affinity_set);

  // Lock and unlock memory pages
  bool locked = lock_memory_pages();
  CHECK(locked);

  bool unlocked = unlock_memory_pages();
  CHECK(unlocked);
}

TEST_CASE("strategy_native: basic quoting and fills", "[strategy][ull]") {
  StrategyConfig config{};
  config.order_size = 10;
  config.max_position = 50;
  config.half_spread = 10;

  NativeMarketMaker mm(config);

  CHECK(mm.position() == 0);
  CHECK(mm.active_bid_cl_id() == 0);
  CHECK(mm.active_ask_cl_id() == 0);

  // Incoming TOB: 500000 @ 500020 (mid = 500010)
  ShmTopOfBook tob{};
  tob.tag = static_cast<std::uint16_t>(ShmMsgType::TopOfBook);
  tob.md_seq = 1;
  tob.bid_px = 500000;
  tob.bid_qty = 10;
  tob.ask_px = 500020;
  tob.ask_qty = 10;
  tob.venue_ns = 1'000'000'000LL;

  auto decision = mm.on_tob(tob);

  CHECK(decision.has_bid_order == 1);
  CHECK(decision.new_bid.px == 500000); // 500010 - 10
  CHECK(decision.new_bid.qty == 10);
  CHECK(decision.new_bid.side == 1);

  CHECK(decision.has_ask_order == 1);
  CHECK(decision.new_ask.px == 500020); // 500010 + 10
  CHECK(decision.new_ask.qty == 10);
  CHECK(decision.new_ask.side == 2);

  const auto bid_cl_id = decision.new_bid.cl_id;
  const auto ask_cl_id = decision.new_ask.cl_id;

  // Process Ack
  ShmOrderAck ack{};
  ack.cl_id = bid_cl_id;
  mm.on_ack(ack);
  CHECK(mm.ack_count() == 1);

  // Process Bid Fill (we bought 10 @ 500000)
  ShmOrderFill fill{};
  fill.cl_id = bid_cl_id;
  fill.fill_px = 500000;
  fill.fill_qty = 10;
  mm.on_fill(fill);

  CHECK(mm.fill_count() == 1);
  CHECK(mm.position() == 10);
  CHECK(mm.realized_pnl() == -5'000'000LL);
  CHECK(mm.active_bid_cl_id() == 0);         // Bid was filled, slot freed
  CHECK(mm.active_ask_cl_id() == ask_cl_id); // Ask still live

  // Market moves up: 500040 @ 500060 (mid = 500050)
  tob.md_seq = 2;
  tob.bid_px = 500040;
  tob.ask_px = 500060;
  tob.venue_ns = 1'000'100'000LL;

  auto move_decision = mm.on_tob(tob);

  // Position is +10, so skew is +2:
  // mid = 500050, target_bid = 500050 - 10 - 2 = 500038
  // target_ask = 500050 + 10 - 2 = 500058
  CHECK(move_decision.has_ask_cancel == 1); // Cancel old ask @ 500020
  CHECK(move_decision.has_ask_order == 1);  // New ask @ 500058
  CHECK(move_decision.new_ask.px == 500058);
  CHECK(move_decision.has_bid_order == 1); // New bid @ 500038
  CHECK(move_decision.new_bid.px == 500038);
}

TEST_CASE("strategy_native: risk limits and cancel all", "[strategy][ull]") {
  StrategyConfig config{};
  config.order_size = 10;
  config.max_position = 20;
  config.half_spread = 5;

  NativeMarketMaker mm(config);

  ShmTopOfBook tob{};
  tob.bid_px = 100000;
  tob.ask_px = 100010;
  tob.venue_ns = 1000;

  auto d1 = mm.on_tob(tob);
  auto bid1 = d1.new_bid.cl_id;

  // Fill bid twice to reach max position
  ShmOrderFill f1{.cl_id = bid1, .fill_px = 100000, .fill_qty = 20};
  mm.on_fill(f1);
  CHECK(mm.position() == 20); // Reached max long

  // Next TOB should NOT post bid (at max position limit)
  tob.md_seq = 2;
  tob.venue_ns = 2000;
  auto d2 = mm.on_tob(tob);
  CHECK(d2.has_bid_order == 0);

  // Stale check
  CHECK_FALSE(mm.is_stale(2000));
  CHECK(mm.is_stale(2000 + 6'000'000'000LL));

  // Inverted market triggers cancel_all
  tob.bid_px = 100020;
  tob.ask_px = 100010; // Inverted!
  auto inv_d = mm.on_tob(tob);
  CHECK(inv_d.has_ask_cancel == 1);
}

TEST_CASE("strategy_native: SIMD multi-level depth quoting", "[strategy][ull][simd]") {
  StrategyConfig config{};
  config.order_size = 10;
  config.max_position = 50;
  config.half_spread = 10;

  NativeMarketMaker mm(config);

  BookDepth4 depth{};
  depth.bid_px[0] = 500000.0;
  depth.bid_qty[0] = 10.0;
  depth.bid_px[1] = 499990.0;
  depth.bid_qty[1] = 20.0;
  depth.bid_px[2] = 499980.0;
  depth.bid_qty[2] = 30.0;
  depth.bid_px[3] = 499970.0;
  depth.bid_qty[3] = 40.0;

  depth.ask_px[0] = 500020.0;
  depth.ask_qty[0] = 10.0;
  depth.ask_px[1] = 500030.0;
  depth.ask_qty[1] = 20.0;
  depth.ask_px[2] = 500040.0;
  depth.ask_qty[2] = 30.0;
  depth.ask_px[3] = 500050.0;
  depth.ask_qty[3] = 40.0;

  auto decision = mm.on_depth(depth, 1, 1'000'000'000LL);

  CHECK(decision.has_bid_order == 1);
  CHECK(decision.has_ask_order == 1);
  CHECK(decision.new_bid.px < decision.new_ask.px);
  CHECK(decision.new_bid.px <= 500000);
  CHECK(decision.new_ask.px >= 500020);
}
