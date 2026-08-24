// In-process telemetry (plan Task 6.5, record A1): a bounded SPSC ring drained by a
// background writer thread, plus the plain-uint64 hot-path counters the owner thread
// snapshots through it. This header is the subsystem's UMBRELLA — the record, the typed
// discriminants and the ring itself live in mm/telemetry_ring.hpp (split at gate P4-i1
// under the repo's 500-line file cap) and are re-exported by the include below; the
// writer thread, the Counters aggregate and the snapshot/event mapping are defined here.
//
// WHY THIS SHAPE. The brief forbids blocking disk logging in the measured path and asks
// for metrics that do not distort it. The hot path therefore only ever (a) increments a
// plain uint64 it exclusively owns (rule A1 — one thread owns the data, so atomics would
// buy nothing and cost an atomic read-modify-write), and (b) copies one fixed-size POD
// record into a preallocated ring with two atomic index operations, refusing — never
// waiting — when the ring is full. Everything with a syscall in it (formatting, file IO,
// flushing) happens on the writer thread. The ring is deliberately IN-PROCESS: both ends
// are threads of this engine, so the whole protocol sits inside TSan's model and is
// stress-verified under it (`ctest --preset tsan`, the million-record case in
// cpp/tests/test_telemetry.cpp; authoritatively under the container toolchain —
// bench/probes/tsan_struct_copy_probe.cpp records why the dev host's runtime cannot see
// the slot copies) — this is a functional telemetry need, not a gratuitous lock-free
// exhibit; the cross-process ring the decision record demoted (D5) stays a ranked
// proposal.
//
// THREAD ROLES, fixed by contract: exactly ONE producer thread calls try_push (the engine
// owner thread) and exactly ONE consumer thread calls try_pop (the TelemetryWriter's).
// The cardinality one level up is NORMATIVE too: ONE ring and ONE writer per engine
// process — never per connection or per session; sessions share the owner thread's single
// producer. Violating that fails concretely, not stylistically: per-session writers
// opening the same --telemetry-out path each open truncating and mutually truncate one
// another's files, and a second producer breaks the single-writer arithmetic on the
// ring's drop counter. `dropped()` and `capacity()` are safe from any thread;
// `pending()` is exact on the consumer, a safe overestimate on the producer, and
// OFF-CONTRACT anywhere else (its contract note has the three-way split). Task 7 wires it
// as: Server owns the Counters, the ring and the writer; the owner thread pushes a
// counters snapshot on a dedicated timer plus sparse event records as they happen.
#pragma once

#include "mm/telemetry_ring.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace mm {

