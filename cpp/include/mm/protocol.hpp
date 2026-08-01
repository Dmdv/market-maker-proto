// Protocol structs. Wire field names are exactly the JSON keys in docs/PROTOCOL.md; the "t"
// tag is not a member but the variant discriminator (kTag* below).
//
// DECLARATION ORDER IS THE WIRE KEY ORDER: both codecs emit members in that order after the "t"
// tag and the golden fixtures pin it byte-for-byte, so reordering a member changes the wire.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace mm {

// The verbatim two-column layout keeps declaration order (== wire key order) auditable at a
// glance; reformatting would rewrap it and bury the order.
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

// Wire "t" tags — the single spelling shared by both codecs and the Python client.
inline constexpr std::string_view kTagTob = "top_of_book";
inline constexpr std::string_view kTagNewOrder = "new_order";
inline constexpr std::string_view kTagCancelOrder = "cancel_order";
inline constexpr std::string_view kTagOrderAck = "order_ack";
inline constexpr std::string_view kTagCancelAck = "cancel_ack";
inline constexpr std::string_view kTagReject = "reject";
inline constexpr std::string_view kTagFill = "fill";

} // namespace mm
