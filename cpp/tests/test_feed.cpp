// tests: the deterministic scripted feed loader and the three shipped scenario files.
//
// Two obligations, and they pull in opposite directions:
//   * the SHIPPED scenarios are pinned event-for-event — `demo.feed` is the the demo spec
//     script and `bench_paced.feed` is what makes the benchmark M3 sample floor reachable, so a
//     silent edit to either must fail here, not surface as an unexplained benchmark or demo
//     result;
//   * every MALFORMED-line rule is pinned individually, each with the reported LINE NUMBER,
//     because a scenario file is the one input this binary reads before it can serve anyone:
//     a rule that silently stops firing turns a typo into a wrong measurement.
// The malformed table is written so that dropping any single rule from feed.cpp reds exactly
// the row that names it (falsifiability); the rows that quote frame_preflight's own wording
// additionally pin the REUSE — an independently re-invented feed grammar would not produce
// those strings.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstring>
#include <ios>
#include <sstream>
#include <streambuf>

#include "mm/feed.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace Catch {
// Without this, a failing vector comparison prints "{?}" for every element and says nothing
// about WHICH event drifted. Exercised on purpose by the last case in this file, so a broken
// stringifier cannot hide behind a green suite (gate finding).
template <> struct StringMaker<mm::FeedEvent> {
  static std::string convert(const mm::FeedEvent &e) {
    if (const auto *s = std::get_if<mm::FeedSet>(&e))
      return "set[" + std::to_string(s->bid_px) + "," + std::to_string(s->bid_qty) + "," +
             std::to_string(s->ask_px) + "," + std::to_string(s->ask_qty) + "]";
    if (const auto *h = std::get_if<mm::FeedHalt>(&e))
      return "halt_ms " + std::to_string(h->ms);
    if (std::holds_alternative<mm::FeedDrop>(e))
      return "drop";
    return "end";
  }
};
} // namespace Catch

namespace {

using Catch::Matchers::ContainsSubstring;

std::filesystem::path scenario(std::string_view name) {
  return std::filesystem::path{MM_SCENARIO_DIR} / name;
}

// A feed file that deletes itself. The name carries a per-process token as well as a
// counter: `catch_discover_tests` registers one ctest test per case, so a `ctest -j` run
// has several copies of this binary writing into the same temp directory at once.
class TempFeed {
public:
  explicit TempFeed(std::string_view text) : path_(unique_path()) {
    std::ofstream out(path_, std::ios::binary);
    out << text;
    out.close();
    REQUIRE(std::filesystem::exists(path_));
  }
  ~TempFeed() {
    std::error_code ec;
    std::filesystem::remove(path_, ec); // best effort: a test failure must not mask itself
  }
  TempFeed(const TempFeed &) = delete;
  TempFeed &operator=(const TempFeed &) = delete;
  TempFeed(TempFeed &&) = delete;
  TempFeed &operator=(TempFeed &&) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  static std::filesystem::path unique_path() {
    static const auto token = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    return std::filesystem::temp_directory_path() /
           ("mm_feed_" + std::to_string(token) + "_" + std::to_string(counter++) + ".feed");
  }

  std::filesystem::path path_;
};

// The book every scenario file rests on (engine_test_support.hpp calls the same numbers
// base_book): bid 500000 x 100 / ask 500010 x 80, raw wire units, prices on the 5-unit tick.
constexpr mm::FeedSet kBase{500000, 100, 500010, 80};

// The client-side constants demo.feed's timings are derived FROM, mirrored here so the
// derivation is asserted rather than narrated: the demo runs `mm-client --qty 100
// --stale-ms 500` with a single reconnect attempt 1 s after a disconnect (mirrored client
// constants). Changing either side without changing the other reds this file instead of surfacing
// as a flaky integration run.
constexpr std::int64_t kDemoQuoteQty = 100;
constexpr std::int64_t kClientStaleMs = 500;
constexpr std::int64_t kClientReconnectMs = 1000;
// The demo's ordinary inter-step hold: long enough for a cancel/new round trip at
// --interval-ms 50, short enough to re-arm the stale timer rather than trip it.
constexpr std::int64_t kHoldMs = 300;

} // namespace

