// Naive codec arm: nlohmann DOM extraction of known fields (plan Task 2 — "naive but
// correct"). This is the measured baseline; the tuned glaze arm must agree on every
// decode/encode behavior (tests pin both against the same golden fixtures).
//
// encode() is a direct declaration-order serializer, NOT DOM build + dump() (codex R1):
// the ICodec contract — caller buffer, no steady-state allocation, canonical wire-key
// order — is normative for BOTH arms, and the dump() path can satisfy none of it (fresh
// DOM nodes + a freshly allocated result string + alphabetically sorted keys).
//
// decode() extracts each KNOWN field by DOM-parsing that field's raw span from the
// shared preflight's FrameScan. Unknown values are never handed to nlohmann at all
// (grok R1, S2): nlohmann MATERIALIZES numbers into doubles, so a whole-frame parse
// rejected out-of-range tokens (1e309, 400-digit integers) in fields the contract says
// are IGNORED — while the tuned arm's validator disagreed in the opposite direction.
// The shared preflight (frame_preflight.cpp) is the single grammar authority and is
// COMMON-MODE across both arms, so the A/B delta measures per-field nlohmann DOM
// extraction vs glaze typed read — not whole-library parse vs parse (codec.hpp).
//
// Also hosts make_codec() — the dispatch lives on the C++20 side so only the single
// C++23 TU, codec_glaze.cpp, ever sees glaze.
#include "mm/codec.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mm {

namespace {

using nlohmann::json;

// DOM-parses ONE known field's span (codec.hpp: FrameScan). Absent key and value that
// nlohmann cannot represent (e.g. an integer token past the double range) both yield
// nullopt — the caller treats either as missing-or-mistyped. UNKNOWN fields are never
// parsed at all: the shared preflight validated their grammar, and their content —
// notably a number's magnitude, which nlohmann materializes into a double — must never
// gate acceptance (grok R1, S2).
std::optional<json> parse_field(const detail::FrameScan &scan, const char *key) {
  const auto span = scan.find(key);
  if (span.empty())
    return std::nullopt;
  json j = json::parse(span, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded())
    return std::nullopt;
  return j;
}

// Typed extractors: return false (-> Malformed) when the key is missing or mistyped.
// All fields are required — see the decode contract in codec.hpp.
//
// The integer extractors run the SHARED token predicate on the raw span FIRST (gate
// P4-i1): "integral token" is expressed once, in mm::detail, for both arms — nlohmann
// below only CONVERTS already-accepted tokens, and still owns the naive arm's range
// verdicts (u64 overflow materializes as a double; the explicit int64 guard below).
bool get_u64(const detail::FrameScan &scan, const char *key, std::uint64_t &out) {
  if (!detail::is_unsigned_integer_token(scan.find(key)))
    return false;
  const auto j = parse_field(scan, key);
  if (!j || !j->is_number_unsigned())
    return false;
  out = j->get<std::uint64_t>();
  return true;
}

bool get_i64(const detail::FrameScan &scan, const char *key, std::int64_t &out) {
  if (!detail::is_integer_token(scan.find(key)))
    return false;
  const auto j = parse_field(scan, key);
  if (!j || !j->is_number_integer())
    return false;
  // is_number_integer() is also true for unsigned values; reject the int64-unrepresentable
  // top half instead of letting get<int64_t>() wrap.
  if (j->is_number_unsigned() &&
      j->get<std::uint64_t>() >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return false;
  out = j->get<std::int64_t>();
  return true;
}

bool get_str(const detail::FrameScan &scan, const char *key, std::string &out) {
  const auto j = parse_field(scan, key);
  if (!j || !j->is_string())
    return false;
  out = j->get<std::string>();
  return true;
}

bool get_bool(const detail::FrameScan &scan, const char *key, bool &out) {
  const auto j = parse_field(scan, key);
  if (!j || !j->is_boolean())
    return false;
  out = j->get<bool>();
  return true;
}

DecodeError malformed(std::string detail) {
  return DecodeError{RejectCode::Malformed, std::move(detail)};
}

// ---- canonical encode helpers -------------------------------------------------------
// Escape form mirrors the tuned arm's glaze writer BYTE-FOR-BYTE (tests pin the
// equality): \b \t \n \f \r \" \\ shorthand; remaining control bytes as uppercase
// \u00XX; everything >= 0x20 passes through raw (outbound strings are valid UTF-8 by
// construction: decoded from preflight-validated input, or engine constants).
void append_escaped(std::string &out, std::string_view s) {
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
    case '"':
      out.append("\\\"");
      break;
    case '\\':
      out.append("\\\\");
      break;
    case '\b':
      out.append("\\b");
      break;
    case '\t':
      out.append("\\t");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\f':
      out.append("\\f");
      break;
    case '\r':
      out.append("\\r");
      break;
    default:
      if (c < 0x20) {
        constexpr char hex[] = "0123456789ABCDEF";
        out.append("\\u00");
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0xF]);
      } else {
        out.push_back(ch);
      }
    }
  }
}

