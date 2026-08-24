// Dual codec interface (plan Task 2). One engine binary, two implementations:
//   CodecKind::Naive — nlohmann DOM extraction of known fields (the baseline arm)
//   CodecKind::Tuned — glaze typed read/write (C++23 TU)
//
// A/B honesty note (gate P4-i1; supersedes plan Task 2 Step 4's "whole-frame
// json::parse" wording): BOTH arms run the SAME shared frame preflight before any
// library parser touches the input, so the preflight cost is COMMON-MODE. The measured
// naive-vs-tuned delta is therefore per-field nlohmann DOM extraction vs glaze typed
// read — NOT whole-library parse vs parse — and Tasks 13/14 must disclose that in the
// "codec C++: nlohmann->glaze" row. "Tuned = the faster arm" is an unverified
// expectation until Task 13's codec microbenchmark exists.
//
// Decode contract (identical for both codecs; tests pin every branch):
//   Enforced in order: shared frame preflight (-> Malformed: grammar, byte policy,
//   key policy, root-is-object — see below), "t" present+string (-> Malformed) and
//   known-inbound (-> UnknownType), "v" present+unsigned-integer TOKEN (-> Malformed)
//   and == 1 (-> UnsupportedVersion), then EVERY field of the target struct present and
//   correctly typed (-> Malformed) — for the SELECTED schema's integer fields the wire
//   token itself must be integral (float/exponent forms like 1e3 or 5.0 are Malformed;
//   unknown fields are never token-checked; the predicate pair detail::is_integer_token/
//   is_unsigned_integer_token is the ONE expression of "integral token", applied by BOTH
//   arms to the preflight's spans before any library extraction) — and finally the
//   identifier shape policy (-> Malformed): decoded cl_id / symbol / side must fit
//   detail::kMaxClIdLen (64) / kMaxSymbolLen (32) / kMaxSideLen (8) BYTES and must not
//   contain bytes < 0x20 or 0x7F even via ESCAPED spellings — identifiers key the
//   engine's order map, are echoed on every report and land in log/telemetry text, where
//   an embedded NUL truncates C-string sinks (two cl_ids collide) and CR/LF forges
//   records (CWE-158/117); an unbounded cl_id would also be retained per order for the
//   session lifetime (CWE-770). kMaxSideLen is deliberately generous (8, not 1) so the
//   semantic BadSide verdict stays engine-owned in validate_order. Error precedence is
//   exactly: preflight -> unknown type -> unsupported version -> field errors ->
//   identifier shape.
//   A DecodeError is raised BEFORE the session's inbound sequencer runs, so the Reject it
//   becomes does not consume the frame's envelope seq and the peer must reuse it — the
//   mirror image of every engine-side reject. That covers every DecodeError code
//   (MALFORMED, UNKNOWN_TYPE, UNSUPPORTED_VERSION) plus the transport-layer MSG_TOO_LARGE
//   path that never reaches the codec. mm/server.hpp's "INBOUND SEQ CONSUMPTION AFTER A
//   REJECT" states the whole rule; it is a client obligation, enforced by a 1002.
//   All struct fields are required ON ENGINE-SIDE INBOUND DECODE — including md_seq and
//   post_only; the closed field set is engine-inbound acceptance policy and partial
//   messages are indistinguishable from bugs. Scope note (gate P4-i1): the plan's Task 4
//   Python NewOrder Struct declares `post_only: bool = True` — a CONSTRUCTION-side
//   convenience for the Python encoder; msgspec still emits the field on every encode
//   (the golden fixture pins the bytes), so wire frames always carry post_only and the
//   deliberate Python decode-side leniency never reaches this engine. The closed set is
//   NOT an obligation on Python's decoders.
//   The UnknownType detail renders the peer tag via detail::sanitized_tag — truncated to
//   detail::kMaxTagDetailBytes (32) on a UTF-8 code-point boundary with a U+2026 marker,
//   control bytes (< 0x20, 0x7F) replaced by '?' — because detail feeds the outbound
//   Reject reason and log lines (CWE-117/116 forgery, CWE-400 amplification).
//   Unknown keys are ignored (additive forward compatibility) and their values
//   are never inspected beyond the preflight's grammar pass — in particular a
//   grammar-valid NUMBER of ANY magnitude is acceptable in an unknown value (0e0,
//   1e309, a 400-digit integer): the two libraries disagree about such tokens in
//   OPPOSITE directions (nlohmann materializes doubles and rejects what overflows;
//   glaze's validator rejects the grammar-valid 0e0 but passes 1e309), so neither
//   library may gate acceptance on a skipped value (grok R1, S2).
//   Both arms return the SAME DecodeError.detail string for the same rejection reason
//   (operability: a log line must not depend on which codec the benchmark flag picked).
//   Decode never throws on peer input (Global Constraints: engine never terminates on
//   peer input). Semantic validation (tick/lot/range/session rules) stays in the
//   engine, on top of Task 1's validate_order — the codec only does parse/shape/
//   version/type errors, where "shape" includes the identifier length caps and the
//   identifier byte policy above. MsgTooLarge is enforced at the transport
//   (read_message_max), never here.
//
// Shared frame policy (detail::frame_preflight — ONE code path both arms run before any
// library parser touches the input, so the arms cannot drift; every rejection is
// Malformed):
//   * Grammar authority: ONE ITERATIVE byte scan validates the ENTIRE frame — nested
//     values included — against the RFC 8259 grammar, so no library parser/validator
//     quirk can decide acceptance (the structural lesson of this file: any policy that
//     lives in a library's quirks WILL drift between the arms). Number tokens are
//     validated LEXICALLY (optional '-', no leading zeros, optional frac/exp with >= 1
//     digit each) and NEVER converted: token magnitude cannot affect the verdict.
//     Escaped surrogates must form valid pairs (both libraries enforce this — the
//     shared scan keeps that verdict shared). Exactly one top-level value is allowed
//     and nothing but whitespace may follow it (trailing garbage and smuggled second
//     frames reject). The root must be an object, checked after the grammar pass
//     (parse errors outrank the root-type error).
//   * Nesting depth: at most detail::kMaxNestingDepth (32) open {/[ levels, counted by
//     the ITERATIVE scan. Inbound schemas are flat (depth 1) and unknown additive
//     values are never semantically inspected, so 32 is far beyond any legitimate frame
//     while keeping both parsers' RECURSION trivially bounded — a size-bounded but
//     deeply nested value must be rejected, never allowed to exhaust the stack
//     (assignment §3: malformed input is rejected without terminating the process).
//   * Bytes: frames are strict RFC 3629 UTF-8 (overlongs/surrogates/truncations
//     reject); a UTF-8 BOM prefix rejects (RFC 8259 §8.1 — frames are exact bytes);
//     raw control bytes < 0x20 reject everywhere (inside strings they are illegal
//     JSON; outside, only \t \n \r and space are whitespace — this also rejects a
//     trailing raw NUL). ESCAPED control characters ("\u0000".."\u001F") inside string
//     values stay legal AT THE FRAME LEVEL — but the KNOWN identifier fields reject
//     them after decode (identifier shape policy above); only unknown/skipped values
//     keep the permissive rule end-to-end.
//   * Top-level keys: wire keys are literal ASCII — a top-level key containing any
//     escape sequence rejects (so "\u0074" is NOT an alias of "t" in either arm), and
//     duplicate top-level keys reject (no last-wins/first-wins ambiguity). At most
//     detail::kMaxTopLevelKeys (32) top-level keys — ~3x the widest inbound schema
//     (new_order: 11) for additive headroom — and a frame past the cap is Malformed.
//     Keys inside nested values get the grammar pass only (never the key policy).
//     Scope (gate P4-i1): this key policy governs ENGINE-side acceptance of untrusted
//     inbound frames only — the engine's own encoder can never emit escaped or duplicate
//     keys, so the Python arm's obligation is merely not to produce them. Python-side
//     parity is limited to duplicate detection (stdlib json's object_pairs_hook);
//     escaped-key rejection is deliberately NOT required of the Python decoders — both
//     stdlib and msgspec see only already-unescaped keys.
//   The scan records the root object's key/value spans in a FIXED-capacity FrameScan
//   (decode is on the measured hot path — no heap allocation in the preflight; grok
//   R1). Those spans double as the naive arm's field index: it DOM-parses KNOWN
//   fields' spans only, so unknown values never reach nlohmann at all.
//
// Encode contract (identical for both codecs; golden fixtures pin every message):
//   Canonical wire form — the "t" tag first, then members in DECLARATION order
//   (protocol.hpp), no whitespace; control characters in strings are escaped
//   (\b \t \n \f \r \" \\ shorthand, uppercase \u00XX otherwise) so output is always
//   valid JSON. encode() writes into the caller's buffer and does not allocate once the
//   buffer capacity is warmed (steady-state allocation-free — BOTH arms).
//   encode() cannot fail for a well-formed OutMsg. If a serializer error ever occurs
//   anyway, the buffer is left EMPTY — never a partial frame — and empty output IS the
//   failure signal (every real message encodes to >= 2 bytes); an assert() alone would
//   vanish under the rel preset's -DNDEBUG and ship a truncated buffer (grok R1).
//   Caller obligation (gate P4-i1): callers MUST NOT write an empty buffer to the wire —
//   treat empty output as an internal error: drop the message and close the session.
//   Task 7's session write path inherits this as a requirement; the signal is only a
//   signal if the one consumer checks it.
//   Decode stays key-order-insensitive: canonical order is an encode-side property.
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

