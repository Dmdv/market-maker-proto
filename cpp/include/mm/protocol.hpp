// Protocol structs (plan Task 2; interfaces normative for later tasks).
//
// Wire field names are EXACTLY the JSON keys documented in docs/PROTOCOL.md, and member
// DECLARATION ORDER IS THE WIRE KEY ORDER: BOTH codecs emit members in declaration order
// after the leading "t" tag (canonical wire form — codec.hpp), and the golden fixtures
// (tests/golden/) pin that byte-for-byte on EVERY outbound message, for both arms.
// Reordering a member is a wire-format change. Decoding stays key-order-insensitive.
//
// The "t" tag is not a struct member — it is the variant discriminator (kTag* below).
// Envelope on every message: v(=1), t, seq (per-direction per-session, from 1, never
// gaps), epoch (engine-assigned per connection). Tob additionally carries md_seq
// (may gap = conflation; decrease = fatal).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace mm {

// The fence below is deliberate, not formatter debt: the plan's verbatim two-column
// layout is retained so member declaration order (== wire key order, see above) stays
// auditable at a glance; formatting would rewrap it and bury the order.
// clang-format off
struct Tob      { std::uint64_t v{1}, seq{}, epoch{}, md_seq{}; std::string symbol;
                  std::int64_t bid_px{}, bid_qty{}, ask_px{}, ask_qty{};
                  bool operator==(const Tob &) const = default; };
struct NewOrder { std::uint64_t v{1}, seq{}, epoch{}, md_seq{}; std::string cl_id, symbol, side; // "B"/"S"
                  std::int64_t px{}, qty{}; bool post_only{true};
                  bool operator==(const NewOrder &) const = default; };
struct CancelOrder { std::uint64_t v{1}, seq{}, epoch{}; std::string cl_id;
                  bool operator==(const CancelOrder &) const = default; };
struct OrderAck  { std::uint64_t v{1}, seq{}, epoch{}; std::string cl_id; std::uint64_t eng_id{};
                   std::string status;  std::int64_t svc_ns{};            // status: "live"
                   bool operator==(const OrderAck &) const = default; };
struct CancelAck { std::uint64_t v{1}, seq{}, epoch{}; std::string cl_id; std::uint64_t eng_id{};
                   std::string status;                                    // "cancelled"
                   bool operator==(const CancelAck &) const = default; };
struct Reject    { std::uint64_t v{1}, seq{}, epoch{}; std::string cl_id;    // may be ""
                   std::string code, reason; // code: mm::to_string(RejectCode) wire spelling
                   bool operator==(const Reject &) const = default; };
struct Fill      { std::uint64_t v{1}, seq{}, epoch{}; std::string cl_id; std::uint64_t eng_id{};
                   std::int64_t px{}, qty{}, leaves{}; std::uint64_t exec_id{};
                   bool operator==(const Fill &) const = default; };
// clang-format on

using InMsg = std::variant<NewOrder, CancelOrder>;
using OutMsg = std::variant<Tob, OrderAck, CancelAck, Reject, Fill>;

// Wire "t" tags — the single spelling both codecs (and Python, Task 4) share.
inline constexpr std::string_view kTagTob = "top_of_book";
inline constexpr std::string_view kTagNewOrder = "new_order";
inline constexpr std::string_view kTagCancelOrder = "cancel_order";
inline constexpr std::string_view kTagOrderAck = "order_ack";
inline constexpr std::string_view kTagCancelAck = "cancel_ack";
inline constexpr std::string_view kTagReject = "reject";
inline constexpr std::string_view kTagFill = "fill";

} // namespace mm