void append_key(std::string &out, std::string_view key) { // wire keys are literal ASCII
  out.push_back('"');
  out.append(key);
  out.append("\":");
}

void field(std::string &out, std::string_view key, std::uint64_t v) {
  append_key(out, key);
  char buf[24]; // u64 max is 20 digits; i64 min is 20 chars incl. the sign
  const auto res = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, res.ptr);
  out.push_back(',');
}

void field(std::string &out, std::string_view key, std::int64_t v) {
  append_key(out, key);
  char buf[24];
  const auto res = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, res.ptr);
  out.push_back(',');
}

// No bool overload: OutMsg carries no bool field (post_only is inbound-only).

void field(std::string &out, std::string_view key, std::string_view v) {
  append_key(out, key);
  out.push_back('"');
  append_escaped(out, v);
  out.push_back('"');
  out.push_back(',');
}

void open_msg(std::string &out, std::string_view tag) {
  out.append("{\"t\":\"");
  out.append(tag);
  out.append("\",");
}

void close_msg(std::string &out) { out.back() = '}'; } // every message has >= 1 field

void encode_msg(std::string &out, const Tob &m) {
  open_msg(out, kTagTob);
  field(out, "v", m.v);
  field(out, "seq", m.seq);
  field(out, "epoch", m.epoch);
  field(out, "md_seq", m.md_seq);
  field(out, "symbol", m.symbol);
  field(out, "bid_px", m.bid_px);
  field(out, "bid_qty", m.bid_qty);
  field(out, "ask_px", m.ask_px);
  field(out, "ask_qty", m.ask_qty);
  close_msg(out);
}

void encode_msg(std::string &out, const OrderAck &m) {
  open_msg(out, kTagOrderAck);
  field(out, "v", m.v);
  field(out, "seq", m.seq);
  field(out, "epoch", m.epoch);
  field(out, "cl_id", m.cl_id);
  field(out, "eng_id", m.eng_id);
  field(out, "status", m.status);
  field(out, "svc_ns", m.svc_ns);
  close_msg(out);
}

void encode_msg(std::string &out, const CancelAck &m) {
  open_msg(out, kTagCancelAck);
  field(out, "v", m.v);
  field(out, "seq", m.seq);
  field(out, "epoch", m.epoch);
  field(out, "cl_id", m.cl_id);
  field(out, "eng_id", m.eng_id);
  field(out, "status", m.status);
  close_msg(out);
}

void encode_msg(std::string &out, const Reject &m) {
  open_msg(out, kTagReject);
  field(out, "v", m.v);
  field(out, "seq", m.seq);
  field(out, "epoch", m.epoch);
  field(out, "cl_id", m.cl_id);
  field(out, "code", m.code);
  field(out, "reason", m.reason);
  close_msg(out);
}

