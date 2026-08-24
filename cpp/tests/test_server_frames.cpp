// Task 7 integration — the FRAME policy and liveness surface: text-only framing and its
// 1002s, the two-tier message-size bound (the 64 KiB policy cap against the transport
// ceiling's 1009), inbound reassembly, control-frame keep-alive against the idle timeout,
// and the acceptor's port reuse. Every close code asserted here is a row of the plan's
// normative close-code table. Split from test_server_ws.cpp under the 500-line cap.
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

// A valid NewOrder grown to EXACTLY `bytes` on the wire through an ignored additive key:
// the size-bound cases need an exact byte count that is also a real command, so what the
// cap decides is acceptance rather than which rejection code comes back.
std::string order_of_exactly(std::size_t bytes, std::uint64_t seq, std::string_view cl_id) {
  const std::string unpadded = new_order_with_unknown_key(seq, 1, cl_id, 499990, 100);
  REQUIRE(unpadded.size() <= bytes);
  return new_order_with_unknown_key(seq, 1, cl_id, 499990, 100,
                                    std::string(bytes - unpadded.size(), 'a'));
}
} // namespace

TEST_CASE("server: first TOB carries seq=1 epoch=1 and a md_seq", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  const json tob = client.read_json();
  CHECK(tob["t"] == "top_of_book");
  CHECK(tob["v"] == 1);
  CHECK(tob["seq"] == 1);
  CHECK(tob["epoch"] == 1);
  CHECK(tob["md_seq"].get<std::uint64_t>() >= 1);
  CHECK(tob["symbol"] == "MOCKUSDT");
  CHECK(tob["bid_px"] == 500000);
  CHECK(tob["ask_px"] == 500010);
}

TEST_CASE("server: a binary frame closes the session 1002", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  client.send_binary(std::string_view{"\x01\x02\x03", 3});
  CHECK(client.read_until_close() == 1002);
}

TEST_CASE("server: malformed RFC 6455 framing is recorded as the close Beast sent", "[server]") {
  // A silent feed: these cases are about the framing verdict alone, and an unread
  // heartbeat backlog turns the closing handshake into a transport reset — a DIFFERENT
  // row of the close-code table, and the one that would mask the rows under test.
  FeedFile feed{R"({"halt_ms":3600000})"};
  ServerRunner srv{base_config(feed)};

  // Raw bytes, because a conforming client composes neither of these. Beast fails the
  // read and puts the close on the wire ITSELF, so the wire half was already right and
  // asserting only it would prove nothing; the RECORD is the half that was wrong — every
  // Beast error but clean-close/timeout/oversize fell into the generic abnormal-teardown
  // branch, narrating 1006 for a close the peer had already received as 1002 or 1007.
  std::vector<unsigned char> frame;
  std::uint16_t expected = 0;
  SECTION("an unmasked client frame (RFC 6455 §5.1) is 1002") {
    frame = {0x81, 0x09, '{', '"', 'g', 'a', 'r', 'b', 'a', 'g', 'e'};
    expected = 1002;
  }
  SECTION("a text frame whose payload is not valid UTF-8 is 1007") {
    frame = {0x81, 0x81, 0x00, 0x00, 0x00, 0x00, 0xFF}; // masked, one 0xFF byte
    expected = 1007;
  }

  WsClient client{srv.port()}; // the handshake is synchronous: the session is up, epoch 1
  boost::asio::write(beast::get_lowest_layer(client.stream()).socket(), boost::asio::buffer(frame));
  CHECK(client.read_until_close() == expected);

  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][0] == 1);
  CHECK((*closed)["args"][1] == expected);
}

TEST_CASE("server: the 64 KiB policy cap is INCLUSIVE — exactly 65536 is accepted", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  // A COMPLETE, valid command sized to exactly the cap, padded through a key the schema
  // ignores. The cap is INCLUSIVE — 65536 is admitted and rejection begins at 65537 — which
  // is what `>` rather than `>=` spells, and the difference is invisible to a case that
  // only ever sends cap+1: both spellings reject that. This one fails under `>=`.
  const std::string at_cap = order_of_exactly(64 * 1024, /*seq=*/1, "edge-1");
  REQUIRE(at_cap.size() == 64 * 1024);
  client.send_text(at_cap);
  const json accepted = next_verdict(client);
  REQUIRE(accepted["t"] == "order_ack");
  CHECK(accepted["cl_id"] == "edge-1");

  // One byte past it is the reject-and-survive arm, so the boundary is pinned on BOTH
  // sides by the same case.
  client.send_text(std::string(64 * 1024 + 1, 'a'));
  const json refused = next_verdict(client);
  REQUIRE(refused["t"] == "reject");
  CHECK(refused["code"] == "MSG_TOO_LARGE");
}