// Owns the consumer thread; RAII: the constructor starts it, the destructor stops, joins
// and flushes. Emits each popped record as one self-describing JSON line (shapes below at
// emit_line; exact strings pinned across the telemetry test TUs), bracketed by two RUN
// MARKERS that are the writer's own, not ring records:
//   open  (constructor, after a successful open)
//         {"ts":..,"wall_ns":..,"ring_capacity":..,"poll_ms":..,"dropped_at_open":..}
//         wall_ns is a WALL-CLOCK ANCHOR ONLY — for correlating this file with outside
//         logs; ts stays steady_clock, and the two are NEVER subtracted against each
//         other (the Global Constraints clock rule).
//   close (writer THREAD, end of run(), after the final drain)
//         {"ts":..,"pending_left":..,"dropped":..,"stream_ok":..}
//         Deliberately NOT the destructor's line to write: the dtor runs on the producer
//         thread, where pending() is only a safe overestimate; on the writer thread,
//         after its own final drain, the leftover count is EXACT. A zero-push run emits
//         exactly the open+close pair (pinned in cpp/tests/test_telemetry_wire.cpp).
//
// Contracts the caller keeps: the ring outlives the writer, and exactly one writer drains
// a given ring (single consumer). The destructor's final drain empties everything pushed
// BEFORE it began — its debt is CUT OFF at the pending() count sampled after the stop
// signal, so destruction completes in BOUNDED time even against a producer that never
// stops pushing (an unbounded drain-until-empty would let such a producer pin the
// consumer inside the final drain forever and hang the join). A producer still pushing
// DURING destruction races the shutdown for delivery (never for memory safety): records
// pushed after the cutoff are that producer's breach of the quiesce-first contract, not
// the writer's debt — Task 7 quiesces the owner thread first. Records left in the RING at
// destruction are not lost: the ring keeps them for a successor writer draining the same
// ring. The FILE is a different story, per-run by design: the stream opens TRUNCATING
// (one file = one run — a reader never has to split a file into runs), so a successor
// that must preserve its predecessor's lines writes a fresh path; reusing the same path
// deliberately starts that file over.
//
// The poll interval bounds live-drain LATENCY only, not shutdown: the wait is
// interruptible, so destruction is prompt whatever the interval. kDefaultPoll is the
// interval Task 7 ships; a non-positive interval would turn the timed wait into a
// busy-spin, so the constructor rejects it (std::invalid_argument — the ring's
// zero-capacity precedent) BEFORE the stream member opens: the open truncates, so a
// rejected construction must leave the previous run's file intact (pinned by the
// pre-seeded half of the throws case in cpp/tests/test_telemetry_errors.cpp). IO failure
// on the OPEN stream (disk full, revoked path — any failure past the successful open)
// degrades to un-emitted lines — ofstream never throws here — because telemetry must
// never take down the engine it narrates; nor is the loss silent: the stream state is
// sticky, and the FIRST failure the writer observes latches output_failed() (the close
// marker's "stream_ok" and Task 7's health checks read it), after which drained records
// are counted rather than formatted, and the end-of-run check — the belt under that
// latch — reports the loss exactly once on stderr, naming the failure SITE ("at the
// open marker" or "mid-run"), the path and the post-latch loss count (writer thread,
// off the hot path; pinned by the RLIMIT_FSIZE cases in
// cpp/tests/test_telemetry_errors.cpp, whose clean-run control pins the latch's
// silence). The latch's coverage starts at the stream's FIRST flush: the constructor
// checks the open marker's own write+flush before the writer thread exists, so a writer
// born onto a dead sink is degraded FROM CONSTRUCTION — output_failed() already true —
// and every record it ever drains is a counted loss; unchecked, that first batch was
// exactly the swallowed one — drain()'s detection sat after its own flush only, so
// nothing latched until the first record's flush (or, with no records, shutdown) and
// the records formatted into the dead stream meanwhile were lost OUTSIDE the
// accounting. The unopenable-at-start case, by contrast, throws in the constructor
// below, before any thread exists, so a misconfiguration is loud at startup (the
// ops-failure policy).
class TelemetryWriter {
public:
  // The plan's 10 ms default, NAMED so it is one pinnable constant rather than a literal
  // buried in a signature: Task 7 constructs the writer without a poll argument. What
  // this value bounds is EVENT visibility latency — how stale the file may be when
  // Task 7's integration tests grep it for a just-pushed event line — NOT snapshot
  // latency, which the owner thread's 1 Hz push cadence sets by itself; the stated price
  // is idle wakeups, roughly a hundred empty polls per snapshot interval on a quiet
  // engine. The VALUE is pinned exactly at compile time (STATIC_REQUIRE == 10ms in
  // cpp/tests/test_telemetry_writer.cpp); the runtime default-constructed-writer case
  // there bounds gross drift only — its wait deadline is 10 s, so any smaller drift is
  // the value pin's catch, not the runtime case's.
  static constexpr auto kDefaultPoll = std::chrono::milliseconds(10);

  // `out` stays by value: the signature is the plan's interface block, verbatim (a
  // once-per-process constructor has nothing to win from shaving the copy).
  TelemetryWriter(SpscTelemetryRing &ring,
                  std::filesystem::path out, // NOLINT(performance-unnecessary-value-param)
                  std::chrono::milliseconds poll = kDefaultPoll)
      // poll_ initializes (and so validates, via checked_poll) BEFORE out_ opens — member
      // order, not init-list order, decides that; a bad poll must throw with the previous
      // run's file untouched. trunc spelled out: the one-file-one-run decision is a
      // contract (class comment), not an accident of ofstream's default openmode.
      : ring_{ring}, poll_{checked_poll(poll)}, out_path_{out.string()},
        out_{out, std::ios::out | std::ios::trunc} {
    if (!out_.is_open())
      throw std::runtime_error("TelemetryWriter: cannot open telemetry output: " + out_path_);
    // Classic locale: the lines are a machine-read surface, so a host locale with digit
    // grouping must not be able to reshape the numbers. Imbued before the open marker —
    // the first line this stream carries is already under the pin.
    out_.imbue(std::locale::classic());
    emit_open_line();
    // The open marker was this stream's FIRST write+flush — check it HERE, before the
    // writer thread exists. Unchecked, a writer born onto a dead sink (output path
    // already at its size cap, dead device) started life reporting HEALTHY, and its
    // first drained batch was formatted into the failed stream ahead of drain()'s
    // post-flush detection — swallowed outside the loss accounting. Same
    // degrade-not-throw contract as any later failure (only the pre-open throws above
    // are startup-loud): construction succeeds, the writer runs as a counted-loss sink,
    // and the end-of-run report names this site.
    if (out_.fail()) {
      failure_site_ = "at the open marker";
      output_failed_.store(true, std::memory_order_relaxed);
    }
    thread_ = std::thread{[this] { run(); }};
  }