// Identifier shape caps (decode contract above; gate P4-i1). cl_id is retained per order
// for the session lifetime and echoed on every report, symbol names one instrument, and
// side is semantically one byte — 8 keeps length errors distinct from the engine-owned
// BadSide verdict. kMaxTagDetailBytes bounds how much of a hostile unknown tag
// DecodeError.detail may carry (sanitized_tag below).
inline constexpr std::size_t kMaxClIdLen = 64;
inline constexpr std::size_t kMaxSymbolLen = 32;
inline constexpr std::size_t kMaxSideLen = 8;
inline constexpr std::size_t kMaxTagDetailBytes = 32;

// Result of the preflight scan: the root object's top-level fields as raw spans into
// the frame (key between its quotes; value as one complete JSON value slice, quotes/
// braces included). FIXED capacity (kMaxTopLevelKeys) so the decode path never heap-
// allocates here — the previous std::vector was the only decode-path allocation (grok
// R1); exceeding the cap rejects as Malformed in the scan itself.
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

// Shared frame preflight (see "Shared frame policy" above). BOTH codec arms call this
// FIRST in decode(), before any library parser touches the input — it is the single
// authority for frame grammar, and its iterative depth cap is what bounds the
// libraries' recursion afterwards. Implemented in frame_preflight.cpp (a dedicated
// C++20 TU with no third-party dependency — it is a pure byte scan), alongside every
// other shared-authority helper below. Returns std::nullopt when the frame passes
// (with `scan` filled); otherwise the Malformed error both arms return verbatim.
std::optional<DecodeError> frame_preflight(std::string_view frame, FrameScan &scan);