TEST_CASE("feed: demo.feed is the seven-step demo script, event for event", "[feed]") {
  // Each row cites the demo step it serves. The client-facing timings assume the demo's
  // --interval-ms 50 and the mirrored client constants above (mirrored client constants).
  const std::vector<mm::FeedEvent> expected{
      kBase,                                 // [0]  step 1 initial two-sided book
      mm::FeedHalt{kHoldMs},                 // [1]  step 2/4.3 hold: the client quotes both sides
      mm::FeedSet{500000, 100, 500015, 80},  // [2]  step 4 ask-only move: one side amends
      mm::FeedHalt{kHoldMs},                 // [3]  hold: the ask amend completes
      mm::FeedSet{499990, 100, 500000, 500}, // [4]  step 5 the ask crosses the bid -> ONE fill
      mm::FeedSet{499990, 100, 500010, 80},  // [5]  step 5 restore 1/2: ask back, bid still low
      mm::FeedHalt{kHoldMs},                 // [6]  hold: the ask cancel/re-quote completes
      kBase,                                 // [7]  step 5 restore 2/2: the base book is back
      mm::FeedHalt{kHoldMs},                 // [8]  hold: the restore-2/2 bid cancel/re-quote
                                             //      completes; step 6's window opens behind it
      kBase,                                 // [9]  step 6 unchanged book: re-arms the stale timer
      mm::FeedHalt{kHoldMs},                 // [10] step 6 quiet hold: both quotes LIVE throughout
      kBase,                                 // [11] step 6 unchanged book
      mm::FeedHalt{kHoldMs},                 // [12] step 6 quiet hold
      kBase,                                 // [13] step 6 unchanged book
      mm::FeedHalt{kHoldMs},                 // [14] step 6 quiet hold
      kBase,                                 // [15] step 6 quiet window closes
      mm::FeedHalt{1200},                    // [16] step 7a stale feed: beyond --stale-ms
      kBase,                                 // [17] resume: the feed is alive again
      mm::FeedDrop{},                        // [18] step 7b disconnect: every session closed 1001
      mm::FeedHalt{1200},                    // [19] reconnect hold, bounded on BOTH sides below
      kBase,                                 // [20] first TOB of the fresh epoch (flat state)
      mm::FeedEnd{},                         // [21] script over: the engine stops, exit 0
  };
  // Everything below reads the LOADED events, never the literals above: assertions taken from
  // `expected` are constant-folded, so an edit to the shipped bytes would break the arithmetic
  // while the checks that EXPLAIN the arithmetic stayed green.
  const auto events = mm::load_feed(scenario("demo.feed"));
  CHECK(events == expected);
  REQUIRE(events.size() == expected.size());

  const auto &ask_move = std::get<mm::FeedSet>(events[2]);
  const auto &cross = std::get<mm::FeedSet>(events[4]);
  const auto &restore = std::get<mm::FeedSet>(events[5]);

  // step 5 is the one step whose determinism is arithmetic rather than scripting. The fill exists
  // only because the moved ask reaches the price the client's bid rests at (the touch of the
  // initial book)...
  CHECK(cross.ask_px == kBase.bid_px);
  // ...and it is FULL rather than partial only because the crossing size covers the demo's
  // configured --qty 100; a larger --qty would leave `leaves > 0` and change the shape of the
  // step, so the premise is named rather than claimed for every configuration.
  CHECK(cross.ask_qty >= kDemoQuoteQty);
  // The SAME event must not also fill the ask side: the moved bid stays below the ask the
  // client is quoting when the cross arrives.
  CHECK(cross.bid_px < ask_move.ask_px);
  // Nor may the NEXT one. After the cross the client's desired ask is the new best ask
  // (= cross.ask_px), so a restore that put the bid straight back to 500000 would re-cross that
  // resting ask and turn step 5 into a two-fill cascade. Hence the two-step restore, and hence
  // these two assertions: they are what pin one-fill determinism ACROSS events.
  CHECK(restore.bid_px < cross.ask_px);
  // Restore 2/2 must stay below the ask the client re-quotes after restore 1/2 (= restore.ask_px),
  // or the returning base book re-crosses it and the cascade comes back through the other door.
  CHECK(std::get<mm::FeedSet>(events[7]).bid_px < restore.ask_px);

  // step 6 needs a window in which the client is connected, two-sided and NOT stale: a whole
  // second connection is exercised inside it (Reject{Malformed} + Reject{TickSize}, both quotes
  // asserted LIVE, a 1 Hz counters snapshot, then a cancel + re-quote round trip). An unchanged
  // book generates no commands (the strategy's duplicate rule) but every TOB re-arms the stale
  // timer, so the repeated base books ARE the window.
  std::int64_t quiet_ms = 0;
  for (const std::size_t i :
       {std::size_t{6}, std::size_t{8}, std::size_t{10}, std::size_t{12}, std::size_t{14}}) {
    const auto hold = std::get<mm::FeedHalt>(events[i]).ms;
    // Every hold across this stretch must re-arm the stale timer rather than trip it — [6] and
    // [8] included, where a longer hold would strand the client mid cancel/re-quote.
    CHECK(hold < kClientStaleMs);
    // ...but neither [6] nor [8] is part of the window, and for the SAME reason: each follows a
    // book that changed a desired price ([5] the ask, [7] the bid), so that side is
    // PENDING_CANCEL/PENDING_NEW for part of the hold and the client is not in the both-quotes-
    // LIVE state this window exists to assert. The window is the holds BETWEEN the repeated base
    // books, which begin at events[9]. The tail of [8] is quiet too, but how much of it is a
    // round trip this file cannot know, so it is budgeted as nothing.
    if (i != 6 && i != 8)
      quiet_ms += hold;
  }
  // What remains must hold the 1 Hz counters snapshot AND the cancel + re-quote round trip that
  // step 6 runs after reading it: 900 ms of fully quiet holds plus the 500 ms stale grace after
  // the last quiet book = 1400 ms. A 1 Hz timer ticks within any 1000 ms of that window, so the
  // slack left for the round trip is at least one ordinary hold — which is what kHoldMs is sized
  // for. Dropping a quiet pair leaves 1100 ms and reds this line rather than surfacing as a
  // flaky run.
  CHECK(quiet_ms + kClientStaleMs >= 1000 + kHoldMs);

  // step 7a is the opposite bound: this halt must OUTLAST the threshold so the client stops.
  CHECK(std::get<mm::FeedHalt>(events[16]).ms > kClientStaleMs);

  // step 7b: the reconnected session must observe a fresh-epoch book, so the post-drop hold is
  // bounded on BOTH sides — longer than the reconnect delay (or the TOB lands before the client
  // is back) and shorter than that delay plus the stale threshold (or the reconnected session
  // trips StopQuoting/close 4000 before its first book ever arrives). One feed interval of
  // slack sits inside those bounds at both ends.
  const auto reconnect_hold = std::get<mm::FeedHalt>(events[19]).ms;
  CHECK(reconnect_hold > kClientReconnectMs);
  CHECK(reconnect_hold < kClientReconnectMs + kClientStaleMs);
}

