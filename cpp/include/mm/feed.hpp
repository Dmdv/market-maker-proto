// Deterministic scripted market-data feed (plan Task 5): the venue-side script that drives
// the engine's book. Loaded ONCE at startup, then replayed by the Task 7 feed timer.
//
// Why a file and not a generator: the assignment §4 demonstration and the §5.2 benchmark runs
// must be byte-reproducible across machines, codecs and client stacks — an A/B whose input
// differs between arms measures the input. A committed script also makes the demo's fill
// arithmetic auditable (the reviewer can read WHY the fill happens), which a random walk
// seeded "deterministically" never is.
//
// WIRE FORMAT — JSON Lines: one JSON object per line, EXACTLY ONE event key per line:
//   {"set":[bid_px,bid_qty,ask_px,ask_qty]}   install a new top of book (raw wire units)
//   {"halt_ms":1200}                          publish nothing for this many milliseconds
//   {"drop":true}                             force-close every session (disconnect demo)
//   {"end":true}                              the script is over; the driver stops
//
// The feed produces BOOK STATES only. It never decides fills — the engine does, by applying
// its own fill rule to each installed book (engine.hpp). Sequencing is the feed's job and
// nobody else's: md_seq is assigned by the driver, one per publish, monotonic by construction
// (F-09), which is why the engine's md_seq guard is defense-in-depth rather than a protocol.
//
// LOOPING is the DRIVER's decision, not the file's (Task 7 `--loop` / `Config::loop_feed`), so
// there is deliberately no loop marker in the format: a scenario meant for looping simply omits
// {"end":true} and the driver wraps to the first event — which is exactly what
// bench/scenarios/bench_paced.feed does. One representation, one owner.
//
// UNITS AND DOMAIN: prices and quantities are RAW wire units (types.hpp boundary rule), never
// ticks/lots and never floats. A side is ABSENT when its qty is 0 — the engine's absence rule
// (engine.hpp), so a scenario expresses a one-sided book as qty 0 rather than by omission.
// Negative prices and quantities are rejected here: they have no venue meaning, and the engine
// would silently swallow them through the same absence rule. The rule is on the SPELLING, so
// `-0` rejects with them rather than arriving as a legal zero nobody wrote.
//
// PARSE POLICY — strict, and it THROWS. load_feed runs at startup only (before the acceptor is
// listening), so an exception is the right shape: there is no session to reject and no state to
// keep consistent, and a scenario typo must stop the process rather than quietly measure the
// wrong thing (Task 7 turns it into one stderr line + exit 2). Every rejection names the 1-based
// LINE NUMBER, because that is the only thing that makes a 12-line script debuggable.
// A line is malformed when ANY of these holds:
//   * it fails the codec's shared frame preflight (mm::detail::frame_preflight, the project's
//     ONE JSON grammar/byte/key authority — see cpp/src/frame_preflight.cpp): a UTF-8 BOM,
//     non-UTF-8 bytes, any RFC 8259 grammar violation, raw control bytes, content after the
//     object (a second event smuggled onto one line), duplicate or escaped top-level keys, a
//     non-object root. Deliberately the SAME policy the wire uses, reused rather than
//     re-invented — two hand-written JSON validators in one repository is one too many;
//   * it does not carry exactly one top-level key, or that key is not one of the four above;
//   * a `set` value is not an array, or an element is not an INTEGER token (float, exponent and
//     string spellings all reject, as on the wire), or is spelled negative (`-0` included), or
//     is out of int64 range, or the array does not hold exactly four elements. The ORDER of
//     those verdicts is deliberate: the four NAMED elements are validated first — so
//     `[[1,2],100,5,8]` names the fragment it carries rather than an element count its own
//     nesting inflated — and the element COUNT last; no element past the fourth is ever
//     inspected, so a surplus element can only ever be reported as an arity error, never
//     indexed;
//   * a `halt_ms` value is not an integer in [1, 86'400'000] ms — positive because a halt that
//     pauses nothing is a typo, and capped at 24 h because the value is multiplied into a
//     duration downstream (the reasoning is in feed.cpp);
//   * a `drop`/`end` value is not the literal `true`;
//   * anything follows the `end` event.
// There is deliberately NO line-length or frame-size cap: this input is a local file read at
// startup, not a peer's frame — feed.cpp states the decision and what would reverse it.
// An unreadable path, a path naming a DIRECTORY, and a file with no events throw too (no line
// to name) — each with its OWN verdict, because "one level too high" and "misspelled" are
// different mistakes and only the message can tell them apart.
#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <variant>
#include <vector>

namespace mm {

// The four event kinds. Each carries a defaulted operator== so a whole script can be compared
// in one expression — the scenario tests pin the shipped files event-for-event, and a
// field-by-field comparison there would be the very thing most likely to drift.
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
// the file and the 1-based line number (startup only — see the parse policy above).
[[nodiscard]] std::vector<FeedEvent> load_feed(const std::filesystem::path &path);

// Same parse, from an already-open stream. Exists so the truncated-read branch is testable:
// `load_feed`'s own `in.bad()` guard is the one meaningful safeguard a file path cannot
// exercise — a std::ifstream over a real file does not fail mid-read on demand — and an
// untestable branch is one whose removal nothing can see (codex hard gate). `label` names the
// source in diagnostics exactly as the path does.
[[nodiscard]] std::vector<FeedEvent> load_feed_stream(std::istream &in, std::string_view label);

} // namespace mm
