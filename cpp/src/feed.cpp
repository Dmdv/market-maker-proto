// Scenario-file loader for the deterministic feed (plan Task 5; the format, the domain rules
// and the throwing contract are documented in mm/feed.hpp).
//
// The JSON grammar is NOT re-implemented here: every line goes through
// mm::detail::frame_preflight — the same authority both codec arms run before any library
// touches an inbound frame (cpp/src/frame_preflight.cpp). That buys three things at the cost of
// one include: identical acceptance for identical bytes on both surfaces, the preflight's
// byte-offset diagnostics for free, and no second hand-written JSON validator to keep in sync.
// This TU adds only what the preflight cannot know: which keys are events, and what each
// event's value must mean.
//
// The same policy SHAPE as the codec, applied to the feed's own fields:
//   * the integral-token predicate (detail::is_integer_token) runs on the RAW value span
//     BEFORE any conversion, so "500000.0", "5e5" and "\"500000\"" reject as spellings rather
//     than surviving as a library's idea of a number;
//   * RANGE is then, and only then, the typed conversion's verdict (std::from_chars);
//   * a peer-ish string echoed into an error message goes through detail::sanitized_tag.
// The peer here is a committed file rather than a socket, so this is defense in depth, not a
// trust boundary — but a policy that changes shape between surfaces is a policy nobody can
// state, and the feed is the one input that decides what every benchmark measured.
//
// What is deliberately NOT reused is the transport's SIZE budget: there is no line-length or
// frame-size cap here, and that is a decision rather than an omission (gate R1, S3). The wire's
// 64 KiB cap (read_message_max) exists because those bytes arrive from a peer, unbounded and
// unbidden; these arrive from a path this process was handed by its own operator, read once,
// before the acceptor is listening. A cap here would refuse a scenario for being LARGE rather
// than MALFORMED — precisely the shape Task 4's gate deleted from the Python codec (see
// PENDING_AMENDMENTS (p)7) — and unlike the wire there is no second end whose accept set it
// would have to match, so it would buy nothing but a new way to reject a legal script. The cost
// it would bound is bounded by the file instead: one line is buffered, and a `set` body is split
// whole before its arity is decided. If a scenario is ever fetched rather than committed, that
// premise is what changed, and this is the paragraph to revisit.
#include "mm/feed.hpp"

