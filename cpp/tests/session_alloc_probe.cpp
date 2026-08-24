// Task 7 — the session write path's and the TOB fan-out's ALLOCATION discipline, measured
// against a real engine over real loopback sockets. Its own executable, and the reason is
// the probe, not the scenario:
//
//   * counting a LIVE engine means counting one thread of a multi-threaded process — the
//     owner thread runs the io_context while the TelemetryWriter runs its own thread — so
//     the counter must be per-thread. cpp/tests/alloc_probe.cpp's state is a plain global
//     (correctly: every case that arms it is single-threaded), and arming it around
//     Server::run() would be a data race on g_new_calls between the owner thread and any
//     other thread that allocates. The counters here are `thread_local`, which makes the
//     "owner thread's count" in the claim literal instead of aspirational, and leaves no
//     race for TSan to find;
//   * replacing the global operator new is a WHOLE-BINARY property, so a second
//     replacement cannot join mm_tests (CMakeLists.txt records that constraint). A second
//     BINARY is the only place a second replacement can live;
//   * and this one intercepts the ALIGNED forms too. alloc_probe.cpp deliberately does not
//     (alloc_probe_support.hpp pins the type-alignment assumption that exemption rests on),
//     but mm::Counters is alignas(64), so ServerImpl inherits alignof 64 and a
//     heap-allocated Server routes its own allocation around an ordinary-only probe. Here
//     it is counted like everything else — and lands in the constant term below.
//
// WHAT IS PINNED — the marginal, not the total. Startup (feed load, bind, codec, ring,
// writer, accepts, handshakes) is a constant per configuration, so the run is measured at
// TWO tick counts and the constant subtracts out:
//     allocations(kManyTicks) - allocations(kFewTicks) == (kManyTicks - kFewTicks) * cost
// The expected per-tick cost is the fan-out's own arithmetic (server.cpp publish_book,
// (r)13): ONE Tob is built per tick, carrying a copy of the instrument symbol; it is then
// COPIED for every session but the last and MOVED into the last. Everything downstream —
// the outbox push, the pop, the encode buffer, the async_write — must contribute ZERO per
// message, which is what "the tick path is allocation-free by construction" (outbox.hpp)
// and "reused across writes: no steady-state allocation" (server_impl.hpp) claim. So:
//     1 (the tick's Tob) + (sessions - 1) copies + 0 for the moved last  ==  sessions
// The `sessions` arms are 1 and 3 because the fan-out and the per-message paths are
// otherwise indistinguishable: a mutant that copies into the LAST session too costs
// sessions + 1, which differs from sessions at every session count, while a mutant on the
// per-MESSAGE path scales with the message count instead.
//
// AND WHY A 64-BYTE SYMBOL. At the shipped 8-byte "MOCKUSDT" every one of those copies
// fits in both stdlibs' small-string buffer and the whole per-tick cost is ZERO — the
// third arm below asserts exactly that, because it is the shipped engine's real property,
// and it is simultaneously the reason the first two arms cannot use it: measured here, the
// last-session copy mutant is invisible at 8 bytes (0 -> 0) and unmissable at 64 (3 -> 4).
//
// Measured, this file's own numbers (host libc++/arm64, all four presets, 10 reps each):
// the constant differs by one between Debug and the sanitizer trees and the marginal does
// not — 1.000/tick at one session, 3.000/tick at three, 0.000/tick at "MOCKUSDT".
#include "mm/server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <string>

// LEAKSANITIZER, and why this probe needs to know about it. LSan ships INSIDE AddressSanitizer
// and is ON BY DEFAULT on Linux, while Apple's ASan does not support it at all — measured:
// `ASAN_OPTIONS=detect_leaks=1` on the macOS host answers "detect_leaks is not supported on this
// platform" and aborts. So the deliberately-retained Client below is invisible on the dev host
// and a hard failure in the authoritative ubuntu:26.04 image: `ctest --preset asan` was 179/179
// on macOS and 178/179 there, this test the only red.
//
// The guards differ by compiler: GCC defines __SANITIZE_ADDRESS__, Clang answers
// __has_feature(address_sanitizer). Both toolchains build this file.
#if defined(__SANITIZE_ADDRESS__)
#define MM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MM_ASAN 1
#endif
#endif

