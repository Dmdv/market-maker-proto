// Shared support for the Task 7 server integration TUs (the plan's single
// test_server_integration.cpp split under the repo's 500-line file cap, along behavioral
// seams — same remedy as the codec/engine/telemetry suites):
//   test_server_ws.cpp      — the WebSocket discipline surface: handshake/subprotocol,
//                             frame policy, size bounds, liveness timeout
//   test_server_session.cpp — the command path: acks, sequencing, epochs, cross-session
//                             isolation, entry-cap policy close
//   test_server_flow.cpp    — flow control and lifecycle: conflation, report-HWM close,
//                             stop(), the telemetry narration contract
//   test_bench_recorder.cpp — the BenchRecorder unit pins (m0' attribution guard)
// All share the `server:` case-name prefix and `[server]` tag (recorder cases use
// `bench_recorder:`/`[bench]`), so `ctest -R server` selects the integration suite.
//
// The clients here are REAL Boost.Beast WebSocket clients over real loopback sockets on an
// ephemeral port (plan Task 7 Step 1): every assertion crosses the wire the product client
// will use. Each op runs async against a per-client io_context with a deadline
// (io.run_for), so a hung server fails the CASE with a readable message instead of eating
// the blanket ctest TIMEOUT.
#pragma once

#include "mm/server.hpp"
#include "telemetry_test_support.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace server_test {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using namespace std::chrono_literals;

// Deadline for a single client op. Generous on purpose: sanitizer trees run the same
// cases, and a deadline is a ceiling on FAILURE latency, never a sleep on the pass path.
inline constexpr auto kOpDeadline = std::chrono::milliseconds{5000};

// Self-deleting scenario file (telemetry_test::TempPath's shape, with feed content and a
// .feed suffix so a directory listing reads honestly during debugging).
class FeedFile {
public:
  explicit FeedFile(std::initializer_list<std::string_view> lines) : path_(unique_path()) {
    std::ofstream out{path_};
    for (const auto line : lines)
      out << line << '\n';
    if (!out)
      throw std::runtime_error("FeedFile: cannot write " + path_.string());
  }
  ~FeedFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec); // best effort: a test failure must not mask itself
  }
  FeedFile(const FeedFile &) = delete;
  FeedFile &operator=(const FeedFile &) = delete;
  FeedFile(FeedFile &&) = delete;
  FeedFile &operator=(FeedFile &&) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  static std::filesystem::path unique_path() {
    static const auto token = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    return std::filesystem::temp_directory_path() /
           ("mm_server_" + std::to_string(token) + "_" + std::to_string(counter++) + ".feed");
  }
  std::filesystem::path path_;
};

// The engine under test on a background thread. Construction binds the ephemeral port
// (Server's constructor contract), so port() is race-free before run() starts; the
// destructor stops and joins, so no case leaks a listener into its successor.
class ServerRunner {
public:
  explicit ServerRunner(mm::Config cfg, mm::Instrument inst = mm::Instrument{.symbol = "MOCKUSDT"})
      : telemetry_path_("srv") {
    cfg.port = 0; // ephemeral: parallel ctest processes must never contend on a port
    // Always redirected: the Config default would drop a telemetry file into the test
    // runner's working directory, and every case reads it back via telemetry_path().
    cfg.telemetry_out = telemetry_path_.path().string();
    server_.emplace(cfg, std::move(inst));
    thread_ = std::thread{[this] { server_->run(); }};
  }
  ~ServerRunner() { stop(); }
  ServerRunner(const ServerRunner &) = delete;
  ServerRunner &operator=(const ServerRunner &) = delete;
  ServerRunner(ServerRunner &&) = delete;
  ServerRunner &operator=(ServerRunner &&) = delete;

  [[nodiscard]] std::uint16_t port() const { return server_->port(); }
  [[nodiscard]] std::filesystem::path telemetry_path() const { return telemetry_path_.path(); }

