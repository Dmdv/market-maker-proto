// Task 1: domain types + order validation (plan interfaces are normative).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

// Promote -Wswitch to an error while this TU parses the header: appending a RejectCode
// enumerator without a to_string() case fails THIS build, before the non-normative
// "UNKNOWN" backstop could ever be reached (no -Werror=switch needed on the mm targets).
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#include <mm/types.hpp>
#pragma GCC diagnostic pop

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Teach Catch2 to print reject codes: failing table rows read e.g. "LOT_SIZE == TICK_SIZE"
// instead of "{?} == {?}" (gate finding — reuses the normative mm::to_string wire spellings).
namespace Catch {
template <> struct StringMaker<mm::RejectCode> {
  static std::string convert(mm::RejectCode c) { return std::string(mm::to_string(c)); }
};
template <> struct StringMaker<std::optional<mm::RejectCode>> {
  static std::string convert(const std::optional<mm::RejectCode> &o) {
    return o ? std::string(mm::to_string(*o)) : std::string("valid (nullopt)");
  }
};
} // namespace Catch

using mm::Instrument;
using mm::Lots;
using mm::RejectCode;
using mm::Ticks;

// ---------------------------------------------------------------------------
// COMPLETE initialization matrix for the strong types.
//
// Four successive gate rounds each found a different *unpinned spelling* — the standard
// traits only model some forms: is_convertible_v<From,T> is exactly copy-init `T x = f;`
// and says nothing about `T x = {f};`; is_constructible_v<T,From> models the parenthesized
// `T(f)` and says nothing about `T{f}` (they differ on narrowing). Rather than add one
// assertion per finding, enumerate EVERY initialization form against EVERY type pair once,
// so no spelling is left unstated.
//
// Forms: copy-init `T x = f`, copy-list-init `T x = {f}` / `g({f})`, direct-init `T(f)`,
// direct-list-init `T{f}`.
template <class T> void sink(T); // declared, never defined — unevaluated contexts only

// Forms x value categories. declval lets each form be probed as rvalue, lvalue and const
// lvalue: an rvalue-only converting constructor (`explicit Lots(Ticks&&)`) would slip past
// lvalue-only checks, so every form is tested against all three categories.
template <class T, class From>
concept CopyInit = requires { sink<T>(std::declval<From>()); };
template <class T, class From>
concept CopyListInit = requires { sink<T>({std::declval<From>()}); };
template <class T, class From>
concept DirectInit = requires { T(std::declval<From>()); };
template <class T, class From>
concept DirectListInit = requires { T{std::declval<From>()}; };

// "No conversion by any spelling" and "explicit spellings only", each over all categories.
template <class T, class From>
inline constexpr bool NoneOfAnyForm = !CopyInit<T, From> && !CopyListInit<T, From> &&
                                      !DirectInit<T, From> && !DirectListInit<T, From>;
template <class T, class From>
inline constexpr bool NoConversion =
    NoneOfAnyForm<T, From> && NoneOfAnyForm<T, From &> && NoneOfAnyForm<T, const From &>;
template <class T, class From>
inline constexpr bool ExplicitOnlyOneCat =
    !CopyInit<T, From> && !CopyListInit<T, From> && DirectInit<T, From> && DirectListInit<T, From>;
template <class T, class From>
inline constexpr bool ExplicitOnly = ExplicitOnlyOneCat<T, From> && ExplicitOnlyOneCat<T, From &> &&
                                     ExplicitOnlyOneCat<T, const From &>;

// From raw int64: the explicit forms are the ONLY way in (Ticks{raw} / Ticks(raw)); the
// implicit ones are shut. This is the price/qty safety property the integer-ticks mandate
// rests on. 2 types x 4 forms x 3 value categories.
static_assert(ExplicitOnly<Ticks, std::int64_t>);
static_assert(ExplicitOnly<Lots, std::int64_t>);