  // Stops the writer thread and JOINS it — the destructor's whole body, and idempotent
  // for its OWNER's repeated calls (a second join on one thread is UB; the latch skips
  // it). It is not a barrier for concurrent callers, which the sole-owner contract below
  // already excludes. Exposed separately because output_failed() sampled BEFORE this has
  // not yet seen the final drain or the close marker's flush, where a dying sink most
  // often surfaces: a caller that reports telemetry health at shutdown (Task 7's
  // telemetry_ok()) must call this first and read the latch after it, or it reports a
  // verdict the run has not finished reaching.
  void stop_and_join() {
    {
      const std::scoped_lock lock{mutex_};
      if (joined_)
        return;
      joined_ = true;
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join(); // the exit path drained through the stop cutoff and flushed before this
  }

  ~TelemetryWriter() { stop_and_join(); }

  // Sole owner of the thread (and of the output stream position): duplicating either end
  // would interleave two drains over one consumer index.
  TelemetryWriter(const TelemetryWriter &) = delete;
  TelemetryWriter &operator=(const TelemetryWriter &) = delete;
  TelemetryWriter(TelemetryWriter &&) = delete;
  TelemetryWriter &operator=(TelemetryWriter &&) = delete;

  // True from the first output-stream failure the writer observed, permanently: a
  // one-way latch (relaxed — a health flag, not an ordering edge). The close marker's
  // "stream_ok" is this latch's negation at close time; Task 7's shutdown health check
  // reads it after stop_and_join(), which is what makes the two agree.
  [[nodiscard]] bool output_failed() const noexcept {
    return output_failed_.load(std::memory_order_relaxed);
  }

private:
  // The ring's checked_capacity shape: validation as the member's initializer, so it runs
  // in declaration order — ahead of the truncating open — and a rejected interval cannot
  // have emptied the file first.
  static std::chrono::milliseconds checked_poll(std::chrono::milliseconds poll) {
    if (poll <= std::chrono::milliseconds::zero())
      throw std::invalid_argument("TelemetryWriter: poll interval must be > 0ms");
    return poll;
  }

  [[nodiscard]] static std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // The run's OPEN marker (shape and the wall_ns anchor-only rule in the class comment).
  // Runs on the CONSTRUCTING thread, before the writer thread exists — no concurrent
  // stream use to order against. Flushed immediately: the marker is on disk before the
  // engine pushes its first record.
  void emit_open_line() {
    out_ << R"({"ts":)" << steady_ns() << R"(,"wall_ns":)"
         << std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()
         << R"(,"ring_capacity":)" << ring_.capacity() << R"(,"poll_ms":)" << poll_.count()
         << R"(,"dropped_at_open":)" << ring_.dropped() << "}\n";
    out_.flush();
  }

  // The run's CLOSE marker (shape and the why-not-the-dtor in the class comment).
  // pending_left is EXACT here: this thread just ran the final drain, and only this
  // thread pops. stream_ok reports the latch AS OF this line — a failure inside the very
  // flush that carries the marker is the belt's to report (run()'s stderr line), because
  // the marker cannot describe its own loss.
  void emit_close_line() {
    out_ << R"({"ts":)" << steady_ns() << R"(,"pending_left":)" << ring_.pending()
         << R"(,"dropped":)" << ring_.dropped() << R"(,"stream_ok":)"
         << (output_failed() ? "false" : "true") << "}\n";
  }

  void run() {
    for (;;) {
      drain();
      std::unique_lock<std::mutex> lock{mutex_};
      if (stop_)
        break;
      if (ring_.pending() > 0)
        continue; // still busy: next batch back-to-back — the timed wait is the idle regime
      if (cv_.wait_for(lock, poll_, [this] { return stop_; }))
        break;
    }
    drain(); // the final batch: everything published before ITS cutoff, then down tools
    emit_close_line();
    out_.flush();
    // Belt half of the failure latch: a failure surfacing only at this last flush (or
    // inside the close marker itself) still latches, so the report below cannot be
    // dodged by failing late.
    if (out_.fail())
      output_failed_.store(true, std::memory_order_relaxed);
    if (output_failed()) {
      // The run's ONE stderr line — writer thread, off the hot path, never a throw
      // (fprintf, not std::cerr: <iostream>'s static init has no business in this
      // header). The site says WHERE the latch caught the stream — the constructor's
      // open-marker check or the writer's own passes — and the count is the post-latch
      // tally: records the latched sink swallowed whole. The failing flush's own
      // partial loss shows as the truncated file, not in this number.
      std::fprintf(stderr,
                   "mm: telemetry writer: output stream failed %s: %s: %llu record(s) lost "
                   "after the failure latched\n",
                   failure_site_, out_path_.c_str(), static_cast<unsigned long long>(lost_lines_));
    }
  }

  // ONE bounded batch: at most the records pending at entry — the tail observed then is a
  // FINITE cutoff. An unbounded drain-until-empty would hand a sustained producer control
  // of this loop's exit: stop_ could never be re-checked and the destructor's join could
  // never return. The entry snapshot loses nothing — FIFO means the budgeted pops are
  // exactly the records published before the batch began; later pushes belong to the next
  // batch or, after stop, to a successor writer. (For this consumer the budget cannot
  // overrun: pending() counts records only this thread removes, so try_pop cannot fail
  // before the budget does — the && is a belt, not a hidden early-exit.)
  void drain() {
    TelemetryRecord rec{};
    bool wrote = false;
    for (std::size_t budget = ring_.pending(); budget > 0 && ring_.try_pop(rec); --budget) {
      if (output_failed()) {
        ++lost_lines_; // the sink is dead: count the loss, skip the formatting, and keep
        continue;      // draining — a failed FILE must never back the RING up
      }
      emit_line(rec);
      wrote = true;
    }
    if (wrote)
      out_.flush(); // each batch reaches disk while the engine runs, not only at shutdown
    // First-failure detection sits AFTER the flush: out_ is buffered, so an emit's bytes
    // fail at the flush that carries them — this is the earliest point the sticky state
    // is real. One-way: nothing ever clears the latch. Deliberately OUTSIDE the `wrote`
    // guard: the check polls the STREAM's sticky state, not this batch's writes, so
    // every pass — an empty one included — latches a stream that failed without a
    // drain-side write, keeping the latch an every-poll invariant of the writer loop
    // rather than a property each write site must separately remember to re-check
    // (today the constructor's open-marker check is the only such site; this is what
    // bounds the exposure if a future edit adds another).
    if (!output_failed() && out_.fail())
      output_failed_.store(true, std::memory_order_relaxed);
  }

  // One record, one line:
  //   counters/totals     {"ts":..,"orders":..,"fills":..,"rejects":..,"cancels":..,
  //                        "conflated":..,"sessions":..,"telemetry_dropped":N}
  //   counters/watermarks {"ts":..,"outbox_depth_hw":..,"send_lag_max_ns":..,
  //                        "stale_books_ignored":..,"telemetry_pending_hw":..,
  //                        "live_orders":..}
  //   named event         {"ts":..,"event":"hwm_close","args":[..]}
  //   anything else       {"ts":..,"kind":..,"code":..,"args":[..]}  — self-describing
  //   fallback, so an unrecognized record is still emitted rather than silently eaten.
  // Key policy, cited from the wire codec's precedent (codec.hpp: "Unknown keys are
  // ignored (additive forward compatibility)"): within v1 of this file format keys are
  // APPEND-ONLY — never renamed, never removed — and consumers ignore unknown keys. The
  // one pre-history exception is the "v" -> "args" rename of the event lane array: "v"
  // collided with the protocol envelope's version key, and no downstream consumer exists
  // yet (Task 7 unbuilt), so the rename landed before the policy's clock started.
  void emit_line(const TelemetryRecord &rec) {
    const TelemetryKind kind{rec.kind}; // POD boundary: fixed base, any uint32 value is valid
    out_ << R"({"ts":)" << rec.ts_ns;
    if (kind == TelemetryKind::Counters && CountersPart{rec.code} == CountersPart::Totals) {
      out_ << R"(,"orders":)" << rec.args[0] << R"(,"fills":)" << rec.args[1] << R"(,"rejects":)"
           << rec.args[2] << R"(,"cancels":)" << rec.args[3] << R"(,"conflated":)" << rec.args[4]
           << R"(,"sessions":)" << rec.args[5] << R"(,"telemetry_dropped":)" << ring_.dropped();
    } else if (kind == TelemetryKind::Counters &&
               CountersPart{rec.code} == CountersPart::Watermarks) {
      // args[5] stays unseated and emits NO key — seating it adds its key here without
      // reshaping the record (args[2] took its reserved seat this way at Task 7).
      out_ << R"(,"outbox_depth_hw":)" << rec.args[0] << R"(,"send_lag_max_ns":)" << rec.args[1]
           << R"(,"stale_books_ignored":)" << rec.args[2] << R"(,"telemetry_pending_hw":)"
           << rec.args[3] << R"(,"live_orders":)" << rec.args[4];
    } else if (const std::string_view name = to_string(TelemetryEvent{rec.code});
               kind == TelemetryKind::Event && !name.empty()) {
      out_ << R"(,"event":")" << name << '"';
      emit_lanes(rec);
    } else {
      out_ << R"(,"kind":)" << rec.kind << R"(,"code":)" << rec.code;
      emit_lanes(rec);
    }
    out_ << "}\n";
  }

