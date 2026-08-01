// tests: protocol structs + dual codec (naive nlohmann / tuned glaze) against the
// golden fixtures in tests/golden/ — the cross-language contract (Python reads the
// same files). Both codecs must agree on every behavior pinned here — accept/reject
// verdict AND RejectCode — and BOTH are pinned byte-identical to every outbound fixture
// (canonical wire form; encode/decode contracts in codec.hpp).
//
// This TU holds the contract / round-trip / encode / hardening-policy cases; the
// arm-equivalence table and the traceability batteries live in
// test_codec_equivalence.cpp, with shared helpers in codec_test_support.hpp
//.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include "codec_test_support.hpp"
#include "mm/codec.hpp"
#include "mm/protocol.hpp"

#include <string>
#include <string_view>
#include <variant>

using namespace codec_test;

TEST_CASE("codec: inbound golden fixtures decode to the expected structs", "[codec]") {
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);

  SECTION("new_order.json") {
    const auto msg = require_decode<mm::NewOrder>(*codec, read_file("new_order.json"));
    CHECK(msg == kGoldenNewOrder);
  }
  SECTION("cancel_order.json") {
    const auto msg = require_decode<mm::CancelOrder>(*codec, read_file("cancel_order.json"));
    CHECK(msg == kGoldenCancelOrder);
  }
  SECTION("post_only:false is honored (default-true must not mask the wire value)") {
    const auto json =
        replace_first(read_file("new_order.json"), "\"post_only\":true", "\"post_only\":false");
    const auto msg = require_decode<mm::NewOrder>(*codec, json);
    CHECK_FALSE(msg.post_only);
  }
  SECTION("unknown keys are ignored (additive forward compatibility, both codecs agree)") {
    const auto json = replace_first(read_file("new_order.json"), "\"post_only\":true",
                                    "\"post_only\":true,\"future_field\":123");
    const auto msg = require_decode<mm::NewOrder>(*codec, json);
    CHECK(msg == kGoldenNewOrder);
  }
}

TEST_CASE("codec: decode is key-order-insensitive (canonical order is an encode-side property)",
          "[codec]") {
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);
  // new_order.json with every key position scrambled and "t" moved last.
  const auto msg = require_decode<mm::NewOrder>(
      *codec,
      R"({"post_only":true,"qty":100,"px":499995,"side":"B","symbol":"MOCKUSDT","cl_id":"C-1","md_seq":41,"epoch":2,"seq":3,"v":1,"t":"new_order"})");
  CHECK(msg == kGoldenNewOrder);
}

TEST_CASE("codec: outbound structs encode to the golden fixtures byte-for-byte", "[codec]") {
  // Canonical wire form — "t" first, then members in declaration order, no whitespace —
  // is the cross-language contract, so EVERY outbound fixture is raw-compared for BOTH
  // arms (pinned for reproducible builds). The independent nlohmann parse additionally proves the
  // bytes are valid JSON to a third reader.
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);
  std::string out;

  const auto check_encode = [&](const mm::OutMsg &msg, const std::string &fixture) {
    codec->encode(msg, out);
    CAPTURE(fixture, out);
    CHECK(out == trim_ws(read_file(fixture)));
    CHECK(nlohmann::json::parse(out) == nlohmann::json::parse(read_file(fixture)));
  };

  check_encode(kGoldenTob, "tob.json");
  check_encode(kGoldenOrderAck, "order_ack.json");
  check_encode(kGoldenCancelAck, "cancel_ack.json");
  check_encode(kGoldenReject, "reject.json");
  check_encode(kGoldenFill, "fill.json");
}

TEST_CASE("codec: encode reuses the caller buffer across messages", "[codec]") {
  // Contract (codec.hpp): steady-state allocation-free. Warmed capacity AND the data
  // address must hold across repeated encodes of every outbound message — checking only
  // for stale residue would let a fresh-DOM + fresh-dump implementation pass (pinned for
  // reproducible builds).
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);
  const mm::OutMsg msgs[] = {mm::OutMsg{kGoldenTob}, mm::OutMsg{kGoldenOrderAck},
                             mm::OutMsg{kGoldenCancelAck}, mm::OutMsg{kGoldenReject},
                             mm::OutMsg{kGoldenFill}};
  std::string out;
  for (const auto &m : msgs) // warm-up round: allocations allowed here only
    codec->encode(m, out);
  const auto warmed_capacity = out.capacity();
  const void *warmed_data = out.data();
  for (int round = 0; round < 4; ++round) {
    for (const auto &m : msgs) {
      codec->encode(m, out);
      CHECK(out.capacity() == warmed_capacity);
      CHECK(static_cast<const void *>(out.data()) == warmed_data);
    }
  }
  // Long tob first, short cancel_ack after: stale residue would corrupt the shorter one.
  codec->encode(mm::OutMsg{kGoldenTob}, out);
  codec->encode(mm::OutMsg{kGoldenCancelAck}, out);
  CHECK(out == trim_ws(read_file("cancel_ack.json")));
}