// Between the strong types, and back out to raw int64: nothing, by any spelling, in any
// value category. A price can never become a quantity, which is the point of two types.
static_assert(NoConversion<Ticks, Lots>);
static_assert(NoConversion<Lots, Ticks>);
static_assert(NoConversion<std::int64_t, Ticks>);
static_assert(NoConversion<std::int64_t, Lots>);

// Non-aggregate-ness is what closes the copy-list-init hole; pin it directly so a refactor
// back to a bare struct breaks the build here, not silently in a caller.
static_assert(!std::is_aggregate_v<Ticks>);
static_assert(!std::is_aggregate_v<Lots>);
static_assert(std::is_default_constructible_v<Ticks>); // Ticks{} stays valid
static_assert(std::is_default_constructible_v<Lots>);

// Pin the normative interface later tasks import: underlying enum types and the
// member types of the strong types / Instrument (a silent change must break here).
static_assert(std::is_same_v<std::underlying_type_t<mm::Side>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<RejectCode>, std::uint8_t>);
// Side values are pinned like RejectCode's ordinals below: a reorder must break here.
static_assert(static_cast<std::uint8_t>(mm::Side::Bid) == 0);
static_assert(static_cast<std::uint8_t>(mm::Side::Ask) == 1);
static_assert(std::is_same_v<decltype(Ticks::v), std::int64_t>);
static_assert(std::is_same_v<decltype(Lots::v), std::int64_t>);
static_assert(std::is_same_v<decltype(Instrument::symbol), std::string>);
static_assert(std::is_same_v<decltype(Instrument::tick_size), std::int64_t>);
static_assert(std::is_same_v<decltype(Instrument::lot_size), std::int64_t>);
static_assert(std::is_same_v<decltype(Instrument::min_price), std::int64_t>);
static_assert(std::is_same_v<decltype(Instrument::max_price), std::int64_t>);
static_assert(std::is_same_v<decltype(Instrument::max_order_qty), std::int64_t>);
static_assert(std::is_same_v<decltype(Instrument::max_live_orders), int>);

namespace {

// The spec instrument: tick_size=5 / lot_size=10 make divisibility checks non-vacuous.
Instrument test_instrument() { return Instrument{.symbol = "MOCKUSDT"}; }

} // namespace

TEST_CASE("types: strong types order and compare") {
  CHECK(Ticks{3} < Ticks{4});
  CHECK(Ticks{4} == Ticks{4});
  CHECK(Lots{2} < Lots{3});
  CHECK(Lots{7} == Lots{7});
}