#include "mm/codec.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace mm {
namespace {

// Where a rejection happened. Threaded through the parse helpers so every message can name
// the file and the 1-based line — the whole point of a line-oriented format.
struct Loc {
  std::string_view source; // a path, or a stream label — diagnostics read the same either way
  std::size_t line;
};

[[noreturn]] void reject(const Loc &loc, const std::string &why) {
  throw std::runtime_error(std::string{loc.source} + ": line " + std::to_string(loc.line) + ": " +
                           why);
}

std::string quoted_name(std::string_view name) { return "\"" + std::string{name} + "\""; }

constexpr std::string_view kJsonWs = " \t\n\r";

std::string_view trim(std::string_view s) {
  const auto first = s.find_first_not_of(kJsonWs);
  if (first == std::string_view::npos)
    return {};
  return s.substr(first, s.find_last_not_of(kJsonWs) - first + 1);
}

// Shape first (integral spelling), range second (typed conversion) — the codec's rule, so a
// number that is merely BIG is told apart from a number that is not an integer at all.
std::int64_t to_i64(const Loc &loc, std::string_view token, std::string_view name) {
  if (!detail::is_integer_token(token))
    reject(loc, quoted_name(name) + " is not an integer");
  // One condition, deliberately: `.ec` is the WHOLE verdict here, matching the codec arms'
  // idiom for the same operation (cpp/src/codec_glaze.cpp). A partial-consume check would be
  // unfalsifiable — is_integer_token above accepts exactly `-?[0-9]+`, so from_chars always
  // consumes the whole span, including on result_out_of_range — and it would misname the
  // failure "out of range" if it somehow fired (gate P4-i1, S3).
  std::int64_t value = 0;
  const auto res = std::from_chars(token.data(), token.data() + token.size(), value);
  if (res.ec != std::errc{})
    reject(loc, quoted_name(name) + " is out of range for a 64-bit integer");
  return value;
}

// `{"set":[bid_px,bid_qty,ask_px,ask_qty]}`. The preflight already proved the value is a
// grammatically valid JSON value, so splitting the array body on commas is safe: any comma
// belonging to a nested array/object or a string leaves a fragment that is not an integer
// token, and the four element checks below reject it.
FeedSet parse_set(const Loc &loc, std::string_view value) {
  static constexpr std::array<std::string_view, 4> kNames{"bid_px", "bid_qty", "ask_px", "ask_qty"};
  static constexpr std::size_t kArity = kNames.size();
  // ONE observable clause. The preflight has already accepted the line as complete JSON, so a
  // value is never empty and a `[` is always matched by its `]` — `size() < 2` and
  // `back() != ']'` could not fire, and a test cannot tell whether they exist (codex hard gate).
  if (value.front() != '[')
    reject(loc, quoted_name("set") + " must be an array of four integers "
                                     "[bid_px, bid_qty, ask_px, ask_qty]");

  const std::string_view body = trim(value.substr(1, value.size() - 2));
  const std::string arity_error =
      quoted_name("set") + " must have exactly four elements [bid_px, bid_qty, ask_px, ask_qty]";
  // `[]` is an arity error rather than a nameless empty first element: an author who wrote an
  // empty array got the SHAPE wrong, and "bid_px is not an integer" would send them looking at
  // a field they did not write.
  if (body.empty())
    reject(loc, arity_error);

  // Split the whole body FIRST, then walk at most kArity elements, then judge the count. The
  // separation is the fix for the gate's S2: the walk's bound is the constant kArity, so a
  // fifth element cannot be indexed — where the previous shape converted as it split and held
  // the index in check with a mid-loop guard that was both load-bearing (dropping it wrote past
  // the fixed arrays) and unfalsifiable (it raised the SAME arity message as the final count
  // check, so no test could tell it had been removed; only the asan preset noticed).
  std::vector<std::string_view> tokens;
  for (std::size_t pos = 0;;) {
    const auto comma = body.find(',', pos);
    tokens.push_back(
        trim(comma == std::string_view::npos ? body.substr(pos) : body.substr(pos, comma - pos)));
    if (comma == std::string_view::npos)
      break;
    pos = comma + 1;
  }

  // Shape before count, for the elements that HAVE names: a nested array or object splits into
  // fragments that are not integer tokens, so naming the field that carries the fragment
  // ("bid_px is not an integer") diagnoses `[[1,2],100,5,8]` better than an element count its
  // own nesting inflated.
  std::array<std::int64_t, kArity> fields{};
  for (std::size_t i = 0; i < kArity && i < tokens.size(); ++i) {
    fields[i] = to_i64(loc, tokens[i], kNames[i]);
    // A negative price or size has no venue meaning, and the engine's absence rule would
    // silently swallow it (qty <= 0 or px <= 0 reads as "side absent"), turning a typo into a
    // one-sided book nobody scripted. Zero stays legal: it IS how absence is expressed — but
    // the rule is on the SPELLING, not the converted value, so `-0` is rejected with every
    // other negative instead of arriving as a legal zero nobody wrote (gate R1, S3).
    if (tokens[i].starts_with('-'))
      reject(loc, quoted_name(kNames[i]) + " must not be negative");
  }
  if (tokens.size() != kArity)
    reject(loc, arity_error);
  return FeedSet{fields[0], fields[1], fields[2], fields[3]};
}

FeedHalt parse_halt(const Loc &loc, std::string_view value) {
  // A day, and the ceiling is deliberate. The longest halt any scenario has a use for is
  // bench_idle.feed's hour (it need only outlast a ~110 s benchmark run), while an unbounded
  // domain accepts INT64_MAX here and defers the consequence to whoever converts it: every
  // consumer turns ms into a duration by multiplying by 1'000'000, which overflows int64 above
  // ~9.2e12 ms. A ceiling four orders of magnitude below that keeps the arithmetic every
  // downstream owner performs in range BY the value's own domain, rather than by each of them
  // remembering to range-check it (gate R1, S3).
  static constexpr std::int64_t kMaxHaltMs = 24 * 60 * 60 * 1000;
  const std::int64_t ms = to_i64(loc, value, "halt_ms");
  // 0 is rejected rather than treated as a no-op: a zero-millisecond pause cannot be what a
  // scenario author meant, and a silently ignored event is the kind of thing that shows up as
  // an unexplained demo timing later.
  if (ms <= 0)
    reject(loc, quoted_name("halt_ms") + " must be a positive integer (milliseconds)");
  if (ms > kMaxHaltMs)
    reject(loc, quoted_name("halt_ms") + " must be at most " + std::to_string(kMaxHaltMs) +
                    " milliseconds (24 hours)");
  return FeedHalt{ms};
}

// `drop` and `end` are flags, and the only honest spelling of a flag that is set is `true`.
// `{"drop":false}` rejects instead of meaning "no drop": a scenario line that does nothing is
// always a mistake, and accepting it would make the file's event count differ from its
// behavior.
void require_true(const Loc &loc, std::string_view value, std::string_view name) {
  if (value != "true")
    reject(loc, quoted_name(name) + " must be the literal true");
}

FeedEvent parse_line(const Loc &loc, std::string_view text) {
  detail::FrameScan scan;
  if (const auto err = detail::frame_preflight(text, scan))
    reject(loc, err->detail);
  if (scan.count != 1)
    reject(loc, "expected exactly one event key, found " + std::to_string(scan.count));

  const std::string_view key = scan.fields[0].key;
  const std::string_view value = scan.fields[0].value;
  if (key == "set")
    return parse_set(loc, value);
  if (key == "halt_ms")
    return parse_halt(loc, value);
  if (key == "drop") {
    require_true(loc, value, "drop");
    return FeedDrop{};
  }
  if (key == "end") {
    require_true(loc, value, "end");
    return FeedEnd{};
  }
  reject(loc, "unknown event key " + quoted_name(detail::sanitized_tag(key)));
}

} // namespace

