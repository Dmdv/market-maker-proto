// Task 6.5 tests: the telemetry writer's FAILURE paths — the third TU of the telemetry
// suite (seam map in telemetry_test_support.hpp). What this half PROMISES, each promise
// its own falsifiable case: an unopenable output path and a non-positive poll interval
// each fail construction loudly, before a thread exists — and before the stream opens,
// so a failed construction leaves the previous run's file intact (the open truncates;
// validation must come first); a mid-run IO failure degrades telemetry to a no-op sink —
// the engine's pushes keep landing and draining, nothing throws, nothing stalls — while
// the first observed failure LATCHES output_failed() and shutdown reports the loss
// exactly once on stderr, naming the failure site, the path and the post-latch loss
// count; a failure on the stream's FIRST flush — the open marker's own — latches in the
// CONSTRUCTOR, so a writer born onto a dead sink never reports healthy, every record it
// drains is a counted loss rather than a line formatted into the dead stream outside
// the accounting, and the report names "at the open marker" (with or without records);
// and — the latch's control half — a CLEAN run never latches and never reports,
// asserted as zero occurrences of the report line rather than "stderr empty" (sanitizer
// runtimes chat on stderr, so emptiness would red under asan/tsan for unrelated
// reasons).
//
// The IO-failure case forces out_.fail() DETERMINISTICALLY instead of hoping for a full
// disk: RLIMIT_FSIZE caps this process's file size, SIGXFSZ is ignored so the crossing
// write surfaces as EFBIG -> badbit rather than killing the process, and stderr is
// captured through a dup2 redirect so the shutdown report is a string assertion, not a
// claim. All three process-wide changes are RAII-restored (a failed REQUIRE must not
// leave the rest of the process capped, deaf to SIGXFSZ, or writing stderr into a temp
// file); under ctest each case is its own process, so nothing leaks across cases either.
#include <catch2/catch_test_macros.hpp>

#include "mm/telemetry.hpp"
#include "telemetry_test_support.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <sys/resource.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using telemetry_test::lines_of;
using telemetry_test::TempPath;
using telemetry_test::wait_for_lines;

// The stable prefix of the writer's failure report (the full line also carries the
// failure SITE — "at the open marker" or "mid-run" — plus the output path and the
// runtime post-latch loss count, so the cases match on this prefix and assert those
// pieces separately).
constexpr std::string_view kFailureReportPrefix = "mm: telemetry writer: output stream failed";

std::size_t failure_report_count(const std::filesystem::path &captured_stderr) {
  std::size_t count = 0;
  for (const std::string &line : lines_of(captured_stderr))
    if (line.find(kFailureReportPrefix) != std::string::npos)
      ++count;
  return count;
}

// The report line itself, empty when none was captured: each case pins the exactly-once
// count above, then asserts the line's pieces (site, path, loss count) through this.
std::string failure_report_line(const std::filesystem::path &captured_stderr) {
  for (const std::string &line : lines_of(captured_stderr))
    if (line.find(kFailureReportPrefix) != std::string::npos)
      return line;
  return {};
}

// Lowers this process's file-size cap (soft limit only) and restores it on destruction.
// Lower-then-re-raise of the soft limit is always permitted, so the restore cannot fail
// for rights reasons.
class FileSizeCapGuard {
public:
  explicit FileSizeCapGuard(rlim_t cap) {
    REQUIRE(getrlimit(RLIMIT_FSIZE, &old_) == 0);
    const rlimit capped{.rlim_cur = cap, .rlim_max = old_.rlim_max};
    REQUIRE(setrlimit(RLIMIT_FSIZE, &capped) == 0);
  }
  ~FileSizeCapGuard() { setrlimit(RLIMIT_FSIZE, &old_); }
  FileSizeCapGuard(const FileSizeCapGuard &) = delete;
  FileSizeCapGuard &operator=(const FileSizeCapGuard &) = delete;
  FileSizeCapGuard(FileSizeCapGuard &&) = delete;
  FileSizeCapGuard &operator=(FileSizeCapGuard &&) = delete;

private:
  rlimit old_{};
};