TEST_CASE("types: validate_order table") {
  const Instrument inst = test_instrument();
  struct Case {
    const char *name;
    std::string_view symbol, side;
    std::int64_t px, qty;
    std::optional<RejectCode> want;
  };
  // NOLINTBEGIN(modernize-use-designated-initializers): positional rows keep the table
  // scannable; field order is pinned by the `struct Case` declaration just above.
  const Case cases[] = {
      {"valid bid", "MOCKUSDT", "B", 500'000, 100, std::nullopt},
      {"valid ask", "MOCKUSDT", "S", 500'000, 100, std::nullopt},
      {"wrong symbol", "FOOUSDT", "B", 500'000, 100, RejectCode::UnknownSymbol},
      {"bad side", "MOCKUSDT", "X", 500'000, 100, RejectCode::BadSide},
      {"off-tick px (101 % 5 != 0)", "MOCKUSDT", "B", 101, 100, RejectCode::TickSize},
      {"off-lot qty (15 % 10 != 0)", "MOCKUSDT", "B", 500'000, 15, RejectCode::LotSize},
      {"qty above max", "MOCKUSDT", "B", 500'000, 10'010, RejectCode::QtyLimit},
      {"qty zero", "MOCKUSDT", "B", 500'000, 0, RejectCode::QtyLimit},
      {"px zero", "MOCKUSDT", "B", 0, 100, RejectCode::PriceRange},
      {"px above max", "MOCKUSDT", "B", 10'000'005, 100, RejectCode::PriceRange},
      // Accept-side boundaries: limits are inclusive (kills `>` → `>=` mutations).
      {"qty at max_order_qty", "MOCKUSDT", "B", 500'000, 10'000, std::nullopt},
      {"px at max_price", "MOCKUSDT", "B", 10'000'000, 100, std::nullopt},
      // Signed-input boundaries: negatives and INT64_MIN must reject deterministically,
      // never wrap or shortcut (pins precedence against e.g. a std::abs refactor).
      {"px -1 off-tick", "MOCKUSDT", "B", -1, 100, RejectCode::TickSize},
      {"px -5 on-tick but below min", "MOCKUSDT", "B", -5, 100, RejectCode::PriceRange},
      {"qty -1 off-lot", "MOCKUSDT", "B", 500'000, -1, RejectCode::LotSize},
      {"qty -10 on-lot but non-positive", "MOCKUSDT", "B", 500'000, -10, RejectCode::QtyLimit},
      {"px INT64_MIN off-tick", "MOCKUSDT", "B", std::numeric_limits<std::int64_t>::min(), 100,
       RejectCode::TickSize},
      {"qty INT64_MIN off-lot", "MOCKUSDT", "B", 500'000, std::numeric_limits<std::int64_t>::min(),
       RejectCode::LotSize},
      // Exact-string matching: superstring/prefix/empty inputs must reject (kills
      // starts_with-style mutants of the symbol and side checks).
      {"symbol superstring", "MOCKUSDT2", "B", 500'000, 100, RejectCode::UnknownSymbol},
      {"symbol prefix", "MOCKUSD", "B", 500'000, 100, RejectCode::UnknownSymbol},
      {"empty symbol", "", "B", 500'000, 100, RejectCode::UnknownSymbol},
      {"side superstring", "MOCKUSDT", "BX", 500'000, 100, RejectCode::BadSide},
      {"empty side", "MOCKUSDT", "", 500'000, 100, RejectCode::BadSide},
  };
  // NOLINTEND(modernize-use-designated-initializers)
  for (const Case &c : cases) {
    INFO(c.name);
    CHECK(mm::validate_order(inst, c.symbol, c.side, c.px, c.qty) == c.want);
  }
  // validate_order is constexpr (documented pure): pin one accept and one reject row at
  // compile time, mirroring the to_string STATIC_CHECK block below.
  STATIC_CHECK(mm::validate_order(Instrument{.symbol = "MOCKUSDT"}, "MOCKUSDT", "B", 500'000,
                                  100) == std::nullopt);
  STATIC_CHECK(mm::validate_order(Instrument{.symbol = "MOCKUSDT"}, "MOCKUSDT", "B", 101, 100) ==
               RejectCode::TickSize);
}

TEST_CASE("types: validate_order reads limits from the Instrument, not constants") {
  // Fully non-default instrument: an implementation hard-coding the spec defaults
  // (tick 5 / lot 10 / max_qty 10'000 / max_px 10'000'000) fails every row here.
  const Instrument alt{.symbol = "MOCKUSDT",
                       .tick_size = 7,
                       .lot_size = 3,
                       .min_price = 7,
                       .max_price = 700,
                       .max_order_qty = 9};
  CHECK(mm::validate_order(alt, "MOCKUSDT", "B", 14, 9) == std::nullopt);
  CHECK(mm::validate_order(alt, "MOCKUSDT", "B", 100, 9) == RejectCode::TickSize);
  CHECK(mm::validate_order(alt, "MOCKUSDT", "B", 14, 10) == RejectCode::LotSize);
  CHECK(mm::validate_order(alt, "MOCKUSDT", "B", 14, 12) == RejectCode::QtyLimit);
  CHECK(mm::validate_order(alt, "MOCKUSDT", "B", 707, 9) == RejectCode::PriceRange);
}

TEST_CASE("types: min_price bound is inclusive") {
  // The default instrument's min_price=1 is off-tick (unobservable), so pin the min
  // bound with an on-tick min_price (kills the `<` → `<=` mutation on the low bound).
  Instrument inst = test_instrument();
  inst.min_price = 500'000;
  CHECK(mm::validate_order(inst, "MOCKUSDT", "B", 500'000, 100) == std::nullopt);
  CHECK(mm::validate_order(inst, "MOCKUSDT", "B", 499'995, 100) == RejectCode::PriceRange);
}

TEST_CASE("types: misconfigured divisors reject instead of UB") {
  // Instrument is a publicly constructible aggregate; a non-positive tick/lot divisor
  // must surface as a rejection, never as `% 0` UB or INT64_MIN % -1 signed overflow.
  Instrument bad_tick = test_instrument();
  bad_tick.tick_size = 0;
  CHECK(mm::validate_order(bad_tick, "MOCKUSDT", "B", 500'000, 100) == RejectCode::TickSize);
  bad_tick.tick_size = -1;
  CHECK(mm::validate_order(bad_tick, "MOCKUSDT", "B", std::numeric_limits<std::int64_t>::min(),
                           100) == RejectCode::TickSize);

  Instrument bad_lot = test_instrument();
  bad_lot.lot_size = 0;
  CHECK(mm::validate_order(bad_lot, "MOCKUSDT", "B", 500'000, 100) == RejectCode::LotSize);
  bad_lot.lot_size = -1;
  CHECK(mm::validate_order(bad_lot, "MOCKUSDT", "B", 500'000,
                           std::numeric_limits<std::int64_t>::min()) == RejectCode::LotSize);
}

TEST_CASE("types: Instrument defaults are the spec configuration") {
  // Later tasks depend on these exact defaults (tick 5 / lot 10 keep validation
  // non-vacuous; max_live_orders=2 is per session, R-1).
  const Instrument inst{};
  CHECK(inst.symbol.empty());
  CHECK(inst.tick_size == 5);
  CHECK(inst.lot_size == 10);
  CHECK(inst.min_price == 1);
  CHECK(inst.max_price == 10'000'000);
  CHECK(inst.max_order_qty == 10'000);
  CHECK(inst.max_live_orders == 2);
}

TEST_CASE("types: validation check order is deterministic") {
  const Instrument inst = test_instrument();
  // symbol before side before tick before lot before qty-limit before price-range:
  // each input violates the named check AND every later one; the first must win.
  CHECK(mm::validate_order(inst, "FOOUSDT", "X", 101, 15) == RejectCode::UnknownSymbol);
  CHECK(mm::validate_order(inst, "MOCKUSDT", "X", 101, 15) == RejectCode::BadSide);
  CHECK(mm::validate_order(inst, "MOCKUSDT", "B", 101, 15) == RejectCode::TickSize);
  CHECK(mm::validate_order(inst, "MOCKUSDT", "B", 0, 10'015) == RejectCode::LotSize);
  CHECK(mm::validate_order(inst, "MOCKUSDT", "B", 0, 10'010) == RejectCode::QtyLimit);
}

// Ordinal pin: reordering the enum or inserting a code before StaleEpoch breaks the build
// here, keeping the existing wire spellings stable (F-20). An *appended* code is NOT
// caught by this assert — it is caught by the exhaustive switch in to_string(), which
// this TU compiles under `#pragma GCC diagnostic error "-Wswitch"` (see the header
// include at the top), so an append is a hard build error here.
// Every ordinal is pinned, not just the last (codex hard-gate S3): swapping any two
// earlier codes must break this build, since the wire spellings are the contract.
static_assert(static_cast<std::uint8_t>(RejectCode::UnsupportedVersion) == 0);
static_assert(static_cast<std::uint8_t>(RejectCode::UnknownType) == 1);
static_assert(static_cast<std::uint8_t>(RejectCode::Malformed) == 2);
static_assert(static_cast<std::uint8_t>(RejectCode::MsgTooLarge) == 3);
static_assert(static_cast<std::uint8_t>(RejectCode::UnknownSymbol) == 4);
static_assert(static_cast<std::uint8_t>(RejectCode::BadSide) == 5);
static_assert(static_cast<std::uint8_t>(RejectCode::TickSize) == 6);
static_assert(static_cast<std::uint8_t>(RejectCode::LotSize) == 7);
static_assert(static_cast<std::uint8_t>(RejectCode::QtyLimit) == 8);
static_assert(static_cast<std::uint8_t>(RejectCode::PriceRange) == 9);
static_assert(static_cast<std::uint8_t>(RejectCode::DupClOrdId) == 10);
static_assert(static_cast<std::uint8_t>(RejectCode::PostOnlyCross) == 11);
static_assert(static_cast<std::uint8_t>(RejectCode::MaxLiveOrders) == 12);
static_assert(static_cast<std::uint8_t>(RejectCode::UnknownClOrdId) == 13);
static_assert(static_cast<std::uint8_t>(RejectCode::AlreadyTerminal) == 14);
static_assert(static_cast<std::uint8_t>(RejectCode::StaleEpoch) == 15,
              "RejectCode reordered or code inserted: re-sync the to_string() wire "
              "spellings (appended codes are caught by -Wswitch, not this assert)");

TEST_CASE("types: to_string emits the 03 §2.3 SCREAMING_SNAKE wire strings") {
  using enum RejectCode;
  // constexpr to_string lets the F-20 wire spellings be contract-checked at build time.
  STATIC_CHECK(mm::to_string(UnsupportedVersion) == "UNSUPPORTED_VERSION");
  STATIC_CHECK(mm::to_string(UnknownType) == "UNKNOWN_TYPE");
  STATIC_CHECK(mm::to_string(Malformed) == "MALFORMED");
  STATIC_CHECK(mm::to_string(MsgTooLarge) == "MSG_TOO_LARGE");
  STATIC_CHECK(mm::to_string(UnknownSymbol) == "UNKNOWN_SYMBOL");
  STATIC_CHECK(mm::to_string(BadSide) == "BAD_SIDE");
  STATIC_CHECK(mm::to_string(TickSize) == "TICK_SIZE");
  STATIC_CHECK(mm::to_string(LotSize) == "LOT_SIZE");
  STATIC_CHECK(mm::to_string(QtyLimit) == "QTY_LIMIT");
  STATIC_CHECK(mm::to_string(PriceRange) == "PRICE_RANGE");
  STATIC_CHECK(mm::to_string(DupClOrdId) == "DUP_CLORDID");
  STATIC_CHECK(mm::to_string(PostOnlyCross) == "POST_ONLY_CROSS");
  STATIC_CHECK(mm::to_string(MaxLiveOrders) == "MAX_LIVE_ORDERS");
  STATIC_CHECK(mm::to_string(UnknownClOrdId) == "UNKNOWN_CLORDID");
  STATIC_CHECK(mm::to_string(AlreadyTerminal) == "ALREADY_TERMINAL");
  STATIC_CHECK(mm::to_string(StaleEpoch) == "STALE_EPOCH");
  // The backstop for out-of-range cast-in values is "UNKNOWN" — a non-normative string
  // that must never reach the wire (runtime CHECK: covers the fallthrough branch).
  CHECK(mm::to_string(static_cast<RejectCode>(200)) == "UNKNOWN");
}

TEST_CASE("types: Catch2 StringMaker specializations are selected") {
  // The StringMakers above otherwise run only on FAILING assertions, so a green suite
  // cannot see them break (signature drift, or a Catch2 upgrade shipping its own
  // optional stringmaker). Pin the dispatch, not just the convert() bodies.
  CHECK(Catch::Detail::stringify(std::optional<RejectCode>{RejectCode::TickSize}) == "TICK_SIZE");
  CHECK(Catch::Detail::stringify(std::optional<RejectCode>{}) == "valid (nullopt)");
  CHECK(Catch::Detail::stringify(RejectCode::LotSize) == "LOT_SIZE");
}