#ifdef MM_ASAN
#include <sanitizer/lsan_interface.h>
#endif
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Per-thread by design (see the file comment): the only thread that ever arms these is the
// one that runs the io_context, so no other thread can read or write them at all.
thread_local bool t_counting = false;
thread_local std::size_t t_calls = 0;
// Second use of the same counter, and the reason this binary can do what the Catch2 suite
// cannot: 0 leaves the probe observational, N makes the Nth COUNTED allocation throw. The
// per-thread storage is what makes it safe here — arming it around Server::run() on the
// owner thread cannot fire on the TelemetryWriter's thread, where an escaping bad_alloc
// would be an uncaught exception on a std::thread and terminate the process. This is the
// closing seam PENDING item (t)13/F4 names for the finalize-on-throw fail-safe: "an armed
// allocation-failure probe (or equivalent injected throw) around io_.run()".
thread_local std::size_t t_throw_at = 0;

// Set at the instant the injected allocation FAILS — not when run() returns, which is after
// the catch block has already published the fault and called finalize(). The stalled-sink
// releaser times its hold from this signal so the stall provably spans the publication
// attempt rather than racing whatever else the run was doing. Cross-thread by construction
// (owner sets, releaser reads), so it is an atomic rather than another thread_local.
std::atomic<bool> g_fault_injected{false};

} // namespace

// Both PAIRS replaced — ordinary and over-aligned. operator new[] and the nothrow forms are
// specified to route through these, and both replacements go through the malloc family, so
// ASan still sees and instruments every allocation.
void *operator new(std::size_t size) {
  if (t_counting) {
    ++t_calls;
    if (t_throw_at != 0 && t_calls == t_throw_at) {
      g_fault_injected.store(true, std::memory_order_release);
      throw std::bad_alloc{};
    }
  }
  void *p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr)
    throw std::bad_alloc{};
  return p;
}
void *operator new(std::size_t size, std::align_val_t align) {
  if (t_counting)
    ++t_calls;
  // aligned_alloc requires a size that is a multiple of the alignment; operator new does
  // not, so the request is rounded up here rather than passed through.
  const std::size_t alignment = static_cast<std::size_t>(align);
  const std::size_t rounded = ((size == 0 ? 1 : size) + alignment - 1) / alignment * alignment;
  void *p = std::aligned_alloc(alignment, rounded);
  if (p == nullptr)
    throw std::bad_alloc{};
  return p;
}
// std::free IS the correct pairing: both replacements above allocate through the malloc
// family. g++ warns anyway because this TU holds the replacement AND an allocation site it
// can inline (make_unique in measure() below), so its middle-end pairs the BUILTIN new it
// recognises with this free and cannot see the replacement in between. alloc_probe.cpp does
// the same thing without the warning only because its allocations all live in other TUs.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

// The feed's first event, so every session is established before md_seq 1 exists. Measured
// establishment for three sequential loopback handshakes is 0.9-2.3 ms across the four
// presets on this host, so this is ~100x the observed cost — and it is not trusted blind:
// each run CHECKS its own establishment against this bound and fails naming the figure,
// because a run whose first ticks fanned out to fewer sessions than it counted would
// otherwise mismeasure silently.
constexpr int kEstablishHaltMs = 250;
// The two tick counts whose difference isolates the per-tick cost. Both are past the
// encode buffer's growth and the asio handler pool's warm-up, and the DIFFERENCE is what
// the assertion divides by, so the absolute values only have to be comfortable.
constexpr int kFewTicks = 500;
constexpr int kManyTicks = 2500;
// Past both stdlibs' small-string capacity (libc++ 22, libstdc++ 15): the length is the
// whole reason a copy is countable — see the file comment's last paragraph.
constexpr std::size_t kPastSsoSymbol = 64;

int g_failures = 0;

void check(bool ok, const std::string &what) {
  if (ok)
    return;
  ++g_failures;
  std::fprintf(stderr, "FAILED: %s\n", what.c_str());
}