TEST_CASE("codec: control characters in outbound strings escape identically in both arms",
          "[codec]") {
  // glaze's default writer copies raw 0x00..0x1F bytes from string values into the
  // output — INVALID JSON for any decoded string that carried them (pinned for reproducible
  // builds). Both arms must emit the SAME bytes (shorthand escapes plus uppercase hex u-escapes),
  // and an independent reader must round-trip the exact original value. Every outbound message type
  // carries at least one string field, so all five are exercised.
  const auto naive = mm::make_codec(mm::CodecKind::Naive);
  const auto tuned = mm::make_codec(mm::CodecKind::Tuned);
  const std::string hostile =
      std::string("a") + '\x01' + "b\nc\"d\\e" + '\x1F' + std::string(1, '\0') + "f\tg";

  struct Case {
    const char *field;
    mm::OutMsg msg;
  };
  const Case cases[] = {
      {"symbol", mm::Tob{.v = 1,
                         .seq = 7,
                         .epoch = 2,
                         .md_seq = 41,
                         .symbol = hostile,
                         .bid_px = 1,
                         .bid_qty = 1,
                         .ask_px = 2,
                         .ask_qty = 1}},
      {"cl_id", mm::OrderAck{.v = 1,
                             .seq = 8,
                             .epoch = 2,
                             .cl_id = hostile,
                             .eng_id = 1,
                             .status = "live",
                             .svc_ns = 1}},
      {"status",
       mm::CancelAck{.v = 1, .seq = 9, .epoch = 2, .cl_id = "C-1", .eng_id = 1, .status = hostile}},
      {"reason",
       mm::Reject{
           .v = 1, .seq = 10, .epoch = 2, .cl_id = "", .code = "TICK_SIZE", .reason = hostile}},
      {"cl_id", mm::Fill{.v = 1,
                         .seq = 11,
                         .epoch = 2,
                         .cl_id = hostile,
                         .eng_id = 1,
                         .px = 1,
                         .qty = 1,
                         .leaves = 0,
                         .exec_id = 1}},
  };
  std::string naive_out, tuned_out;
  for (const auto &c : cases) {
    INFO("hostile field = " << c.field);
    naive->encode(c.msg, naive_out);
    tuned->encode(c.msg, tuned_out);
    CAPTURE(naive_out);
    CHECK(naive_out == tuned_out);
    const auto parsed = nlohmann::json::parse(naive_out, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(parsed.is_discarded()); // the encoded bytes must BE valid JSON
    CHECK(parsed.at(c.field).get<std::string>() == hostile);
  }
}

TEST_CASE("codec: error battery — decode never throws, yields the right code", "[codec]") {
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);
  const auto new_order = read_file("new_order.json");

  SECTION("truncated JSON -> Malformed") {
    const auto err = require_error(*codec, std::string_view(new_order).substr(0, 40));
    CHECK(err.code == mm::RejectCode::Malformed);
  }
  SECTION("empty input -> Malformed") {
    CHECK(require_error(*codec, "").code == mm::RejectCode::Malformed);
  }
  SECTION("root is not an object -> Malformed") {
    CHECK(require_error(*codec, "[1,2,3]").code == mm::RejectCode::Malformed);
  }
  SECTION("v:2 -> UnsupportedVersion") {
    const auto json = replace_first(new_order, "\"v\":1", "\"v\":2");
    CHECK(require_error(*codec, json).code == mm::RejectCode::UnsupportedVersion);
  }
  SECTION("v missing -> Malformed") {
    const auto json = replace_first(new_order, "\"v\":1,", "");
    CHECK(require_error(*codec, json).code == mm::RejectCode::Malformed);
  }
  SECTION("v mistyped (string) -> Malformed") {
    const auto json = replace_first(new_order, "\"v\":1", "\"v\":\"1\"");
    CHECK(require_error(*codec, json).code == mm::RejectCode::Malformed);
  }
  SECTION("t:nope -> UnknownType") {
    const auto json = replace_first(new_order, "\"t\":\"new_order\"", "\"t\":\"nope\"");
    CHECK(require_error(*codec, json).code == mm::RejectCode::UnknownType);
  }
  SECTION("t missing -> Malformed") {
    const auto json = replace_first(new_order, "\"t\":\"new_order\",", "");
    CHECK(require_error(*codec, json).code == mm::RejectCode::Malformed);
  }
  SECTION("outbound tag arriving inbound -> UnknownType") {
    // The engine decodes InMsg only; a client echoing back an engine tag is a type error,
    // not a parse error.
    CHECK(require_error(*codec, read_file("tob.json")).code == mm::RejectCode::UnknownType);
  }
  SECTION("t-unknown wins over v-unsupported (check order is t before v)") {
    auto json = replace_first(new_order, "\"t\":\"new_order\"", "\"t\":\"nope\"");
    json = replace_first(json, "\"v\":1", "\"v\":2");
    CHECK(require_error(*codec, json).code == mm::RejectCode::UnknownType);
  }
  SECTION("missing cl_id -> Malformed") {
    const auto json = replace_first(new_order, "\"cl_id\":\"C-1\",", "");
    CHECK(require_error(*codec, json).code == mm::RejectCode::Malformed);
  }
  SECTION("px as string -> Malformed") {
    const auto json = replace_first(new_order, "\"px\":499995", "\"px\":\"499995\"");
    CHECK(require_error(*codec, json).code == mm::RejectCode::Malformed);
  }
  SECTION("70 KiB of 'a' -> Malformed (transport owns MsgTooLarge; codec sees garbage)") {
    const std::string big(70 * 1024, 'a');
    CHECK(require_error(*codec, big).code == mm::RejectCode::Malformed);
  }
}

