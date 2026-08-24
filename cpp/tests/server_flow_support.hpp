// Shared fixtures for the Task 7 server FLOW suites — the three TUs split out of
// test_server_flow.cpp under the repository's 500-line cap (outbox/conflation policy,
// process shutdown, and the durable artifacts). Everything here is either process-wide
// state a case BORROWS and must hand back on every exit path, or a reader for an artifact
// the engine wrote; the cases themselves carry no infrastructure.
#pragma once

#include "server_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/resource.h>

#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>

using namespace server_test;

// A NAMED namespace with inline linkage, not an anonymous one: three TUs include this and
// each uses a different subset, which under an anonymous namespace makes every fixture the
// others do not call an -Wunused-function warning in a tree that ships zero.
namespace server_flow {

inline json last_totals(const std::vector<json> &lines) {
  json last;
  for (const auto &j : lines)
    if (j.contains("orders"))
      last = j;
  return last;
}

inline bool is_tob(const json &msg) { return msg["t"] == "top_of_book"; }

// ---- process-wide state these cases BORROW, restored on every exit path ----

// Captures a signal's disposition and puts it back from the destructor. The blast radius of
// a restore reachable only by falling off the end of a case is process-wide AND cross-case:
// Catch2 runs every case in one process, so an assertion that unwinds past it silently
// changes the disposition for every case that follows — the classic way one real failure
// becomes an unreadable cascade.
class SigactionGuard {
public:
  explicit SigactionGuard(int signo) : signo_(signo) {
    REQUIRE(::sigaction(signo_, nullptr, &saved_) == 0);
  }
  ~SigactionGuard() { ::sigaction(signo_, &saved_, nullptr); }
  SigactionGuard(const SigactionGuard &) = delete;
  SigactionGuard &operator=(const SigactionGuard &) = delete;
  SigactionGuard(SigactionGuard &&) = delete;
  SigactionGuard &operator=(SigactionGuard &&) = delete;

  [[nodiscard]] const struct sigaction &saved() const { return saved_; }

private:
  int signo_;
  struct sigaction saved_{};
};

// The two guards that make a telemetry sink fail DETERMINISTICALLY, lifted from the
// technique cpp/tests/test_telemetry_errors.cpp already runs on: RLIMIT_FSIZE caps this
// process's file size, and a write past the cap raises SIGXFSZ — whose default disposition
// TERMINATES the process — so it is ignored and the crossing write surfaces as EFBIG, which
// the stream records as sticky badbit. Lowering then re-raising a SOFT limit is always
// permitted, so the restore cannot fail for rights reasons.
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

// mm::Server driven directly, for the ONE read the shared ServerRunner does not expose:
// telemetry_ok(), the (s)9 shutdown health verdict and the figure mm_engine prints as
// telemetry_ok= on its shutdown line. A constant true and a constant false both survive a
// suite that never calls it, so the two cases below own their Server rather than leave that
// hole open. Lifecycle discipline is ServerRunner's, deliberately — ephemeral port bound in
// the constructor, run() on a background thread, stop-and-join in the destructor so a failed
// REQUIRE cannot leak a thread into the next case — and the accessor is the only difference.
// FOLD THIS INTO server_test_support.hpp when the harness reopens (it is frozen this wave).
class HealthRunner {
public:
  explicit HealthRunner(mm::Config cfg) : telemetry_path_("health") {
    cfg.port = 0;
    cfg.telemetry_out = telemetry_path_.path().string();
    server_.emplace(std::move(cfg), mm::Instrument{.symbol = "MOCKUSDT"});
    thread_ = std::thread{[this] { server_->run(); }};
  }
  ~HealthRunner() { stop(); }
  HealthRunner(const HealthRunner &) = delete;
  HealthRunner &operator=(const HealthRunner &) = delete;
  HealthRunner(HealthRunner &&) = delete;
  HealthRunner &operator=(HealthRunner &&) = delete;

  [[nodiscard]] std::uint16_t port() const { return server_->port(); }
  [[nodiscard]] std::filesystem::path telemetry_path() const { return telemetry_path_.path(); }

  void stop() {
    server_->stop();
    if (thread_.joinable())
      thread_.join();
  }
  // Valid only after run() returned, which is what the join above is: same contract as
  // ServerRunner::counters_after_stop, and the reason the name says so.
  [[nodiscard]] bool telemetry_ok_after_stop() {
    stop();
    return server_->telemetry_ok();
  }

private:
  telemetry_test::TempPath telemetry_path_;
  std::optional<mm::Server> server_;
  std::thread thread_;
};

// ---- artifact readers ----

// The --bench-out dump read back through the Task 11 harness contract (bench_recorder.hpp):
// SIX little-endian uint64 header words, then the four counted int64 streams, and nothing
// else. test_bench_recorder.cpp reads the same shape against a hand-driven recorder; this
// reader is what makes the SERVER's half — sample capture through artifact — visible at all.
struct BenchDump {
  std::uint64_t svc{}, m0_m0p{}, m0p_m3{}, m0_m3{}, saturated{}, peak_sessions{};
  std::uintmax_t file_size{};
};

inline BenchDump read_bench_dump(const std::filesystem::path &path) {
  REQUIRE(std::filesystem::exists(path));
  std::ifstream in{path, std::ios::binary};
  REQUIRE(in.is_open());
  std::uint64_t header[6]{};
  in.read(reinterpret_cast<char *>(header), sizeof header);
  REQUIRE(in.good());
  return BenchDump{header[0],
                   header[1],
                   header[2],
                   header[3],
                   header[4],
                   header[5],
                   std::filesystem::file_size(path)};
}

// wait_for_line answers "at least one". The peak-session case needs "every one": with a
// session still in the map a gauge and a watermark read alike, which is the whole
// distinction under test.
inline bool wait_for_count(const std::filesystem::path &path,
                           const std::function<bool(const json &)> &pred, std::size_t want,
                           std::chrono::milliseconds deadline = kOpDeadline) {
  const auto giveup = std::chrono::steady_clock::now() + deadline;
  for (;;) {
    const auto lines = live_telemetry_lines(path);
    if (static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), pred)) >= want)
      return true;
    if (std::chrono::steady_clock::now() >= giveup)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
}

inline std::filesystem::path upper_spelling(const std::filesystem::path &path) {
  std::string name = path.filename().string();
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return path.parent_path() / name;
}

// Whether THIS volume folds case, decided by identity rather than by reputation: create the
// file, then ask whether the upper-case spelling names the same inode. The case-variant
// collision below is a real defect only where the answer is yes, and a case that asserted
// the refusal unconditionally would red on a case-sensitive volume for no defect at all.
inline bool filesystem_folds_case(const std::filesystem::path &probe) {
  {
    std::ofstream create{probe};
    REQUIRE(create.good());
  }
  std::error_code ec;
  return std::filesystem::equivalent(probe, upper_spelling(probe), ec) && !ec;
}

} // namespace server_flow