  void emit_lanes(const TelemetryRecord &rec) {
    out_ << R"(,"args":[)";
    for (std::size_t lane = 0; lane < std::size(rec.args); ++lane)
      out_ << (lane == 0 ? "" : ",") << rec.args[lane];
    out_ << ']';
  }

  SpscTelemetryRing &ring_;
  std::chrono::milliseconds poll_; // declared before out_: validated first (ctor note)
  std::string out_path_;           // the failure report names the file it lost lines from
  std::ofstream out_;
  std::atomic<bool> output_failed_{false}; // one-way latch: first observed stream failure
  const char *failure_site_ = "mid-run";   // report wording only; rewritten solely by the
                                           // constructor's open-marker check, before the
                                           // thread starts (happens-before via the start)
  std::uint64_t lost_lines_{0};            // writer thread only: records swallowed after the latch
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_{false};   // guarded by mutex_
  bool joined_{false}; // guarded by mutex_: a second join on one thread is UB
  std::thread thread_; // last member, defensively: the ctor BODY starts the thread after
                       // every member exists whatever the order — declaration order
                       // would become load-bearing only if a future edit moved the start
                       // into the mem-init-list, and last-ness makes that edit safe
                       // instead of latently broken
};

static_assert(!std::is_copy_constructible_v<TelemetryWriter> &&
              !std::is_copy_assignable_v<TelemetryWriter> &&
              !std::is_move_constructible_v<TelemetryWriter> &&
              !std::is_move_assignable_v<TelemetryWriter>);