TEST_CASE("server: oversize message within the transport bound rejects and survives", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  // Above the 64 KiB policy cap but below the transport ceiling: fully reassembled, so
  // the session can answer on the message path and keep running.
  client.send_text(std::string(64 * 1024 + 1, 'a'));
  json reject;
  for (;;) { // TOB heartbeats interleave; take the first reject
    reject = client.read_json();
    if (reject["t"] == "reject")
      break;
  }
  CHECK(reject["code"] == "MSG_TOO_LARGE");
  CHECK(reject["cl_id"] == "");

  // Alive: a valid order on the same connection still round-trips.
  client.send_text(new_order(1, 1, "alive-1", 499990, 100));
  for (;;) {
    const json msg = client.read_json();
    if (msg["t"] == "order_ack") {
      CHECK(msg["cl_id"] == "alive-1");
      break;
    }
  }
}

// Fragmented inbound messages are legal WS and Beast reassembles them, but every OTHER
// client in this suite sets auto_fragment(false), so the whole frame-policy requirement was
// asserted only for single-frame messages. Both sections write through the raw stream to
// opt back in: the session reads MESSAGES, and a read that handed the command path a FRAME
// would still pass every one-frame case in the file.
TEST_CASE("server: a fragmented command is reassembled before the command path sees it",
          "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  auto &ws = client.stream();
  ws.text(true); // the opcode belongs to the FIRST frame of the message

  SECTION("a valid NewOrder split across two frames is acked") {
    const std::string cmd = new_order(1, 1, "frag-ok", 499990, 100);
    const std::size_t half = cmd.size() / 2;
    ws.write_some(false, boost::asio::buffer(cmd.data(), half));
    ws.write_some(true, boost::asio::buffer(cmd.data() + half, cmd.size() - half));

    const json ack = next_verdict(client);
    REQUIRE(ack["t"] == "order_ack");
    CHECK(ack["cl_id"] == "frag-ok");
  }
  SECTION("the policy cap is applied to the REASSEMBLED message, not to any one frame") {
    // Above the 64 KiB policy cap and below the 128 KiB transport ceiling, delivered as
    // frames each well UNDER the cap: the recoverable Reject{MSG_TOO_LARGE} is owed to the
    // assembled size, so a bound that ever looked at a frame would let this through.
    const std::string payload(64 * 1024 + 1024, 'a');
    constexpr std::size_t kFragment = 8 * 1024;
    for (std::size_t sent = 0; sent < payload.size(); sent += kFragment) {
      const std::size_t n = std::min(kFragment, payload.size() - sent);
      ws.write_some(sent + n == payload.size(), boost::asio::buffer(payload.data() + sent, n));
    }
    const json reject = next_verdict(client);
    REQUIRE(reject["t"] == "reject");
    CHECK(reject["code"] == "MSG_TOO_LARGE");

    // The policy tier is the RECOVERABLE one: a 1009 here would mean the two-tier bound
    // collapsed for multi-frame messages while the single-frame case stayed green.
    client.send_text(new_order(1, 1, "frag-alive", 499990, 100));
    const json ack = next_verdict(client);
    REQUIRE(ack["t"] == "order_ack");
    CHECK(ack["cl_id"] == "frag-alive");
  }
}

TEST_CASE("server: message past the transport ceiling closes 1009 mid-reassembly", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  // Beyond the transport ceiling: the frame header already announces more than the
  // server will ever reassemble, so the read fails mid-message and the session dies as
  // 1009 — there is no clean message boundary left to answer on. WIRE delivery of that
  // close frame races the peer's own remaining flood (a reset while it is still sending
  // destroys buffered inbound data — TCP, not policy), so the deterministic assertion
  // is the RECORDED discipline: telemetry pins the 1009; the client observes either the
  // frame or the reset, and in every outcome the connection is dead.
  try {
    client.send_text(std::string(3 * 64 * 1024, 'a'));
    const auto code = client.read_until_close();
    CHECK(code == 1009);
  } catch (const std::exception &) {
    // the reset arm: the send or the read died with the connection
  }
  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][0] == 1);
  CHECK((*closed)["args"][1] == 1009);
}

