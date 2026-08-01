// Shared frame authority for BOTH codec arms (contract in codec.hpp): strict UTF-8, the
// iterative RFC 8259 scan, the integral-token predicates, tag/identifier policy. No parser deps.
#include "mm/codec.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mm::detail {
namespace {

// Strict RFC 3629 UTF-8: rejects overlong forms, surrogate code points, values past U+10FFFF,
// stray continuation bytes and truncated tails.
bool utf8_valid(std::string_view s) {
  const auto *p = reinterpret_cast<const unsigned char *>(s.data());
  const auto *const end = p + s.size();
  while (p < end) {
    const unsigned char c = *p;
    if (c < 0x80) {
      ++p;
      continue;
    }
    int len = 0;
    unsigned char lo = 0x80, hi = 0xBF; // bounds for the FIRST continuation byte
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
    } else if (c == 0xE0) {
      len = 3;
      lo = 0xA0;
    } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
      len = 3;
    } else if (c == 0xED) {
      len = 3;
      hi = 0x9F;
    } else if (c == 0xF0) {
      len = 4;
      lo = 0x90;
    } else if (c >= 0xF1 && c <= 0xF3) {
      len = 4;
    } else if (c == 0xF4) {
      len = 4;
      hi = 0x8F;
    } else {
      return false; // 0x80..0xC1 stray continuation / overlong lead, 0xF5..0xFF
    }
    if (end - p < len || p[1] < lo || p[1] > hi)
      return false;
    for (int k = 2; k < len; ++k)
      if (p[k] < 0x80 || p[k] > 0xBF)
        return false;
    p += len;
  }
  return true;
}

constexpr bool is_dec_digit(unsigned char c) { return c >= '0' && c <= '9'; }

// ONE iterative full-grammar RFC 8259 pass — the single JSON authority for BOTH arms: number
// tokens are checked by SHAPE, never converted; depth capped via a 64-bit mask, no recursion.
class FrameScanner {
public:
  FrameScanner(std::string_view frame, FrameScan &out) : f_(frame), out_(out) {}

  std::optional<DecodeError> run();

private:
  enum class Tok : std::uint8_t { Ok, Bad, RawControl, UnpairedSurrogate };
  enum class St : std::uint8_t {
    Value,
    ArrValueOrClose,
    ObjKeyOrClose,
    ObjKey,
    ObjColon,
    AfterValue
  };

  [[nodiscard]] DecodeError reject(std::string_view what) const {
    return DecodeError{RejectCode::Malformed,
                       std::string(what) + " (byte " + std::to_string(i_) + ")"};
  }

  void skip_ws() {
    while (i_ < f_.size() && (f_[i_] == ' ' || f_[i_] == '\t' || f_[i_] == '\n' || f_[i_] == '\r'))
      ++i_;
  }

  // RFC 8259 §6, lexically: optional '-', "0" or nonzero-led digits, optional frac (>= 1 digit),
  // optional exponent (optional sign, >= 1 digit). Junk after the token fails structurally ("01").
  bool scan_number() {
    const std::size_t n = f_.size();
    if (f_[i_] == '-')
      ++i_;
    if (i_ >= n || !is_dec_digit(f_[i_]))
      return false;
    if (f_[i_] == '0')
      ++i_;
    else
      while (i_ < n && is_dec_digit(f_[i_]))
        ++i_;
    if (i_ < n && f_[i_] == '.') {
      ++i_;
      if (i_ >= n || !is_dec_digit(f_[i_]))
        return false;
      while (i_ < n && is_dec_digit(f_[i_]))
        ++i_;
    }
    if (i_ < n && (f_[i_] == 'e' || f_[i_] == 'E')) {
      ++i_;
      if (i_ < n && (f_[i_] == '+' || f_[i_] == '-'))
        ++i_;
      if (i_ >= n || !is_dec_digit(f_[i_]))
        return false;
      while (i_ < n && is_dec_digit(f_[i_]))
        ++i_;
    }
    return true;
  }

  // Reads 4 hex digits into a code unit; 0x110000 flags a malformed escape.
  unsigned scan_hex4() {
    if (f_.size() - i_ < 4)
      return 0x110000;
    unsigned cu = 0;
    for (int k = 0; k < 4; ++k, ++i_) {
      const char c = f_[i_];
      unsigned d = 0;
      if (c >= '0' && c <= '9')
        d = static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        d = static_cast<unsigned>(c - 'a') + 10;
      else if (c >= 'A' && c <= 'F')
        d = static_cast<unsigned>(c - 'A') + 10;
      else
        return 0x110000;
      cu = cu << 4 | d;
    }
    return cu;
  }

