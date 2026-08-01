// integration — the HANDSHAKE and ADMISSION surface: subprotocol negotiation and
// its refusals, the browser-origin gate, compression policy, the pre-upgrade read bounds
// in body/header/time, the two admission tiers, and the socket options no peer can
// observe. The frame-policy half is in test_server_frames.cpp.
#include "server_test_support.hpp"

// The acceptor's INTERNAL header, reachable because cpp/src is on mm_tests' include path
// (CMakeLists) — the one white-box seam in this file, for the socket options no peer can
// observe. mm_core's public interface is unchanged: cpp/include stays Beast-free.
#include "server_impl.hpp"

#include <boost/asio/write.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace server_test;

namespace {

mm::Config base_config(const FeedFile &feed) {
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  return cfg;
}

// A two-line heartbeat: the same book forever (identical values re-arm nothing in the
// engine's fill sweep, but every publish advances md_seq and reaches every session).
FeedFile heartbeat_feed() {
  return FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
}

// A well-formed mm.v1 upgrade carrying `extra_headers` before the blank line and `body`
// after it, and whatever the acceptor answered. Hand-built rather than routed through
// http_upgrade_response because the pre-upgrade bounds are crossed by requests Beast's own
// writer will not compose: it emits no oversize header block, and it would set
// Content-Length itself. `reply` is empty when the parse failed — the acceptor answers a
// request it could not parse by dropping the socket, so "no response at all" IS the verdict.
std::optional<http::response<http::string_body>>
raw_upgrade_reply(std::uint16_t port, std::string_view extra_headers, std::string_view body) {
  boost::asio::io_context io;
  tcp::socket sock{io};
  sock.connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), port});
  std::string req = "GET / HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Protocol: mm.v1\r\n";
  req.append(extra_headers);
  req.append("\r\n");
  req.append(body);
  // Error-tolerant: the server may already have hung up on the header block mid-write, and
  // that hang-up is the behaviour under test rather than a failure of the case.
  beast::error_code write_ec;
  boost::asio::write(sock, boost::asio::buffer(req), write_ec);

  beast::flat_buffer buf;
  http::response<http::string_body> res;
  std::optional<beast::error_code> read_ec;
  http::async_read(sock, buf, res, [&](beast::error_code ec, std::size_t) { read_ec = ec; });
  io.run_for(std::chrono::seconds{2});
  REQUIRE(read_ec.has_value()); // still pending = the acceptor neither answered nor dropped
  if (*read_ec)
    return std::nullopt;
  return res;
}

} // namespace

TEST_CASE("server: handshake without mm.v1 is refused with HTTP 400", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};

  SECTION("no Sec-WebSocket-Protocol header at all") {
    CHECK(http_upgrade_status(srv.port(), nullptr) == 400);
  }
  SECTION("a subprotocol offer that does not include mm.v1") {
    CHECK(http_upgrade_status(srv.port(), "chat.v2") == 400);
  }
  // Exact token match (RFC 6455): case folding would silently accept a peer that
  // advertised a different protocol spelling.
  SECTION("MM.V1 is not mm.v1 — case is significant") {
    CHECK(http_upgrade_status(srv.port(), "MM.V1") == 400);
  }
  // Prefix match is also wrong: "mm.v1.extra" is a different token, not the v1 protocol.
  SECTION("mm.v1 as a prefix of a longer token is not an offer of mm.v1") {
    CHECK(http_upgrade_status(srv.port(), "mm.v1.extra") == 400);
  }

  // Outside the SECTIONs, so Catch2 runs it once per section: every refusal above must ALSO
  // be countable engine-side. A subprotocol mismatch is the likeliest wire-level
  // misconfiguration there is, and while it narrated nothing an engine whose only client
  // was misconfigured wrote a telemetry file identical to an engine nobody was dialling.
  // args[1] is the reason discriminant, not decoration: the 403 and the 503 emit the same
  // event name, and the status alone (lane 0) cannot separate two gates that answer alike.
  const auto refused = wait_for_line(srv.telemetry_path(), event_named("upgrade_refused"));
  REQUIRE(refused.has_value());
  CHECK((*refused)["args"][0] == 400);
  CHECK((*refused)["args"][1] == 0);
}