std::vector<FeedEvent> load_feed(const std::filesystem::path &path) {
  // A directory is not "a file with no events". Measured on macOS (gate R1, S3): the ifstream
  // OPENS one and reads nothing, so a --feed pointed one level too high was reported as an empty
  // script. On Linux the open likewise succeeds but the first READ fails (EISDIR), which this
  // loader would report as a truncated file. Both misname the mistake, and they misname it
  // differently, so the directory is rejected here instead — one verdict on every platform,
  // whichever way the stream would have failed. That verdict is its OWN, not the missing-path
  // one: `--feed bench/scenarios` (a real path, one level too high) and `--feed bench/scenario`
  // (a typo) are different mistakes, and a module that distinguishes "is not an integer" from
  // "is out of range" does not get to alias those two (gate P4-i1, S3). The non-throwing status
  // overload is deliberate: if the query itself fails, this is not the diagnosis, and the open
  // below owns the verdict.
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec))
    throw std::runtime_error(path.string() + " is a directory, not a feed file");

  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open feed file " + path.string());

  return load_feed_stream(in, path.string());
}

std::vector<FeedEvent> load_feed_stream(std::istream &in, std::string_view label) {
  const std::string source{label};
  std::vector<FeedEvent> events;
  std::string line;
  std::size_t line_no = 0;
  bool ended = false;
  while (std::getline(in, line)) {
    ++line_no;
    const Loc loc{source, line_no};
    // The script has one terminator, and it terminates: events after `end` would be loaded and
    // never replayed, so the file would not say what it does.
    if (ended)
      reject(loc, "content after the " + quoted_name("end") + " event");
    FeedEvent event = parse_line(loc, line);
    ended = std::holds_alternative<FeedEnd>(event);
    events.push_back(std::move(event));
  }
  // getline stops on EOF and on failure alike; only bad() distinguishes a truncated read from
  // a complete file, and a partially loaded script must never be replayed as if it were whole.
  if (in.bad())
    throw std::runtime_error("error reading feed file " + source);
  if (events.empty())
    throw std::runtime_error("feed file " + source + " contains no events");
  return events;
}

} // namespace mm
