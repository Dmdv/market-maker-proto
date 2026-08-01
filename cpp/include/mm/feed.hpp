// Deterministic scripted market-data feed: the venue-side script that drives the engine's book,
// loaded at startup and replayed by the feed timer — a committed script keeps runs reproducible.
//
// WIRE FORMAT — JSON Lines, EXACTLY ONE event key per line, values in RAW wire units:
//   {"set":[bid_px,bid_qty,ask_px,ask_qty]}   install a new top of book
//   {"halt_ms":1200}                          publish nothing for this many milliseconds
//   {"drop":true}                             force-close every session (disconnect demo)
//   {"end":true}                              the script is over; the driver stops
//
// The feed produces BOOK STATES only — the engine decides fills. md_seq is assigned by the driver,
// one per publish, monotonic; a side is ABSENT when qty is 0. Looping is a driver flag (`--loop`).
//
// PARSE POLICY — strict, and it THROWS: a typo stops the process at startup, before the acceptor
// listens. Rejections name the 1-based line; validation reuses the codec's frame preflight.
#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <variant>
#include <vector>

namespace mm {

// The four event kinds. Defaulted operator== so the scenario tests can compare a whole script
// in one expression rather than field by field.
struct FeedSet {
  std::int64_t bid_px{}, bid_qty{}, ask_px{}, ask_qty{};
  bool operator==(const FeedSet &) const = default;
};
struct FeedHalt {
  std::int64_t ms{}; // publish nothing for ms (drives the stale-feed demo step)
  bool operator==(const FeedHalt &) const = default;
};
struct FeedDrop { // engine force-closes all sessions (disconnect demo)
  bool operator==(const FeedDrop &) const = default;
};
struct FeedEnd {
  bool operator==(const FeedEnd &) const = default;
};

using FeedEvent = std::variant<FeedSet, FeedHalt, FeedDrop, FeedEnd>;

// Loads and validates a scenario file. Throws std::runtime_error on a malformed line, naming
// the file and the 1-based line number.
[[nodiscard]] std::vector<FeedEvent> load_feed(const std::filesystem::path &path);

// Same parse, from an already-open stream: exists so the truncated-read branch (`in.bad()`) is
// testable, which a real file cannot exercise. `label` names the source in diagnostics.
[[nodiscard]] std::vector<FeedEvent> load_feed_stream(std::istream &in, std::string_view label);

} // namespace mm