  // Begins the graceful shutdown WITHOUT joining (Server::stop only posts). The one way
  // to assert the graceful 1001 on the wire: the caller's thread is also the client's io
  // thread, so a joining stop() would hold the client dead through the whole closing
  // handshake — the server's close-grace timer would sever the transport at 2 s and the
  // buffered close frame would then race the sever's RST (measured 29/40 lost under
  // tsan). Initiate, drain the handshake as a live peer, then stop()/join.
  void initiate_stop() { server_->stop(); }

  // Idempotent; joining gives the caller happens-before on counters()/telemetry_ok().
  void stop() {
    server_->stop();
    if (thread_.joinable())
      thread_.join();
  }

  [[nodiscard]] mm::Counters counters_after_stop() {
    stop();
    return server_->counters();
  }

private:
  telemetry_test::TempPath telemetry_path_;
  std::optional<mm::Server> server_;
  std::thread thread_;
};

// How a read ended when it did not end with a message. THREE outcomes, three types,
// because two of them are opposite verdicts about the peer and one `bool` used to answer
// for both: a transport error proves the far end is GONE, a deadline proves only that it
// was SILENT, and an oracle that reads the second as the first passes against a connection
// that is merely idle. They are deliberately siblings — none is a base of another — so no
// catch can re-collapse the distinction by widening.

// The peer closed instead of delivering a message; carries the close code so callers can
// assert the discipline table's row directly.
struct PeerClosed {
  std::uint16_t code;
};

// The transport failed with no close frame: EOF, reset, or a stream cut mid-frame. The
// ONLY outcome that is evidence the far end is gone.
struct TransportDied : std::runtime_error {
  explicit TransportDied(const std::string &what) : std::runtime_error(what) {}
};

// The deadline expired with the transport still OPEN. Evidence of nothing about the peer,
// so it is never an assertion's answer — it FAILS the case, naming the op that waited.
struct ReadDeadline : std::runtime_error {
  explicit ReadDeadline(const std::string &what) : std::runtime_error(what) {}
};

class WsClient {
public:
  struct Options {
    std::string subprotocol{"mm.v1"};
    int rcvbuf{0};        // >0: set SO_RCVBUF before connect (backpressure cases)
    bool handshake{true}; // false: stop after TCP connect (raw-HTTP cases drive it themselves)
  };

  // Two overloads instead of `Options opt = {}`: a default argument built from a nested
  // class's member initializers is ill-formed inside the enclosing class definition.
  explicit WsClient(std::uint16_t port) : WsClient(port, Options{}) {}
  WsClient(std::uint16_t port, Options opt) : ws_(io_) {
    tcp::endpoint ep{boost::asio::ip::make_address_v4("127.0.0.1"), port};
    auto &sock = beast::get_lowest_layer(ws_).socket();
    sock.open(tcp::v4());
    if (opt.rcvbuf > 0)
      sock.set_option(boost::asio::socket_base::receive_buffer_size{opt.rcvbuf});
    sock.connect(ep);
    if (!opt.handshake)
      return;
    ws_.set_option(
        websocket::stream_base::decorator([proto = opt.subprotocol](websocket::request_type &req) {
          if (!proto.empty())
            req.set(http::field::sec_websocket_protocol, proto);
        }));
    // One frame per message unless a case opts into fragmentation: the transport-bound
    // case needs the oversize to be announced by a single frame header.
    ws_.auto_fragment(false);
    ws_.handshake(response_, "127.0.0.1", "/");
  }

  [[nodiscard]] const websocket::response_type &upgrade_response() const { return response_; }

  void send_text(std::string_view frame) {
    ws_.text(true);
    ws_.write(boost::asio::buffer(frame));
  }
  void send_binary(std::string_view payload) {
    ws_.binary(true);
    ws_.write(boost::asio::buffer(payload));
  }

