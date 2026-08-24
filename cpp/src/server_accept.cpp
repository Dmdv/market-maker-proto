// The acceptor half of the Task 7 server (split out of cpp/src/server.cpp when the fixes
// of gate R1 — pending-upgrade tracking, admission control, accept re-arm — carried that
// file past the repo's 500-line cap): the listen loop, the HTTP/WebSocket upgrade and the
// mm.v1 subprotocol negotiation. Everything here runs on the ONE owner thread (record D6)
// as part of ServerImpl; the process lifecycle, feed driver and telemetry pump stay in
// cpp/src/server.cpp, one connection's state machine in cpp/src/session.cpp, the shared
// declarations in cpp/src/server_impl.hpp.
#include "server_impl.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mm::server_detail {

namespace {

// The upgrade request's subprotocol offer is a comma-separated token list (possibly
// spread over several headers); the engine accepts iff one token is exactly mm.v1.
bool offers_mm_v1(const http::request<http::string_body> &req) {
  for (auto it = req.find(http::field::sec_websocket_protocol); it != req.end(); ++it) {
    if (it->name() != http::field::sec_websocket_protocol)
      continue;
    std::string_view list = it->value();
    while (!list.empty()) {
      const std::size_t comma = list.find(',');
      std::string_view token = list.substr(0, comma);
      while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
        token.remove_prefix(1);
      while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
        token.remove_suffix(1);
      if (token == kSubprotocol)
        return true;
      if (comma == std::string_view::npos)
        break;
      list.remove_prefix(comma + 1);
    }
  }
  return false;
}

} // namespace

// One in-flight upgrade: owns the socket and the HTTP state until the WebSocket accept
// succeeds (the stream is then moved into the Session) or the refusal response is
// written. Namespace-scope (not the anonymous namespace) because server_impl.hpp forward
// declares it: ServerImpl keeps weak references to the set still in flight.
struct PendingUpgrade : std::enable_shared_from_this<PendingUpgrade> {
  explicit PendingUpgrade(tcp::socket socket) : stream(std::move(socket)) {
    // The pre-upgrade read is the ONE read this engine performs for a peer that has
    // proven nothing, so it does not get Beast's defaults (1 MB body, 8 KiB header).
    // Measured on the shipped build: an upgrade declaring Content-Length: 900000 was read
    // in full and still answered 101, so max_sessions pending upgrades reached ~1 MB of
    // heap EACH before the mm.v1 gate ever ran. An RFC 6455 upgrade carries no body at
    // all, so a zero body limit makes a body-bearing upgrade an outright parse failure —
    // the RFC-correct verdict — and the header limit is 4096 rather than the ~200 B the
    // mm.v1 request actually needs, because a legitimate client may carry a few extra
    // headers and that figure is the case, not a ceiling.
    parser.body_limit(0);
    parser.header_limit(4096);
  }
  beast::tcp_stream stream;
  beast::flat_buffer buffer;
  http::request_parser<http::string_body> parser;
  http::response<http::string_body> response;
};

namespace {

// args[1] of the upgrade_refused event. The HTTP status in lane 0 is what the PEER was
// told, not a discriminant: two gates may answer with one status, and a reader holding
// only the status could not tell which fired. Additive — a new refusal takes the next
// value; the lane contract is in mm/telemetry_ring.hpp.
enum class RefusalReason : std::uint64_t {
  Subprotocol = 0, // 400: the offer list carried no mm.v1
  Origin = 1,      // 403: the request carried an Origin header
  Admission = 2,   // 503: at the concurrent-connection bound
};

// Ends an upgrade with an HTTP status the peer can read (400 wrong subprotocol, 403 a
// browser origin, 503 at the admission bound) instead of a bare close: a refusal a client
// can parse is a refusal it can act on. The half-close after the write is what keeps a
// reset from destroying the response still in the socket's send buffer.
void refuse_upgrade(ServerImpl &srv, const std::shared_ptr<PendingUpgrade> &pending,
                    http::status status, RefusalReason reason, std::string_view body) {
  // Every refusal narrates from HERE — the one place all of them pass through. A refused
  // upgrade is the likeliest wire-level misconfiguration there is, and while it emitted
  // nothing, an engine whose only client offers the wrong subprotocol wrote a telemetry
  // file identical to an engine nobody was dialling: sessions 0, totals flat, no events.
  // The admission tier ALSO emits admission_refused, deliberately: the two lines answer
  // different questions (which refusal went out vs. what load the bound compared) and
  // neither is derivable from the other.
  srv.emit_event(TelemetryEvent::UpgradeRefused, static_cast<std::uint64_t>(status),
                 static_cast<std::uint64_t>(reason));
  pending->response = {status, pending->parser.get().version()};
  pending->response.set(http::field::content_type, "text/plain");
  pending->response.body() = std::string{body};
  pending->response.prepare_payload();
  http::async_write(pending->stream, pending->response, [pending](beast::error_code, std::size_t) {
    beast::error_code ignored;
    // Best-effort half-close on a refused upgrade (the verdict IS `ignored`).
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    (void)pending->stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
  });
}

} // namespace