// Hot-path counters: plain uint64, single-writer on the engine owner thread — NO atomics,
// deliberately (rule A1: one thread owns the data; the values cross threads only inside
// snapshot records, whose transport the ring's ordering contract covers). The owner
// thread snapshots them through the ring on its telemetry timer.
//
// Semantic class per field — the file reader's contract (C = cumulative, monotone for
// the process lifetime; G = gauge, an instantaneous level; M = maximum since
// construction, NEVER reset between snapshots — Task 13 reads the M fields as
// queue-buildup evidence precisely because they cannot relax):
//   C  orders, fills, rejects, cancels — one increment per engine action.
//   C  conflated — engine-wide cumulative: a CLOSING session's per-session conflation
//      history folds into a cumulative base at close, so a close never subtracts history
//      from the next snapshot (Task 7's aggregation rule, PENDING item (s)12).
//   G  sessions — sessions currently OPEN: open increments, close decrements.
//      Cumulative opens are recoverable by counting session_open event lines, so the
//      gauge is the non-redundant export.
//   G  live_orders — live orders engine-wide, across all sessions (the only live-count
//      the snapshot carries; Task 10's assertion reads it, PENDING item (s)13).
//   M  outbox_depth_hw, send_lag_max_ns — lifetime high-water marks.
//   M  telemetry_pending_hw — owner-thread max of the producer-side pending() read; that
//      read is a safe OVERESTIMATE (its contract above), the correct direction for a
//      high-water mark — it can only round the alarm UP, never hide it.
//   C  stale_books_ignored — the engine's md_seq-guard drop counter, copied from
//      OrderEngine::stale_books_ignored() at snapshot time (item (k)'s Task 7 obligation:
//      the feed is monotonic by construction, so any nonzero value is a
//      producer-regression alarm). Seated in the WATERMARKS record's reserved lane 2 —
//      totals was already full — so record placement here does not imply the M class.
//
// alignas(64): the owner thread's hottest stores stay on cache lines no neighboring
// Server member shares. Eleven 8-byte fields are 88 payload bytes, so the alignment
// rounds sizeof to 128 — the CHOSEN trade, pinned with the padding stated in
// test_telemetry.cpp: 40 tail-padding bytes once per engine process buy line isolation,
// where the alternative (no alignas, sizeof 88) would let a Counters placed after an
// unrelated hot member share its first line silently. Forward consequence: any type
// embedding Counters (Task 7's Server) inherits alignof 64 and heap-allocates through
// ALIGNED new, outside the test suite's alloc-probe interception (noted in
// cpp/tests/alloc_probe_support.hpp).
struct alignas(64) Counters {
  std::uint64_t orders{}, fills{}, rejects{}, cancels{}, conflated{}, outbox_depth_hw{},
      send_lag_max_ns{}, sessions{}, telemetry_pending_hw{}, live_orders{}, stale_books_ignored{};
};