// A write past RLIMIT_FSIZE raises SIGXFSZ, whose default disposition TERMINATES the
// process; ignoring it turns the excess write into an EFBIG failure the stream records
// as sticky badbit — exactly the mid-run IO failure under test. Restores the previous
// disposition on destruction.
class IgnoreSigxfszGuard {
public:
  IgnoreSigxfszGuard() : old_(std::signal(SIGXFSZ, SIG_IGN)) { REQUIRE(old_ != SIG_ERR); }
  ~IgnoreSigxfszGuard() { std::signal(SIGXFSZ, old_); }
  IgnoreSigxfszGuard(const IgnoreSigxfszGuard &) = delete;
  IgnoreSigxfszGuard &operator=(const IgnoreSigxfszGuard &) = delete;
  IgnoreSigxfszGuard(IgnoreSigxfszGuard &&) = delete;
  IgnoreSigxfszGuard &operator=(IgnoreSigxfszGuard &&) = delete;

private:
  void (*old_)(int);
};

// Redirects stderr into a file so the writer's shutdown report is capturable, restoring
// the real stderr on destruction (flush first: freopen may leave the stream buffered).
// The restoring dup2 targets fileno(stderr) rather than a hardcoded 2, so the restore
// is correct whichever descriptor freopen left the stream on.
class StderrCaptureGuard {
public:
  explicit StderrCaptureGuard(const std::filesystem::path &target)
      : saved_fd_(dup(fileno(stderr))) {
    REQUIRE(saved_fd_ != -1);
    const bool redirected = std::freopen(target.string().c_str(), "w", stderr) != nullptr;
    if (!redirected) {
      // freopen closes stderr even when the reopen fails, and the REQUIRE below aborts
      // this constructor — the destructor never runs for a throwing construction. Restore
      // the descriptor and release the duplicate HERE, or the failure leaves the process
      // with stderr closed and saved_fd_ leaked. fileno() on a closed stream is
      // UNDEFINED, so the restore targets STDERR_FILENO — the descriptor the redirect is
      // defined against.
      dup2(saved_fd_, STDERR_FILENO);
      close(saved_fd_);
    }
    REQUIRE(redirected);
    // freopen re-buffered the stream for its regular-file target (stderr's unbuffered
    // default does not survive it), and C17 permits setvbuf only before any other
    // operation on the freshly associated stream — so the mode is pinned back to
    // unbuffered HERE, at the one conforming point. The destructor's dup2 swaps only the
    // DESCRIPTOR, so this mode is also the post-restore mode; without it, post-restore
    // stderr writes would sit in a BLOCK buffer and print out of order against unbuffered
    // writers (measured inversion).
    std::setvbuf(stderr, nullptr, _IONBF, 0);
  }
  ~StderrCaptureGuard() {
    std::fflush(stderr);
    dup2(saved_fd_, fileno(stderr));
    close(saved_fd_);
  }
  StderrCaptureGuard(const StderrCaptureGuard &) = delete;
  StderrCaptureGuard &operator=(const StderrCaptureGuard &) = delete;
  StderrCaptureGuard(StderrCaptureGuard &&) = delete;
  StderrCaptureGuard &operator=(StderrCaptureGuard &&) = delete;

private:
  int saved_fd_;
};

} // namespace

TEST_CASE("telemetry: an unopenable output path fails construction loudly", "[telemetry]") {
  mm::SpscTelemetryRing ring;
  const auto bad =
      std::filesystem::temp_directory_path() / "mm_telemetry_no_such_dir" / "out.jsonl";
  REQUIRE_THROWS_AS(mm::TelemetryWriter(ring, bad), std::runtime_error);
}

TEST_CASE("telemetry: a non-positive poll interval fails construction loudly and leaves the "
          "previous run's file intact",
          "[telemetry]") {
  // cv_.wait_for with a non-positive timeout returns immediately, so a 0ms poll would
  // silently busy-spin the writer thread at 100% of a core — same fail-loudly policy as
  // the ring's zero capacity: a misconfiguration is loud at startup, before any thread.
  // The rejection must also precede the OPEN (poll_ is declared before out_ and validated
  // in its initializer): the open truncates, so with the order reversed a rejected
  // construction would empty the previous run's file on its way out — the pre-seeded
  // line below must SURVIVE both throws.
  mm::SpscTelemetryRing ring;
  const TempPath out{"bad_poll"};
  {
    std::ofstream pre{out.path()};
    pre << "{\"prior_run\":1}\n";
  }
  REQUIRE_THROWS_AS(mm::TelemetryWriter(ring, out.path(), 0ms), std::invalid_argument);
  REQUIRE_THROWS_AS(mm::TelemetryWriter(ring, out.path(), -1ms), std::invalid_argument);
  const auto lines = lines_of(out.path());
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == R"({"prior_run":1})");
}

