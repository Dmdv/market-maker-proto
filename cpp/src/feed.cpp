// Scenario-file loader for the deterministic feed (format and throwing contract in mm/feed.hpp).
// Every line runs the codec's shared frame_preflight; no size cap: the file is operator-supplied.
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

// Where a rejection happened: every message names the file and the 1-based line.
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
  // `.ec` is the WHOLE verdict: is_integer_token accepted exactly `-?[0-9]+`, so from_chars
  // always consumes the whole span and a partial-consume check could never fire.
  std::int64_t value = 0;
  const auto res = std::from_chars(token.data(), token.data() + token.size(), value);
  if (res.ec != std::errc{})
    reject(loc, quoted_name(name) + " is out of range for a 64-bit integer");
  return value;
}

// `{"set":[bid_px,bid_qty,ask_px,ask_qty]}`. The preflight already proved the value is valid
// JSON, so splitting on commas is safe: a nested comma leaves a fragment no element check accepts.
FeedSet parse_set(const Loc &loc, std::string_view value) {
  static constexpr std::array<std::string_view, 4> kNames{"bid_px", "bid_qty", "ask_px", "ask_qty"};
  static constexpr std::size_t kArity = kNames.size();
  // The preflight accepted the line as complete JSON, so only the leading `[` can still be wrong.
  if (value.front() != '[')
    reject(loc, quoted_name("set") + " must be an array of four integers "
                                     "[bid_px, bid_qty, ask_px, ask_qty]");

  const std::string_view body = trim(value.substr(1, value.size() - 2));
  const std::string arity_error =
      quoted_name("set") + " must have exactly four elements [bid_px, bid_qty, ask_px, ask_qty]";
  // `[]` is an arity error, not a nameless empty first element: "bid_px is not an integer" would
  // send the author looking at a field they never wrote.
  if (body.empty())
    reject(loc, arity_error);

  // Split the whole body FIRST, then walk at most kArity elements, then judge the count: the
  // walk's bound is the constant, so a fifth element can never index past the fixed arrays.
  std::vector<std::string_view> tokens;
  for (std::size_t pos = 0;;) {
    const auto comma = body.find(',', pos);
    tokens.push_back(
        trim(comma == std::string_view::npos ? body.substr(pos) : body.substr(pos, comma - pos)));
    if (comma == std::string_view::npos)
      break;
    pos = comma + 1;
  }

  // Shape before count for the elements that HAVE names: `[[1,2],100,5,8]` splits into fragments,
  // so "bid_px is not an integer" diagnoses it better than a count its own nesting inflated.
  std::array<std::int64_t, kArity> fields{};
  for (std::size_t i = 0; i < kArity && i < tokens.size(); ++i) {
    fields[i] = to_i64(loc, tokens[i], kNames[i]);
    // A negative price or size has no venue meaning and the engine's absence rule (px/qty <= 0)
    // would swallow it. Zero stays legal; the test is on the SPELLING, so `-0` rejects too.
    if (tokens[i].starts_with('-'))
      reject(loc, quoted_name(kNames[i]) + " must not be negative");
  }
  if (tokens.size() != kArity)
    reject(loc, arity_error);
  return FeedSet{fields[0], fields[1], fields[2], fields[3]};
}

FeedHalt parse_halt(const Loc &loc, std::string_view value) {
  // A day. Unbounded, this would accept INT64_MAX, and every consumer's ms * 1'000'000 overflows
  // int64 above ~9.2e12 ms; a ceiling far below that keeps the arithmetic in range by domain.
  static constexpr std::int64_t kMaxHaltMs = 24 * 60 * 60 * 1000;
  const std::int64_t ms = to_i64(loc, value, "halt_ms");
  // 0 rejects rather than doing nothing: a zero-length pause cannot be what an author meant.
  if (ms <= 0)
    reject(loc, quoted_name("halt_ms") + " must be a positive integer (milliseconds)");
  if (ms > kMaxHaltMs)
    reject(loc, quoted_name("halt_ms") + " must be at most " + std::to_string(kMaxHaltMs) +
                    " milliseconds (24 hours)");
  return FeedHalt{ms};
}

// `drop` and `end` are flags, and the only honest spelling of a set flag is `true`.
// `{"drop":false}` rejects: a line that does nothing is always a mistake.
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
  // A directory is not "a file with no events": opening one succeeds on macOS (and reads
  // nothing) but fails at the first read on Linux, so reject it here with its own verdict.
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
    // The script has one terminator, and it terminates: events after `end` are never replayed.
    if (ended)
      reject(loc, "content after the " + quoted_name("end") + " event");
    FeedEvent event = parse_line(loc, line);
    ended = std::holds_alternative<FeedEnd>(event);
    events.push_back(std::move(event));
  }
  // getline stops on EOF and on failure alike; only bad() tells a truncated read from a whole
  // file, and a partially loaded script must never be replayed.
  if (in.bad())
    throw std::runtime_error("error reading feed file " + source);
  if (events.empty())
    throw std::runtime_error("feed file " + source + " contains no events");
  return events;
}

} // namespace mm
