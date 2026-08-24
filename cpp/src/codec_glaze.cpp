// Tuned codec arm: glaze typed read/write (plan Task 2). THE one C++23 TU in the tree —
// glaze requires C++23, so this file is compiled via the mm_codec_glaze OBJECT library
// with CXX_STANDARD 23; nothing glaze-related may leak into a header (mm_core stays C++20).
//
// Encode: glz::write on the tagged OutMsg variant — glaze emits the "t" tag first, then
// the struct members in glz::meta (== declaration) order; the golden fixtures pin that
// byte-for-byte for EVERY outbound message, in BOTH arms. Control characters in strings
// are escaped via glaze's escape_control_characters extension option (codex R1: the
// default writer copies raw 0x00..0x1F bytes into the output — INVALID JSON).
//
// Decode: the shared preflight (mm::detail::frame_preflight — THE grammar authority:
// full RFC 8259 validation with lexical, magnitude-blind number tokens, plus the depth
// cap, UTF-8/BOM/NUL policy and top-level key policy; contract in codec.hpp) runs
// FIRST, then the contract order (t known -> v==1 -> schema-scoped integer-token check
// -> full strict field set -> identifier shape) is enforced on the preflight's
// FrameScan spans — glaze itself runs exactly TWICE per accepted frame (the TagProbe
// and the strict typed read; gate P4-i1 collapsed the former VerProbe/NewOrderInts/
// CancelOrderInts re-reads onto spans the scan already held). The reads skip unknown
// values WITHOUT validating them (grok R1, S2): glaze's validator rejects the
// grammar-valid 0e0 spelling while passing 1e309 unconverted — the opposite of the
// naive arm's nlohmann — so no glaze validation pass may gate acceptance; the shared
// preflight already rejected trailing garbage, smuggled second frames and malformed
// unknown values before glaze sees a byte.
#include "mm/codec.hpp"

#include <glaze/glaze.hpp>

#include <array>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mm::glaze_detail {

// The ONE remaining probe, kept as a real glaze read on purpose (gate P4-i1): the "t"
// span may carry escape sequences that only a JSON string decode unescapes, and the
// naive arm's nlohmann extraction unescapes them too — comparing the RAW span would
// diverge. The nullable member distinguishes "missing" (nullopt) from "mistyped"
// (glaze type error); both are Malformed with the naive arm's exact detail.
struct TagProbe {
  std::optional<std::string> t;
};

} // namespace mm::glaze_detail

// The fence below is deliberate, not formatter debt: one key/member pair per visual
// column keeps every glz::meta aligned with protocol.hpp declaration order, so
// decl order == wire key order stays auditable at a glance.
// clang-format off
template <> struct glz::meta<mm::glaze_detail::TagProbe> {
  using T = mm::glaze_detail::TagProbe;
  static constexpr auto value = object("t", &T::t);
};

// Wire structs: members listed EXPLICITLY in declaration order (protocol.hpp pins
// declaration order == wire key order; relying on reflection would silently re-derive
// the contract instead of stating it).
template <> struct glz::meta<mm::Tob> {
  using T = mm::Tob;
  static constexpr auto value =
      object("v", &T::v, "seq", &T::seq, "epoch", &T::epoch, "md_seq", &T::md_seq,
             "symbol", &T::symbol, "bid_px", &T::bid_px, "bid_qty", &T::bid_qty,
             "ask_px", &T::ask_px, "ask_qty", &T::ask_qty);
};
template <> struct glz::meta<mm::NewOrder> {
  using T = mm::NewOrder;
  static constexpr auto value =
      object("v", &T::v, "seq", &T::seq, "epoch", &T::epoch, "md_seq", &T::md_seq,
             "cl_id", &T::cl_id, "symbol", &T::symbol, "side", &T::side,
             "px", &T::px, "qty", &T::qty, "post_only", &T::post_only);
};
template <> struct glz::meta<mm::CancelOrder> {
  using T = mm::CancelOrder;
  static constexpr auto value =
      object("v", &T::v, "seq", &T::seq, "epoch", &T::epoch, "cl_id", &T::cl_id);
};
template <> struct glz::meta<mm::OrderAck> {
  using T = mm::OrderAck;
  static constexpr auto value =
      object("v", &T::v, "seq", &T::seq, "epoch", &T::epoch, "cl_id", &T::cl_id,
             "eng_id", &T::eng_id, "status", &T::status, "svc_ns", &T::svc_ns);
};
template <> struct glz::meta<mm::CancelAck> {
  using T = mm::CancelAck;
  static constexpr auto value =
      object("v", &T::v, "seq", &T::seq, "epoch", &T::epoch, "cl_id", &T::cl_id,
             "eng_id", &T::eng_id, "status", &T::status);
};
template <> struct glz::meta<mm::Reject> {
  using T = mm::Reject;
  static constexpr auto value =
      object("v", &T::v, "seq", &T::seq, "epoch", &T::epoch, "cl_id", &T::cl_id,
             "code", &T::code, "reason", &T::reason);
};
template <> struct glz::meta<mm::Fill> {
  using T = mm::Fill;
  static constexpr auto value =
      object("v", &T::v, "seq", &T::seq, "epoch", &T::epoch, "cl_id", &T::cl_id,
             "eng_id", &T::eng_id, "px", &T::px, "qty", &T::qty, "leaves", &T::leaves,
             "exec_id", &T::exec_id);
};
// clang-format on

