// Engine entry point: CLI -> Config -> Server. One startup line, one shutdown line, nothing per
// message on stdout — periodic state goes to the telemetry file. Any startup failure: stderr, 2.
#include "mm/server.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr const char *kUsage =
    "usage: mm_engine --feed PATH [--port N] [--bind ADDR] [--codec naive|tuned]\n"
    "                 [--interval-ms N] [--loop] [--bench-out PATH] [--hwm N]\n"
    "                 [--telemetry-out PATH] [--telemetry-verbose] [--max-sessions N]\n"
    "                 [--max-session-entries N] [--upgrade-timeout-ms N]\n"
    "       mm_engine --version\n";

// `--version` prints the toolchain too: the benchmark manifest reads it from THIS binary rather
// than inferring the compiler from its own environment. CMake stamps the fields.
[[noreturn]] void print_version() {
  std::printf("mm_engine %s\n", MM_ENGINE_VERSION);
  std::printf("compiler: %s\n", MM_ENGINE_COMPILER);
  std::printf("standard: %s\n", MM_ENGINE_STD);
  std::printf("build-type: %s\n", MM_ENGINE_BUILD_TYPE);
  std::printf("cxx-flags: %s\n", MM_ENGINE_CXX_FLAGS);
  // Same rationale as usage_error's: parsing runs before any thread exists.
  std::exit(0); // NOLINT(concurrency-mt-unsafe)
}

[[noreturn]] void usage_error(std::string_view detail) {
  std::fprintf(stderr, "mm_engine: %.*s\n%s", static_cast<int>(detail.size()), detail.data(),
               kUsage);
  // Parsing runs before any thread exists (main's first statement), so exit's unsafety cannot bite.
  std::exit(2); // NOLINT(concurrency-mt-unsafe)
}

std::int64_t parse_int(std::string_view flag, std::string_view value, std::int64_t min,
                       std::int64_t max) {
  std::int64_t out = 0;
  try {
    std::size_t used = 0;
    out = std::stoll(std::string{value}, &used);
    if (used != value.size())
      throw std::invalid_argument{"trailing characters"};
  } catch (const std::exception &) {
    usage_error(std::string{flag} + ": not an integer: " + std::string{value});
  }
  if (out < min || out > max)
    usage_error(std::string{flag} + ": out of range: " + std::string{value});
  return out;
}

mm::Config parse_args(int argc, char **argv) {
  mm::Config cfg;
  cfg.handle_signals = true; // the binary owns SIGINT/SIGTERM; library embedders do not
  bool verbose_requested = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&]() -> std::string_view {
      if (i + 1 >= argc)
        usage_error(std::string{arg} + ": missing value");
      return argv[++i];
    };
    // FIRST, before the value-taking lambda: `--version` takes no value and needs no --feed.
    if (arg == "--version") {
      print_version();
    } else if (arg == "--port") {
      cfg.port = static_cast<std::uint16_t>(parse_int(arg, value(), 0, 65535));
    } else if (arg == "--bind") {
      // Exposing an engine that authenticates nothing beyond loopback is an operator ACT, so it
      // has a flag and no default. The Server constructor validates it.
      cfg.bind_address = std::string{value()};
    } else if (arg == "--feed") {
      cfg.feed_path = std::string{value()};
    } else if (arg == "--codec") {
      const std::string_view kind = value();
      if (kind == "naive")
        cfg.codec = mm::CodecKind::Naive;
      else if (kind == "tuned")
        cfg.codec = mm::CodecKind::Tuned;
      else
        usage_error("--codec: expected naive|tuned");
    } else if (arg == "--interval-ms") {
      cfg.feed_interval_ms = parse_int(arg, value(), 0, 86'400'000);
    } else if (arg == "--loop") {
      cfg.loop_feed = true;
    } else if (arg == "--bench-out") {
      cfg.bench_out = std::string{value()};
    } else if (arg == "--hwm") {
      cfg.report_hwm = static_cast<std::size_t>(parse_int(arg, value(), 1, 1 << 24));
    } else if (arg == "--telemetry-out") {
      cfg.telemetry_out = std::string{value()};
    } else if (arg == "--telemetry-verbose") {
      verbose_requested = true;
    } else if (arg == "--max-sessions") {
      // The two admission bounds MULTIPLY into worst-case resident memory (mm/server.hpp), so
      // both are operator-reachable: more of one is paid for with less of the other.
      cfg.max_sessions = static_cast<std::size_t>(parse_int(arg, value(), 1, 4096));
    } else if (arg == "--max-session-entries") {
      cfg.max_session_entries = static_cast<std::size_t>(parse_int(arg, value(), 1, 1 << 24));
    } else if (arg == "--upgrade-timeout-ms") {
      cfg.upgrade_timeout_ms = parse_int(arg, value(), 1, 86'400'000);
    } else {
      usage_error("unknown argument: " + std::string{arg});
    }
  }
  if (cfg.feed_path.empty())
    usage_error("--feed is required");
  // Per-message telemetry is forced OFF under measurement; saying so beats silently dropping it.
  cfg.telemetry_verbose = verbose_requested && cfg.bench_out.empty();
  if (verbose_requested && !cfg.bench_out.empty())
    std::fprintf(stderr, "mm_engine: --telemetry-verbose is forced off under --bench-out "
                         "(per-message events would distort the measured run)\n");
  return cfg;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const mm::Config cfg = parse_args(argc, argv);
    mm::Server server{cfg, mm::Instrument{.symbol = "MOCKUSDT"}};
    // MM_ENGINE_VERSION is injected by CMake from project(VERSION) — the single source of truth.
    std::printf("mm_engine %s listening port=%u codec=%s instrument=MOCKUSDT feed=%s\n",
                MM_ENGINE_VERSION, server.port(),
                cfg.codec == mm::CodecKind::Tuned ? "tuned" : "naive", cfg.feed_path.c_str());
    std::fflush(stdout);
    server.run();
    const mm::Counters counters = server.counters();
    std::printf("mm_engine shutdown orders=%" PRIu64 " fills=%" PRIu64 " conflated=%" PRIu64
                " telemetry_ok=%d\n",
                counters.orders, counters.fills, counters.conflated, server.telemetry_ok() ? 1 : 0);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mm_engine: %s\n", e.what());
    return 2;
  }
}