// Self-deleting scratch path (telemetry_test_support.hpp's TempPath shape; local because
// this binary links no test support headers — it deliberately carries no Catch2 either,
// whose per-assertion machinery would allocate on a thread this probe is counting).
class TempFile {
public:
  explicit TempFile(const char *suffix) {
    static const auto token = std::random_device{}();
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("mm_alloc_" + std::to_string(token) + "_" + std::to_string(counter++) + suffix);
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;
  TempFile(TempFile &&) = delete;
  TempFile &operator=(TempFile &&) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

// One client, kept as a whole because the io_context has to outlive the stream that binds
// it and both have to outlive the reader thread.
struct Client {
  boost::asio::io_context io;
  websocket::stream<beast::tcp_stream> ws{io};
};

struct Measurement {
  std::size_t allocations{0};
  std::int64_t establish_us{0};
};

// One complete engine run: `ticks` book publishes fanned out to `sessions` live sessions,
// with the owner thread's own allocations counted end to end.
Measurement measure(int ticks, int sessions, const std::string &symbol) {
  TempFile feed{".feed"};
  TempFile telemetry{".jsonl"};
  {
    std::ofstream out{feed.path()};
    out << R"({"halt_ms":)" << kEstablishHaltMs << "}\n";
    for (int i = 0; i < ticks; ++i)
      out << R"({"set":[100,10,110,20]})" << '\n';
    out << R"({"end":true})" << '\n'; // exhaustion without --loop is a graceful stop
  }

  mm::Config cfg;
  cfg.port = 0; // ephemeral: parallel ctest processes must never contend on a port
  cfg.feed_path = feed.path().string();
  cfg.telemetry_out = telemetry.path().string();
  // Zero interval: the ticks fire as fast as the loop drains, so the run costs the halt
  // above plus the ticks themselves rather than ticks x an interval. Conflation is then
  // heavy and DELIBERATE — the fan-out's per-tick cost is paid whether or not the message
  // survives to a write, so the figure this measures is invariant to how many do.
  cfg.feed_interval_ms = 0;
  cfg.max_sessions = 16;
  mm::Server server{cfg, mm::Instrument{.symbol = symbol}};
  const std::uint16_t port = server.port();

  Measurement result;
  // The owner thread arms its OWN counters: there is no other correct place. Arming from
  // the calling thread would leave a thread_local this thread never reads, and arming a
  // shared global would be the race this file exists to avoid.
  std::thread owner{[&] {
    t_calls = 0;
    t_counting = true;
    server.run();
    t_counting = false;
    result.allocations = t_calls;
  }};

  const auto started = std::chrono::steady_clock::now();
  std::vector<std::unique_ptr<Client>> clients;
  for (int i = 0; i < sessions; ++i) {
    // SEQUENTIAL, one upgrade in flight at a time. Concurrent handshakes were measured to
    // move the run's constant by +-2 allocations — the acceptor's in-flight-upgrade vector
    // grows differently depending on how many overlap — and a jittering constant does not
    // subtract out.
    auto client = std::make_unique<Client>();
    beast::get_lowest_layer(client->ws)
        .socket()
        .connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), port});
    client->ws.set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
      req.set(http::field::sec_websocket_protocol, "mm.v1");
    }));
    client->ws.handshake("127.0.0.1", "/");
    clients.push_back(std::move(client));
  }
  result.establish_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();

  // Readers, one thread each: a session whose peer stops reading stalls its write and the
  // engine conflates behind it, which is legal but would leave the sessions asymmetric.
  std::vector<std::thread> readers;
  readers.reserve(clients.size());
  for (const auto &client : clients)
    readers.emplace_back([raw = client.get()] {
      beast::flat_buffer buffer;
      for (;;) {
        beast::error_code ec;
        raw->ws.read(buffer, ec);
        buffer.clear();
        if (ec) // the engine's 1001 at feed exhaustion, or the transport behind it
          return;
      }
    });

  owner.join();
  for (auto &reader : readers)
    reader.join();
  return result;
}