  // Next text message, parsed. Every non-message ending is one of the three types above,
  // so a case that catches one of them cannot be answered by another.
  json read_json(std::chrono::milliseconds deadline = kOpDeadline) {
    buffer_.clear();
    std::optional<beast::error_code> result;
    ws_.async_read(buffer_, [&](beast::error_code ec, std::size_t) { result = ec; });
    io_.restart();
    io_.run_for(deadline);
    if (!result) {
      // The socket is closed only AFTER the outcome is decided: this arm is the one that
      // found the transport alive, and its own cleanup must not be mistakable for the
      // death it just ruled out.
      beast::get_lowest_layer(ws_).socket().close();
      throw ReadDeadline("WsClient::read_json: no message within " +
                         std::to_string(deadline.count()) +
                         " ms; the transport was still open, so this is silence, not death");
    }
    if (*result == websocket::error::closed)
      throw PeerClosed{static_cast<std::uint16_t>(ws_.reason().code)};
    if (*result)
      throw TransportDied("WsClient::read_json: " + result->message());
    return json::parse(beast::buffers_to_string(buffer_.data()));
  }

  // Reads until the peer's close frame; returns its code. Messages passed over on the way
  // are appended to sink (the drain-before-close assertions read them).
  std::uint16_t read_until_close(std::vector<json> *sink = nullptr,
                                 std::chrono::milliseconds deadline = kOpDeadline) {
    const auto giveup = std::chrono::steady_clock::now() + deadline;
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          giveup - std::chrono::steady_clock::now());
      if (left <= 0ms)
        throw ReadDeadline("WsClient::read_until_close: no close frame within " +
                           std::to_string(deadline.count()) +
                           " ms; the transport was still open, so this is silence, not death");
      try {
        json msg = read_json(left);
        if (sink != nullptr)
          sink->push_back(std::move(msg));
      } catch (const PeerClosed &closed) {
        return closed.code;
      }
    }
  }

  // Client-initiated clean close: sends the frame, then drains to the peer's reply.
  void close(websocket::close_code code) { ws_.close(websocket::close_reason{code}); }

  // The same close carrying NO code: the close frame's body is EMPTY, which is legal
  // (RFC 6455 5.5.1 — a browser's argument-less WebSocket.close() sends one). Its own
  // entry point rather than close(close_code::none), which reads like "do not close",
  // because this is the ONLY shape that reaches the session's close-code normalisation:
  // Beast puts the empty body on the wire, reports close_code::none (0) to the
  // application, and answers 1000 — so the engine must record a code the wire never
  // carried.
  void close_without_code() { ws_.close(websocket::close_reason{websocket::close_code::none}); }

  // The liveness case's probe: true once the transport dies WITHOUT a close frame (an
  // idle-timeout reap severs the socket; a clean close would surface as PeerClosed).
  // Catches TransportDied ALONE. A ReadDeadline propagates and fails the case, which is
  // the entire point of the three types: this used to catch std::runtime_error, so it
  // answered "the peer died" for a peer that was live and silent — an idle-reap assertion
  // that could not tell a reaped connection from an un-reaped one.
  bool transport_died(std::chrono::milliseconds deadline = kOpDeadline) {
    try {
      (void)read_until_close(nullptr, deadline);
      return false; // clean close = a live, polite peer
    } catch (const TransportDied &) {
      return true;
    }
  }

  [[nodiscard]] websocket::stream<beast::tcp_stream> &stream() { return ws_; }

private:
  boost::asio::io_context io_;
  websocket::stream<beast::tcp_stream> ws_;
  websocket::response_type response_;
  beast::flat_buffer buffer_;
};

