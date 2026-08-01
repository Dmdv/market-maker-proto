// tests, second TU: the codec arm-equivalence table plus the traceability
// batteries (missing-field matrix, make_codec identity pin, sanitized-tag detail,
// encode value-domain agreement, inbound-fixture key-order pin).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include "codec_test_support.hpp"
#include "mm/codec.hpp"
#include "mm/protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <variant>
#include <vector>

using namespace codec_test;

// ---------------------------------------------------------------------------
// Codec equivalence: the two arms sit behind ONE runtime flag in ONE engine binary and
// are A/B-benchmarked as "equivalent functionality", so an accept/reject divergence is a
// correctness defect. Every input here was either empirically divergent at some point
// (commit 220b665 closed the first four; the review R1 batch closed the schema-scoping,
// key-policy, and byte-policy rows; the review batch closed the number-token rows —
// the S2 note in codec.hpp) or pins a shared policy decision (codec.hpp). Each case
// asserts the verdict, RejectCode AND detail of BOTH arms, plus their equality.
TEST_CASE("codec: both arms agree on frame-level rejections", "[codec]") {
  const auto tuned = mm::make_codec(mm::CodecKind::Tuned);
  const auto naive = mm::make_codec(mm::CodecKind::Naive);
  const std::string cancel = R"({"t":"cancel_order","v":1,"seq":4,"epoch":2,"cl_id":"C-1"})";
  const std::string norder =
      R"({"t":"new_order","v":1,"seq":1,"epoch":1,"md_seq":1,"cl_id":"B-1","symbol":"MOCKUSDT","side":"B","px":499995,"qty":100,"post_only":true})";
  // Appends one unknown member after cl_id on the valid control frame.
  const auto with_unknown = [&](const std::string &extra) {
    return replace_first(cancel, R"("cl_id":"C-1")", R"("cl_id":"C-1",)" + extra);
  };
  constexpr auto kMalformed = mm::RejectCode::Malformed;
  constexpr std::optional<mm::RejectCode> kAccept = std::nullopt;
  struct Case {
    const char *name;
    std::string json;
    std::optional<mm::RejectCode> expect_reject; // nullopt: both arms must ACCEPT
  };
  const Case cases[] = {
      // frame-grammar strictness (commit 220b665)
      {"exponent token in integer px",
       R"({"t":"new_order","v":1,"seq":1,"epoch":1,"md_seq":1,"cl_id":"B-1","symbol":"MOCKUSDT","side":"B","px":1e3,"qty":100,"post_only":true})",
       kMalformed},
      {"float token in integer px",
       R"({"t":"new_order","v":1,"seq":1,"epoch":1,"md_seq":1,"cl_id":"B-1","symbol":"MOCKUSDT","side":"B","px":500000.0,"qty":100,"post_only":true})",
       kMalformed},
      {"trailing garbage after a valid object", cancel + " NOT_JSON_AT_ALL", kMalformed},
      {"a second object smuggled after the frame", cancel + R"({"t":"x"})", kMalformed},
      {"malformed value in an unknown field",
       R"({"t":"cancel_order","v":1,"seq":1,"epoch":1,"cl_id":"B-1","junk":@@@})", kMalformed},
      {"well-formed control frame", cancel, kAccept},
      // schema-scoped integer tokens (pinned for reproducible builds): unknown fields are never
      // token-checked
      {"unknown px on cancel_order may carry an exponent",
       replace_first(cancel, "\"cl_id\":\"C-1\"", "\"cl_id\":\"C-1\",\"px\":1e3"), kAccept},
      {"unknown px on cancel_order may carry a bool",
       replace_first(cancel, "\"cl_id\":\"C-1\"", "\"cl_id\":\"C-1\",\"px\":true"), kAccept},
      {"unknown md_seq on cancel_order may carry an exponent",
       replace_first(cancel, "\"cl_id\":\"C-1\"", "\"cl_id\":\"C-1\",\"md_seq\":1e3"), kAccept},
      {"unknown t outranks a bad integer token (precedence)", R"({"t":"nope","v":1,"px":1e3})",
       mm::RejectCode::UnknownType},
      {"v as float token 1e0 is Malformed even though it equals 1",
       replace_first(cancel, "\"v\":1", "\"v\":1e0"), kMalformed},
      {"v as float token 2e0 is Malformed, never UnsupportedVersion",
       replace_first(cancel, "\"v\":1", "\"v\":2e0"), kMalformed},
      {"seq above the u64 range",
       replace_first(cancel, "\"seq\":4", "\"seq\":18446744073709551616"), kMalformed},
      {"seq negative for an unsigned field", replace_first(cancel, "\"seq\":4", "\"seq\":-1"),
       kMalformed},
      // The adjacent unpinned spelling: -0 is a grammar-valid integral
      // token whose sign verdict must come from the SHARED unsigned predicate in both
      // arms, never from a library's refusal of '-' into a uint64_t.
      {"seq as -0 on an unsigned field", replace_first(cancel, "\"seq\":4", "\"seq\":-0"),
       kMalformed},
      // top-level key policy (pinned for reproducible builds): literal ASCII keys, duplicates
      // reject
      {"escaped alias of the t key",
       "{\"\\u0074\":\"cancel_order\",\"v\":1,\"seq\":4,\"epoch\":2,\"cl_id\":\"C-1\"}",
       kMalformed},
      {"escape sequence inside another top-level key",
       "{\"t\":\"cancel_order\",\"v\":1,\"seq\":4,\"epoch\":2,\"cl_\\u0069d\":\"C-1\"}",
       kMalformed},
      {"duplicate seq: bad first, good last",
       replace_first(cancel, "\"seq\":4", "\"seq\":\"bad\",\"seq\":4"), kMalformed},
      {"duplicate seq: good first, bad last",
       replace_first(cancel, "\"seq\":4", "\"seq\":4,\"seq\":\"bad\""), kMalformed},
      {"duplicate t with identical values",
       replace_first(cancel, "\"t\":\"cancel_order\"",
                     "\"t\":\"cancel_order\",\"t\":\"cancel_order\""),
       kMalformed},
      // byte-level lexical policy (pinned for reproducible builds): BOM / NUL / UTF-8 shared by
      // both arms
      {"leading UTF-8 BOM", "\xEF\xBB\xBF" + cancel, kMalformed},
      {"trailing raw NUL", cancel + std::string(1, '\0'), kMalformed},
      {"raw NUL between tokens",
       std::string(R"({"t":"cancel_order",)") + '\0' + R"("v":1,"seq":4,"epoch":2,"cl_id":"C-1"})",
       kMalformed},
      {"invalid raw UTF-8 inside a string",
       replace_first(cancel, "C-1", std::string("C-1") + '\xFF'), kMalformed},
      {"overlong UTF-8 encoding", replace_first(cancel, "C-1", std::string("C") + '\xC0' + '\xAF'),
       kMalformed},
      {"raw control byte inside a string", replace_first(cancel, "C-1", std::string("C") + '\x01'),
       kMalformed},
      {"escaped NUL in cl_id rejects (identifier byte policy, a review)",
       replace_first(cancel, "C-1", "a\\u0000b"), kMalformed},
      {"escaped DEL in cl_id rejects", replace_first(cancel, "C-1", "a\\u007Fb"), kMalformed},
      {"escaped control characters stay legal inside an UNKNOWN value",
       with_unknown("\"x\":\"a\\u0000b\""), kAccept},
      // nesting depth guard (pinned for reproducible builds)
      {"nesting at the shared depth limit", nested_cancel(mm::detail::kMaxNestingDepth - 1),
       kAccept},
      {"nesting one level past the limit", nested_cancel(mm::detail::kMaxNestingDepth), kMalformed},
      // number-token grammar in UNKNOWN fields: the shared preflight is
      // the grammar authority and validates numbers LEXICALLY, so a grammar-valid
      // number of ANY magnitude is skippable. Without the shared pass the arms diverge
      // in OPPOSITE directions: nlohmann materializes doubles (rejecting 1e309 and
      // 400-digit tokens) while glaze's validator rejects 0e0 but passes 1e309.
      {"unknown 0e0 (glaze validator quirk)", with_unknown(R"("x":0e0)"), kAccept},
      {"unknown 0e9", with_unknown(R"("x":0e9)"), kAccept},
      {"unknown -0e0", with_unknown(R"("x":-0e0)"), kAccept},
      {"unknown 0E0", with_unknown(R"("x":0E0)"), kAccept},
      {"unknown 0.0e0", with_unknown(R"("x":0.0e0)"), kAccept},
      {"unknown 1e309 (nlohmann double overflow)", with_unknown(R"("x":1e309)"), kAccept},
      {"unknown 1e999999", with_unknown(R"("x":1e999999)"), kAccept},
      {"unknown 9.9e308", with_unknown(R"("x":9.9e308)"), kAccept},
      {"unknown 400-digit integer", with_unknown("\"x\":1" + std::string(400, '7')), kAccept},
      {"unknown numbers nested in objects and arrays",
       with_unknown(R"("x":{"deep":[0e0,1e309,[9.9e308]],"z":0E7})"), kAccept},
      // grammar-INVALID number spellings stay rejected by BOTH arms, anywhere
      {"unknown 01 has a leading zero", with_unknown(R"("x":01)"), kMalformed},
      {"unknown +1", with_unknown(R"("x":+1)"), kMalformed},
      {"unknown .5", with_unknown(R"("x":.5)"), kMalformed},
      {"unknown 5.", with_unknown(R"("x":5.)"), kMalformed},
      {"unknown 1e lacks exponent digits", with_unknown(R"("x":1e)"), kMalformed},
      {"unknown 1e+", with_unknown(R"("x":1e+)"), kMalformed},
      {"unknown --1", with_unknown(R"("x":--1)"), kMalformed},
      {"unknown 0x10", with_unknown(R"("x":0x10)"), kMalformed},
      {"unknown NaN", with_unknown(R"("x":NaN)"), kMalformed},
      {"unknown Infinity", with_unknown(R"("x":Infinity)"), kMalformed},
      {"grammar-invalid number nested in an unknown array", with_unknown(R"("x":[[01]])"),
       kMalformed},
      // KNOWN integer fields keep the stricter integral-token rule at any magnitude
      {"known seq as 0e0", replace_first(cancel, "\"seq\":4", "\"seq\":0e0"), kMalformed},
      {"known seq as 1e309", replace_first(cancel, "\"seq\":4", "\"seq\":1e309"), kMalformed},
      {"known seq as a 400-digit integer",
       replace_first(cancel, "\"seq\":4", "\"seq\":1" + std::string(400, '7')), kMalformed},
      // escaped-surrogate policy is shared (BOTH libraries reject unpaired surrogate
      // escapes; the preflight keeps that verdict shared for skipped and known values)
      {"unpaired high surrogate escape in an unknown value", with_unknown("\"x\":\"\\uD800\""),
       kMalformed},
      {"valid surrogate pair in a string value", replace_first(cancel, "C-1", "\\uD83D\\uDE00"),
       kAccept},
      // top-level key cap (fixed-capacity preflight scan: no decode-path allocation)
      {"32 top-level keys accepted", cancel_with_keys(32), kAccept},
      {"33 top-level keys rejected", cancel_with_keys(33), kMalformed},
      // int64 range boundaries on the SELECTED schema's i64 fields
      {"px one past int64 max",
       replace_first(norder, "\"px\":499995", "\"px\":9223372036854775808"), kMalformed},
      {"qty one past int64 max",
       replace_first(norder, "\"qty\":100", "\"qty\":9223372036854775808"), kMalformed},
      {"px at int64 max", replace_first(norder, "\"px\":499995", "\"px\":9223372036854775807"),
       kAccept},
      {"px at int64 min", replace_first(norder, "\"px\":499995", "\"px\":-9223372036854775808"),
       kAccept},
      // identifier shape policy: length caps + control-byte rule, shared
      // detail strings — codec.hpp kMaxClIdLen/kMaxSymbolLen/kMaxSideLen
      {"cl_id at the 64-byte cap", replace_first(cancel, "C-1", std::string(64, 'c')), kAccept},
      {"cl_id past the 64-byte cap", replace_first(cancel, "C-1", std::string(65, 'c')),
       kMalformed},
      {"symbol at the 32-byte cap", replace_first(norder, "MOCKUSDT", std::string(32, 's')),
       kAccept},
      {"symbol past the 32-byte cap", replace_first(norder, "MOCKUSDT", std::string(33, 's')),
       kMalformed},
      {"side at the 8-byte cap (semantic BadSide stays engine-owned)",
       replace_first(norder, "\"side\":\"B\"", "\"side\":\"BBBBBBBB\""), kAccept},
      {"side past the 8-byte cap",
       replace_first(norder, "\"side\":\"B\"", "\"side\":\"BBBBBBBBB\""), kMalformed},
      // hostile unknown tag: detail is sanitized (truncated + controls replaced) but the
      // verdict stays UnknownType with byte-identical details across arms
      {"oversized control-laden unknown tag",
       replace_first(cancel, "\"t\":\"cancel_order\"",
                     "\"t\":\"bad\\u0001" + std::string(60, 'X') + "\""),
       mm::RejectCode::UnknownType},
  };
  for (const Case &c : cases) {
    INFO(c.name);
    auto tuned_result = tuned->decode(c.json);
    auto naive_result = naive->decode(c.json);
    const auto *tuned_err = std::get_if<mm::DecodeError>(&tuned_result);
    const auto *naive_err = std::get_if<mm::DecodeError>(&naive_result);
    CHECK((tuned_err != nullptr) == c.expect_reject.has_value());
    CHECK((naive_err != nullptr) == c.expect_reject.has_value());
    if (c.expect_reject && tuned_err && naive_err) {
      CHECK(tuned_err->code == *c.expect_reject);
      CHECK(naive_err->code == *c.expect_reject);
      CHECK(tuned_err->code == naive_err->code);     // the equivalence invariant itself
      CHECK(tuned_err->detail == naive_err->detail); // ONE detail per shared reason
    } else if (!c.expect_reject) {
      // Accept rows compare the DECODED PAYLOADS, not just the verdict: a
      // same-verdict value- or alternative-level differential — the exact class of the
      // R1 S1, where one frame decoded as CANCEL_ORDER on naive and NEW_ORDER on tuned
      // — must fail here, covering unescaping, defaulting and alternative selection.
      const auto *tuned_msg = std::get_if<mm::InMsg>(&tuned_result);
      const auto *naive_msg = std::get_if<mm::InMsg>(&naive_result);
      if (tuned_msg && naive_msg)
        CHECK(*tuned_msg == *naive_msg);
    }
  }
}