TEST_CASE("telemetry: a mid-run IO failure latches, degrades to a no-op sink, and shutdown "
          "reports it once on stderr",
          "[telemetry]") {
  // The open marker consumes ~110 bytes of the cap and 40 reject lines are ~1.8 KiB, so
  // whatever the writer's batching, some flush crosses the 512-byte cap: the kernel
  // fills the file to the cap exactly, fails the remainder with EFBIG (SIGXFSZ ignored),
  // and the stream goes stickily bad.
  constexpr rlim_t kCap = 512;
  const TempPath out{"io_fail"};
  const TempPath err{"io_fail_stderr"};
  mm::SpscTelemetryRing ring;
  {
    const FileSizeCapGuard cap{kCap};
    const IgnoreSigxfszGuard no_sigxfsz;
    const StderrCaptureGuard capture{err.path()};
    mm::TelemetryWriter writer{ring, out.path(), 1ms};
    REQUIRE_FALSE(writer.output_failed()); // healthy so far: the latch starts clear
    for (std::uint64_t seq = 0; seq < 40; ++seq)
      REQUIRE(ring.try_push(
          mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq)));
    // Bounded wait for the crossing flush (file_size needs no lines_of parse — the last
    // "line" is cut mid-byte at the cap by design).
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::filesystem::file_size(out.path()) < kCap &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(2ms);
    REQUIRE(std::filesystem::file_size(out.path()) == kCap);
    // The writer's own post-flush check observes the failure within the same drain that
    // crossed the cap; the wait below only covers this thread catching up to that.
    const auto latch_deadline = std::chrono::steady_clock::now() + 10s;
    while (!writer.output_failed() && std::chrono::steady_clock::now() < latch_deadline)
      std::this_thread::sleep_for(2ms);
    REQUIRE(writer.output_failed());

    // The engine-survival half: after the latch, pushes keep landing and the writer
    // keeps draining them — counted as lost, never formatted — no throw, no stall, no
    // growth. The producer-side pending() read is the contract's safe overestimate, so
    // observing 0 means genuinely drained. These records drain strictly after the latch
    // (they are pushed after it was observed), so the shutdown report's loss count must
    // cover every one of them.
    for (std::uint64_t seq = 40; seq < 80; ++seq)
      REQUIRE(ring.try_push(
          mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq)));
    // A fresh budget for this second wait (the writer TU's per-wait convention): reusing
    // the crossing-flush deadline would leave the drain only that wait's remainder.
    const auto drain_deadline = std::chrono::steady_clock::now() + 10s;
    while (ring.pending() > 0 && std::chrono::steady_clock::now() < drain_deadline)
      std::this_thread::sleep_for(2ms);
    REQUIRE(ring.pending() == 0);
    REQUIRE(std::filesystem::file_size(out.path()) == kCap);
  } // writer destroyed INSIDE the guards' scope: its stderr report lands in `err`

  // Exactly once for the whole run — the belt check at shutdown, not a line per failed
  // emit — naming the mid-run site (the open marker succeeded under this cap), the path
  // and a post-latch loss count that covers at least the second batch (records drained
  // pre-latch are the truncated file's loss, not this count's).
  REQUIRE(failure_report_count(err.path()) == 1);
  const std::string report = failure_report_line(err.path());
  CHECK(report.find("failed mid-run") != std::string::npos);
  CHECK(report.find(out.path().string()) != std::string::npos);
  const std::size_t count_at = report.rfind(": ");
  REQUIRE(count_at != std::string::npos);
  CHECK(std::stoull(report.substr(count_at + 2)) >= 40);
}