// One arm: two runs at the same session count, differing only in tick count.
void pin_per_tick_cost(int sessions, const std::string &symbol, std::size_t expected_per_tick) {
  const Measurement few = measure(kFewTicks, sessions, symbol);
  const Measurement many = measure(kManyTicks, sessions, symbol);
  const std::string arm =
      "sessions=" + std::to_string(sessions) + " symbol=" + std::to_string(symbol.size()) + "B";

  // The precondition, checked rather than assumed (see kEstablishHaltMs).
  for (const Measurement *run : {&few, &many})
    check(run->establish_us < kEstablishHaltMs * 1000,
          arm + ": sessions took " + std::to_string(run->establish_us) +
              " us to establish, past the " + std::to_string(kEstablishHaltMs) +
              " ms feed halt — the early ticks fanned out to fewer sessions than this arm "
              "counts, so raise kEstablishHaltMs rather than trusting the figures below");

  const std::size_t expected = expected_per_tick * (kManyTicks - kFewTicks);
  std::printf("[alloc] %s: %d ticks -> %zu allocs, %d ticks -> %zu allocs, %.3f/tick "
              "(expected %zu), established in %lld/%lld us\n",
              arm.c_str(), kFewTicks, few.allocations, kManyTicks, many.allocations,
              static_cast<double>(many.allocations - few.allocations) /
                  static_cast<double>(kManyTicks - kFewTicks),
              expected_per_tick, static_cast<long long>(few.establish_us),
              static_cast<long long>(many.establish_us));
  // Exact, both-sided, because both sides are real verdicts: above it, a copy or a lost
  // buffer reuse has entered a path documented as allocation-free; below it, the fan-out
  // is no longer handing every session its own Tob.
  check(many.allocations >= few.allocations && many.allocations - few.allocations == expected,
        arm + ": per-tick owner-thread allocations were " +
            std::to_string(many.allocations - few.allocations) + " over " +
            std::to_string(kManyTicks - kFewTicks) + " ticks, expected " +
            std::to_string(expected));
}

} // namespace