TEST_CASE("codec: every required inbound field missing -> Malformed on both arms", "[codec]") {
  // The closed field set is engine-inbound policy (codec.hpp) — including the two fields
  // that HAVE struct defaults (md_seq, post_only), where a dropped extraction would
  // silently default on one arm while the other rejects via error_on_missing_keys
  //.
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);

  SECTION("new_order") {
    const auto field = GENERATE(as<std::string_view>{}, "seq", "epoch", "md_seq", "cl_id", "symbol",
                                "side", "px", "qty", "post_only");
    INFO("missing field = " << field);
    const auto err = require_error(*codec, remove_field(read_file("new_order.json"), field));
    CHECK(err.code == mm::RejectCode::Malformed);
  }
  SECTION("cancel_order") {
    const auto field = GENERATE(as<std::string_view>{}, "seq", "epoch", "cl_id");
    INFO("missing field = " << field);
    const auto err = require_error(*codec, remove_field(read_file("cancel_order.json"), field));
    CHECK(err.code == mm::RejectCode::Malformed);
  }
}

TEST_CASE("codec: make_codec dispatches each CodecKind to its own implementation", "[codec]") {
  // Identity pin: swapping the dispatch branches — or serving one arm for
  // both kinds — kept every behavioral test green while inverting the engine's --codec
  // flag and every naive-vs-tuned attribution in the benchmark docs. RTTI is enabled in all
  // presets; the detail factories are the per-TU normative constructors.
  const auto naive = mm::make_codec(mm::CodecKind::Naive);
  const auto tuned = mm::make_codec(mm::CodecKind::Tuned);
  const auto nlohmann_arm = mm::detail::make_codec_nlohmann();
  const auto glaze_arm = mm::detail::make_codec_glaze();
  // Named references: typeid on a smart-pointer dereference would warn (the operator*
  // call counts as a potentially-evaluated side effect).
  const mm::ICodec &naive_obj = *naive;
  const mm::ICodec &tuned_obj = *tuned;
  const mm::ICodec &nlohmann_obj = *nlohmann_arm;
  const mm::ICodec &glaze_obj = *glaze_arm;
  CHECK(typeid(naive_obj) == typeid(nlohmann_obj));
  CHECK(typeid(tuned_obj) == typeid(glaze_obj));
  CHECK(typeid(naive_obj) != typeid(tuned_obj));
}