  // `i_` at the opening quote; on Ok leaves it past the closing quote. Escapes are validated,
  // surrogate pairing included (both libraries reject unpaired ones, so the shared scan must).
  Tok scan_string() {
    const std::size_t n = f_.size();
    ++i_;
    while (i_ < n) {
      const auto c = static_cast<unsigned char>(f_[i_]);
      if (c == '"') {
        ++i_;
        return Tok::Ok;
      }
      if (c < 0x20)
        return Tok::RawControl;
      if (c != '\\') {
        ++i_;
        continue;
      }
      if (++i_ >= n)
        return Tok::Bad;
      switch (f_[i_]) {
      case '"':
      case '\\':
      case '/':
      case 'b':
      case 'f':
      case 'n':
      case 'r':
      case 't':
        ++i_;
        break;
      case 'u': {
        ++i_;
        const unsigned cu = scan_hex4();
        if (cu > 0xFFFF)
          return Tok::Bad;
        if (cu >= 0xDC00 && cu <= 0xDFFF)
          return Tok::UnpairedSurrogate;    // low surrogate with no preceding high
        if (cu >= 0xD800 && cu <= 0xDBFF) { // high surrogate: a low one must follow
          if (n - i_ < 2 || f_[i_] != '\\' || f_[i_ + 1] != 'u')
            return Tok::UnpairedSurrogate;
          i_ += 2;
          const unsigned lo = scan_hex4();
          if (lo > 0xFFFF)
            return Tok::Bad;
          if (lo < 0xDC00 || lo > 0xDFFF)
            return Tok::UnpairedSurrogate;
        }
        break;
      }
      default:
        return Tok::Bad; // not a legal JSON escape
      }
    }
    return Tok::Bad; // unterminated string
  }

  [[nodiscard]] DecodeError string_error(Tok t) const {
    return reject(t == Tok::RawControl          ? "raw control byte inside a string"
                  : t == Tok::UnpairedSurrogate ? "unpaired surrogate escape in a string"
                                                : "invalid string escape or unterminated string");
  }

  // A value just ended at `i_`: record a root-object field span, then pop to AfterValue
  // or finish the top-level value.
  std::optional<DecodeError> value_done() {
    if (depth_ == 1 && have_pending_key_) {
      // Invariant: `value_start_` is set on ENTRY to every depth-1 value and consumed exactly
      // once, here — the container-close paths decrement `depth_` before calling in.
      assert(value_start_ < i_);
      if (out_.count == kMaxTopLevelKeys)
        return reject("too many top-level keys");
      out_.fields[out_.count] = {pending_key_, f_.substr(value_start_, i_ - value_start_)};
      ++out_.count;
      have_pending_key_ = false;
    }
    if (depth_ == 0)
      done_ = true;
    else
      st_ = St::AfterValue;
    return std::nullopt;
  }

  std::string_view f_;
  FrameScan &out_;
  std::size_t i_ = 0;
  std::size_t depth_ = 0;
  std::uint64_t object_mask_ = 0;  // bit d set: the container opened AT depth d is an object
  std::string_view pending_key_{}; // root-object key awaiting its value
  bool have_pending_key_ = false;
  std::size_t value_start_ = 0;
  bool root_is_object_ = false;
  bool done_ = false;
  St st_ = St::Value;
};

std::optional<DecodeError> FrameScanner::run() {
  static_assert(kMaxNestingDepth <= 64, "container-kind mask is 64 bits");
  skip_ws();
  if (i_ >= f_.size())
    return reject("empty frame");
  root_is_object_ = f_[i_] == '{';

  while (!done_) {
    skip_ws();
    if (i_ >= f_.size())
      return reject("truncated frame");
    const auto c = static_cast<unsigned char>(f_[i_]);
    if (c < 0x20)
      return reject("raw control byte outside a string");
    switch (st_) {
    case St::ObjKeyOrClose:
      if (c == '}') { // empty object
        ++i_;
        --depth_;
        if (auto err = value_done())
          return err;
        break;
      }
      [[fallthrough]];
    case St::ObjKey: {
      if (c != '"')
        return reject("expected a string key");
      const std::size_t key_start = i_ + 1;
      if (const Tok t = scan_string(); t != Tok::Ok)
        return string_error(t);
      if (depth_ == 1 && root_is_object_) {
        pending_key_ = f_.substr(key_start, i_ - 1 - key_start);
        have_pending_key_ = true;
      }
      st_ = St::ObjColon;
      break;
    }
    case St::ObjColon:
      if (c != ':')
        return reject("expected ':'");
      ++i_;
      st_ = St::Value;
      break;
    case St::ArrValueOrClose:
      if (c == ']') { // empty array
        ++i_;
        --depth_;
        if (auto err = value_done())
          return err;
        break;
      }
      [[fallthrough]];
    case St::Value:
      if (depth_ == 1 && root_is_object_)
        value_start_ = i_;
      switch (c) {
      case '{':
      case '[':
        if (depth_ == kMaxNestingDepth)
          return reject("nesting depth exceeds the limit");
        if (c == '{')
          object_mask_ |= std::uint64_t{1} << depth_;
        else
          object_mask_ &= ~(std::uint64_t{1} << depth_);
        ++depth_;
        ++i_;
        st_ = c == '{' ? St::ObjKeyOrClose : St::ArrValueOrClose;
        break;
      case '"':
        if (const Tok t = scan_string(); t != Tok::Ok)
          return string_error(t);
        if (auto err = value_done())
          return err;
        break;
      case 't':
      case 'f':
      case 'n': {
        const std::string_view lit = c == 't' ? "true" : c == 'f' ? "false" : "null";
        if (f_.substr(i_, lit.size()) != lit)
          return reject("invalid literal");
        i_ += lit.size();
        if (auto err = value_done())
          return err;
        break;
      }
      default:
        if (c != '-' && !is_dec_digit(c))
          return reject("invalid number token");
        if (!scan_number())
          return reject("invalid number token");
        if (auto err = value_done())
          return err;
        break;
      }
      break;
    case St::AfterValue: { // depth >= 1 here: depth 0 exits through `done_`
      const bool in_object = (object_mask_ >> (depth_ - 1)) & 1U;
      if (c == ',') {
        ++i_;
        st_ = in_object ? St::ObjKey : St::Value;
      } else if (c == (in_object ? '}' : ']')) {
        ++i_;
        --depth_;
        if (auto err = value_done())
          return err;
      } else {
        return reject("expected ',' or a matching close bracket");
      }
      break;
    }
    }
  }

  skip_ws();
  if (i_ < f_.size()) // trailing garbage / a second smuggled frame
    return reject(static_cast<unsigned char>(f_[i_]) < 0x20 ? "raw control byte outside a string"
                                                            : "trailing content after the frame");
  if (!root_is_object_)
    return reject("root is not an object");
  return std::nullopt;
}