// Tagged variant: glaze writes {"t":"<id>", <members...>}; ids MUST stay aligned with
// the OutMsg alternative order in protocol.hpp.
template <> struct glz::meta<mm::OutMsg> {
  static constexpr std::string_view tag = "t";
  static constexpr auto ids =
      std::array{mm::kTagTob, mm::kTagOrderAck, mm::kTagCancelAck, mm::kTagReject, mm::kTagFill};
};

namespace mm {
namespace {

// decode() takes a string_view into the transport's frame buffer — no null terminator
// guarantee, hence null_terminated=false on every read.
constexpr glz::opts kLax{.null_terminated = false, .error_on_unknown_keys = false};
constexpr glz::opts kStrict{.null_terminated = false,
                            .error_on_unknown_keys = false, // "t" and future additive keys
                            .error_on_missing_keys = true}; // ALL struct fields required

// escape_control_characters is a glaze EXTENSION option (not a member of base glz::opts;
// see opts.hpp — extension options are added by deriving). Without it glaze copies raw
// 0x00..0x1F bytes from string values into the output, producing INVALID JSON for any
// decoded string that carried an escaped control character (codex R1). Escape form:
// \b \t \n \f \r \" \\ shorthand + uppercase hex u-escapes for the rest of 0x00..0x1F —
// the naive arm's serializer mirrors this byte-for-byte (tests pin the equality).
struct WriteOpts : glz::opts {
  bool escape_control_characters = true;
};

DecodeError malformed(std::string detail) {
  return DecodeError{RejectCode::Malformed, std::move(detail)};
}

// A JSON number is grammar-valid as `1e3` or `5.0`, so the preflight cannot reject it —
// but every KNOWN integer field's token must be integral (shared predicate rationale in
// frame_preflight.cpp). Checks the SELECTED schema's fields on the preflight's spans,
// with each field's SIGNEDNESS carried by the caller (gate P4-i2): u64 fields get the
// unsigned predicate, so the tuned arm's sign verdict for them comes from the shared
// predicate exactly as the naive arm's does — only RANGE stays with the strict typed
// read. Missing fields pass here (the strict read owns missing-field errors). Callers
// report both failure kinds with the ONE shared "<schema>: missing or mistyped field"
// detail the naive arm uses (grok R1: shared rejection reasons carry shared details),
// so token-check-vs-strict-read ordering is not observable.
bool integer_fields_are_integral(const detail::FrameScan &scan,
                                 std::initializer_list<std::string_view> signed_keys,
                                 std::initializer_list<std::string_view> unsigned_keys) {
  for (const auto key : signed_keys) {
    if (const auto tok = scan.find(key); !tok.empty() && !detail::is_integer_token(tok))
      return false;
  }
  for (const auto key : unsigned_keys) {
    if (const auto tok = scan.find(key); !tok.empty() && !detail::is_unsigned_integer_token(tok))
      return false;
  }
  return true;
}

class GlazeCodec final : public ICodec {
public:
  std::variant<InMsg, DecodeError> decode(std::string_view input) override {
    // Shared preflight FIRST — and it is the WHOLE frame gate (grok R1, S2): grammar
    // authority (number tokens validated lexically, never converted), root-is-object,
    // trailing-content rejection, depth cap, byte + key policy. The ordering stays
    // load-bearing: the ITERATIVE depth cap is what bounds glaze's recursive unknown-
    // value skips in the reads below — swapping them reintroduces the stack-exhaustion
    // S1. From here on glaze runs on known-valid frames with NON-validating skips, so
    // its 0e0-vs-1e309 validator quirks can no longer decide acceptance.
    detail::FrameScan scan;
    if (auto err = detail::frame_preflight(input, scan))
      return std::move(*err);

    // Fast path: if "t" is the literal unescaped wire tag, resolve the schema directly
    // from the preflight span without a full-frame glaze TagProbe pass. If absent,
    // escaped, or unknown, fall back to glz::read<kLax>(tag, input) to preserve exact
    // escape decoding and error wording parity with the naive arm.
    const auto ttok = scan.find("t");
    bool is_new = false;
    if (ttok == "\"new_order\"") [[likely]] {
      is_new = true;
    } else if (ttok == "\"cancel_order\"") {
      is_new = false;
    } else {
      glaze_detail::TagProbe tag{};
      if (glz::read<kLax>(tag, input) || !tag.t)
        return malformed("t missing or not a string");
      is_new = *tag.t == kTagNewOrder;
      if (!is_new && *tag.t != kTagCancelOrder)
        // The tag is attacker-sized and may carry escaped controls; sanitized_tag bounds
        // and cleans it before it reaches the Reject reason / log line (codec.hpp).
        return DecodeError{RejectCode::UnknownType,
                           "unknown inbound t: " + detail::sanitized_tag(*tag.t)};
    }

    // v comes straight from the scan's span (gate P4-i1 — no glaze re-read): the naive
    // arm classifies a non-integer token ("v":2e0, "v":"1") as Malformed, never
    // UnsupportedVersion, so token validity is checked BEFORE the version compare, on
    // the exact wire bytes. from_chars owns the u64 range verdict.
    const auto vtok = scan.find("v");
    std::uint64_t v{};
    if (vtok.empty() || !detail::is_unsigned_integer_token(vtok) ||
        std::from_chars(vtok.data(), vtok.data() + vtok.size(), v).ec != std::errc{})
      return malformed("v missing or not an unsigned integer");
    if (v != 1)
      return DecodeError{RejectCode::UnsupportedVersion, "v=" + std::to_string(v)};

    // Field errors come LAST (documented precedence): only the selected schema's integer
    // fields are token-checked — an unknown "px" on a cancel_order is ignored exactly as
    // the naive arm ignores it (codex R1). The identifier shape policy (codec.hpp) runs
    // after the strict read, on the decoded strings, exactly as in the naive arm.
    if (is_new) {
      if (!integer_fields_are_integral(scan, /*signed_keys=*/{"px", "qty"},
                                       /*unsigned_keys=*/{"seq", "epoch", "md_seq"}))
        return malformed("new_order: missing or mistyped field");
      NewOrder o;
      if (glz::read<kStrict>(o, input))
        return malformed("new_order: missing or mistyped field");
      if (auto err = detail::check_inbound_strings(o))
        return std::move(*err);
      return InMsg{std::move(o)};
    }
    if (!integer_fields_are_integral(scan, /*signed_keys=*/{},
                                     /*unsigned_keys=*/{"seq", "epoch"}))
      return malformed("cancel_order: missing or mistyped field");
    CancelOrder c;
    if (glz::read<kStrict>(c, input))
      return malformed("cancel_order: missing or mistyped field");
    if (auto err = detail::check_inbound_strings(c))
      return std::move(*err);
    return InMsg{std::move(c)};
  }

  void encode(const OutMsg &msg, std::string &out) override {
    // Reuses the caller buffer's capacity: no allocation steady-state (glaze resizes the
    // string but never shrinks capacity; tests pin capacity + data-address stability).
    const auto ec = glz::write<WriteOpts{}>(msg, out);
    if (ec) [[unlikely]] {
      // Writing these aggregate types cannot fail — but if it ever does, never ship a
      // partial frame: clear() leaves the documented empty-buffer failure signal
      // (codec.hpp) in EVERY build type, where a bare assert() would vanish under the
      // rel preset's -DNDEBUG and let a truncated buffer through (grok R1). The assert
      // still keeps dev builds loud.
      out.clear();
      assert(false && "glz::write failed on an OutMsg");
    }
  }
};

} // namespace

namespace detail {
std::unique_ptr<ICodec> make_codec_glaze() { return std::make_unique<GlazeCodec>(); }
} // namespace detail

} // namespace mm