// Raw HTTP round trip on the listening port — the reject-before-upgrade cases (missing or
// wrong subprotocol -> HTTP 400, an Origin header -> HTTP 403, the admission bound ->
// HTTP 503) never reach the WebSocket layer, so they use plain HTTP; and the
// extension-offer case needs a request Beast's own client would not compose (it offers
// compression only when the client stream enables it, which is the thing under test).
// `origin` is the fourth argument rather than a separate helper so the Origin gate's two
// arms are the SAME request differing in one header: 403 with it, 101 without. Only the
// pair distinguishes the gate from an acceptor that refuses everything.
inline http::response<http::string_body> http_upgrade_response(std::uint16_t port,
                                                               const char *subprotocol,
                                                               const char *extensions = nullptr,
                                                               const char *origin = nullptr) {
  boost::asio::io_context io;
  tcp::socket sock{io};
  sock.connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), port});
  http::request<http::empty_body> req{http::verb::get, "/", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::upgrade, "websocket");
  req.set(http::field::connection, "Upgrade");
  req.set(http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
  req.set(http::field::sec_websocket_version, "13");
  if (subprotocol != nullptr)
    req.set(http::field::sec_websocket_protocol, subprotocol);
  if (extensions != nullptr)
    req.set(http::field::sec_websocket_extensions, extensions);
  if (origin != nullptr)
    req.set(http::field::origin, origin);
  http::write(sock, req);
  beast::flat_buffer buf;
  http::response<http::string_body> res;
  http::read(sock, buf, res);
  return res;
}

inline unsigned http_upgrade_status(std::uint16_t port, const char *subprotocol) {
  return http_upgrade_response(port, subprotocol).result_int();
}

// The suite's read-until idiom, and the ONE place its bound lives. Bounded because
// skipping forever turns a wrong answer into a deadline expiry, which names nothing;
// `awaited` is what the FAIL says the case was waiting for.
inline json next_matching(WsClient &client, const std::function<bool(const json &)> &wanted,
                          std::string_view awaited, int max_skipped = 64) {
  for (int seen = 0; seen <= max_skipped; ++seen) {
    json msg = client.read_json();
    if (wanted(msg))
      return msg;
  }
  FAIL("no " << awaited << " arrived within " << max_skipped << " other messages");
  return {}; // unreachable: FAIL throws
}

// The next NON-heartbeat message. TOB ticks are this wire's background noise; everything
// else is a VERDICT a case means to inspect, so reading with this makes a wrong verdict
// fail on its own content.
inline json next_verdict(WsClient &client, int max_ticks = 64) {
  return next_matching(
      client, [](const json &msg) { return msg["t"] != "top_of_book"; }, "verdict", max_ticks);
}

// Expects the peer's close frame, tolerating ONLY top-of-book ticks before it — the same
// background-noise doctrine next_verdict applies. Broadcasts are enqueued asynchronously on
// the owner thread, so on a loaded host (CI shared runners, measured 2026-07-31: two
// docs-only diffs failed exactly here) a tick can legally arrive ahead of the close. Any
// OTHER frame still fails loudly on its own content, so a wrong verdict stays a wrong
// verdict and never becomes a timeout.
inline std::uint16_t expect_close(WsClient &client, int max_ticks = 64) {
  try {
    for (int i = 0; i < max_ticks; ++i) {
      const json unexpected = client.read_json();
      if (unexpected["t"] == "top_of_book")
        continue;
      FAIL("expected the peer's close frame, got: " << unexpected.dump());
    }
    FAIL("expected the peer's close frame; still top_of_book after " << max_ticks << " ticks");
  } catch (const PeerClosed &closed) {
    return closed.code;
  }
  return 0; // unreachable: FAIL throws
}

// ---- protocol frame builders (all required inbound fields — the codec's closed set) ----

inline std::string new_order(std::uint64_t seq, std::uint64_t epoch, std::string_view cl_id,
                             std::int64_t px, std::int64_t qty, std::uint64_t md_seq = 1,
                             std::string_view side = "B") {
  json j{{"t", "new_order"}, {"v", 1},         {"seq", seq},           {"epoch", epoch},
         {"md_seq", md_seq}, {"cl_id", cl_id}, {"symbol", "MOCKUSDT"}, {"side", side},
         {"px", px},         {"qty", qty},     {"post_only", true}};
  return j.dump();
}