TEST_CASE("codec: unknown-tag detail is truncated and control-sanitized", "[codec]") {
  // DecodeError.detail feeds the outbound Reject reason and log lines, so a hostile tag
  // must not carry control bytes (log forgery) or attacker-chosen length (allocation /
  // queue amplification) into it — codec.hpp kMaxTagDetailBytes.
  const auto kind = GENERATE(mm::CodecKind::Naive, mm::CodecKind::Tuned);
  INFO("codec = " << kind_name(kind));
  const auto codec = mm::make_codec(kind);

  SECTION("control bytes replaced, overlong tail truncated at the cap") {
    // decoded tag = "bad\x01" + 40*'a' (44 bytes) -> 32 sanitized bytes + U+2026
    const auto err =
        require_error(*codec, "{\"t\":\"bad\\u0001" + std::string(40, 'a') + "\",\"v\":1}");
    CHECK(err.code == mm::RejectCode::UnknownType);
    CHECK(err.detail == "unknown inbound t: bad?" + std::string(28, 'a') + "\xE2\x80\xA6");
  }
  SECTION("truncation cuts on a UTF-8 code-point boundary") {
    // 31*'a' + U+03A9 (2 bytes) = 33 bytes: a 32-byte cut would split the Omega
    const auto err =
        require_error(*codec, "{\"t\":\"" + std::string(31, 'a') + "\xCE\xA9\",\"v\":1}");
    CHECK(err.code == mm::RejectCode::UnknownType);
    CHECK(err.detail == "unknown inbound t: " + std::string(31, 'a') + "\xE2\x80\xA6");
  }
}

