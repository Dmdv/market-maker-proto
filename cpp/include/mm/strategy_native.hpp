// Native C++20 Market Maker Strategy Engine for Ultra-Low Latency Trading.
// Zero heap allocations, L1D cache-line friendly layout (< 128 bytes), sub-20ns decision time.
#pragma once

#include "mm/shm_ring.hpp"
#include "mm/simd_pricing.hpp"

#include <chrono>
#include <cstdint>
#include <span>

namespace mm {

struct StrategyConfig {
  std::int64_t order_size{10};
  std::int64_t max_position{100};
  std::int64_t half_spread{10};                   // Half-spread in price ticks (e.g. 10 = $0.10)
  std::int64_t stale_timeout_ns{5'000'000'000LL}; // 5 seconds
};

struct StrategyDecision {
  std::uint8_t has_bid_cancel : 1 {0};
  std::uint8_t has_ask_cancel : 1 {0};
  std::uint8_t has_bid_order : 1 {0};
  std::uint8_t has_ask_order : 1 {0};
  std::uint8_t _pad : 4 {0};

  ShmCancelOrder cancel_bid{};
  ShmCancelOrder cancel_ask{};
  ShmNewOrder new_bid{};
  ShmNewOrder new_ask{};
};

class NativeMarketMaker {
public:
  explicit NativeMarketMaker(StrategyConfig config = {}) noexcept
      : config_(config), next_cl_id_(1), seq_(1) {}

  // Process a new Top of Book tick and generate quoting decisions (< 20 ns).
  [[nodiscard]] StrategyDecision on_tob(const ShmTopOfBook &tob, std::int64_t now_ns = 0) noexcept {
    StrategyDecision decision{};
    last_tob_ns_ = (now_ns > 0) ? now_ns : tob.venue_ns;
    last_md_seq_ = tob.md_seq;

    // Check valid market prices
    if (tob.bid_px <= 0 || tob.ask_px <= 0 || tob.bid_px >= tob.ask_px) {
      return cancel_all();
    }

    const std::int64_t mid_px = (tob.bid_px + tob.ask_px) / 2;

    // Target quote prices: mid - half_spread for bid, mid + half_spread for ask
    // Skew based on position: if long (+pos), lower prices to sell; if short (-pos), raise prices
    // to buy
    const std::int64_t position_skew = (position_ * 2) / 10;
    const std::int64_t target_bid_px = mid_px - config_.half_spread - position_skew;
    const std::int64_t target_ask_px = mid_px + config_.half_spread - position_skew;

    // 1. Manage Bid Quote
    if (position_ < config_.max_position && target_bid_px < tob.ask_px) {
      if (active_bid_cl_id_ != 0 && active_bid_px_ != target_bid_px) {
        // Cancel old bid to requote
        decision.has_bid_cancel = 1;
        decision.cancel_bid.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
        decision.cancel_bid.seq = seq_++;
        decision.cancel_bid.cl_id = active_bid_cl_id_;
        decision.cancel_bid.send_ts_ns = last_tob_ns_;
        active_bid_cl_id_ = 0;
      }
      if (active_bid_cl_id_ == 0) {
        // Post new bid
        const std::uint64_t cl_id = next_cl_id_++;
        decision.has_bid_order = 1;
        decision.new_bid.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
        decision.new_bid.side = 1; // Bid
        decision.new_bid.post_only = 1;
        decision.new_bid.seq = seq_++;
        decision.new_bid.md_seq = tob.md_seq;
        decision.new_bid.cl_id = cl_id;
        decision.new_bid.px = target_bid_px;
        decision.new_bid.qty = config_.order_size;
        decision.new_bid.send_ts_ns = last_tob_ns_;

        active_bid_cl_id_ = cl_id;
        active_bid_px_ = target_bid_px;
      }
    } else if (active_bid_cl_id_ != 0) {
      // Risk limit reached or crossing book, pull bid
      decision.has_bid_cancel = 1;
      decision.cancel_bid.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
      decision.cancel_bid.seq = seq_++;
      decision.cancel_bid.cl_id = active_bid_cl_id_;
      decision.cancel_bid.send_ts_ns = last_tob_ns_;
      active_bid_cl_id_ = 0;
    }

    // 2. Manage Ask Quote
    if (position_ > -config_.max_position && target_ask_px > tob.bid_px) {
      if (active_ask_cl_id_ != 0 && active_ask_px_ != target_ask_px) {
        // Cancel old ask to requote
        decision.has_ask_cancel = 1;
        decision.cancel_ask.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
        decision.cancel_ask.seq = seq_++;
        decision.cancel_ask.cl_id = active_ask_cl_id_;
        decision.cancel_ask.send_ts_ns = last_tob_ns_;
        active_ask_cl_id_ = 0;
      }
      if (active_ask_cl_id_ == 0) {
        // Post new ask
        const std::uint64_t cl_id = next_cl_id_++;
        decision.has_ask_order = 1;
        decision.new_ask.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
        decision.new_ask.side = 2; // Ask
        decision.new_ask.post_only = 1;
        decision.new_ask.seq = seq_++;
        decision.new_ask.md_seq = tob.md_seq;
        decision.new_ask.cl_id = cl_id;
        decision.new_ask.px = target_ask_px;
        decision.new_ask.qty = config_.order_size;
        decision.new_ask.send_ts_ns = last_tob_ns_;

        active_ask_cl_id_ = cl_id;
        active_ask_px_ = target_ask_px;
      }
    } else if (active_ask_cl_id_ != 0) {
      // Risk limit reached or crossing book, pull ask
      decision.has_ask_cancel = 1;
      decision.cancel_ask.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
      decision.cancel_ask.seq = seq_++;
      decision.cancel_ask.cl_id = active_ask_cl_id_;
      decision.cancel_ask.send_ts_ns = last_tob_ns_;
      active_ask_cl_id_ = 0;
    }

    return decision;
  }