TEST_CASE("telemetry: an open-marker flush failure latches at construction and every drained "
          "record is a counted loss",
          "[telemetry]") {
  // The first-flush window (terminal-gate codex S2): the open marker is the stream's
  // FIRST write+flush, made in the constructor. Unchecked, a writer born onto a dead
  // sink reported HEALTHY — output_failed() false, no stderr — while its first drained
  // batch was formatted into the already-failed stream ahead of drain()'s post-flush
  // detection: swallowed OUTSIDE the loss accounting. Cap 0 fails the marker's write
  // wholesale and deterministically (the truncating open itself only shrinks the file,
  // always under the cap), so the latch must be observable the moment construction
  // returns, and every record pushed afterwards must land in the report's loss count.
  //
  // Harness shape: the cap guard's scope covers ONLY construction — the shutdown report
  // goes to the captured stderr FILE, which sits under the same RLIMIT_FSIZE, so a cap
  // small enough to fail the ~107-byte marker would also swallow the ~150-byte report
  // this case exists to read. Lifting the cap resurrects nothing: badbit is sticky, and
  // that stickiness (not the cap) is what the latch reports.
  const TempPath out{"open_fail"};
  const TempPath err{"open_fail_stderr"};
  mm::SpscTelemetryRing ring;
  {
    const StderrCaptureGuard capture{err.path()};
    std::optional<mm::TelemetryWriter> writer; // outlives the cap; destroyed before `capture`
    {
      const FileSizeCapGuard cap{0};
      const IgnoreSigxfszGuard no_sigxfsz;
      writer.emplace(ring, out.path(), 1ms);
      REQUIRE(writer->output_failed()); // latched by the CONSTRUCTOR, before any drain ran
    }
    for (std::uint64_t seq = 0; seq < 40; ++seq)
      REQUIRE(ring.try_push(
          mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq)));
    const auto drain_deadline = std::chrono::steady_clock::now() + 10s;
    while (ring.pending() > 0 && std::chrono::steady_clock::now() < drain_deadline)
      std::this_thread::sleep_for(2ms);
    REQUIRE(ring.pending() == 0); // the dead sink still drains: telemetry never backs up
    REQUIRE(writer->output_failed());
  } // writer destroyed inside the capture's scope: its stderr report lands in `err`

  // One report line for the run, naming the site, the path, and a loss count that
  // covers every record — all 40 drained strictly after the construction-time latch.
  REQUIRE(failure_report_count(err.path()) == 1);
  const std::string report = failure_report_line(err.path());
  CHECK(report.find("failed at the open marker") != std::string::npos);
  CHECK(report.find(out.path().string()) != std::string::npos);
  const std::size_t count_at = report.rfind(": ");
  REQUIRE(count_at != std::string::npos);
  CHECK(std::stoull(report.substr(count_at + 2)) >= 40);
}

TEST_CASE("telemetry: a zero-record run with a failed open marker still reports the site once",
          "[telemetry]") {
  // The window's pure form: nothing is ever pushed, so no drain ever writes — before
  // the constructor's check existed, nothing latched until the shutdown belt, and the
  // writer spent its whole run reporting healthy over a dead sink. Zero lost records is
  // the point: the report's value here is the SITE — the operator learns the file died
  // at its first flush — and the exact ": 0 record(s)" suffix pins that the loss
  // accounting starts empty rather than inheriting phantom counts.
  const TempPath out{"open_fail_empty"};
  const TempPath err{"open_fail_empty_stderr"};
  mm::SpscTelemetryRing ring;
  {
    const StderrCaptureGuard capture{err.path()};
    std::optional<mm::TelemetryWriter> writer; // the record-bearing case's harness shape
    {
      const FileSizeCapGuard cap{0};
      const IgnoreSigxfszGuard no_sigxfsz;
      writer.emplace(ring, out.path(), 1ms);
      REQUIRE(writer->output_failed()); // latched by the constructor: no drain has run yet
    }
  } // writer destroyed (report -> err, cap already lifted), then stderr restored
  REQUIRE(failure_report_count(err.path()) == 1);
  const std::string report = failure_report_line(err.path());
  CHECK(report.find("failed at the open marker") != std::string::npos);
  CHECK(report.ends_with(": 0 record(s) lost after the failure latched"));
}

TEST_CASE("telemetry: a clean run never latches the failure flag and reports nothing on stderr",
          "[telemetry]") {
  // The latch's control half, against the always-latch / always-report mutant space: a
  // healthy stream must leave output_failed() false for the whole run and put NO report
  // line on stderr. Asserted as zero occurrences of the report prefix, deliberately NOT
  // as "stderr is empty": sanitizer runtimes chat on stderr, so an emptiness assertion
  // would red under asan/tsan for reasons unrelated to the writer.
  const TempPath out{"clean"};
  const TempPath err{"clean_stderr"};
  mm::SpscTelemetryRing ring;
  {
    const StderrCaptureGuard capture{err.path()};
    {
      mm::TelemetryWriter writer{ring, out.path(), 1ms};
      for (std::uint64_t seq = 0; seq < 5; ++seq)
        REQUIRE(ring.try_push(
            mm::event_record(mm::TelemetryEvent::Reject, static_cast<std::int64_t>(seq), seq)));
      REQUIRE(wait_for_lines(out.path(), 6).size() == 6); // open marker + the five records
      CHECK_FALSE(writer.output_failed()); // everything drained and the latch stayed clear
    } // writer destroyed inside the capture: a shutdown-path report could not escape it
  }
  REQUIRE(failure_report_count(err.path()) == 0);
  // The run's lines all landed: both markers present, nothing eaten by a phantom latch.
  REQUIRE(lines_of(out.path()).size() == 7);
}