void ServerImpl::abort_pending_upgrades() {
  // Upgrades already past the TCP accept are NOT in sessions_ and so are invisible to the
  // graceful shutdown path. Two consequences if they are left running: each one pins
  // io_.run() for up to its pre-upgrade read timeout, and one that completes its handshake
  // afterwards would insert a session into a server that has already finalized. Closing
  // the socket completes their reads with operation_aborted, where the stopping_ gates in
  // on_upgrade_request retire them.
  for (const auto &weak : upgrades_)
    if (const auto pending = weak.lock())
      pending->stream.close();
  // The WebSocket-handshake half of the same claim. Past `stream ws{std::move(...)}` the
  // live socket belongs to the SESSION: Beast's move leaves the source as-if newly
  // created (not open), so the loop above closes a detached stream and aborts nothing,
  // while async_accept stays in flight on the real one. Such a session is not in
  // sessions_ either, so begin_stop's graceful sweep misses it too — leaving io_.run()
  // pinned for the WebSocket HANDSHAKE timeout (30 s), three times the pre-upgrade read
  // bound this function was written for.
  for (const auto &weak : handshaking_)
    if (const auto session = weak.lock())
      beast::get_lowest_layer(session->stream()).close();
}

// The accept path's socket options, lifted out of the accept handler and given NAMESPACE
// scope (not the anonymous namespace) so a white-box test can declare it, open a loopback
// socket, call it and read the options back. That seam is the only pin available: deleting
// the TCP_NODELAY line leaves all 161 cases green, and no peer can observe the far end's
// socket options, so a black-box case cannot see the loss.
void apply_socket_options(tcp::socket &socket, const Config &cfg) {
  beast::error_code ec;
  // TCP_NODELAY at accept (plan): a quoting session's frames are latency-bound and tiny —
  // Nagle coalescing is the wrong trade everywhere on this wire. Best-effort in that a
  // socket which refuses the option still carries a valid session, but NOT silent: an
  // engine sold on latency must not run Nagled indistinguishably from one that is not.
  // stderr rather than a telemetry event because the accept path is not the measured path
  // and the event surface is reserved for wire verdicts.
  // NOLINTNEXTLINE(bugprone-unused-return-value)
  (void)socket.set_option(tcp::no_delay{true}, ec);
  if (ec)
    std::fprintf(stderr,
                 "mm: TCP_NODELAY failed on an accepted socket (%s): this session runs "
                 "NAGLED and its latency figures are not comparable\n",
                 ec.message().c_str());
  if (cfg.so_sndbuf > 0) {
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    (void)socket.set_option(boost::asio::socket_base::send_buffer_size{cfg.so_sndbuf}, ec);
    if (ec)
      std::fprintf(stderr,
                   "mm: SO_SNDBUF=%d failed on an accepted socket (%s): the kernel default "
                   "stands, so the configured send-queue bound is not in force\n",
                   cfg.so_sndbuf, ec.message().c_str());
  }
}