TEST_CASE("server: a dead peer is reaped by the idle timeout and recorded as 1001", "[server]") {
  auto feed = heartbeat_feed();
  auto cfg = base_config(feed);
  cfg.idle_timeout_ms = 300; // test-sized; the shipped default is 30 s
  ServerRunner srv{cfg};

  WsClient client{srv.port()};
  // The client never reads: keep-alive pings queue unanswered in its socket, no pong ever
  // arrives, and the engine must reap the session at the idle timeout.
  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][0] == 1);    // epoch
  CHECK((*closed)["args"][1] == 1001); // recorded close code: going_away
  // The peer's transport dies without a close frame — 1001 cannot be DELIVERED to a dead
  // peer, only recorded.
  CHECK(client.transport_died());
}

// The dead-peer case above cannot distinguish "idle timeout alone" from "keep-alive
// pings + idle timeout": a client that never reads fails both ways. A live reader that
// never sends application data survives ONLY because Beast auto-answers keep-alive
// pings during read — with keep_alive_pings=false that peer is reaped as idle.
TEST_CASE("server: a reading peer survives past the idle timeout via keep-alive pings",
          "[server]") {
  auto feed = heartbeat_feed();
  auto cfg = base_config(feed);
  // 1000, not the sibling dead-peer case's 300, and the arithmetic is the reason: Beast
  // arms the timer at idle/2 and kills the session on the SECOND expiry with no inbound,
  // so a peer dies one full idle_timeout after its last pong — and this client answers
  // pings ONLY while its own io_context runs, i.e. only inside read_json. At 300 ms the
  // whole margin was one 150 ms ping interval, so a single descheduling window in a
  // parallel sanitizer run reaps the session and reds the case for a reason unrelated to
  // the property. This case asserts the POSITIVE (survival), which slowness can only make
  // less true — the opposite of its dead-peer sibling, which is why only this one needs
  // the headroom.
  cfg.idle_timeout_ms = 1000; // test-sized; the shipped default is 30 s
  ServerRunner srv{cfg};
  WsClient client{srv.port()};

  // Drain heartbeats for 3x the idle timeout. No application frames are sent: the only
  // inbound the server sees is the pongs its keep-alive pings provoke.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{3000};
  while (std::chrono::steady_clock::now() < deadline) {
    const json msg = client.read_json();
    CHECK(msg["t"] == "top_of_book");
  }
  // Still a live session: a command round-trips after the idle window has elapsed.
  client.send_text(new_order(1, 1, "still-alive", 499990, 100));
  const json verdict = next_verdict(client);
  REQUIRE(verdict["t"] == "order_ack");
  CHECK(verdict["cl_id"] == "still-alive");
}

// Application frames are text (JSON v1). A mutant that stamps outbound frames binary
// still delivers parseable payloads — the opcode is the property under test.
TEST_CASE("server: outbound application frames are text, not binary", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{base_config(feed)};
  WsClient client{srv.port()};

  const json tob = client.read_json();
  REQUIRE(tob["t"] == "top_of_book");
  CHECK(client.stream().got_text());
  CHECK_FALSE(client.stream().got_binary());
}

TEST_CASE("server: acceptor sets SO_REUSEADDR (immediate same-port rebind)", "[server]") {
  auto feed = heartbeat_feed();
  std::uint16_t port = 0;
  {
    ServerRunner first{base_config(feed)};
    port = first.port();
    WsClient client{port};
    (void)client.read_json(); // a live connection, so close leaves TIME_WAIT state behind
    first.stop();
  }
  // Rebinding the same port immediately after a stop with connections in TIME_WAIT is
  // exactly what SO_REUSEADDR buys; without it this bind refuses.
  telemetry_test::TempPath telemetry{"rebind"};
  auto cfg = base_config(feed);
  cfg.port = port;
  cfg.telemetry_out = telemetry.path().string();
  mm::Server second{cfg, mm::Instrument{.symbol = "MOCKUSDT"}};
  CHECK(second.port() == port);
}