// The ONE "integral token" predicate pair (decode contract above): both arms apply these
// to FrameScan spans BEFORE any library extraction, so the verdict can never drift into
// a library quirk — including the SIGN verdict for unsigned fields, which both arms take
// from is_unsigned_integer_token (gate P4-i2). Only RANGE verdicts (u64/i64 overflow)
// stay with each arm's typed conversion.
[[nodiscard]] bool is_integer_token(std::string_view raw) noexcept;
[[nodiscard]] bool is_unsigned_integer_token(std::string_view raw) noexcept;

// Renders a peer-supplied tag safely for DecodeError.detail (decode contract above):
// truncated to kMaxTagDetailBytes on a UTF-8 code-point boundary (U+2026 marks the cut),
// control bytes (< 0x20, 0x7F) replaced by '?'.
[[nodiscard]] std::string sanitized_tag(std::string_view tag);

// Identifier shape policy (decode contract above): length caps + control-byte rejection
// on the decoded cl_id / symbol / side. ONE implementation, called by both arms after
// the typed read, so the detail strings stay byte-identical. nullopt = passes.
[[nodiscard]] std::optional<DecodeError> check_inbound_strings(const NewOrder &o);
[[nodiscard]] std::optional<DecodeError> check_inbound_strings(const CancelOrder &c);
} // namespace detail

} // namespace mm