// CSWSH. WebSocket has no preflight and no same-origin policy, and a browser may offer any
// subprotocol it likes, so without this gate any page a user on a reachable network visits
// could open a socket to an engine that authenticates nothing. Both arms are the SAME
// request differing in ONE header — the 403 alone cannot distinguish a working gate from
// an acceptor that refuses everything, which is precisely how a broken gate would look.
TEST_CASE("server: an Origin header is refused 403; the same request without one is 101",
          "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};

  SECTION("a browser origin is refused, and the refusal is countable") {
    const auto res = http_upgrade_response(srv.port(), "mm.v1", nullptr, "http://evil.example");
    CHECK(res.result_int() == 403);
    CHECK(res.body().find("browser origins are not accepted") != std::string::npos);
    const auto refused = wait_for_line(srv.telemetry_path(), event_named("upgrade_refused"));
    REQUIRE(refused.has_value());
    CHECK((*refused)["args"][0] == 403);
    CHECK((*refused)["args"][1] == 1);
  }
  SECTION("no Origin: the native mm.v1 client is untouched by the gate") {
    const auto res = http_upgrade_response(srv.port(), "mm.v1");
    CHECK(res.result_int() == 101);
    CHECK(res[http::field::sec_websocket_protocol] == "mm.v1");
  }
}

// Beast stamps `Server: Boost.Beast/NNN` on the 101 whenever the decorator leaves the field
// unset, while the hand-built refusals carry no banner at all — so the disclosure and the
// INCONSISTENCY are one defect. Both halves are asserted here because either alone passes
// under half the fix.
TEST_CASE("server: no library-version banner reaches an unauthenticated peer", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};

  const auto accepted = http_upgrade_response(srv.port(), "mm.v1");
  REQUIRE(accepted.result_int() == 101);
  CHECK(accepted[http::field::server] == "mm_engine");
  const auto refused = http_upgrade_response(srv.port(), "chat.v2");
  REQUIRE(refused.result_int() == 400);
  CHECK(refused.count(http::field::server) == 0);
}

TEST_CASE("server: handshake with mm.v1 accepts, names mm.v1, no compression", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  const auto &res = client.upgrade_response();
  CHECK(res.result_int() == 101);
  // The response NAMES the selected subprotocol — the negotiation is visible on the wire,
  // not implied by the accept.
  REQUIRE(res.count(http::field::sec_websocket_protocol) == 1);
  CHECK(res[http::field::sec_websocket_protocol] == "mm.v1");
  // permessage-deflate must be absent from the response extensions (explicitly disabled
  // server-side; the A/B latency rationale lives at the server's option site).
  const auto ext = res[http::field::sec_websocket_extensions];
  CHECK(std::string{ext}.find("permessage-deflate") == std::string::npos);
}

// The offer is a comma-separated token list (RFC 6455 §11.3.4), optionally with OWS
// around tokens and optionally split across repeated headers. A mutant that treats the
// whole header value as one token, skips OWS, or only reads the first header value
// still passes the single-token happy path above — these sections pin the list rules.
//
// Field-boundary OWS (leading/trailing the whole header value) is stripped by the HTTP
// parser before offers_mm_v1 sees it, so sole-token " mm.v1 " is not a falsifier. The
// load-bearing OWS is INTERNAL: after a comma and before the next comma.
TEST_CASE("server: mm.v1 is recognised inside a multi-token Sec-WebSocket-Protocol offer",
          "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};

  SECTION("mm.v1 is not the first token; leading OWS after the comma is insignificant") {
    // "chat.v2, mm.v1" — the space after the comma is internal, so a no-strip mutant
    // compares " mm.v1" and refuses.
    const auto res = http_upgrade_response(srv.port(), "chat.v2, mm.v1");
    CHECK(res.result_int() == 101);
    CHECK(res[http::field::sec_websocket_protocol] == "mm.v1");
  }
  SECTION("trailing OWS before the next comma is insignificant") {
    // "mm.v1 ,chat.v2" — the space is internal (before the comma), so a no-trailing-strip
    // mutant compares "mm.v1 " and refuses. Field-trailing "mm.v1 " alone is NOT a
    // falsifier: the HTTP parser trims it.
    const auto res = http_upgrade_response(srv.port(), "mm.v1 ,chat.v2");
    CHECK(res.result_int() == 101);
    CHECK(res[http::field::sec_websocket_protocol] == "mm.v1");
  }
  SECTION("mm.v1 only on a second Sec-WebSocket-Protocol header") {
    // Beast's field setter overwrites; a raw dual-header request is the only way to
    // present the repeated-header shape the acceptor iterates.
    boost::asio::io_context io;
    tcp::socket sock{io};
    sock.connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), srv.port()});
    const std::string req = "GET / HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "Sec-WebSocket-Protocol: chat.v2\r\n"
                            "Sec-WebSocket-Protocol: mm.v1\r\n"
                            "\r\n";
    boost::asio::write(sock, boost::asio::buffer(req));
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    // Deadlined like every other read in this suite (server_test_support.hpp's opening
    // note), and like the non-upgrade case 25 lines below: a blocking http::read against an
    // acceptor that never answers eats the blanket ctest TIMEOUT and names nothing, where
    // this fails the CASE at the request that hung.
    std::optional<beast::error_code> read_ec;
    http::async_read(sock, buf, res, [&](beast::error_code ec, std::size_t) { read_ec = ec; });
    io.run_for(std::chrono::seconds{2});
    REQUIRE(read_ec.has_value());
    REQUIRE(!*read_ec);
    CHECK(res.result_int() == 101);
    CHECK(res[http::field::sec_websocket_protocol] == "mm.v1");
  }
}

