// Shared support for the codec test TUs:
//   test_codec.cpp             — contract / round-trip / encode / hardening-policy cases
//   test_codec_equivalence.cpp — the arm-equivalence table + traceability batteries
// Everything here is `inline` in a named namespace: fixture readers, frame mutators,
// the golden struct values, and the decode/expect helpers both TUs share.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include "mm/codec.hpp"
#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

namespace codec_test {

inline std::string read_file(const std::string &name) {
  std::ifstream in(std::string(MM_GOLDEN_DIR) + "/" + name, std::ios::binary);
  REQUIRE(in.is_open());
  std::ostringstream ss;
  ss << in.rdbuf();
  return std::move(ss).str();
}

// Golden fixtures are single-line JSON with a trailing newline. The byte comparison
// trims OUTER whitespace only: reject.json carries meaningful spaces INSIDE its reason
// string, so stripping all whitespace would corrupt the expectation.
inline std::string trim_ws(std::string s) {
  const auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!s.empty() && is_ws(s.back()))
    s.pop_back();
  std::size_t start = 0;
  while (start < s.size() && is_ws(s[start]))
    ++start;
  return s.substr(start);
}

inline std::string_view kind_name(mm::CodecKind k) {
  return k == mm::CodecKind::Naive ? "naive" : "tuned";
}

// Single mutation helper: replace the first occurrence of `from` in the fixture text.
inline std::string replace_first(std::string text, std::string_view from, std::string_view to) {
  const auto pos = text.find(from);
  REQUIRE(pos != std::string::npos);
  text.replace(pos, from.size(), to);
  return text;
}

// Removes one top-level field (key + value + one adjacent comma) from single-line
// fixture text. Handles the LAST field of a schema, which has no trailing comma, by
// eating the preceding one. Fixture values never contain ',' or '}' internally.
inline std::string remove_field(std::string text, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const auto kpos = text.find(needle);
  REQUIRE(kpos != std::string::npos);
  const auto vend = text.find_first_of(",}", kpos + needle.size());
  REQUIRE(vend != std::string::npos);
  if (text[vend] == ',')
    text.erase(kpos, vend + 1 - kpos); // interior field: eat the trailing comma
  else
    text.erase(kpos - 1, vend - (kpos - 1)); // last field: eat the preceding comma
  return text;
}

// A cancel_order whose unknown field "x" carries `extra` levels of nested arrays; total
// frame depth = 1 (the root object) + extra. Used to pin the shared nesting-depth guard.
inline std::string nested_cancel(std::size_t extra) {
  std::string s = R"({"t":"cancel_order","v":1,"seq":4,"epoch":2,"cl_id":"C-1","x":)";
  s.append(extra, '[');
  s.append(extra, ']');
  s += "}";
  return s;
}

// A cancel_order padded with distinct unknown keys up to `total` top-level keys (the
// base frame has 5). Pins the shared preflight's fixed-capacity key policy (codec.hpp:
// kMaxTopLevelKeys).
inline std::string cancel_with_keys(std::size_t total) {
  std::string s = R"({"t":"cancel_order","v":1,"seq":4,"epoch":2,"cl_id":"C-1")";
  for (std::size_t k = 5; k < total; ++k)
    s += ",\"x" + std::to_string(k) + "\":0";
  return s + "}";
}

inline const mm::NewOrder kGoldenNewOrder{
    .v = 1,
    .seq = 3,
    .epoch = 2,
    .md_seq = 41,
    .cl_id = "C-1",
    .symbol = "MOCKUSDT",
    .side = "B",
    .px = 499995,
    .qty = 100,
    .post_only = true,
};

inline const mm::CancelOrder kGoldenCancelOrder{
    .v = 1,
    .seq = 4,
    .epoch = 2,
    .cl_id = "C-1",
};

inline const mm::Tob kGoldenTob{
    .v = 1,
    .seq = 7,
    .epoch = 2,
    .md_seq = 41,
    .symbol = "MOCKUSDT",
    .bid_px = 500000,
    .bid_qty = 100,
    .ask_px = 500010,
    .ask_qty = 80,
};

inline const mm::OrderAck kGoldenOrderAck{
    .v = 1,
    .seq = 8,
    .epoch = 2,
    .cl_id = "C-1",
    .eng_id = 1001,
    .status = "live",
    .svc_ns = 18500,
};

inline const mm::CancelAck kGoldenCancelAck{
    .v = 1, .seq = 9, .epoch = 2, .cl_id = "C-1", .eng_id = 1001, .status = "cancelled"};

inline const mm::Reject kGoldenReject{
    .v = 1,
    .seq = 10,
    .epoch = 2,
    .cl_id = "",
    // Built FROM the normative source, not respelled: mm::to_string(RejectCode) is THE
    // wire spelling (types.hpp), and this fixture's byte-compare against
    // reject.json is what pins the enum spelling to the golden contract — three
    // previously unlinked literals.
    .code = std::string(mm::to_string(mm::RejectCode::TickSize)),
    .reason = "px not a multiple of tick_size",
};

inline const mm::Fill kGoldenFill{
    .v = 1,
    .seq = 11,
    .epoch = 2,
    .cl_id = "C-1",
    .eng_id = 1001,
    .px = 499995,
    .qty = 100,
    .leaves = 0,
    .exec_id = 501,
};

inline mm::DecodeError require_error(mm::ICodec &codec, std::string_view json) {
  std::variant<mm::InMsg, mm::DecodeError> result;
  REQUIRE_NOTHROW(result = codec.decode(json));
  REQUIRE(std::holds_alternative<mm::DecodeError>(result));
  return std::get<mm::DecodeError>(result);
}

template <typename T> T require_decode(mm::ICodec &codec, std::string_view json) {
  std::variant<mm::InMsg, mm::DecodeError> result;
  REQUIRE_NOTHROW(result = codec.decode(json));
  if (const auto *err = std::get_if<mm::DecodeError>(&result))
    FAIL("decode failed: " << mm::to_string(err->code) << " — " << err->detail);
  auto &msg = std::get<mm::InMsg>(result);
  REQUIRE(std::holds_alternative<T>(msg));
  return std::get<T>(msg);
}

} // namespace codec_test