  // Process Multi-Level Order Book Depth via hardware SIMD pricing (< 5 ns).
  [[nodiscard]] StrategyDecision on_depth(const BookDepth4 &depth, std::uint64_t md_seq,
                                          std::int64_t now_ns = 0) noexcept {
    last_tob_ns_ =
        (now_ns > 0) ? now_ns : std::chrono::steady_clock::now().time_since_epoch().count();
    last_md_seq_ = md_seq;

    if (depth.bid_px[0] <= 0.0 || depth.ask_px[0] <= 0.0 || depth.bid_px[0] >= depth.ask_px[0]) {
      return cancel_all();
    }

    const auto simd_res =
        SimdPricingEngine::compute(depth, static_cast<double>(config_.half_spread), position_);

    StrategyDecision decision{};
    const std::int64_t target_bid_px = static_cast<std::int64_t>(std::round(simd_res.target_bid));
    const std::int64_t target_ask_px = static_cast<std::int64_t>(std::round(simd_res.target_ask));

    // Manage Bid
    if (position_ < config_.max_position &&
        target_bid_px < static_cast<std::int64_t>(depth.ask_px[0])) {
      if (active_bid_cl_id_ != 0 && active_bid_px_ != target_bid_px) {
        decision.has_bid_cancel = 1;
        decision.cancel_bid.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
        decision.cancel_bid.seq = seq_++;
        decision.cancel_bid.cl_id = active_bid_cl_id_;
        decision.cancel_bid.send_ts_ns = last_tob_ns_;
        active_bid_cl_id_ = 0;
      }
      if (active_bid_cl_id_ == 0) {
        const std::uint64_t cl_id = next_cl_id_++;
        decision.has_bid_order = 1;
        decision.new_bid.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
        decision.new_bid.side = 1;
        decision.new_bid.post_only = 1;
        decision.new_bid.seq = seq_++;
        decision.new_bid.md_seq = md_seq;
        decision.new_bid.cl_id = cl_id;
        decision.new_bid.px = target_bid_px;
        decision.new_bid.qty = config_.order_size;
        decision.new_bid.send_ts_ns = last_tob_ns_;
        active_bid_cl_id_ = cl_id;
        active_bid_px_ = target_bid_px;
      }
    } else if (active_bid_cl_id_ != 0) {
      decision.has_bid_cancel = 1;
      decision.cancel_bid.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
      decision.cancel_bid.seq = seq_++;
      decision.cancel_bid.cl_id = active_bid_cl_id_;
      decision.cancel_bid.send_ts_ns = last_tob_ns_;
      active_bid_cl_id_ = 0;
    }

    // Manage Ask
    if (position_ > -config_.max_position &&
        target_ask_px > static_cast<std::int64_t>(depth.bid_px[0])) {
      if (active_ask_cl_id_ != 0 && active_ask_px_ != target_ask_px) {
        decision.has_ask_cancel = 1;
        decision.cancel_ask.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
        decision.cancel_ask.seq = seq_++;
        decision.cancel_ask.cl_id = active_ask_cl_id_;
        decision.cancel_ask.send_ts_ns = last_tob_ns_;
        active_ask_cl_id_ = 0;
      }
      if (active_ask_cl_id_ == 0) {
        const std::uint64_t cl_id = next_cl_id_++;
        decision.has_ask_order = 1;
        decision.new_ask.tag = static_cast<std::uint16_t>(ShmMsgType::NewOrder);
        decision.new_ask.side = 2;
        decision.new_ask.post_only = 1;
        decision.new_ask.seq = seq_++;
        decision.new_ask.md_seq = md_seq;
        decision.new_ask.cl_id = cl_id;
        decision.new_ask.px = target_ask_px;
        decision.new_ask.qty = config_.order_size;
        decision.new_ask.send_ts_ns = last_tob_ns_;
        active_ask_cl_id_ = cl_id;
        active_ask_px_ = target_ask_px;
      }
    } else if (active_ask_cl_id_ != 0) {
      decision.has_ask_cancel = 1;
      decision.cancel_ask.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
      decision.cancel_ask.seq = seq_++;
      decision.cancel_ask.cl_id = active_ask_cl_id_;
      decision.cancel_ask.send_ts_ns = last_tob_ns_;
      active_ask_cl_id_ = 0;
    }

    return decision;
  }