// A non-upgrade HTTP request that merely names the subprotocol must still be refused by
// OUR policy path (is_upgrade is load-bearing). Status 400 alone is not enough: Beast's
// own accept failure also answers 400, with a different body — the body pins which
// refuse wrote the response.
TEST_CASE("server: a non-upgrade request is refused even when it names mm.v1", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};

  boost::asio::io_context io;
  tcp::socket sock{io};
  sock.connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), srv.port()});
  // Deliberately NOT an upgrade: no Upgrade/Connection/Key/Version.
  const std::string req = "GET / HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Sec-WebSocket-Protocol: mm.v1\r\n"
                          "\r\n";
  boost::asio::write(sock, boost::asio::buffer(req));
  beast::flat_buffer buf;
  http::response<http::string_body> res;
  std::optional<beast::error_code> read_ec;
  http::async_read(sock, buf, res, [&](beast::error_code ec, std::size_t) { read_ec = ec; });
  io.run_for(std::chrono::seconds{2});
  REQUIRE(read_ec.has_value());
  REQUIRE(!*read_ec);
  CHECK(res.result_int() == 400);
  // The engine's refuse_upgrade reason — not Beast's "Connection field is missing".
  CHECK(res.body().find("subprotocol mm.v1 is required") != std::string::npos);
}

// The pre-upgrade read is the ONE read this engine performs for a peer that has proven
// nothing, and Beast's defaults (1 MB body, 8 KiB header) are not a decision:
// measured on the shipped build before the bound, an upgrade declaring
// Content-Length: 900000 was read in full and STILL answered 101, so max_sessions pending
// upgrades each reached ~1 MB of heap before the mm.v1 gate ran. The third section is the
// load-bearing one — without it the bound is indistinguishable from a client-compatibility
// break, because both rejections would also fire on a legitimate request.
TEST_CASE("server: the pre-upgrade read is bounded in body and in header block", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};

  SECTION("a body-bearing upgrade is a parse failure, never a 101") {
    // RFC 6455 upgrades carry no body at all, so body_limit(0) makes any DECLARED body the
    // RFC-correct verdict: refused. 64 bytes, not the measured 900 KB — the limit is on the
    // declaration, and a small body keeps the write from racing the server's hang-up.
    const std::string body(64, 'x');
    CHECK_FALSE(raw_upgrade_reply(srv.port(), "Content-Length: 64\r\n", body).has_value());
  }
  SECTION("a header block past 4096 bytes is a parse failure, never a 101") {
    std::string headers;
    while (headers.size() < 5 * 1024)
      headers.append("X-Pad-" + std::to_string(headers.size()) + ": " + std::string(64, 'p') +
                     "\r\n");
    CHECK_FALSE(raw_upgrade_reply(srv.port(), headers, "").has_value());
  }
  SECTION("Content-Length: 0 still gets 101 — the bound is on the body, not on the header") {
    // A zero-length body is a legal thing for a client library to announce, and body_limit(0)
    // admits it (`0 > 0` is false). This is the arm that keeps the bound from silently
    // becoming a compatibility break: it fails the moment the limit is tightened to
    // "reject any Content-Length", which is the obvious wrong way to write the same fix.
    const auto res = raw_upgrade_reply(srv.port(), "Content-Length: 0\r\n", "");
    REQUIRE(res.has_value());
    CHECK(res->result_int() == 101);
    CHECK((*res)[http::field::sec_websocket_protocol] == "mm.v1");
  }
}

TEST_CASE("server: a client that OFFERS permessage-deflate still gets no compression", "[server]") {
  // The accept case above proves the response is extension-free against a client that
  // never asked. That cannot distinguish "disabled" from "never offered": the whole point
  // of server_enable=false is what happens when a peer DOES offer, which is the only
  // arm a negotiated compression arm would show up in.
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};

  const auto res = http_upgrade_response(srv.port(), "mm.v1", "permessage-deflate");
  REQUIRE(res.result_int() == 101);
  CHECK(std::string{res[http::field::sec_websocket_extensions]}.find("permessage-deflate") ==
        std::string::npos);
}