void ServerImpl::accept_next() {
  acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
    if (ec) {
      // Only shutdown retires the listener. Every OTHER accept failure is transient by
      // nature — descriptor exhaustion, a connection the peer reset between SYN and
      // accept — and returning here left the process alive with a listener that would
      // never accept again, which is the failure mode nobody notices until the client
      // does. Re-arm behind kAcceptRetryDelay so a persistent failure cannot spin.
      if (stopping_ || !acceptor_.is_open())
        return;
      accept_backoff_.expires_after(kAcceptRetryDelay);
      accept_backoff_.async_wait([this](beast::error_code timer_ec) {
        if (!timer_ec && !stopping_ && acceptor_.is_open())
          accept_next();
      });
      return;
    }
    // The accept-time tier of Config::max_sessions. The readable 503 below can only
    // answer a peer that has PRESENTED an upgrade request; one that connects and sends
    // nothing never reaches it, yet still costs a descriptor and a PendingUpgrade for the
    // whole pre-upgrade read bound — unbounded in the exact regime the accept backoff
    // exists for. Here the close IS the only refusal expressible on the wire;
    // admission_refused is the LOCAL record an operator can read. Its lanes are the tier's
    // OWN arithmetic: lane 0 is what THIS bound compared (in-flight upgrades, with this
    // connection not yet among them), lane 1 = 0 names the accept tier. The upgrade tier
    // below compares a different quantity by design, so one unified figure would
    // desynchronise the event from the check that fired it.
    const std::size_t in_flight = live_upgrades();
    if (!stopping_ && in_flight >= cfg_.max_sessions) {
      emit_event(TelemetryEvent::AdmissionRefused, in_flight, /*tier=*/0);
      beast::error_code close_ec;
      socket.close(close_ec);
    } else if (!stopping_) {
      apply_socket_options(socket, cfg_);
      on_upgrade_request(std::move(socket));
    }
    accept_next();
  });
}

std::size_t ServerImpl::live_upgrades() {
  std::erase_if(upgrades_,
                [](const std::weak_ptr<PendingUpgrade> &weak) { return weak.expired(); });
  return upgrades_.size();
}

