// Domain types + order validation (plan Task 1; interfaces normative for later tasks).
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mm {

// Ticks/Lots are deliberately NON-aggregates with explicit constructors (codex hard-gate,
// Task 1): an aggregate leaks raw int64 in through copy-list-initialization —
// `Ticks x = {raw};` and `f({raw})` both compile — which the is_convertible/is_constructible
// traits cannot detect (they model the parenthesized/unbraced forms).
// PROHIBITED after this change: copy-list-initialization (`Ticks x = {raw}`, `f({raw})`).
// LEGAL, by design: direct-list-init `Ticks{raw}`, explicit direct-init `Ticks(raw)`, and
// default construction `Ticks{}`. test_types.cpp pins each of these forms explicitly.
struct Ticks {
  // v = price / tick_size (03 §1.1) — NOT raw price units. The wire carries raw integer
  // price units; the engine converts at the boundary, after validate_order() (which
  // operates purely in raw wire units).
  std::int64_t v{};
  constexpr Ticks() noexcept = default;
  explicit constexpr Ticks(std::int64_t ticks) noexcept : v{ticks} {}
  auto operator<=>(const Ticks &) const = default;
};
struct Lots {
  // v = qty / lot_size (03 §1.1) — NOT raw quantity units (same boundary rule as Ticks).
  std::int64_t v{};
  constexpr Lots() noexcept = default;
  explicit constexpr Lots(std::int64_t lots) noexcept : v{lots} {}
  auto operator<=>(const Lots &) const = default;
};

// Wire spelling (Task 2 NewOrder.side): "B" -> Side::Bid, "S" -> Side::Ask;
// validate_order() below is the single acceptor of that set.
enum class Side : std::uint8_t { Bid, Ask };

struct Instrument {
  std::string symbol; // "MOCKUSDT"
  // Invariant: tick_size >= 1 and lot_size >= 1 (positive divisors); validate_order
  // guards it defensively (mechanics at the guard below). That guard is strictly a
  // defensive UB backstop: the shipped binary constructs Instrument with the spec
  // defaults below and exposes no external tick/lot override (Task 7's Config carries no
  // instrument parameters), so a non-positive divisor can only arise from a code change.
  // If tick/lot ever become externally configurable, they must be validated at startup
  // (stderr + exit 2), not left to this backstop's lie-about-cause reject.
  std::int64_t tick_size{5}; // price units per tick (unit = $0.01)
  std::int64_t lot_size{10}; // qty units per lot    (unit = 0.001 base)
  // Bounds, not necessarily valid prices: the default min_price=1 is a deliberate open
  // lower bound and is off-tick, so the effective floor is the smallest multiple of
  // tick_size >= min_price (5 with the defaults).
  std::int64_t min_price{1}, max_price{10'000'000};
  std::int64_t max_order_qty{10'000};
  // PER SESSION (R-1): counted within one connection's orders, never globally.
  int max_live_orders{2};
};

// StaleEpoch: well-formed command from a previous epoch (A3 F-12) — reject, session stays up.
enum class RejectCode : std::uint8_t {
  UnsupportedVersion,
  UnknownType,
  Malformed,
  MsgTooLarge,
  UnknownSymbol,
  BadSide,
  TickSize,
  LotSize,
  QtyLimit,
  PriceRange,
  DupClOrdId,
  PostOnlyCross,
  MaxLiveOrders,
  UnknownClOrdId,
  AlreadyTerminal,
  StaleEpoch
};

// Emits the 03 §2.3 SCREAMING_SNAKE wire strings — the single normative spelling (F-20).
[[nodiscard]] constexpr std::string_view to_string(RejectCode code) noexcept {
  switch (code) {
  case RejectCode::UnsupportedVersion:
    return "UNSUPPORTED_VERSION";
  case RejectCode::UnknownType:
    return "UNKNOWN_TYPE";
  case RejectCode::Malformed:
    return "MALFORMED";
  case RejectCode::MsgTooLarge:
    return "MSG_TOO_LARGE";
  case RejectCode::UnknownSymbol:
    return "UNKNOWN_SYMBOL";
  case RejectCode::BadSide:
    return "BAD_SIDE";
  case RejectCode::TickSize:
    return "TICK_SIZE";
  case RejectCode::LotSize:
    return "LOT_SIZE";
  case RejectCode::QtyLimit:
    return "QTY_LIMIT";
  case RejectCode::PriceRange:
    return "PRICE_RANGE";
  case RejectCode::DupClOrdId:
    return "DUP_CLORDID";
  case RejectCode::PostOnlyCross:
    return "POST_ONLY_CROSS";
  case RejectCode::MaxLiveOrders:
    return "MAX_LIVE_ORDERS";
  case RejectCode::UnknownClOrdId:
    return "UNKNOWN_CLORDID";
  case RejectCode::AlreadyTerminal:
    return "ALREADY_TERMINAL";
  case RejectCode::StaleEpoch:
    return "STALE_EPOCH";
  }
  // Backstop, reached only for out-of-range values cast into RejectCode. "UNKNOWN" is
  // NOT in the normative 03 §2.3 reject-code list and must never reach the wire; every
  // in-range enumerator returns from the exhaustive switch above. Guards: -Wswitch warns
  // on a missing case (an appended enumerator), and the test's static_assert pins
  // StaleEpoch's ordinal against reordering/insertion.
  return "UNKNOWN";
}

// Returns nullopt if valid, else the failing code. Pure; no IO.
// Straight-line if-chain; check order is part of the contract (tests pin it):
// symbol → side → tick → lot → qty limit → price range.
[[nodiscard]] constexpr std::optional<RejectCode>
validate_order(const Instrument &inst, std::string_view symbol, std::string_view side,
               std::int64_t px, std::int64_t qty) noexcept {
  if (symbol != inst.symbol)
    return RejectCode::UnknownSymbol;
  if (side != "B" && side != "S")
    return RejectCode::BadSide;
  // Divisor guards uphold the Instrument invariant (tick/lot >= 1) against misconfigured
  // aggregates: `% 0` is UB and `INT64_MIN % -1` is signed-division overflow. With the
  // guard, divisors are >= 1 and the modulo below is well-defined for all int64 inputs.
  if (inst.tick_size <= 0 || px % inst.tick_size != 0)
    return RejectCode::TickSize;
  if (inst.lot_size <= 0 || qty % inst.lot_size != 0)
    return RejectCode::LotSize;
  // Non-positive qty has no dedicated code in the closed 03 §2.3 list; folded into
  // QTY_LIMIT — Reject.reason must distinguish ("qty must be positive" vs "qty exceeds
  // max_order_qty").
  if (qty <= 0 || qty > inst.max_order_qty)
    return RejectCode::QtyLimit;
  if (px < inst.min_price || px > inst.max_price)
    return RejectCode::PriceRange;
  return std::nullopt;
}

} // namespace mm