TEST_CASE("server: the engine refuses connections past its concurrent-session bound", "[server]") {
  auto feed = heartbeat_feed();
  auto cfg = base_config(feed);
  cfg.max_sessions = 1; // test-sized; the shipped default is 64
  ServerRunner srv{cfg};

  WsClient only{srv.port()};
  (void)only.read_json(); // established: the one slot is spoken for

  // 503 rather than a silent close, and the incumbent is untouched by the refusal.
  REQUIRE(http_upgrade_status(srv.port(), "mm.v1") == 503);
  // EXACT, not `>= 1`: a constant 1 emitted at both tiers survived every case in this file
  // while `>= 1` was the bar. This tier's bound compares sessions + in-flight upgrades and
  // the refused request IS one of them, so at max_sessions=1 the figure is 2 — one more
  // than the accept tier reports for the same ceiling, which is exactly the discriminant
  // lane 1 exists to carry.
  const auto refused = wait_for_line(srv.telemetry_path(), event_named("admission_refused"));
  REQUIRE(refused.has_value());
  CHECK((*refused)["args"][0] == 2);
  CHECK((*refused)["args"][1] == 1);
  // The 503 narrates TWICE, by design and not by oversight: admission_refused carries the
  // load the bound compared, upgrade_refused records that a refusal went out and which gate
  // wrote it. Neither is derivable from the other, so asserting both is what stops a later
  // "duplicate telemetry" cleanup from dropping one.
  const auto sent = wait_for_line(srv.telemetry_path(), event_named("upgrade_refused"));
  REQUIRE(sent.has_value());
  CHECK((*sent)["args"][0] == 503);
  CHECK((*sent)["args"][1] == 2);
  only.send_text(new_order(1, 1, "still-here", 499990, 100));
  const json verdict = next_verdict(only);
  REQUIRE(verdict["t"] == "order_ack");
  CHECK(verdict["cl_id"] == "still-here");
}

TEST_CASE("server: a connection that never presents an upgrade is bounded at ACCEPT", "[server]") {
  // The 503 above answers a peer that ASKED. This tier answers the peer that never does:
  // the admission check lives inside the upgrade-request completion, so a socket that
  // connects and stays mute never reaches it and holds a descriptor plus a PendingUpgrade
  // for the whole ten-second pre-upgrade read — with nothing counting them, in exactly
  // the descriptor-exhaustion regime the accept backoff exists for. What the case can
  // observe is the difference between the two bounds: past the ceiling the engine must
  // drop the socket promptly, not in ten seconds. admission_refused is the local record
  // that makes the drop countable (and replaces the settle sleep for the SECOND socket).
  auto feed = heartbeat_feed();
  auto cfg = base_config(feed);
  cfg.max_sessions = 1; // test-sized; the shipped default is 64
  ServerRunner srv{cfg};

  WsClient mute{srv.port(), {.handshake = false}}; // TCP only: the one pre-upgrade slot
  // The first socket still needs a settle: "accepted into upgrades_" has no event of its
  // own. Once it owns the slot, the second connect produces admission_refused.
  std::this_thread::sleep_for(std::chrono::milliseconds{250});

  boost::asio::io_context io;
  tcp::socket refused{io};
  refused.connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), srv.port()});
  const auto event = wait_for_line(srv.telemetry_path(), event_named("admission_refused"));
  REQUIRE(event.has_value());
  // EXACT, and deliberately DIFFERENT from the 503 tier's 2 for the same max_sessions=1:
  // this bound compares in-flight upgrades with the refused connection not yet among them,
  // so 1 is what it compared. The pair of exact values is the only thing that falsifies a
  // constant emitted at both tiers, and lane 1 names which arithmetic produced it.
  CHECK((*event)["args"][0] == 1);
  CHECK((*event)["args"][1] == 0);
  // Nothing was WRITTEN to this peer, so it has no upgrade_refused of its own: the accept
  // tier closes a socket that has said nothing to answer. The absence is the assertion —
  // without it, routing every drop through refuse_upgrade would look like an improvement.
  CHECK_FALSE(wait_for_line(srv.telemetry_path(), event_named("upgrade_refused"),
                            std::chrono::milliseconds{300})
                  .has_value());
  std::array<char, 8> sink{};
  std::optional<beast::error_code> result;
  refused.async_read_some(boost::asio::buffer(sink),
                          [&](beast::error_code ec, std::size_t) { result = ec; });
  io.run_for(std::chrono::seconds{2}); // a fifth of the pre-upgrade read bound
  REQUIRE(result.has_value());         // still pending here = the socket was NOT bounded
  CHECK(static_cast<bool>(*result));   // ...and what arrived was the disconnect, not data
}