TEST_CASE("codec: negative integers and non-ASCII strings encode identically in both arms",
          "[codec]") {
  // No golden fixture carries a negative number or a non-ASCII string, so the sign path
  // of the naive int64 serializer and the two escapers' >= 0x80 passthrough agreement
  // were unpinned: a glaze WriteOpts change enabling \uXXXX escaping of
  // non-ASCII would ship different bytes per arm, and non-ASCII cl_id is reachable (the
  // preflight accepts any valid RFC 3629 sequence and the engine echoes cl_id).
  const auto naive = mm::make_codec(mm::CodecKind::Naive);
  const auto tuned = mm::make_codec(mm::CodecKind::Tuned);
  std::string naive_out, tuned_out;

  SECTION("negative px and leaves on a fill") {
    const mm::OutMsg msg = mm::Fill{.v = 1,
                                    .seq = 11,
                                    .epoch = 2,
                                    .cl_id = "C-1",
                                    .eng_id = 1001,
                                    .px = -499995,
                                    .qty = 100,
                                    .leaves = -5,
                                    .exec_id = 501};
    naive->encode(msg, naive_out);
    tuned->encode(msg, tuned_out);
    CAPTURE(naive_out);
    CHECK(naive_out == tuned_out);
    const auto parsed = nlohmann::json::parse(naive_out);
    CHECK(parsed.at("px").get<std::int64_t>() == -499995);
    CHECK(parsed.at("leaves").get<std::int64_t>() == -5);
  }
  SECTION("multi-byte UTF-8 in a string field") {
    const std::string emoji = "\xF0\x9F\x98\x80\xCE\xA9"; // U+1F600 U+03A9, raw UTF-8
    const mm::OutMsg msg = mm::OrderAck{.v = 1,
                                        .seq = 8,
                                        .epoch = 2,
                                        .cl_id = emoji,
                                        .eng_id = 1001,
                                        .status = "live",
                                        .svc_ns = 1};
    naive->encode(msg, naive_out);
    tuned->encode(msg, tuned_out);
    CAPTURE(naive_out);
    CHECK(naive_out == tuned_out);
    const auto parsed = nlohmann::json::parse(naive_out);
    CHECK(parsed.at("cl_id").get<std::string>() == emoji); // exact original bytes round-trip
  }
}

