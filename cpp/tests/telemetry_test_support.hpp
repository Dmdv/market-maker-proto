// Shared support for the telemetry test TUs (test_telemetry_writer.cpp outgrew
// the 500-line file cap when the failure-path cases landed and again when
// the a review wire batch landed; split along the same behavioral seams as the
// codec/engine suites):
//   test_telemetry.cpp        — the SPSC ring: FIFO, full-drop, pow2 rounding,
//                               allocation-free steady state, the 1M threaded stress
//   test_telemetry_writer.cpp — snapshot mapping, writer thread, JSON-lines contract
//   test_telemetry_errors.cpp — the failure paths: construction rejections (unopenable
//                               path, non-positive poll — the previous run's file must
//                               survive both), forced mid-run IO loss, and the clean-run
//                               control on the failure latch
//   test_telemetry_wire.cpp   — the wire-shape edges: the open/close run markers, the
//                               self-describing fallbacks for undefined codes on both
//                               kinds, and the busy-ring drain cadence
// All four share the `telemetry:` case-name prefix and `[telemetry]` tag, so
// `ctest -R telemetry` and `./mm_tests "[telemetry]"` select the whole suite.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace telemetry_test {

// One self-deleting output file per case under the system temp dir (test_feed.cpp's
// proven TempFeed shape). The tag only NAMES the case; what keeps files apart is the
// per-process random token + counter: ctest runs each case as its own process, so
// tag-only paths kept one suite run's parallel cases apart but let two CONCURRENT suite
// runs (two build trees, same case, same temp dir) truncate each other's live files
// mid-assertion — a measured false RED on the sustained-producer case. Uniqueness also
// retires the stale-lines hazard a crashed earlier run used to pose, so there is no
// up-front remove. Removal is the DESTRUCTOR's job, not a trailing statement's: a failed
// REQUIRE skips everything after it, so an end-of-case remove leaks exactly the runs
// that fail — and unique names make that leak unbounded.
class TempPath {
public:
  explicit TempPath(std::string_view tag) : path_(unique_path(tag)) {}
  ~TempPath() {
    std::error_code ec;
    std::filesystem::remove(path_, ec); // best effort: a test failure must not mask itself
  }
  TempPath(const TempPath &) = delete;
  TempPath &operator=(const TempPath &) = delete;
  TempPath(TempPath &&) = delete;
  TempPath &operator=(TempPath &&) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  static std::filesystem::path unique_path(std::string_view tag) {
    static const auto token = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    return std::filesystem::temp_directory_path() /
           ("mm_telemetry_" + std::to_string(token) + "_" + std::to_string(counter++) + "_" +
            std::string{tag} + ".jsonl");
  }

  std::filesystem::path path_;
};

inline std::vector<std::string> lines_of(const std::filesystem::path &path) {
  std::ifstream in{path};
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);)
    lines.push_back(line);
  return lines;
}

// The writer's drain cadence is real time, so live-drain assertions WAIT for the file to
// reach `want` lines (bounded) instead of sleeping and hoping. The default budget is the
// suite's generous convention; the drain-cadence case passes a TIGHT budget instead,
// because there the deadline IS the oracle (a poll-cadence mutant cannot make it).
inline std::vector<std::string>
wait_for_lines(const std::filesystem::path &path, std::size_t want,
               std::chrono::steady_clock::duration budget = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  for (;;) {
    auto lines = lines_of(path);
    if (lines.size() >= want || std::chrono::steady_clock::now() > deadline)
      return lines;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

} // namespace telemetry_test