// ---- (t)13/F4 + hard gate HG-1: finalize-on-throw AND a full-ring fault marker ----
// Gate G-B findings GB-1/GB-2, plus the HG-1 survivorship: a mutant that ships the fault
// through emit_event (lossy try_push, result ignored) instead of publish_record is
// indistinguishable from a clean crash when the ring has a free slot — both get the line
// through. Outrunning the writer's 10 ms full-ring drain is not enough either (measured:
// verbose + interval 0 still left a free slot at the injection). The ring must be FULL at
// the fault and STAY full for publish_record's 250 ms wait, which means stalling the
// writer, not merely flooding it.
//
// Mechanism: telemetry_out is a FIFO whose reader deliberately stops reading. Once the
// pipe buffer fills (~64 KiB on this host), the writer's flush blocks inside drain(), so
// it cannot pop; the owner thread then fills all 4096 ring slots with tob_out. At that
// point emit_event drops the marker silently and is done. The shipped path instead HOLDS the
// record when its first bounded attempt (kFinalSnapshotWait, 250 ms) expires, and finalize()
// re-offers it on a much larger budget (kFaultPublishWait, 5 s) once the reader resumes and
// the writer drains — so the marker reaches the ARTIFACT, which is the channel a post-mortem
// reader and the Task 11 harness actually consume. The stderr line is the last resort for a
// sink that never drains at all, and this case asserts it did NOT fire: reaching it here
// would mean the re-offer never ran.
//
// This is the seam (t)13 names, and it can only live here: forcing the throw needs an armed
// allocation failure on the OWNER thread specifically, and only this binary's thread_local
// probe can arm one thread without arming the TelemetryWriter's, where an escaping bad_alloc
// would terminate the process instead of testing it.
void pin_engine_fault_artifact() {
  // Enough ticks that, with the writer stalled after handshake, the 4096-slot ring is full
  // long before the injection fires. Pipe buffer (~64 KiB) holds only a few hundred lines,
  // so the rest of the flood lands in the ring; 8k ticks is ~2x capacity with margin for
  // session_open / snapshots.
  constexpr int kFaultTicks = 8000;
  // Stall longer than publish_record's 250 ms deadline (and finalize's two snapshot waits)
  // so a lossy try_push cannot be rescued by a late drain, then resume so stop_and_join
  // is not wedged on a blocked flush.
  // Must outlast fault + 250 ms publish + finalize snapshot waits under asan/tsan too.
  // Long enough that the FIRST publication attempt (kFinalSnapshotWait, 250 ms) provably
  // expires with the ring full — that is what makes the lossy try_push drop the marker and
  // fail this case — and comfortably shorter than the held record's last-chance budget
  // (kFaultPublishWait, 5 s), so a recovering sink still gets the marker into the artifact.
  // The gap between those two numbers is the whole behaviour under test.
  constexpr auto kWriterStall = std::chrono::milliseconds{1200};

  // Named pipe + a reader thread that can freeze the writer's drain by not reading.
  // capture_ accumulates whatever does land (open marker, and engine_fault if publish
  // succeeded after the stall lifted mid-wait).
  struct StallingFifo {
    std::filesystem::path path;
    std::atomic<bool> stall{false};
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::string capture;
    std::thread thr;

    explicit StallingFifo(std::filesystem::path p) : path(std::move(p)) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
      if (::mkfifo(path.c_str(), 0600) != 0)
        throw std::system_error(errno, std::generic_category(), "mkfifo telemetry stall");
      thr = std::thread([this] {
        // Open blocks until the writer's ofstream opens the write end — that is the
        // rendezvous that lets run() start without a race on a reader-less FIFO.
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
          return;
        // Non-blocking so a stalled phase can still observe stop_ without a wedged read.
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        char buf[8192];
        for (;;) {
          if (stop.load(std::memory_order_acquire) && !stall.load(std::memory_order_acquire)) {
            // Final drain after the producer side has closed.
            for (;;) {
              const ssize_t n = ::read(fd, buf, sizeof(buf));
              if (n > 0) {
                const std::scoped_lock lock{mu};
                capture.append(buf, static_cast<std::size_t>(n));
              } else {
                break;
              }
            }
            break;
          }
          if (stall.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
          }
          const ssize_t n = ::read(fd, buf, sizeof(buf));
          if (n > 0) {
            const std::scoped_lock lock{mu};
            capture.append(buf, static_cast<std::size_t>(n));
          } else if (n == 0) {
            break; // writer closed
          } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
          } else {
            break;
          }
        }
        ::close(fd);
      });
    }
    // Join the reader only after the writer has closed (one_run finished): a snapshot
    // taken before this join can miss the final pipe contents still in the kernel buffer.
    void finish() {
      stall.store(false, std::memory_order_release);
      stop.store(true, std::memory_order_release);
      if (thr.joinable())
        thr.join();
    }
    ~StallingFifo() {
      finish();
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
    StallingFifo(const StallingFifo &) = delete;
    StallingFifo &operator=(const StallingFifo &) = delete;

    [[nodiscard]] std::string snapshot() {
      const std::scoped_lock lock{mu};
      return capture;
    }
  };

  // freopen stderr into a temp file for the duration of the fault run — the "could not be
  // recorded" line is the kill when publish_record times out still full. Restores on
  // scope exit so later arms of this binary are not speaking into a closed stream.
  class StderrCapture {
  public:
    explicit StderrCapture(const std::filesystem::path &target) : saved_fd_(::dup(fileno(stderr))) {
      if (saved_fd_ < 0)
        throw std::system_error(errno, std::generic_category(), "dup stderr");
      if (std::freopen(target.string().c_str(), "w", stderr) == nullptr) {
        ::dup2(saved_fd_, STDERR_FILENO);
        ::close(saved_fd_);
        throw std::runtime_error("freopen stderr for engine-fault capture");
      }
      std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    ~StderrCapture() {
      std::fflush(stderr);
      ::dup2(saved_fd_, fileno(stderr));
      ::close(saved_fd_);
    }
    StderrCapture(const StderrCapture &) = delete;
    StderrCapture &operator=(const StderrCapture &) = delete;

  private:
    int saved_fd_;
  };

  // SELF-CALIBRATING. Run 1 counts owner-thread allocations on a normal file (writer free
  // to drain — we only need the total). Run 2 injects at the midpoint under the FIFO stall.
  const auto one_run = [&](std::size_t throw_at, bool *threw, bool *ok_after,
                           const std::filesystem::path &telemetry_path,
                           StallingFifo *fifo) -> std::size_t {
    TempFile feed{".feed"};
    {
      std::ofstream out{feed.path()};
      // Short establish halt so the client is up before the flood that fills the ring.
      out << R"({"halt_ms":)" << kEstablishHaltMs << "}\n";
      for (int i = 0; i < kFaultTicks; ++i)
        out << R"({"set":[101,10,111,20]})" << '\n';
      out << R"({"end":true})" << '\n';
    }
    mm::Config cfg;
    cfg.port = 0;
    cfg.feed_path = feed.path().string();
    cfg.telemetry_out = telemetry_path.string();
    cfg.feed_interval_ms = 0;
    // Per-tick tob_out is the flood that fills the ring once the writer is stalled. Without
    // verbose the owner only emits sparse events — far fewer than 4096 before feed end.
    cfg.telemetry_verbose = true;
    mm::Server server{cfg, mm::Instrument{.symbol = std::string(kPastSsoSymbol, 'S')}};
    const std::uint16_t port = server.port();

    std::size_t counted = 0;
    std::thread owner{[&] {
      t_calls = 0;
      t_throw_at = throw_at;
      g_fault_injected.store(false, std::memory_order_release);
      t_counting = true;
      try {
        server.run();
      } catch (const std::exception &) {
        if (threw != nullptr)
          *threw = true; // main.cpp's exit-2 path: a crashed engine must not report success
      }
      t_counting = false;
      t_throw_at = 0;
      counted = t_calls;
      if (ok_after != nullptr)
        *ok_after = server.telemetry_ok();
    }};

    // ONE session, load-bearing: no client ⇒ no Tob fan-out ⇒ nowhere inside a handler to
    // place the injection (and nothing to fill the ring with under verbose). Heap + detached
    // reader: the exception path never tears the peer down, so a joinable reader would hang
    // or race a same-socket cancel under TSan. Leaking until process exit is fine — this
    // binary ends right after the case.
    auto *client = new Client;
    // RETAINED ON PURPOSE, and declared so to LSan rather than switching LSan off. The comment
    // above explains why the peer cannot be torn down here; what LSan sees is an unreachable
    // heap object at exit, which is exactly what it is built to report. Ignoring THIS object
    // keeps the leak check armed for every other allocation in the binary — whereas
    // ASAN_OPTIONS=detect_leaks=0 would blind it wholesale, and excluding the test from the asan
    // preset would stop the allocation probe running under the gate that most needs a live
    // engine. The counting itself is unaffected either way: the replaced operator new still
    // routes through malloc, which ASan still instruments.
#ifdef MM_ASAN
    __lsan_ignore_object(client);
#endif
    beast::get_lowest_layer(client->ws)
        .socket()
        .connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), port});
    client->ws.set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
      req.set(http::field::sec_websocket_protocol, "mm.v1");
    }));
    client->ws.handshake("127.0.0.1", "/");
    std::thread{[client] {
      beast::flat_buffer buffer;
      for (;;) {
        beast::error_code ec;
        client->ws.read(buffer, ec);
        buffer.clear();
        if (ec)
          return;
      }
    }}.detach();

    std::thread releaser;
    std::atomic<bool> signal_seen{false};
    if (fifo != nullptr) {
      // Writer is live (run() opened the FIFO). Stop reading so its next flush blocks and
      // the ring can fill; resume after the publish deadline so finalize's join returns.
      fifo->stall.store(true, std::memory_order_release);
      // The release is timed FROM THE INJECTED FAULT, not from here. Starting the clock at
      // stall time makes the window a race against however long the run takes to reach the
      // throw: a slow build could recover the sink BEFORE the fault, so the first publish
      // attempt would find room, and the case would pass without ever exercising the held
      // record or finalize's re-offer — green for the wrong reason, which is the shape this
      // whole gate exists to catch. The owner thread signals the fault; the releaser waits
      // for that signal, then holds the stall long enough to outlast the 250 ms first
      // attempt and well inside the 5 s last-chance budget.
      releaser = std::thread{[fifo, &signal_seen, stall_for = kWriterStall] {
        // BOUNDED wait for the fault. An unbounded one would spin forever on any run where
        // the injection never fires — and the join below would then hang the whole probe
        // into the blanket ctest timeout, which names no cause. Releasing anyway on expiry
        // lets the run finish and the assertions report the REAL problem (no rethrow, no
        // artifact marker) instead of a timeout that could mean anything.
        const auto giveup = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!g_fault_injected.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < giveup)
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        // Recorded, so a timeout is diagnosed as ITSELF rather than inferred from whatever
        // fails downstream: "the injection never fired" and "the fault fired but was not
        // narrated" are different defects and must not share one failure message.
        signal_seen = g_fault_injected.load(std::memory_order_acquire);
        std::this_thread::sleep_for(stall_for);
        fifo->stall.store(false, std::memory_order_release);
      }};
    }

    owner.join();
    if (releaser.joinable())
      releaser.join();
    if (fifo != nullptr && throw_at != 0)
      check(signal_seen.load(std::memory_order_acquire),
            "engine fault: the injected allocation failure never fired within the releaser's "
            "wait, so the stall never spanned a publication attempt — this run proved nothing "
            "about the held record (check the calibrated throw target)");
    return counted;
  };

  TempFile calibrate{".jsonl"};
  const std::size_t total = one_run(0, nullptr, nullptr, calibrate.path(), nullptr);
  check(total > 8, "engine fault: the calibration run allocated only " + std::to_string(total) +
                       " times on the owner thread — too few to place an injection inside a "
                       "handler; the probe or the run shape changed");
  if (total <= 8)
    return;

  TempFile fifo_path{".fifo"};
  TempFile err_path{".err"};
  StallingFifo fifo{fifo_path.path()};
  bool threw = false;
  bool ok_after = true;
  // Place the injection AFTER the ring is full under stall. Pipe buffer holds a few
  // hundred lines; 4096 more fill the ring — ~4.5k ticks. total ≈ startup + kFaultTicks
  // with one alloc/tick at the 64-byte symbol; fire ~5k ticks into the flood so asan/tsan
  // still hit the full-ring window before the stall lifts.
  const std::size_t startup = total > static_cast<std::size_t>(kFaultTicks)
                                  ? total - static_cast<std::size_t>(kFaultTicks)
                                  : 0;
  const std::size_t throw_at = startup + 5000;
  check(throw_at < total, "engine fault: throw_at " + std::to_string(throw_at) +
                              " is not inside the calibrated "
                              "run (total " +
                              std::to_string(total) + ")");
  if (throw_at >= total)
    return;
  {
    // Capture only the fault run's stderr — the "could not be recorded" kill signal.
    StderrCapture err_cap{err_path.path()};
    (void)one_run(throw_at, &threw, &ok_after, fifo.path, &fifo);
    std::fflush(stderr);
  } // restores stderr before any check diagnostics

  check(threw, "engine fault: run() did not rethrow — the injected bad_alloc never escaped a "
               "handler, so this case proved nothing (calibrated total was " +
                   std::to_string(total) + ")");
  if (!threw)
    return;

  // (1) telemetry_ok() must be FALSE, and must STAY false through finalize(), which runs on
  // this same path and recomputes the flag. That is the latch; a plain assignment erases it.
  check(!ok_after, "engine fault: telemetry_ok() reported TRUE for a run that unwound out of "
                   "an Asio handler — the health latch contradicts the exit code");

  // (2) HG-1: the abort must be narrated IN THE ARTIFACT. The reader resumes, so the sink
  // recovers and the writer drains — which is the state the fix is built for: the first
  // publish attempt finds the ring full, the record is HELD, and finalize() re-offers it
  // after the drain. Accepting a stderr line here instead would answer a finding about the
  // durable record with a message on the channel main.cpp already writes, and would leave
  // the artifact still shaped like a graceful stop (HG2-1). stderr belongs to the sink that
  // never recovers, which is not this scenario.
  fifo.finish();
  const std::string artifact = fifo.snapshot();
  check(artifact.find("engine_fault") != std::string::npos,
        "engine fault: under a stalled-then-recovered telemetry writer (ring full at the "
        "fault) a CRASHED run's artifact carries NO engine_fault record — the marker was "
        "lossy-dropped, indistinguishably from ordinary ring saturation (HG-1). "
        "calibrated_total=" +
            std::to_string(total));

  // ...and the stderr fallback must NOT have fired: a recovering sink is precisely the case
  // the held record exists to serve, so reaching the last resort here would mean the
  // re-offer never happened.
  {
    std::ifstream err{err_path.path()};
    for (std::string line; std::getline(err, line);)
      check(line.find("mm: engine fault could not be recorded") == std::string::npos,
            "engine fault: the stderr last-resort fired even though the sink recovered — "
            "finalize()'s re-offer of the held record did not run");
  }
}

int main() {
  try {
    // One session: the fan-out has only the moved last hand-off, so the tick's own Tob is
    // the whole cost. A copy into the last session doubles this.
    pin_per_tick_cost(1, std::string(kPastSsoSymbol, 'S'), 1);
    // Three: two copies plus the tick's own Tob, and the last is still moved.
    pin_per_tick_cost(3, std::string(kPastSsoSymbol, 'S'), 3);
    // The SHIPPED instrument, and the shipped claim: at "MOCKUSDT" the whole per-tick path
    // — fan-out copies included — allocates nothing at all.
    pin_per_tick_cost(3, "MOCKUSDT", 0);
    pin_engine_fault_artifact();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAILED: the probe itself threw: %s\n", e.what());
    return 1;
  }
  if (g_failures != 0)
    std::fprintf(stderr, "%d allocation assertion(s) failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