TEST_CASE("feed: bench_idle.feed opens a connect window, then one book, then silence", "[feed]") {
  const auto events = mm::load_feed(scenario("bench_idle.feed"));
  const std::vector<mm::FeedEvent> expected{mm::FeedHalt{3'000}, kBase, mm::FeedHalt{3'600'000},
                                            mm::FeedEnd{}};
  CHECK(events == expected);
  // The LEADING halt exists because ServerImpl arms the feed with a zero delay
  // (`schedule_feed(milliseconds{0})`), so without it the single book is published before any
  // client can finish a WebSocket handshake — and a client that misses it never receives a
  // book at all, never adopts a session epoch, and can therefore never send a command. The
  // harness measured exactly that: zero samples, because `SessionDriver` refuses to
  // stamp an outbound command before an epoch exists. Three seconds is far longer than a
  // loopback handshake needs and is still silent for the whole measurement.
  CHECK(std::get<mm::FeedHalt>(events[0]).ms >= 1'000);
  // The idle scenario is the feed-silent M1 floor: exactly ONE book is
  // published and the halt then outlasts any benchmark run (the benchmark protocol react mode is
  // ≈110 s + warm-up), so the owner thread sees no book work while M1 is being measured.
  CHECK(std::get<mm::FeedHalt>(events[2]).ms > 1000 * 1000);
}

TEST_CASE("feed: bench_paced.feed alternates two books and is written for --loop", "[feed]") {
  const auto events = mm::load_feed(scenario("bench_paced.feed"));
  REQUIRE(events.size() == 2);
  const auto &a = std::get<mm::FeedSet>(events[0]);
  const auto &b = std::get<mm::FeedSet>(events[1]);
  CHECK(a == kBase);
  CHECK(b == mm::FeedSet{500005, 100, 500015, 80});
  // Every event is a `set`: at --interval-ms 1 the file must publish a book on EVERY tick,
  // which is what makes ≈1k TOB/s (and the ≥100k M3 react samples in ≈110 s) hold.
  for (const auto &e : events)
    CHECK(std::holds_alternative<mm::FeedSet>(e));
  // Repetition is the driver's decision, not the file's (`--loop` / Config::loop_feed),
  // so the file deliberately carries NO `end` terminator — with it, a paced run would stop
  // after two ticks instead of pacing for the whole benchmark.
  CHECK(!std::holds_alternative<mm::FeedEnd>(events.back()));
  // That absence is the whole of the feed's share of "loop re-yields". Re-yielding itself is the
  // DRIVER's rule (`Config::loop_feed`, i = (i + 1) % size) and is discharged by the server layer's
  // feed-driver tests: replaying this vector modulo its own size here would assert an identity
  // of std::vector indexing — true for any file content and any behavior of load_feed — and
  // would make an uncovered criterion look covered2).
}

TEST_CASE("feed: every malformed-line rule rejects, naming the line", "[feed]") {
  struct Bad {
    std::string_view name;
    std::string_view text;
    std::string_view line;   // the location the message must name
    std::string_view detail; // the reason it must give
  };
  // The multi-line rows start with valid events, so they also prove the counter advances
  // rather than always reporting line 1.
  const std::vector<Bad> cases{
      // --- rules inherited verbatim from the codec's frame preflight (policy reuse) ---
      {"BOM", "\xEF\xBB\xBF{\"end\":true}\n", "line 1", "UTF-8 BOM"},
      {"invalid UTF-8", "{\"\xFF\":true}\n", "line 1", "invalid UTF-8"},
      {"trailing garbage", "{\"end\":true} junk\n", "line 1", "trailing content after the frame"},
      {"a second object on one line", "{\"drop\":true}{\"end\":true}\n", "line 1",
       "trailing content after the frame"},
      {"duplicate key", "{\"drop\":true,\"drop\":true}\n", "line 1", "duplicate top-level key"},
      {"escaped key", "{\"\\u0064rop\":true}\n", "line 1", "escape sequence in a top-level key"},
      {"root is not an object", "[1,2]\n", "line 1", "root is not an object"},
      {"unterminated object", "{\"end\":true\n", "line 1", "truncated frame"},
      {"blank line", "\n{\"end\":true}\n", "line 1", "empty frame"},
      {"whitespace-only line", "{\"drop\":true}\n   \n", "line 2", "empty frame"},
      // --- the feed's own event grammar ---
      {"no event key", "{}\n", "line 1", "exactly one event key, found 0"},
      {"two event kinds on one line", "{\"drop\":true,\"end\":true}\n", "line 1",
       "exactly one event key, found 2"},
      {"unknown event key", "{\"quack\":true}\n", "line 1", "unknown event key \"quack\""},
      // The row above cannot see the SANITIZER on that key: 5 printable bytes are below
      // kMaxTagDetailBytes (32) and carry no control bytes, so detail::sanitized_tag is the
      // identity on them and echoing the raw key would pass identically. Preflight already
      // rejects raw control bytes and escaped top-level keys, so LENGTH is the only dimension
      // left that separates the two — this 40-byte key pins the truncation and its U+2026
      // marker, and reds the moment the sanitizer call is dropped.
      {"unknown event key long enough to be truncated by the shared sanitizer",
       "{\"quackquackquackquackquackquackquackquack\":true}\n", "line 1",
       "unknown event key \"quackquackquackquackquackquackqu\xE2\x80\xA6\""},
      // --- `set` shape and domain ---
      // Three rows for three SHAPES, not three clauses. The guard is one observable test —
      // `front() != '['` — because a successful preflight already guarantees a complete JSON
      // value, so an empty value or an unmatched `]` cannot reach here. Each
      // row still earns its place: without them, an object-valued `set` was diagnosed as a bad
      // first ELEMENT and a string-valued one as a bad element COUNT.
      {"set is not an array", "{\"set\":5}\n", "line 1", "must be an array of four integers"},
      {"set is an object", "{\"set\":{\"a\":1}}\n", "line 1", "must be an array of four integers"},
      {"set is a string", "{\"set\":\"500000\"}\n", "line 1", "must be an array of four integers"},
      {"set is empty", "{\"set\":[]}\n", "line 1", "must have exactly four elements"},
      // `[]` is already empty before the body is trimmed, so it cannot tell whether the trim
      // happens. `[ ]` can: without it the whitespace becomes a nameless first element and the
      // verdict flips to "bid_px is not an integer" — exactly the misdiagnosis the empty-array
      // branch exists to prevent.
      {"set is whitespace-only", "{\"set\":[ ]}\n", "line 1", "must have exactly four elements"},
      {"set is too short", "{\"set\":[1,2,3]}\n", "line 1", "must have exactly four elements"},
      {"set is too long", "{\"set\":[500000,100,500010,80,1]}\n", "line 1",
       "must have exactly four elements"},
      // The row above is satisfied by EITHER arity check the loader could hold, so it cannot
      // tell them apart. This one can: the fifth element is a value the element rules would
      // reject with a DIFFERENT message ("must not be negative"), so an arity verdict here
      // proves the fifth element is never READ — which is what makes the four names and the
      // four slots impossible to index past. (It says nothing about where the count is judged
      // relative to the four NAMED elements; those are judged first, and the rows below show
      // it.) Without that bound the extra element was an out-of-bounds write that left this
      // suite green.
      {"set is too long, and the fifth element is never read",
       "{\"set\":[500000,100,500010,80,-1]}\n", "line 1", "must have exactly four elements"},
      {"set element is fractional", "{\"set\":[500000.0,100,5,8]}\n", "line 1",
       "\"bid_px\" is not an integer"},
      {"set element is a string", "{\"set\":[\"500000\",100,5,8]}\n", "line 1",
       "\"bid_px\" is not an integer"},
      {"set element is null", "{\"set\":[500000,null,5,8]}\n", "line 1",
       "\"bid_qty\" is not an integer"},
      // The THIRD slot, which every other element row leaves unnamed: without a row pointing
      // here, a mislabeled kNames[2] would ship green.
      {"set element is null in the third slot", "{\"set\":[500000,100,null,80]}\n", "line 1",
       "\"ask_px\" is not an integer"},
      {"set element is nested", "{\"set\":[[1,2],100,5,8]}\n", "line 1",
       "\"bid_px\" is not an integer"},
      {"set element overflows int64", "{\"set\":[9223372036854775808,100,5,8]}\n", "line 1",
       "\"bid_px\" is out of range"},
      {"negative price", "{\"set\":[-500000,100,500010,80]}\n", "line 1",
       "\"bid_px\" must not be negative"},
      {"negative quantity", "{\"set\":[500000,100,500010,-80]}\n", "line 1",
       "\"ask_qty\" must not be negative"},
      // `-0` is a negative SPELLING that a value check cannot see: it converts to 0, which is
      // the legal "side absent" quantity, so a rule written on the converted value accepts a
      // price of minus nothing. The domain is the spelling.
      {"negative zero", "{\"set\":[-0,100,500010,80]}\n", "line 1",
       "\"bid_px\" must not be negative"},
      // --- `halt_ms` shape and domain ---
      {"halt_ms in exponent form", "{\"halt_ms\":1e3}\n", "line 1",
       "\"halt_ms\" is not an integer"},
      {"halt_ms is fractional", "{\"halt_ms\":1.5}\n", "line 1", "\"halt_ms\" is not an integer"},
      {"halt_ms is a boolean", "{\"halt_ms\":true}\n", "line 1", "\"halt_ms\" is not an integer"},
      {"halt_ms overflows int64", "{\"halt_ms\":9223372036854775808}\n", "line 1", "out of range"},
      {"halt_ms is zero", "{\"halt_ms\":0}\n", "line 1", "must be a positive integer"},
      {"halt_ms is negative", "{\"halt_ms\":-1}\n", "line 1", "must be a positive integer"},
      // The domain has a ceiling as well as a floor: an unbounded halt is accepted arithmetic
      // nonsense whose consequence lands on whoever multiplies it into a duration.
      {"halt_ms above the 24-hour ceiling", "{\"halt_ms\":86400001}\n", "line 1",
       "\"halt_ms\" must be at most 86400000"},
      {"halt_ms at INT64_MAX", "{\"halt_ms\":9223372036854775807}\n", "line 1",
       "must be at most 86400000"},
      // --- flag events ---
      {"drop is false", "{\"drop\":false}\n", "line 1", "\"drop\" must be the literal true"},
      {"end is a number", "{\"end\":1}\n", "line 1", "\"end\" must be the literal true"},
      // --- script-level rules ---
      {"event after end", "{\"end\":true}\n{\"drop\":true}\n", "line 2",
       "content after the \"end\" event"},
      // --- the line counter itself ---
      {"error on the fourth line",
       "{\"drop\":true}\n{\"halt_ms\":10}\n{\"drop\":true}\n{\"halt_ms\":x}\n", "line 4",
       "invalid number token"},
  };

  for (const auto &c : cases) {
    INFO("case: " << c.name);
    const TempFeed file{c.text};
    CHECK_THROWS_MATCHES(mm::load_feed(file.path()), std::runtime_error,
                         Catch::Matchers::MessageMatches(ContainsSubstring(std::string{c.line}) &&
                                                         ContainsSubstring(std::string{c.detail})));
  }
}

TEST_CASE("feed: file-level failures throw without inventing a line", "[feed]") {
  SECTION("a missing file names the path it could not open") {
    const auto missing = std::filesystem::temp_directory_path() / "mm_feed_does_not_exist.feed";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    CHECK_THROWS_MATCHES(
        mm::load_feed(missing), std::runtime_error,
        Catch::Matchers::MessageMatches(ContainsSubstring("cannot open feed file") &&
                                        ContainsSubstring(missing.string())));
  }
  SECTION("an empty file is a misconfiguration, not an empty script") {
    const TempFeed file{""};
    CHECK_THROWS_MATCHES(mm::load_feed(file.path()), std::runtime_error,
                         Catch::Matchers::MessageMatches(ContainsSubstring("no events")));
  }
  SECTION("a directory names ITS mistake, not the missing-path one") {
    // Measured on macOS: std::ifstream OPENS a directory and reads nothing, so a --feed
    // pointed one level too high used to be reported as "contains no events"; on Linux the
    // same path fails the first read (EISDIR) and would be reported as a truncated file.
    // Neither names the mistake, so the loader rejects it up front — and this test is written
    // against the verdict, not against either platform's stream behavior.
    // The verdict is its own: aliasing it onto "cannot open" would make `--feed bench/scenarios`
    // (one level too high) and `--feed bench/scenario` (a typo) indistinguishable, which the
    // section below is written to forbid.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mm_feed_dir_" + std::to_string(std::random_device{}()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE(std::filesystem::create_directory(dir));
    CHECK_THROWS_MATCHES(
        mm::load_feed(dir), std::runtime_error,
        Catch::Matchers::MessageMatches(ContainsSubstring("is a directory, not a feed file") &&
                                        ContainsSubstring(dir.string())));
    // ...and it is NOT the message a nonexistent path gets, so the two mistakes stay apart.
    CHECK_THROWS_MATCHES(
        mm::load_feed(dir), std::runtime_error,
        Catch::Matchers::MessageMatches(!ContainsSubstring("cannot open feed file")));
    std::filesystem::remove(dir, ec);
  }
}

TEST_CASE("feed: accepted spellings the JSON grammar allows", "[feed]") {
  SECTION("whitespace inside the object and the array is legal") {
    const TempFeed file{"{ \"set\" : [ 500000 , 100 , 500010 , 80 ] }\n{\"end\" : true}\n"};
    CHECK(mm::load_feed(file.path()) == std::vector<mm::FeedEvent>{kBase, mm::FeedEnd{}});
  }
  SECTION("CRLF line endings load, and a missing final newline loads") {
    const TempFeed file{"{\"drop\":true}\r\n{\"end\":true}"};
    CHECK(mm::load_feed(file.path()) == std::vector<mm::FeedEvent>{mm::FeedDrop{}, mm::FeedEnd{}});
  }
  SECTION("a zero quantity is how a scenario expresses an ABSENT side (engine.hpp)") {
    const TempFeed file{"{\"set\":[500000,100,0,0]}\n"};
    CHECK(mm::load_feed(file.path()) == std::vector<mm::FeedEvent>{mm::FeedSet{500000, 100, 0, 0}});
  }
  SECTION("a script without `end` is legal: it is the --loop shape bench_paced.feed uses") {
    const TempFeed file{"{\"drop\":true}\n"};
    CHECK(mm::load_feed(file.path()) == std::vector<mm::FeedEvent>{mm::FeedDrop{}});
  }
  SECTION("the 24-hour halt ceiling is itself legal, and bench_idle's hour sits under it") {
    // Pins the cap AT its stated value: a tighter ceiling would red here, and the shipped
    // hour-long idle halt (pinned in its own case above) must keep loading.
    const TempFeed file{"{\"halt_ms\":86400000}\n"};
    CHECK(mm::load_feed(file.path()) == std::vector<mm::FeedEvent>{mm::FeedHalt{86'400'000}});
  }
  SECTION("a line far past the wire's 64 KiB frame cap loads: no size budget, by decision") {
    // The transport caps a frame because those bytes come from a peer. A scenario file is
    // handed to this process by its own operator and read once before the acceptor listens, so
    // a cap here would refuse a legal script for being LARGE rather than malformed — the shape
    // the Python codec's gate deleted from the Python codec. This pins the decision so that adding
    // a budget has to be a deliberate edit to a red test, not a quiet tightening.
    std::string padded = "{\"set\":[500000,100,500010,";
    padded.append(100 * 1024, ' ');
    padded += "80]}\n";
    const TempFeed file{padded};
    CHECK(mm::load_feed(file.path()) == std::vector<mm::FeedEvent>{kBase});
  }
}

TEST_CASE("feed: Catch2 StringMaker for FeedEvent is selected", "[feed]") {
  // The StringMaker above otherwise runs only on FAILING assertions, so a green suite would
  // never execute it and a broken diagnostic would ship unnoticed (gate finding).
  CHECK(Catch::Detail::stringify(mm::FeedEvent{kBase}) == "set[500000,100,500010,80]");
  CHECK(Catch::Detail::stringify(mm::FeedEvent{mm::FeedHalt{1200}}) == "halt_ms 1200");
  CHECK(Catch::Detail::stringify(mm::FeedEvent{mm::FeedDrop{}}) == "drop");
  CHECK(Catch::Detail::stringify(mm::FeedEvent{mm::FeedEnd{}}) == "end");
}

// A streambuf that serves `ok` bytes, then reports a hard read error. std::ifstream over a real
// file cannot be made to fail mid-read on demand, which is why load_feed's `in.bad()` guard was
// the one meaningful branch no test could see — the stream overload exists to
// make it observable.
namespace {
class FailAfter : public std::streambuf {
public:
  explicit FailAfter(std::string ok) : ok_{std::move(ok)} {}

private:
  // getline pulls through underflow(). Serve the good bytes once, then THROW: streambuf
  // exceptions escaping a read are exactly what sets badbit on the stream.
  int underflow() override {
    if (!served_) {
      served_ = true;
      setg(ok_.data(), ok_.data(), ok_.data() + ok_.size());
      return traits_type::to_int_type(*gptr());
    }
    throw std::ios_base::failure{"simulated device error"};
  }
  std::string ok_;
  bool served_{false};
};
} // namespace

TEST_CASE("feed: a truncated read is never replayed as a whole script", "[feed]") {
  // The stream yields one complete, VALID event and then fails. Without the bad() guard the
  // loader would return that single event and the caller would replay a partial scenario as if
  // it were the whole file — the failure mode the guard exists to prevent.
  FailAfter buf{"{\"set\":[500000,100,500010,80]}\n"};
  std::istream in{&buf};
  in.exceptions(std::ios::goodbit);
  REQUIRE_THROWS_WITH(mm::load_feed_stream(in, "truncated.feed"),
                      Catch::Matchers::ContainsSubstring("error reading feed file truncated.feed"));
}

TEST_CASE("feed: the stream overload parses identically to the file one", "[feed]") {
  // The seam must not become a second dialect: same bytes, same events, same diagnostics.
  std::istringstream ok{"{\"set\":[500000,100,500010,80]}\n{\"end\":true}\n"};
  const auto from_stream = mm::load_feed_stream(ok, "inline.feed");
  REQUIRE(from_stream.size() == 2);
  CHECK(std::holds_alternative<mm::FeedSet>(from_stream[0]));
  CHECK(std::holds_alternative<mm::FeedEnd>(from_stream[1]));

  std::istringstream bad{"{\"halt_ms\":0}\n"};
  REQUIRE_THROWS_WITH(mm::load_feed_stream(bad, "inline.feed"),
                      Catch::Matchers::ContainsSubstring("inline.feed: line 1"));
}