// The same NewOrder carrying a key the v1 schema does not know — the wire's additive
// forward-compatibility policy (codec.hpp: "Unknown keys are ignored"). `pad` is that
// key's string value, so a caller can also size the whole frame to an exact byte count
// with a command that is otherwise completely valid.
inline std::string new_order_with_unknown_key(std::uint64_t seq, std::uint64_t epoch,
                                              std::string_view cl_id, std::int64_t px,
                                              std::int64_t qty, std::string_view pad = "") {
  json j = json::parse(new_order(seq, epoch, cl_id, px, qty));
  j["zz_from_a_later_version"] = pad; // sorts last: skipped wherever it lands in the object
  return j.dump();
}

inline std::string cancel_order(std::uint64_t seq, std::uint64_t epoch, std::string_view cl_id) {
  json j{{"t", "cancel_order"}, {"v", 1}, {"seq", seq}, {"epoch", epoch}, {"cl_id", cl_id}};
  return j.dump();
}

// ---- telemetry narration probes ----

// TWO readers, split by what each is allowed to tolerate. Neither may DROP a line: one
// reader that quietly skipped whatever would not parse made the very contract these cases
// inspect — every telemetry line is one complete JSON record — unfalsifiable, because if
// the contract held the skip was dead code and if it did not the suite hid the breach.
//   telemetry_lines      — the FINAL artifact, read after stop(). ServerImpl::finalize
//                          joins the writer and closes the file before run() returns, so
//                          every byte the run will ever write is already on disk: anything
//                          that does not parse, a truncated tail included, is a real defect.
//   live_telemetry_lines — read while the writer runs. Its ofstream is flushed once per
//                          drained batch, so the LAST line can be a fragment mid-flush;
//                          that one unterminated tail is the only thing skipped, and every
//                          complete line is held to the same bar as above.

// One record, or a failure carrying the line that broke the contract.
inline json parse_telemetry_line(const std::string &line) {
  json parsed = json::parse(line, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded())
    FAIL("telemetry: line is not a JSON record: " << line);
  return parsed; // on the FAIL path: unreachable, FAIL throws
}

inline std::vector<json> telemetry_lines(const std::filesystem::path &path) {
  std::vector<json> out;
  for (const auto &line : telemetry_test::lines_of(path))
    out.push_back(parse_telemetry_line(line));
  return out;
}

// Newline-TERMINATED records only. telemetry_test::lines_of cannot serve the live path:
// std::getline hands back an unterminated tail as though it were a record, which is the
// one distinction this reader needs. getline sets eofbit only when it stopped at
// end-of-file INSTEAD of at the delimiter, so eof() after a successful extraction is
// exactly "this line had no '\n'".
inline std::vector<json> live_telemetry_lines(const std::filesystem::path &path) {
  std::ifstream in{path};
  std::vector<json> out;
  for (std::string line; std::getline(in, line);) {
    if (in.eof())
      break; // the writer's half-flushed tail; it is complete on the next poll
    out.push_back(parse_telemetry_line(line));
  }
  return out;
}

// Waits (bounded) for a telemetry line matching `pred` — the file is written by the
// engine's writer thread on its own cadence, so live assertions poll rather than sleep.
inline std::optional<json> wait_for_line(const std::filesystem::path &path,
                                         const std::function<bool(const json &)> &pred,
                                         std::chrono::milliseconds deadline = kOpDeadline) {
  const auto giveup = std::chrono::steady_clock::now() + deadline;
  for (;;) {
    for (const auto &line : live_telemetry_lines(path))
      if (pred(line))
        return line;
    if (std::chrono::steady_clock::now() >= giveup)
      return std::nullopt;
    std::this_thread::sleep_for(20ms);
  }
}

inline std::function<bool(const json &)> event_named(std::string_view name) {
  return [name = std::string{name}](const json &j) {
    return j.contains("event") && j["event"] == name;
  };
}

} // namespace server_test