void ServerImpl::on_upgrade_request(tcp::socket socket) {
  auto pending = std::make_shared<PendingUpgrade>(std::move(socket));
  (void)live_upgrades(); // compacts before the push_back below extends
  upgrades_.push_back(pending);
  // A bound on the not-yet-upgraded read: a client that connects and never sends the
  // request must not hold a half-open slot forever. Replaced by the WebSocket-layer
  // timeout (idle + keep-alive pings) the moment the accept path takes over. Config-sized
  // rather than a constant so a case can drive it inside a test's patience: this deadline
  // is the ONLY thing that recycles an accept-tier slot held by a mute peer — the sibling
  // control's own case passes only BECAUSE it expires those sockets — yet deleting it left
  // the whole suite green, so a future edit could drop it silently and leak descriptors.
  pending->stream.expires_after(std::chrono::milliseconds{cfg_.upgrade_timeout_ms});
  http::async_read(
      pending->stream, pending->buffer, pending->parser,
      [this, pending](beast::error_code ec, std::size_t) {
        if (ec || stopping_)
          return; // the socket dies with `pending`
        auto &req = pending->parser.get();
        if (!websocket::is_upgrade(req) || !offers_mm_v1(req)) {
          // The negotiation is strict (plan): no mm.v1 offered -> HTTP 400, never a
          // subprotocol-less accept the client would then misparse frames against.
          refuse_upgrade(*this, pending, http::status::bad_request, RefusalReason::Subprotocol,
                         "subprotocol mm.v1 is required\n");
          return;
        }
        // Cross-site WebSocket hijacking. WebSocket has no preflight and no same-origin
        // policy, and a browser may offer any subprotocol it likes, so without this gate
        // any page a user on a reachable network visits could open a socket to an engine
        // that authenticates nothing, place orders and read the book — measured: an
        // upgrade carrying `Origin: http://evil.example` was answered 101 and its
        // new_order acked. A native mm.v1 client never sends Origin, so the check costs it
        // nothing; a browser cannot suppress the header, so it is a reliable discriminant.
        // If a browser client is ever wanted, the extensible form is a Config allow-list
        // that is EMPTY by default — identical behaviour to this, no rework.
        if (req.count(http::field::origin) != 0) {
          refuse_upgrade(*this, pending, http::status::forbidden, RefusalReason::Origin,
                         "browser origins are not accepted\n");
          return;
        }
        // Admission control (Config::max_sessions): the ONE bound on concurrent
        // connections. `live_upgrades()` counts this request too, so the comparison is
        // against what accepting it would make the total. 503 rather than a silent close:
        // a refusal a peer can read is a refusal it can back off from. The same local
        // record as the accept-time tier, so both refusals are countable — here lane 0
        // carries sessions + in-flight upgrades (what THIS bound compared) and lane 1 = 1
        // names the upgrade tier.
        const std::size_t load = sessions_.size() + live_upgrades();
        if (load > cfg_.max_sessions) {
          emit_event(TelemetryEvent::AdmissionRefused, load, /*tier=*/1);
          refuse_upgrade(*this, pending, http::status::service_unavailable,
                         RefusalReason::Admission, "engine at its concurrent-session limit\n");
          return;
        }
        websocket::stream<beast::tcp_stream> ws{std::move(pending->stream)};
        // The WebSocket layer owns liveness from here: Beast's own idle timeout +
        // keep-alive pings; the tcp_stream timer must be off or the two fight.
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(websocket::stream_base::timeout{
            .handshake_timeout =
                websocket::stream_base::timeout::suggested(beast::role_type::server)
                    .handshake_timeout,
            .idle_timeout = std::chrono::milliseconds{cfg_.idle_timeout_ms},
            .keep_alive_pings = true});
        // permessage-deflate stays OFF on both roles: compression trades CPU (and, with
        // context takeover, per-connection window state) for bytes this wire does not
        // need — frames are ~100-200 B — and a compressed arm would make the
        // naive-vs-tuned codec A/B measure zlib instead of serialization.
        websocket::permessage_deflate pmd;
        pmd.server_enable = false;
        pmd.client_enable = false;
        ws.set_option(pmd);
        ws.read_message_max(kTransportMsgBytes); // two-tier bound: see server_impl.hpp
        ws.set_option(websocket::stream_base::decorator([](http::response<http::string_body> &res) {
          res.set(http::field::sec_websocket_protocol, kSubprotocol);
          // Beast stamps its own `Server: Boost.Beast/NNN` banner ONLY when the decorator
          // left the field unset, so setting it here is what keeps a library version off
          // the wire to a peer that has authenticated nothing — and makes the 101 match
          // the hand-built refusals above, which carry no banner at all.
          res.set(http::field::server, "mm_engine");
        }));
        auto session = std::make_shared<Session>(*this, std::move(ws));
        // Shutdown's only handle on this socket from here until the handshake resolves:
        // `pending->stream` was moved from above, so abort_pending_upgrades cannot reach
        // it, and the session is not in sessions_ yet either.
        handshaking_.push_back(session);
        session->stream().async_accept(req, [this, session, pending](beast::error_code ec2) {
          // Drop this session's claim (and any expired sibling) before anything else:
          // a completed handshake keeps its shared_ptr in sessions_ forever, so leaving
          // the weak entry behind would grow handshaking_ by one per connection.
          std::erase_if(handshaking_, [&session](const std::weak_ptr<Session> &weak) {
            const auto held = weak.lock();
            return !held || held == session;
          });
          // stopping_ is checked HERE and not only before the accept: begin_stop can land
          // between the two, and a handshake completing after it would insert a session
          // into a server that has closed its acceptor, may already have finalized, and
          // has nothing left that would ever close this one. Dropping `session` here
          // closes the stream with it — no epoch burned, same as a failed handshake.
          if (ec2 || stopping_)
            return;
          const std::uint64_t epoch = ++next_epoch_;
          sessions_.emplace(epoch, session);
          // Folded into the run's peak-session watermark HERE, at the one place a session
          // enters the map: the bench dump carries that peak because the recorded m0->m0'
          // is only the true venue-production window at ONE session (above it a reader
          // cannot tell a fan-out minimum from a correct dump).
          note_session_added();
          emit_event(TelemetryEvent::SessionOpen, epoch);
          session->start(epoch);
        });
      });
}

} // namespace mm::server_detail
