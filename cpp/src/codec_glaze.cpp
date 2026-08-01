// Tuned codec arm: glaze typed read/write. THE one C++23 TU (glaze needs C++23; mm_core stays
// C++20). Shared preflight owns the grammar; encode output is byte-identical to the naive arm.
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

// A real glaze read, not a raw span compare: the "t" value may carry escapes that only a JSON
// string decode unescapes. nullopt distinguishes "missing" from "mistyped"; both are Malformed.
struct TagProbe {
  std::optional<std::string> t;
};

} // namespace mm::glaze_detail

// One key/member pair per visual column: decl order == wire key order stays auditable.
// clang-format off
template <> struct glz::meta<mm::glaze_detail::TagProbe> {
  using T = mm::glaze_detail::TagProbe;
  static constexpr auto value = object("t", &T::t);
};

// Members listed EXPLICITLY, not by reflection: protocol.hpp pins decl order == wire key order.
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

// Tagged variant: glaze writes {"t":"<id>", ...}; ids MUST match OutMsg's alternative order.
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

// escape_control_characters is a glaze EXTENSION option (added by deriving, not a glz::opts
// member); without it raw 0x00..0x1F bytes reach the output. Naive arm mirrors it byte-for-byte.
struct WriteOpts : glz::opts {
  bool escape_control_characters = true;
};

DecodeError malformed(std::string detail) {
  return DecodeError{RejectCode::Malformed, std::move(detail)};
}

// `1e3` and `5.0` are grammar-valid JSON numbers, so only a KNOWN integer field's token can
// reject them. Signedness comes from the caller; missing fields pass (the strict read owns those).
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
    // Shared preflight FIRST — it is the WHOLE frame gate. The order is load-bearing: its
    // ITERATIVE depth cap bounds glaze's recursive unknown-value skips in the reads below.
    detail::FrameScan scan;
    if (auto err = detail::frame_preflight(input, scan))
      return std::move(*err);

    // On a preflight-valid frame a probe read error can only mean its one key is mistyped.
    glaze_detail::TagProbe tag{};
    if (glz::read<kLax>(tag, input) || !tag.t)
      return malformed("t missing or not a string");
    const bool is_new = *tag.t == kTagNewOrder;
    if (!is_new && *tag.t != kTagCancelOrder)
      // The tag is attacker-sized and may carry escaped controls; sanitized_tag bounds
      // and cleans it before it reaches the Reject reason / log line (codec.hpp).
      return DecodeError{RejectCode::UnknownType,
                         "unknown inbound t: " + detail::sanitized_tag(*tag.t)};

    // Token validity is checked on the raw span BEFORE the version compare: a non-integer
    // ("v":2e0, "v":"1") is Malformed, never UnsupportedVersion. from_chars owns the range.
    const auto vtok = scan.find("v");
    std::uint64_t v{};
    if (vtok.empty() || !detail::is_unsigned_integer_token(vtok) ||
        std::from_chars(vtok.data(), vtok.data() + vtok.size(), v).ec != std::errc{})
      return malformed("v missing or not an unsigned integer");
    if (v != 1)
      return DecodeError{RejectCode::UnsupportedVersion, "v=" + std::to_string(v)};

    // Field errors come LAST: only the SELECTED schema's integer fields are token-checked, so an
    // unknown "px" on a cancel_order is ignored. Identifier shape runs after the strict read.
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
      // Never ship a partial frame: clear() leaves the documented empty-buffer failure signal
      // in EVERY build type, where a bare assert() would vanish under -DNDEBUG.
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