// One identifier rule, one detail string per (rule, field) — shared by both arms so the
// DecodeError.detail equality invariant holds (contract in codec.hpp).
std::optional<DecodeError> check_identifier(std::string_view value, std::string_view name,
                                            std::size_t cap) {
  if (value.size() > cap)
    return DecodeError{RejectCode::Malformed, std::string(name) + " exceeds the length cap"};
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F)
      return DecodeError{RejectCode::Malformed, "control byte in " + std::string(name)};
  }
  return std::nullopt;
}

} // namespace

// Every numeric field is an integer count of ticks/lots/sequence, so a KNOWN integer field's
// token must be integral. Both arms apply this before extraction; only RANGE stays per-arm.
bool is_integer_token(std::string_view raw) noexcept {
  if (raw.starts_with('-'))
    raw.remove_prefix(1);
  return !raw.empty() && raw.find_first_not_of("0123456789") == std::string_view::npos;
}

bool is_unsigned_integer_token(std::string_view raw) noexcept {
  return !raw.empty() && raw.find_first_not_of("0123456789") == std::string_view::npos;
}

// The peer tag is attacker-sized and reaches Reject reasons and log lines: truncate on a UTF-8
// boundary, mark the cut, and replace control bytes with '?' (CWE-117 log forgery, CWE-400).
std::string sanitized_tag(std::string_view tag) {
  std::size_t take = tag.size() <= kMaxTagDetailBytes ? tag.size() : kMaxTagDetailBytes;
  while (take > 0 && take < tag.size() && (static_cast<unsigned char>(tag[take]) & 0xC0) == 0x80)
    --take; // never cut a multi-byte sequence in half
  std::string out;
  out.reserve(take + 3);
  for (const char ch : tag.substr(0, take)) {
    const auto c = static_cast<unsigned char>(ch);
    out.push_back(c < 0x20 || c == 0x7F ? '?' : ch);
  }
  if (take < tag.size())
    out.append("\xE2\x80\xA6"); // U+2026 ellipsis: the tag was truncated
  return out;
}

std::optional<DecodeError> check_inbound_strings(const NewOrder &o) {
  if (auto err = check_identifier(o.cl_id, "cl_id", kMaxClIdLen))
    return err;
  if (auto err = check_identifier(o.symbol, "symbol", kMaxSymbolLen))
    return err;
  return check_identifier(o.side, "side", kMaxSideLen);
}

std::optional<DecodeError> check_inbound_strings(const CancelOrder &c) {
  return check_identifier(c.cl_id, "cl_id", kMaxClIdLen);
}

std::optional<DecodeError> frame_preflight(std::string_view frame, FrameScan &scan) {
  if (frame.starts_with("\xEF\xBB\xBF"))
    return DecodeError{RejectCode::Malformed, "UTF-8 BOM"};
  if (!utf8_valid(frame))
    return DecodeError{RejectCode::Malformed, "invalid UTF-8"};
  if (auto err = FrameScanner(frame, scan).run())
    return err;
  // Literal-ASCII key policy first: with escapes rejected, raw-byte span equality below
  // IS semantic key equality — no unescaping needed to detect duplicates.
  for (std::size_t a = 0; a < scan.count; ++a)
    if (scan.fields[a].key.find('\\') != std::string_view::npos)
      return DecodeError{RejectCode::Malformed, "escape sequence in a top-level key"};
  for (std::size_t a = 1; a < scan.count; ++a) // <= 32 keys: the quadratic scan is trivial
    for (std::size_t b = 0; b < a; ++b)        // and never mutates the recorded spans
      if (scan.fields[a].key == scan.fields[b].key)
        return DecodeError{RejectCode::Malformed, "duplicate top-level key"};
  return std::nullopt;
}

} // namespace mm::detail