TEST_CASE("codec: inbound golden fixtures keep declaration key order (byte contract)", "[codec]") {
  // Python msgspec must reproduce the INBOUND fixtures byte-for-byte on encode
  // ("fixtures were written to match" — plan). The outbound fixtures are byte-pinned via
  // encode above; decode is deliberately key-order-insensitive, so nothing else pins the
  // two inbound files' key ORDER against protocol.hpp declaration order.
  const auto keys_of = [](const std::string &text) {
    std::vector<std::string> keys;
    const auto parsed = nlohmann::ordered_json::parse(text);
    for (const auto &item : parsed.items())
      keys.push_back(item.key());
    return keys;
  };
  CHECK(keys_of(read_file("new_order.json")) ==
        std::vector<std::string>{"t", "v", "seq", "epoch", "md_seq", "cl_id", "symbol", "side",
                                 "px", "qty", "post_only"});
  CHECK(keys_of(read_file("cancel_order.json")) ==
        std::vector<std::string>{"t", "v", "seq", "epoch", "cl_id"});
}

// ---------------------------------------------------------------------------
// Direct predicate pins (review mutation finding, final-state review).
//
// The equivalence cases assert only that `"seq":-1` is rejected end-to-end — which stays
// true even if is_unsigned_integer_token loses its sign check, because glaze's from_chars
// and nlohmann's conversion both fail independently downstream. Proven by mutant M6
// (is_unsigned_integer_token -> return is_integer_token(raw)): the whole suite stayed green
// while the predicate answered TRUE for "-1". Library rejection is NOT a pin on the gate;
// these assertions are.
TEST_CASE("codec: integral-token predicates pin the sign gate directly", "[codec]") {
  using mm::detail::is_integer_token;
  using mm::detail::is_unsigned_integer_token;

  SECTION("unsigned predicate rejects every signed form") {
    CHECK(!is_unsigned_integer_token("-1"));
    CHECK(!is_unsigned_integer_token("-0")); // negative zero is still signed syntax
    CHECK(!is_unsigned_integer_token("-9223372036854775808"));
  }
  SECTION("unsigned predicate accepts unsigned integers") {
    CHECK(is_unsigned_integer_token("0"));
    CHECK(is_unsigned_integer_token("1"));
    CHECK(is_unsigned_integer_token("18446744073709551615")); // u64 max: syntax, not range
  }
  SECTION("the signed predicate is deliberately wider — the two are not interchangeable") {
    CHECK(is_integer_token("-1")); // exactly the case the unsigned gate must refuse
    CHECK(is_integer_token("-0"));
    CHECK(is_integer_token("0"));
  }
  SECTION("both reject non-integral syntax") {
    for (std::string_view bad : {"1e3", "0e0", "5.0", "1.", ".5", "+1", "", "-", "1e"}) {
      INFO(bad);
      CHECK(!is_integer_token(bad));
      CHECK(!is_unsigned_integer_token(bad));
    }
  }
  SECTION("leading zeros are NOT this layer's job") {
    // "01" is digits-only, so both predicates accept it — by design. RFC 8259 forbids a
    // leading zero, and frame_preflight's grammar scan rejects the frame long before a
    // token span reaches these predicates. Pinning the division of labour here so a future
    // reader does not "fix" the predicate and silently duplicate the grammar rule.
    CHECK(is_integer_token("01"));
    CHECK(is_unsigned_integer_token("01"));
  }
}
