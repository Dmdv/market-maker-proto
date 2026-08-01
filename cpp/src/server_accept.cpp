// The acceptor half of the server: the listen loop, the HTTP/WebSocket upgrade and the mm.v1
// subprotocol negotiation. Owner thread only; shared declarations in cpp/src/server_impl.hpp.
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

// One in-flight upgrade: owns the socket and the HTTP state until the WebSocket accept succeeds
// (the stream then moves into the Session) or the refusal response is written.
struct PendingUpgrade : std::enable_shared_from_this<PendingUpgrade> {
  explicit PendingUpgrade(tcp::socket socket) : stream(std::move(socket)) {
    // The pre-upgrade read serves a peer that has proven nothing, so it does not get Beast's
    // defaults (1 MB body): an RFC 6455 upgrade carries no body, and 4 KiB of headers is ample.
    parser.body_limit(0);
    parser.header_limit(4096);
  }
  beast::tcp_stream stream;
  beast::flat_buffer buffer;
  http::request_parser<http::string_body> parser;
  http::response<http::string_body> response;
};

namespace {

// args[1] of the upgrade_refused event. The HTTP status in lane 0 is what the PEER was told, not
// a discriminant: two gates may answer with one status. Additive — new refusals take new values.
enum class RefusalReason : std::uint64_t {
  Subprotocol = 0, // 400: the offer list carried no mm.v1
  Origin = 1,      // 403: the request carried an Origin header
  Admission = 2,   // 503: at the concurrent-connection bound
};

// Ends an upgrade with an HTTP status the peer can read (400 wrong subprotocol, 403 a browser
// origin, 503 at the admission bound): a refusal a client can parse is one it can act on.
void refuse_upgrade(ServerImpl &srv, const std::shared_ptr<PendingUpgrade> &pending,
                    http::status status, RefusalReason reason, std::string_view body) {
  // Every refusal narrates from HERE, the one place all of them pass through: otherwise an
  // engine whose client offers the wrong subprotocol looks identical to one nobody is dialling.
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
  // Upgrades past the TCP accept are not in sessions_, so graceful shutdown cannot see them:
  // each pins io_.run(), and one completing later would insert a session into a finalized server.
  for (const auto &weak : upgrades_)
    if (const auto pending = weak.lock())
      pending->stream.close();
  // The WebSocket-handshake half of the same claim: past `stream ws{std::move(...)}` the live
  // socket belongs to the SESSION, so the loop above closes a detached stream and aborts nothing.
  for (const auto &weak : handshaking_)
    if (const auto session = weak.lock())
      beast::get_lowest_layer(session->stream()).close();
}

// The accept path's socket options, at namespace scope so a white-box test can call it and read
// the options back: no peer can observe the far end's options, so nothing else can pin them.
void apply_socket_options(tcp::socket &socket, const Config &cfg) {
  beast::error_code ec;
  // TCP_NODELAY at accept: quoting frames are tiny and latency-bound, so Nagle is a bad trade.
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
      // Only shutdown retires the listener; every OTHER accept failure is transient (descriptor
      // exhaustion, a reset between SYN and accept). Re-arm behind a delay so it cannot spin.
      if (stopping_ || !acceptor_.is_open())
        return;
      accept_backoff_.expires_after(kAcceptRetryDelay);
      accept_backoff_.async_wait([this](beast::error_code timer_ec) {
        if (!timer_ec && !stopping_ && acceptor_.is_open())
          accept_next();
      });
      return;
    }
    // The accept-time tier of Config::max_sessions: the readable 503 below can only answer a peer
    // that PRESENTED a request, so a peer that connects and sends nothing needs this bound.
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
  // A bound on the not-yet-upgraded read: a client that connects and never sends the request
  // must not hold a slot forever. The WebSocket idle timeout replaces it once accept takes over.
  pending->stream.expires_after(std::chrono::milliseconds{cfg_.upgrade_timeout_ms});
  http::async_read(
      pending->stream, pending->buffer, pending->parser,
      [this, pending](beast::error_code ec, std::size_t) {
        if (ec || stopping_)
          return; // the socket dies with `pending`
        auto &req = pending->parser.get();
        if (!websocket::is_upgrade(req) || !offers_mm_v1(req)) {
          // The negotiation is strict: no mm.v1 offered -> HTTP 400, never a subprotocol-less
          // accept the client would then misparse frames against.
          refuse_upgrade(*this, pending, http::status::bad_request, RefusalReason::Subprotocol,
                         "subprotocol mm.v1 is required\n");
          return;
        }
        // Cross-site WebSocket hijacking: WebSocket has no preflight and no same-origin policy,
        // so any page could otherwise drive this engine. A native client never sends Origin.
        if (req.count(http::field::origin) != 0) {
          refuse_upgrade(*this, pending, http::status::forbidden, RefusalReason::Origin,
                         "browser origins are not accepted\n");
          return;
        }
        // Admission control (Config::max_sessions): live_upgrades() counts this request too, so
        // the bound compares what accepting it would make the total. 503 lets a peer back off.
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
        // permessage-deflate stays OFF on both roles: frames are ~100-200 B, so compression
        // trades CPU for bytes this wire does not need and would make the codec A/B measure zlib.
        websocket::permessage_deflate pmd;
        pmd.server_enable = false;
        pmd.client_enable = false;
        ws.set_option(pmd);
        ws.read_message_max(kTransportMsgBytes); // two-tier bound: see server_impl.hpp
        ws.set_option(websocket::stream_base::decorator([](http::response<http::string_body> &res) {
          res.set(http::field::sec_websocket_protocol, kSubprotocol);
          // Beast stamps its own `Server: Boost.Beast/NNN` banner only when the decorator left
          // the field unset, so setting it keeps a library version off the wire.
          res.set(http::field::server, "mm_engine");
        }));
        auto session = std::make_shared<Session>(*this, std::move(ws));
        // Shutdown's only handle on this socket until the handshake resolves: `pending->stream`
        // was moved from above, and the session is not in sessions_ yet either.
        handshaking_.push_back(session);
        session->stream().async_accept(req, [this, session, pending](beast::error_code ec2) {
          // Drop this session's claim (and any expired sibling) first: a completed handshake
          // keeps its shared_ptr in sessions_, so a stale entry would grow this vector per connect.
          std::erase_if(handshaking_, [&session](const std::weak_ptr<Session> &weak) {
            const auto held = weak.lock();
            return !held || held == session;
          });
          // stopping_ is re-checked HERE: begin_stop can land during the handshake, and a session
          // inserted afterwards would never be closed. Dropping `session` closes the stream too.
          if (ec2 || stopping_)
            return;
          const std::uint64_t epoch = ++next_epoch_;
          sessions_.emplace(epoch, session);
          // Folded into the run's peak-session watermark at the one place a session enters the
          // map: above one session the recorded m0->m0' window is a fan-out minimum, not the truth.
          note_session_added();
          emit_event(TelemetryEvent::SessionOpen, epoch);
          session->start(epoch);
        });
      });
}

} // namespace mm::server_detail