TEST_CASE("codec: nesting depth guard — bounded rejection, never stack exhaustion", "[codec]") {
  // A size-bounded but deeply nested unknown value must be REJECTED by policy, not
  // recursed into (protocol rule: malformed input never terminates the process). Depth
  // is counted ITERATIVELY in the shared preflight before either arm's recursive parser
  // runs (codec.hpp); the suite also runs under the asan preset, which turns any stack
  // overshoot into a hard failure.
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);

  SECTION("at the shared limit: accepted") {
    const auto msg =
        require_decode<mm::CancelOrder>(*codec, nested_cancel(mm::detail::kMaxNestingDepth - 1));
    CHECK(msg == kGoldenCancelOrder);
  }
  SECTION("one level past the limit: Malformed") {
    CHECK(require_error(*codec, nested_cancel(mm::detail::kMaxNestingDepth)).code ==
          mm::RejectCode::Malformed);
  }
  SECTION("100k-deep bomb: Malformed, process survives") {
    CHECK(require_error(*codec, nested_cancel(100'000)).code == mm::RejectCode::Malformed);
  }
}

TEST_CASE("codec: escaped-control policy — identifiers reject, unknown values stay legal",
          "[codec]") {
  // Escaped C0 controls are legal JSON and the preflight preserves them inside string
  // values — but a decoded NUL inside cl_id truncates C-string sinks (two distinct
  // cl_ids collide as order-map keys / in logs) and CR/LF forges records (CWE-158/117),
  // so the KNOWN identifier fields reject decoded bytes < 0x20 and 0x7F in both arms
  // while unknown/skipped values keep the permissive documented rule (codec.hpp,
  // a review — this case previously pinned the NUL-in-cl_id frame as ACCEPTED).
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);

  SECTION("escaped NUL in cl_id -> Malformed (identifier byte policy)") {
    const auto err = require_error(
        *codec, "{\"t\":\"cancel_order\",\"v\":1,\"seq\":4,\"epoch\":2,\"cl_id\":\"a\\u0000b\"}");
    CHECK(err.code == mm::RejectCode::Malformed);
  }
  SECTION("escaped NUL inside an UNKNOWN field's value is still accepted") {
    const auto msg = require_decode<mm::CancelOrder>(
        *codec, "{\"t\":\"cancel_order\",\"v\":1,\"seq\":4,\"epoch\":2,\"cl_id\":\"C-1\",\"x\":"
                "\"a\\u0000b\"}");
    CHECK(msg == kGoldenCancelOrder);
  }
  SECTION("surrogate-pair escape in cl_id decodes to the exact UTF-8 bytes") {
    // U+1F600 via 😀 — non-ASCII identifiers pass the byte policy (only
    // < 0x20 and 0x7F reject) and must decode to the same UTF-8 bytes in both arms.
    const auto msg = require_decode<mm::CancelOrder>(
        *codec,
        "{\"t\":\"cancel_order\",\"v\":1,\"seq\":4,\"epoch\":2,\"cl_id\":\"\\uD83D\\uDE00\"}");
    CHECK(msg.cl_id == "\xF0\x9F\x98\x80");
  }
}
