// Unit tests for SIMD-Vectorized Pricing Engine (ARM NEON & AVX2).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "mm/simd_pricing.hpp"

using namespace mm;
using Catch::Matchers::WithinRel;

TEST_CASE("simd_pricing: mathematical accuracy of microprice and OBI", "[simd][pricing]") {
  BookDepth4 depth{};
  // 4 Bid levels: 500000 @ 10, 499990 @ 20, 499980 @ 30, 499970 @ 40 (Total Bid Qty = 100)
  depth.bid_px[0] = 500000.0;
  depth.bid_qty[0] = 10.0;
  depth.bid_px[1] = 499990.0;
  depth.bid_qty[1] = 20.0;
  depth.bid_px[2] = 499980.0;
  depth.bid_qty[2] = 30.0;
  depth.bid_px[3] = 499970.0;
  depth.bid_qty[3] = 40.0;

  // 4 Ask levels: 500010 @ 5, 500020 @ 15, 500030 @ 25, 500040 @ 35 (Total Ask Qty = 80)
  depth.ask_px[0] = 500010.0;
  depth.ask_qty[0] = 5.0;
  depth.ask_px[1] = 500020.0;
  depth.ask_qty[1] = 15.0;
  depth.ask_px[2] = 500030.0;
  depth.ask_qty[2] = 25.0;
  depth.ask_px[3] = 500040.0;
  depth.ask_qty[3] = 35.0;

  auto res = SimdPricingEngine::compute(depth, /*base_half_spread=*/10.0, /*position=*/0);

  // Total Bid Qty = 100, Total Ask Qty = 80 -> Total Qty = 180
  // OBI = (100 - 80) / 180 = 20 / 180 = +0.111111...
  CHECK_THAT(res.order_book_imbalance, WithinRel(20.0 / 180.0, 1e-5));

  // VWAP Bid: (500000*10 + 499990*20 + 499980*30 + 499970*40) / 100 = 499980.0
  CHECK_THAT(res.vwap_bid, WithinRel(499980.0, 1e-5));

  // VWAP Ask: (500010*5 + 500020*15 + 500030*25 + 500040*35) / 80 = 40002500 / 80 = 500031.25
  CHECK_THAT(res.vwap_ask, WithinRel(500031.25, 1e-5));

  // Micro-price should be between top bid and top ask
  CHECK(res.micro_price > depth.bid_px[0]);
  CHECK(res.micro_price < depth.ask_px[0]);

  // Target Bid < Target Ask
  CHECK(res.target_bid < res.target_ask);
}

TEST_CASE("simd_pricing: inventory risk skewing", "[simd][pricing]") {
  BookDepth4 depth{};
  depth.bid_px[0] = 100.0;
  depth.bid_qty[0] = 10.0;
  depth.ask_px[0] = 102.0;
  depth.ask_qty[0] = 10.0;

  auto neutral = SimdPricingEngine::compute(depth, 1.0, /*position=*/0);
  auto long_pos = SimdPricingEngine::compute(depth, 1.0, /*position=*/+10, /*gamma=*/0.05);
  auto short_pos = SimdPricingEngine::compute(depth, 1.0, /*position=*/-10, /*gamma=*/0.05);

  // When long, quotes should skew lower to attract sellers / offload inventory
  CHECK(long_pos.target_bid < neutral.target_bid);
  CHECK(long_pos.target_ask < neutral.target_ask);

  // When short, quotes should skew higher to attract buyers / cover inventory
  CHECK(short_pos.target_bid > neutral.target_bid);
  CHECK(short_pos.target_ask > neutral.target_ask);
}

TEST_CASE("simd_pricing: zero quantity fallback", "[simd][pricing]") {
  BookDepth4 empty_depth{};
  empty_depth.bid_px[0] = 50.0;
  empty_depth.ask_px[0] = 52.0;

  auto res = SimdPricingEngine::compute(empty_depth, 1.0);
  CHECK_THAT(res.micro_price, WithinRel(51.0, 1e-5));
  CHECK(res.order_book_imbalance == 0.0);
}
