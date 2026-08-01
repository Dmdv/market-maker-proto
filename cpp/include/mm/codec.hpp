// Dual JSON codec interface: one engine binary, two implementations — CodecKind::Naive
// (nlohmann DOM extraction of known fields) and CodecKind::Tuned (glaze typed read, C++23 TU).
//
// Decode contract (identical for both codecs), enforced in this order: shared frame preflight ->
// "t" present, string and known -> "v" present, unsigned and == 1 -> every target-struct field.
//
// Every field of the selected schema must be present and correctly typed, and its integer fields
// need integral wire tokens: 1e3 and 5.0 are Malformed. Unknown fields are never token-checked.
//
// detail::is_integer_token / is_unsigned_integer_token is the ONE expression of "integral token",
// applied by both arms to preflight spans before any extraction, so no library quirk decides it.
//
// Identifier shape: decoded cl_id / symbol / side must fit kMaxClIdLen / kMaxSymbolLen /
// kMaxSideLen bytes and hold no byte < 0x20 or 0x7F, even escaped (CWE-158/117/770).
//
// Error precedence: preflight -> unknown type -> unsupported version -> field errors -> shape.
//
// A DecodeError is raised BEFORE the session's inbound sequencer runs, so the Reject it becomes
// does not consume the frame's envelope seq and the peer must reuse it (see mm/server.hpp).
//
// All struct fields are required on engine-side inbound decode, md_seq and post_only included:
// the closed field set is acceptance policy — a partial message is indistinguishable from a bug.
//
// Unknown keys are ignored (additive forward compatibility) and their values never inspected
// past the grammar pass: the two libraries disagree about extreme numbers in opposite directions.
//
// The UnknownType detail renders the peer tag through detail::sanitized_tag, because detail feeds
// the outbound Reject reason and log lines (CWE-117/116 forgery, CWE-400 amplification).
//
// Both arms return the SAME DecodeError.detail for the same rejection, so a log line does not
// depend on the selected codec. Decode never throws on peer input.
//
// Semantic validation (tick/lot/range/session rules) stays in the engine on top of
// validate_order; MsgTooLarge is enforced at the transport (read_message_max), never here.
//
// Shared frame policy: detail::frame_preflight is ONE code path both arms run before any library
// parser touches the input, so the arms cannot drift. Every rejection below is Malformed.
//
// Grammar authority: one iterative byte scan validates the ENTIRE frame against RFC 8259, so no
// library quirk can decide acceptance. Number tokens are validated lexically, NEVER converted.
//
// Exactly one top-level value with nothing but whitespace after it, escaped surrogates in valid
// pairs, and an object at the root (checked after the grammar pass, which outranks it).
//
// Nesting: at most detail::kMaxNestingDepth open {/[ levels, counted iteratively — that is what
// bounds both parsers' recursion, so a small deeply nested frame cannot exhaust the stack.
//
// Bytes: strict RFC 3629 UTF-8, no BOM (RFC 8259 §8.1), raw control bytes < 0x20 rejected
// everywhere. Escaped control characters stay legal here; the known identifier fields reject them.
//
// Top-level keys: literal ASCII only (so "\u0074" is not "t"), no duplicates, at most
// detail::kMaxTopLevelKeys of them. Keys inside nested values get the grammar pass only.
//
// The scan records the root object's key/value spans in a fixed-capacity FrameScan (no heap
// allocation on the decode path); those spans double as the naive arm's field index.
//
// Encode contract (both codecs; golden fixtures pin every message): canonical wire form — "t"
// first, then members in protocol.hpp declaration order, no whitespace, control chars escaped.
//
// encode() writes into the caller's buffer and does not allocate once its capacity is warmed. It
// cannot fail for a well-formed OutMsg; on any serializer error the buffer is left EMPTY.
//
// Empty output IS the failure signal, and callers MUST NOT write it to the wire: treat it as an
// internal error, drop the message and close the session.
//
// Decode stays key-order-insensitive: canonical order is an encode-side property.
#pragma once

#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace mm {

enum class CodecKind { Naive, Tuned };

struct DecodeError {
  RejectCode code; // UnsupportedVersion | UnknownType | Malformed
  std::string detail;
};

class ICodec {
public:
  virtual ~ICodec() = default;
  virtual std::variant<InMsg, DecodeError> decode(std::string_view json) = 0;
  virtual void encode(const OutMsg &,
                      std::string &out) = 0; // reuses caller buffer: no alloc steady-state
};

std::unique_ptr<ICodec> make_codec(CodecKind);

namespace detail {
// Per-TU factories: the glaze implementation lives in the single C++23 TU
// (cpp/src/codec_glaze.cpp); make_codec dispatches from the C++20 side.
std::unique_ptr<ICodec> make_codec_nlohmann();
std::unique_ptr<ICodec> make_codec_glaze();

inline constexpr std::size_t kMaxNestingDepth = 32;
inline constexpr std::size_t kMaxTopLevelKeys = 32;

// Identifier shape caps (decode contract above). kMaxSideLen is 8, not 1, so a length error
// stays distinct from the engine-owned BadSide verdict; kMaxTagDetailBytes bounds sanitized_tag.
inline constexpr std::size_t kMaxClIdLen = 64;
inline constexpr std::size_t kMaxSymbolLen = 32;
inline constexpr std::size_t kMaxSideLen = 8;
inline constexpr std::size_t kMaxTagDetailBytes = 32;

// The root object's top-level fields as raw spans into the frame (key between its quotes, value
// as one complete JSON value slice). FIXED capacity: no heap on the decode path; over-cap rejects.
struct TopLevelField {
  std::string_view key, value;
};
struct FrameScan {
  std::array<TopLevelField, kMaxTopLevelKeys> fields{};
  std::size_t count = 0;
  // Value span for a top-level key; empty when absent (a present field's value span is
  // never empty — the grammar guarantees at least one byte).
  [[nodiscard]] std::string_view find(std::string_view key) const noexcept {
    for (std::size_t i = 0; i < count; ++i)
      if (fields[i].key == key)
        return fields[i].value;
    return {};
  }
};

// Shared frame preflight (policy above): both arms call this FIRST in decode(), ahead of the
// library parser. Returns nullopt on pass, with `scan` filled; otherwise the Malformed error.
std::optional<DecodeError> frame_preflight(std::string_view frame, FrameScan &scan);

// The ONE "integral token" predicate pair (decode contract above): both arms apply these to
// FrameScan spans before any library extraction, so only RANGE verdicts stay with each arm.
[[nodiscard]] bool is_integer_token(std::string_view raw) noexcept;
[[nodiscard]] bool is_unsigned_integer_token(std::string_view raw) noexcept;

// Renders a peer-supplied tag safely for DecodeError.detail: truncated to kMaxTagDetailBytes on
// a UTF-8 code-point boundary (U+2026 marks the cut), control bytes (< 0x20, 0x7F) become '?'.
[[nodiscard]] std::string sanitized_tag(std::string_view tag);

// Identifier shape policy (decode contract above): length caps + control-byte rejection on the
// decoded cl_id / symbol / side. ONE implementation shared by both arms. nullopt = passes.
[[nodiscard]] std::optional<DecodeError> check_inbound_strings(const NewOrder &o);
[[nodiscard]] std::optional<DecodeError> check_inbound_strings(const CancelOrder &c);
} // namespace detail

} // namespace mm