// The snapshot's ring form: totals in one record, watermarks in the other. Lane budget,
// stated because both records are now spoken for: totals is FULL — every lane seated by
// a cumulative-or-gauge field — so one more totals-class counter needs a THIRD
// CountersPart, and the writer's self-describing fallback makes that a safe (never
// silent) intermediate state, pinned in cpp/tests/test_telemetry_wire.cpp. Watermarks
// seats: args[2] carries the engine's stale-book drop counter (its reserved seat, taken
// at Task 7 per item (k)) and args[5] is the one free lane left — seating it reshapes
// neither RECORD; the watermarks JSON line just gains one key per seated lane.
// `telemetry_dropped` is deliberately NOT a lane: the writer stamps the ring's own count
// at emit time, so the figure covers drops later than this record's push.
[[nodiscard]] constexpr std::array<TelemetryRecord, 2>
snapshot_records(const Counters &c, std::int64_t ts_ns) noexcept {
  return {
      TelemetryRecord{.ts_ns = ts_ns,
                      .kind = static_cast<std::uint32_t>(TelemetryKind::Counters),
                      .code = static_cast<std::uint32_t>(CountersPart::Totals),
                      .args = {c.orders, c.fills, c.rejects, c.cancels, c.conflated, c.sessions}},
      TelemetryRecord{.ts_ns = ts_ns,
                      .kind = static_cast<std::uint32_t>(TelemetryKind::Counters),
                      .code = static_cast<std::uint32_t>(CountersPart::Watermarks),
                      .args = {c.outbox_depth_hw, c.send_lag_max_ns, c.stale_books_ignored,
                               c.telemetry_pending_hw, c.live_orders}}};
}

// An event in ring form; keeps the kind/code pairing in one place instead of at every
// emit site.
[[nodiscard]] constexpr TelemetryRecord event_record(TelemetryEvent event, std::int64_t ts_ns,
                                                     std::uint64_t a0 = 0,
                                                     std::uint64_t a1 = 0) noexcept {
  return TelemetryRecord{.ts_ns = ts_ns,
                         .kind = static_cast<std::uint32_t>(TelemetryKind::Event),
                         .code = static_cast<std::uint32_t>(event),
                         .args = {a0, a1}};
}

} // namespace mm