// The OTHER half of that same defence, and the half nothing pinned: the accept-tier bound
// above only recycles a slot BECAUSE this deadline expires the socket holding it, so the
// sibling case passes on borrowed evidence. Deleting the deadline left all 161 cases green
// — a future edit could drop it silently and leak a descriptor per mute peer, with the
// sibling still passing. Config-sized so the assertion fits inside a test's patience.
TEST_CASE("server: a peer that connects and never sends is dropped at the pre-upgrade bound",
          "[server]") {
  auto feed = heartbeat_feed();
  auto cfg = base_config(feed);
  cfg.upgrade_timeout_ms = 300; // test-sized; the shipped default is 10 s
  ServerRunner srv{cfg};

  boost::asio::io_context io;
  tcp::socket mute{io};
  mute.connect(tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), srv.port()});

  std::array<char, 8> sink{};
  std::optional<beast::error_code> result;
  mute.async_read_some(boost::asio::buffer(sink),
                       [&](beast::error_code ec, std::size_t) { result = ec; });
  // ~6x the configured deadline, and a fifth of the SHIPPED one: a mutant that expires
  // never, or that ignores upgrade_timeout_ms and keeps the 10 s constant, leaves this read
  // pending and reds the REQUIRE below.
  io.run_for(std::chrono::seconds{2});
  REQUIRE(result.has_value());
  CHECK(static_cast<bool>(*result)); // the drop, not data: the engine sends nothing pre-upgrade
}

// White-box, because no peer can observe the far end's socket options: deleting the
// TCP_NODELAY line leaves all 161 cases green, so a black-box case cannot see a latency
// engine start running Nagled. cpp/src is on the tests' include path for this one seam and
// server_impl.hpp carries the declaration, so a signature change is a compile error here
// rather than a link error with nothing naming which side drifted.
TEST_CASE("server: apply_socket_options sets TCP_NODELAY, and SO_SNDBUF when configured",
          "[server]") {
  // A CONNECTED pair: TCP_NODELAY on an unopened socket is a different assertion, and one
  // that would pass on a platform that defers the option to connect.
  boost::asio::io_context io;
  tcp::acceptor acceptor{io, tcp::endpoint{boost::asio::ip::make_address_v4("127.0.0.1"), 0}};
  tcp::socket peer{io};
  peer.connect(acceptor.local_endpoint());
  tcp::socket accepted = acceptor.accept();

  mm::Config cfg;
  SECTION("the default Config leaves SO_SNDBUF alone and still disables Nagle") {
    mm::server_detail::apply_socket_options(accepted, cfg);
    tcp::no_delay no_delay;
    accepted.get_option(no_delay);
    CHECK(no_delay.value());
  }
  SECTION("a configured SO_SNDBUF reaches the socket") {
    // The falsifiable claim is the DIFFERENCE below (the socket moved), never the floor: a
    // floor alone passes whether or not the option was ever applied. So the request only
    // has to be small enough that every kernel honours it and large enough that the drop
    // from the default is unmistakable — 32 KiB against a REQUIRED default of at least 4x
    // that. Deriving it from the default instead (e.g. before/4) is what fails here: on
    // Linux a socket's AUTO-TUNED default sits above net.core.wmem_max, which setsockopt
    // clamps to, so a quarter of 1313280 still asked for more than the 212992 the kernel
    // would grant and the floor failed for a reason that is not this code. Measured in the
    // authoritative ubuntu:26.04 container, where the pre-fix form was the only red case.
    boost::asio::socket_base::send_buffer_size before;
    accepted.get_option(before);
    constexpr int requested = 32 * 1024;
    REQUIRE(before.value() > 4 * requested); // environment precondition, not a claim about mm
    cfg.so_sndbuf = requested;
    mm::server_detail::apply_socket_options(accepted, cfg);

    tcp::no_delay no_delay;
    accepted.get_option(no_delay);
    CHECK(no_delay.value());
    boost::asio::socket_base::send_buffer_size after;
    accepted.get_option(after);
    CHECK(after.value() < before.value()); // the option REACHED the socket
    // ...and a FLOOR, never equality: kernels round the request UP for their own
    // bookkeeping (Linux stores twice what it is given), so `==` would pin the platform.
    CHECK(after.value() >= requested);
    // Both together are the kill. The floor alone cannot fail against a large default, and
    // the difference alone would accept a socket the option had merely shrunk to nothing.
  }
}
