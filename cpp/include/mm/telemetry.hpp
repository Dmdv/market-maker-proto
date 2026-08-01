// In-process telemetry: hot-path counters and a bounded SPSC ring (mm/telemetry_ring.hpp)
// drained by one background writer thread — one ring and one writer per engine process.
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

// RAII owner of the writer thread; the ring must outlive it and have no other consumer. Emits
// one JSON line per record; post-open IO failure degrades to a counted loss, never a throw.
class TelemetryWriter {
public:
  // Bounds event visibility latency in the output file, not snapshot latency (the owner
  // thread's 1 Hz push cadence sets that); the price is idle wakeups on a quiet engine.
  static constexpr auto kDefaultPoll = std::chrono::milliseconds(10);

  // `out` by value: a once-per-process constructor has nothing to win from shaving the copy.
  TelemetryWriter(SpscTelemetryRing &ring,
                  std::filesystem::path out, // NOLINT(performance-unnecessary-value-param)
                  std::chrono::milliseconds poll = kDefaultPoll)
      // Member order, not init-list order, runs checked_poll before out_ opens: a rejected
      // interval must throw with the previous run's file untouched. trunc: one file, one run.
      : ring_{ring}, poll_{checked_poll(poll)}, out_path_{out.string()},
        out_{out, std::ios::out | std::ios::trunc} {
    if (!out_.is_open())
      throw std::runtime_error("TelemetryWriter: cannot open telemetry output: " + out_path_);
    // Classic locale: the lines are machine-read, so a host locale's digit grouping must not
    // reshape the numbers. Imbued before the open marker, this stream's first line.
    out_.imbue(std::locale::classic());
    emit_open_line();
    // The open marker is this stream's first write+flush; check it here, before the writer
    // thread exists, so a writer born onto a dead sink starts degraded rather than healthy.
    if (out_.fail()) {
      failure_site_ = "at the open marker";
      output_failed_.store(true, std::memory_order_relaxed);
    }
    thread_ = std::thread{[this] { run(); }};
  }

  // Stops and JOINS the writer thread; idempotent for its sole owner (a second join is UB).
  // Exposed separately: output_failed() covers the final drain and close flush only after it.
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

  // Latches permanently on the first output-stream failure the writer observed (relaxed: a
  // health flag, not an ordering edge). Read after stop_and_join() for a whole-run verdict.
  [[nodiscard]] bool output_failed() const noexcept {
    return output_failed_.load(std::memory_order_relaxed);
  }

private:
  // Runs as poll_'s initializer, i.e. in declaration order, ahead of the truncating open.
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

  // Run's open marker: {"ts","wall_ns","ring_capacity","poll_ms","dropped_at_open"}. wall_ns
  // is a wall-clock anchor for correlating with outside logs, never subtracted against ts.
  void emit_open_line() {
    out_ << R"({"ts":)" << steady_ns() << R"(,"wall_ns":)"
         << std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()
         << R"(,"ring_capacity":)" << ring_.capacity() << R"(,"poll_ms":)" << poll_.count()
         << R"(,"dropped_at_open":)" << ring_.dropped() << "}\n";
    out_.flush();
  }

  // Run's close marker: {"ts","pending_left","dropped","stream_ok"}. Emitted on the writer
  // thread after its final drain, where pending_left is exact; on the producer it is not.
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
    drain(); // the final batch: everything published before ITS cutoff, then stop
    emit_close_line();
    out_.flush();
    // A failure surfacing only at this last flush, or inside the close marker itself, still
    // latches, so the report below cannot be dodged by failing late.
    if (out_.fail())
      output_failed_.store(true, std::memory_order_relaxed);
    if (output_failed()) {
      // The run's one stderr line — writer thread, off the hot path, never a throw (fprintf,
      // not std::cerr: <iostream>'s static init has no business in a header).
      std::fprintf(stderr,
                   "mm: telemetry writer: output stream failed %s: %s: %llu record(s) lost "
                   "after the failure latched\n",
                   failure_site_, out_path_.c_str(), static_cast<unsigned long long>(lost_lines_));
    }
  }

  // ONE bounded batch: at most the records pending at entry. An unbounded drain-until-empty
  // would let a sustained producer own this loop, so stop_ is never rechecked and join hangs.
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
    // Detection sits AFTER the flush: out_ is buffered, so an emit's bytes fail at the flush
    // carrying them. Outside the `wrote` guard, so an empty pass latches a dead stream too.
    if (!output_failed() && out_.fail())
      output_failed_.store(true, std::memory_order_relaxed);
  }

  // One record, one line; an unrecognized kind/code still emits the self-describing
  // {"ts","kind","code","args"} form. Within v1 keys are append-only; unknown keys ignored.
  void emit_line(const TelemetryRecord &rec) {
    const TelemetryKind kind{rec.kind}; // POD boundary: fixed base, any uint32 value is valid
    out_ << R"({"ts":)" << rec.ts_ns;
    if (kind == TelemetryKind::Counters && CountersPart{rec.code} == CountersPart::Totals) {
      out_ << R"(,"orders":)" << rec.args[0] << R"(,"fills":)" << rec.args[1] << R"(,"rejects":)"
           << rec.args[2] << R"(,"cancels":)" << rec.args[3] << R"(,"conflated":)" << rec.args[4]
           << R"(,"sessions":)" << rec.args[5] << R"(,"telemetry_dropped":)" << ring_.dropped();
    } else if (kind == TelemetryKind::Counters &&
               CountersPart{rec.code} == CountersPart::Watermarks) {
      // args[5] stays unseated and emits no key; seating it adds a key, not a record shape.
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
  const char *failure_site_ = "mid-run";   // report wording only; set by the constructor's
                                           // open-marker check, before the thread starts
  std::uint64_t lost_lines_{0};            // writer thread only: records swallowed after the latch
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_{false};   // guarded by mutex_
  bool joined_{false}; // guarded by mutex_: a second join on one thread is UB
  std::thread thread_; // last member: every other member exists before the ctor body starts
                       // the thread, and stays so if a future edit moves the start earlier
};

static_assert(!std::is_copy_constructible_v<TelemetryWriter> &&
              !std::is_copy_assignable_v<TelemetryWriter> &&
              !std::is_move_constructible_v<TelemetryWriter> &&
              !std::is_move_assignable_v<TelemetryWriter>);

// Hot-path counters: plain uint64, single-writer on the owner thread — no atomics; they cross
// threads only inside snapshot records. High-water fields never reset. alignas(64): own line.
struct alignas(64) Counters {
  std::uint64_t orders{}, fills{}, rejects{}, cancels{}, conflated{}, outbox_depth_hw{},
      send_lag_max_ns{}, sessions{}, telemetry_pending_hw{}, live_orders{}, stale_books_ignored{};
};

// The snapshot's ring form: totals in one record, watermarks in the other. Totals is full, so
// another totals-class counter needs a third CountersPart; watermarks has one free lane.
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