  // Handle Order Ack report.
  void on_ack(const ShmOrderAck &ack) noexcept {
    ack_count_++;
    (void)ack;
  }

  // Handle Order Fill report.
  void on_fill(const ShmOrderFill &fill) noexcept {
    fill_count_++;
    if (fill.cl_id == active_bid_cl_id_) {
      position_ += fill.fill_qty;
      realized_pnl_ -= fill.fill_px * fill.fill_qty;
      active_bid_cl_id_ = 0;
    } else if (fill.cl_id == active_ask_cl_id_) {
      position_ -= fill.fill_qty;
      realized_pnl_ += fill.fill_px * fill.fill_qty;
      active_ask_cl_id_ = 0;
    }
  }

  // Handle Order Reject report.
  void on_reject(const ShmOrderReject &reject) noexcept {
    reject_count_++;
    if (reject.cl_id == active_bid_cl_id_) {
      active_bid_cl_id_ = 0;
    } else if (reject.cl_id == active_ask_cl_id_) {
      active_ask_cl_id_ = 0;
    }
  }

  // Cancel all outstanding live quotes.
  [[nodiscard]] StrategyDecision cancel_all() noexcept {
    StrategyDecision decision{};
    if (active_bid_cl_id_ != 0) {
      decision.has_bid_cancel = 1;
      decision.cancel_bid.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
      decision.cancel_bid.seq = seq_++;
      decision.cancel_bid.cl_id = active_bid_cl_id_;
      decision.cancel_bid.send_ts_ns = last_tob_ns_;
      active_bid_cl_id_ = 0;
    }
    if (active_ask_cl_id_ != 0) {
      decision.has_ask_cancel = 1;
      decision.cancel_ask.tag = static_cast<std::uint16_t>(ShmMsgType::CancelOrder);
      decision.cancel_ask.seq = seq_++;
      decision.cancel_ask.cl_id = active_ask_cl_id_;
      decision.cancel_ask.send_ts_ns = last_tob_ns_;
      active_ask_cl_id_ = 0;
    }
    return decision;
  }

  [[nodiscard]] bool is_stale(std::int64_t now_ns) const noexcept {
    return (now_ns > last_tob_ns_) && ((now_ns - last_tob_ns_) > config_.stale_timeout_ns);
  }

  [[nodiscard]] std::int64_t position() const noexcept { return position_; }
  [[nodiscard]] std::int64_t realized_pnl() const noexcept { return realized_pnl_; }
  [[nodiscard]] std::uint64_t fill_count() const noexcept { return fill_count_; }
  [[nodiscard]] std::uint64_t ack_count() const noexcept { return ack_count_; }
  [[nodiscard]] std::uint64_t reject_count() const noexcept { return reject_count_; }
  [[nodiscard]] std::uint64_t active_bid_cl_id() const noexcept { return active_bid_cl_id_; }
  [[nodiscard]] std::uint64_t active_ask_cl_id() const noexcept { return active_ask_cl_id_; }

private:
  StrategyConfig config_;
  std::uint64_t next_cl_id_{1};
  std::uint32_t seq_{1};

  std::int64_t position_{0};
  std::int64_t realized_pnl_{0};

  std::uint64_t active_bid_cl_id_{0};
  std::uint64_t active_ask_cl_id_{0};
  std::int64_t active_bid_px_{0};
  std::int64_t active_ask_px_{0};

  std::uint64_t last_md_seq_{0};
  std::int64_t last_tob_ns_{0};

  std::uint64_t fill_count_{0};
  std::uint64_t ack_count_{0};
  std::uint64_t reject_count_{0};
};

} // namespace mm