void encode_msg(std::string &out, const Fill &m) {
  open_msg(out, kTagFill);
  field(out, "v", m.v);
  field(out, "seq", m.seq);
  field(out, "epoch", m.epoch);
  field(out, "cl_id", m.cl_id);
  field(out, "eng_id", m.eng_id);
  field(out, "px", m.px);
  field(out, "qty", m.qty);
  field(out, "leaves", m.leaves);
  field(out, "exec_id", m.exec_id);
  close_msg(out);
}

class NlohmannCodec final : public ICodec {
public:
  std::variant<InMsg, DecodeError> decode(std::string_view input) override {
    // Shared preflight FIRST (codec.hpp): the single grammar authority, so verdicts are
    // identical across arms by construction; its iterative depth cap also bounds the
    // per-field nlohmann parses below (every span inherits the frame's depth budget).
    detail::FrameScan scan;
    if (auto err = detail::frame_preflight(input, scan))
      return std::move(*err);

    // Envelope discipline, in contract order: t (present -> known), then v (present ->
    // unsigned -> == 1), then the full field set of the tagged struct. Extraction is
    // per-KNOWN-field DOM parses over the preflight's spans — unknown values never
    // touch nlohmann, so their content cannot gate acceptance (grok R1, S2).
    const auto t = parse_field(scan, "t");
    if (!t || !t->is_string())
      return malformed("t missing or not a string");
    const auto &tag = t->get_ref<const std::string &>();
    const bool is_new = tag == kTagNewOrder;
    if (!is_new && tag != kTagCancelOrder)
      // The tag is attacker-sized and may carry escaped controls; sanitized_tag bounds
      // and cleans it before it reaches the Reject reason / log line (codec.hpp).
      return DecodeError{RejectCode::UnknownType,
                         "unknown inbound t: " + detail::sanitized_tag(tag)};

    std::uint64_t v{};
    if (!get_u64(scan, "v", v))
      return malformed("v missing or not an unsigned integer");
    if (v != 1)
      return DecodeError{RejectCode::UnsupportedVersion, "v=" + std::to_string(v)};

    if (is_new) {
      NewOrder o;
      o.v = v;
      if (!get_u64(scan, "seq", o.seq) || !get_u64(scan, "epoch", o.epoch) ||
          !get_u64(scan, "md_seq", o.md_seq) || !get_str(scan, "cl_id", o.cl_id) ||
          !get_str(scan, "symbol", o.symbol) || !get_str(scan, "side", o.side) ||
          !get_i64(scan, "px", o.px) || !get_i64(scan, "qty", o.qty) ||
          !get_bool(scan, "post_only", o.post_only))
        return malformed("new_order: missing or mistyped field");
      if (auto err = detail::check_inbound_strings(o)) // identifier shape policy (codec.hpp)
        return std::move(*err);
      return InMsg{std::move(o)};
    }
    CancelOrder c;
    c.v = v;
    if (!get_u64(scan, "seq", c.seq) || !get_u64(scan, "epoch", c.epoch) ||
        !get_str(scan, "cl_id", c.cl_id))
      return malformed("cancel_order: missing or mistyped field");
    if (auto err = detail::check_inbound_strings(c)) // identifier shape policy (codec.hpp)
      return std::move(*err);
    return InMsg{std::move(c)};
  }

  void encode(const OutMsg &msg, std::string &out) override {
    // Canonical wire form straight into the caller's buffer: clear() keeps capacity and
    // the appends reuse it — steady-state allocation-free once warmed (contract in
    // codec.hpp; tests pin capacity AND data-address stability across encodes). This
    // direct serializer has no failure mode, matching the shared encode contract.
    out.clear();
    std::visit([&out](const auto &m) { encode_msg(out, m); }, msg);
  }
};

} // namespace

namespace detail {
std::unique_ptr<ICodec> make_codec_nlohmann() { return std::make_unique<NlohmannCodec>(); }
} // namespace detail

std::unique_ptr<ICodec> make_codec(CodecKind kind) {
  return kind == CodecKind::Naive ? detail::make_codec_nlohmann() : detail::make_codec_glaze();
}

} // namespace mm
