// integration — the session EPOCH lifecycle: what a connection owns and what it
// gives back. Live-order cancellation at disconnect and at close INTENT (which are
// different instants), the fresh epoch a reconnect mints, the stale-epoch verdict,
// cross-session isolation, the close-code recording of a peer-chosen close,
// and the entry-cap policy close (a backlog item Split from test_server_session.cpp
// when the Phase-4 gate work carried it past the repository's 500-line cap, along the same
// behavioural seam the original split used: that file keeps the per-message path (service
// acks and their svc_ns pairing, report ordering, inbound and outbound sequencing), this
// one keeps everything scoped to the epoch rather than to a message.
#include "server_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

using namespace server_test;

namespace {

mm::Config heartbeat_config(const FeedFile &feed) {
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  return cfg;
}

FeedFile heartbeat_feed() {
  return FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
}

// Reads until a message of tag `t` arrives (TOB heartbeats interleave with reports).
// BOUNDED, through the shared helper that owns the bound — see the same helper's rationale
// in test_server_session.cpp; it is duplicated rather than shared because this TU also does
// `using namespace server_test;`, which would make a same-named helper there ambiguous.
json next_of(WsClient &client, std::string_view t) {
  return next_matching(client, [t](const json &msg) { return msg["t"] == t; }, t);
}

} // namespace

TEST_CASE("server: a disconnect cancels the session's live orders in the ENGINE", "[server]") {
  // The gauge case below narrates cancel-on-disconnect through telemetry, but the gauge
  // sums live_count only over sessions still in the map — so it reads zero the instant
  // the map entry is erased, whether or not the engine was ever told. This case asks the
  // ENGINE instead: an order that outlived its session would still be swept by the next
  // book, and the fill counter counts a fill wherever it is routed.
  // The leading halt is the connect window: a client that arrives after the resting book
  // would meet the CROSSING one first and draw POST_ONLY_CROSS, which would zero the fill
  // count for the wrong reason entirely.
  FeedFile feed{R"({"halt_ms":300})", R"({"set":[500000,100,500010,80]})", R"({"halt_ms":1500})",
                R"({"set":[499990,100,500000,120]})", R"({"halt_ms":100})"};
  auto cfg = heartbeat_config(feed);
  cfg.loop_feed = false; // the crossing book then exhaustion: the engine stops itself
  ServerRunner srv{cfg};

  WsClient client{srv.port()};  // epoch 1: the session whose order must not outlive it
  WsClient watcher{srv.port()}; // epoch 2, orderless — the case's CLOCK, see below

  const json tob = next_of(client, "top_of_book");
  REQUIRE(tob["ask_px"] == 500010); // the RESTING book: the order below must not cross
  client.send_text(new_order(1, 1, "orphan-1", 500000, 100, tob["md_seq"].get<std::uint64_t>()));
  const json ack = next_verdict(client);
  REQUIRE(ack["t"] == "order_ack");
  CHECK(ack["cl_id"] == "orphan-1");
  client.close(websocket::close_code::normal);

  // The teardown is what cancels, so the crossing book must land strictly after it.
  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][0] == 1);
  // ...and the counters may not be read until that book has actually been SWEPT. Stopping
  // the engine on the test thread's own schedule would beat the book to the engine and
  // leave fills at zero because nothing ever crossed — the reading that proves nothing.
  // The watcher's own TOB stream is the event to wait on.
  (void)next_matching(
      watcher, [](const json &msg) { return msg["t"] == "top_of_book" && msg["ask_px"] == 500000; },
      "the crossing book on the watcher's stream");

  const auto counters = srv.counters_after_stop();
  CHECK(counters.orders == 1); // the order was real: without this the fill count is vacuous
  CHECK(counters.fills == 0);  // ...and the book that crossed it found nothing left to fill
}

TEST_CASE("server: a close INTENT cancels the live orders, not the teardown behind it",
          "[server]") {
  // The case above proves cancel-on-disconnect at TEARDOWN and would stay green with the
  // retirement moved back there: a peer's clean close tears the session down inside one
  // round trip, so intent and teardown are the same instant and its comment says as much
  // ("the teardown is what cancels"). They are NOT the same instant on a close the peer
  // never answers. The session sits in the server's map for the whole closing handshake,
  // and an epoch still live in the engine keeps being swept by every publish in that
  // window — each fill COUNTED into Counters::fills and then dropped unsent, because
  // pump() stops popping the moment the close frame is queued.
  //
  // So: the order rests, a close INTENT is declared, and the client then reads nothing —
  // its close reply never comes, only the 2 s close grace ends the session, and the
  // crossing book below lands squarely inside that window. Scope: this reaches the one
  // latch point through begin_close, which is every intent site a peer can drive. The
  // other two — the 1009 mid-reassembly close and the 1011 empty-encode close — latch
  // through the same call but have no window and no seam respectively;
  // the limitations backlog carries both with the measurement.
  FeedFile feed{R"({"halt_ms":300})", R"({"set":[500000,100,500010,80]})", R"({"halt_ms":700})",
                R"({"set":[499990,100,500000,120]})", R"({"halt_ms":100})"};
  auto cfg = heartbeat_config(feed);
  cfg.loop_feed = false;       // the crossing book then exhaustion: the engine stops itself
  cfg.max_session_entries = 3; // test-sized: the policy-close section's trigger
  ServerRunner srv{cfg};

  WsClient client{srv.port()};  // epoch 1: the session whose order must not outlive its close
  WsClient watcher{srv.port()}; // epoch 2, orderless — the case's CLOCK, as above

  const json tob = next_of(client, "top_of_book");
  REQUIRE(tob["ask_px"] == 500010); // the RESTING book: the order below must not cross
  client.send_text(new_order(1, 1, "doomed-1", 500000, 100, tob["md_seq"].get<std::uint64_t>()));
  REQUIRE(next_verdict(client)["cl_id"] == "doomed-1");

  // BOTH close dispositions, because the retirement sits above the branch between them
  // and a retirement conditioned on either arm has to fail on one of these two.
  std::uint16_t intent_code = 0;
  SECTION("a 1002 protocol close, which SHEDS what is queued") {
    intent_code = 1002;
    client.send_binary(std::string_view{"\x01\x02\x03", 3});
  }
  SECTION("a 1008 policy close, which DRAINS what is queued first") {
    intent_code = 1008;
    // Two more entries breach the per-session cap of three (the resting order is the
    // first): a policy close, drain and all, from a session that is not misbehaving on
    // the wire at all. The rejects and the close frame fit in the socket buffers, so the
    // drain completes without this client ever reading — which is what leaves the close
    // frame unanswered and the grace timer in charge.
    for (std::uint64_t seq = 2; seq <= 3; ++seq)
      client.send_text(new_order(seq, 1, "capped-" + std::to_string(seq), 499991, 100));
  }

  // The crossing book must have been SWEPT before the counters are read; the watcher's
  // own TOB stream is the event to wait on (stopping on the test thread's schedule would
  // beat the book to the engine and leave fills at zero for the wrong reason entirely).
  (void)next_matching(
      watcher, [](const json &msg) { return msg["t"] == "top_of_book" && msg["ask_px"] == 500000; },
      "the crossing book on the watcher's stream");
  // Drain the watcher as a LIVE peer: feed exhaustion has already begun the shutdown, and
  // answering its 1001 keeps a second close grace off this case's runtime.
  CHECK(watcher.read_until_close() == 1001);

  // The close the section actually provoked, so a fill count of zero can never be read as
  // proof of a retirement when some OTHER close path (or none) ran.
  const auto closed = wait_for_line(srv.telemetry_path(), [](const json &line) {
    return line.contains("event") && line["event"] == "session_close" && line["args"][0] == 1;
  });
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][1] == intent_code);

  const auto counters = srv.counters_after_stop();
  CHECK(counters.orders == 1); // the order was real: without this the fill count is vacuous
  CHECK(counters.fills == 0);  // ...and the close INTENT had already cancelled it
}

TEST_CASE("server: a peer's EMPTY close body is recorded as the 1000 that went out", "[server]") {
  // An empty close body is legal and conformant — RFC 6455 5.5.1, and a browser's
  // argument-less WebSocket.close() sends one — and Beast ANSWERS it 1000 while reporting
  // reason().code as close_code::none (0) to the application. Recording that raw 0 would
  // put a value that is not a close code at all into session_close args[1], violating the
  // same invariant the 1006-vs-1002/1007 recording fix exists for: the record must be the
  // code that went out. Nothing else in this suite can see it — every other peer-initiated
  // close closes with an explicit 1000, where the normalisation and its absence agree.
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};

  WsClient client{srv.port()};
  (void)next_of(client, "top_of_book"); // the session is live before it is closed
  client.close_without_code();

  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][0] == 1);
  CHECK((*closed)["args"][1] == 1000);
}

TEST_CASE("server: a reconnect gets a fresh epoch and the live-order gauge returns to zero",
          "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};

  // The engine-wide live_orders gauge narrates the order's life; the run-start baseline
  // snapshot is ALSO a zero, so the flat-after-close probe must only look at lines that
  // appear after the live==1 line.
  const auto gauge_at_or_after = [&](std::size_t from, int want) -> std::optional<std::size_t> {
    const auto giveup = std::chrono::steady_clock::now() + kOpDeadline;
    for (;;) {
      const auto lines = telemetry_lines(srv.telemetry_path());
      for (std::size_t i = from; i < lines.size(); ++i)
        if (lines[i].contains("live_orders") && lines[i]["live_orders"] == want)
          return i;
      if (std::chrono::steady_clock::now() >= giveup)
        return std::nullopt;
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
  };

  std::optional<std::size_t> live_idx;
  {
    WsClient first{srv.port()};
    CHECK(next_of(first, "top_of_book")["epoch"] == 1);
    first.send_text(new_order(1, 1, "held-1", 499990, 100));
    (void)next_of(first, "order_ack");
    live_idx = gauge_at_or_after(0, 1);
    REQUIRE(live_idx.has_value());
    first.close(websocket::close_code::normal);
  }

  // The gauge returns to zero without any cancel being sent — the disconnect IS the
  // cancel. Scope, stated because the name used to over-reach: the guarantee is bound to
  // the DISCONNECT, not to the reconnect. Nothing on this wire identifies a client across
  // connections, so epoch 2 is a new session with a fresh cl_id namespace rather than a
  // continuation whose predecessor it could be made to wait for.
  REQUIRE(gauge_at_or_after(*live_idx + 1, 0).has_value());

  WsClient second{srv.port()};
  CHECK(next_of(second, "top_of_book")["epoch"] == 2);
}

TEST_CASE("server: a stale-epoch command is rejected and the session stays up", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};

  { // burn epoch 1 so the reconnect below genuinely has a PREVIOUS epoch to be stale from
    WsClient first{srv.port()};
    (void)next_of(first, "top_of_book");
    first.close(websocket::close_code::normal);
  }

  WsClient client{srv.port()};
  CHECK(next_of(client, "top_of_book")["epoch"] == 2);
  client.send_text(new_order(1, /*epoch=*/1, "stale-1", 499990, 100));
  const json reject = next_of(client, "reject");
  CHECK(reject["code"] == "STALE_EPOCH");
  CHECK(reject["cl_id"] == "stale-1");
  CHECK(reject["epoch"] == 2); // the report envelope carries the SESSION's epoch

  // Session stays up, and the stale command consumed inbound seq 1.
  client.send_text(new_order(2, 2, "fresh-1", 499990, 100));
  CHECK(next_of(client, "order_ack")["cl_id"] == "fresh-1");

  // ...and NOTHING was created, which is the third clause of the stale-epoch row and the
  // one no assertion above can reach: a build that hands the stale command to the engine
  // under the CURRENT epoch and rejects the peer anyway satisfies the verdict, the report's
  // epoch and the round trip that follows, while a genuinely live order sits in the engine
  // under a cl_id the client believes was refused. A cancel is the wire-visible proof —
  // decisive, black-box, and the same shape the cross-session isolation case uses.
  client.send_text(cancel_order(3, 2, "stale-1"));
  const json unknown = next_verdict(client);
  REQUIRE(unknown["t"] == "reject");
  CHECK(unknown["code"] == "UNKNOWN_CLORDID");
  CHECK(unknown["cl_id"] == "stale-1");
}

TEST_CASE("server: cross-session isolation", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};

  WsClient a{srv.port()};
  (void)next_of(a, "top_of_book");
  a.send_text(new_order(1, 1, "shared-id", 499990, 100));
  (void)next_of(a, "order_ack");

  WsClient b{srv.port()};
  (void)next_of(b, "top_of_book");

  // B cancels A's cl_id: per-session namespaces make it unknown, and A is untouched. Read
  // the VERDICT rather than the expected tag — a session that reached into its neighbour's
  // namespace answers cancel_ack here, and that answer must fail on its own content.
  b.send_text(cancel_order(1, 2, "shared-id"));
  const json refused = next_verdict(b);
  REQUIRE(refused["t"] == "reject");
  CHECK(refused["code"] == "UNKNOWN_CLORDID");

  // B then reuses the same cl_id for its own valid order: fresh namespace, accepted.
  b.send_text(new_order(2, 2, "shared-id", 499990, 100));
  const json accepted = next_verdict(b);
  REQUIRE(accepted["t"] == "order_ack");
  CHECK(accepted["cl_id"] == "shared-id");

  // Closing B cancels only B's order: A's is still live enough to be cancelled BY A.
  b.close(websocket::close_code::normal);
  a.send_text(cancel_order(2, 1, "shared-id"));
  const json ack = next_verdict(a);
  REQUIRE(ack["t"] == "cancel_ack");
  CHECK(ack["cl_id"] == "shared-id");
  CHECK(ack["status"] == "cancelled");
}

TEST_CASE("server: entry-cap breach closes 1008 and other sessions are unaffected", "[server]") {
  auto feed = heartbeat_feed();
  auto cfg = heartbeat_config(feed);
  cfg.max_session_entries = 4; // test-sized (a backlog item
  ServerRunner srv{cfg};

  WsClient victim{srv.port()};
  (void)next_of(victim, "top_of_book");
  WsClient bystander{srv.port()};
  (void)next_of(bystander, "top_of_book");

  // Four INVALID orders: each is rejected yet mints a tombstone entry — rejected-order
  // tombstones are exactly the growth max_live_orders does not gate.
  std::vector<json> passed;
  for (std::uint64_t i = 1; i <= 4; ++i)
    victim.send_text(new_order(i, 1, "bad-" + std::to_string(i), /*px=*/499991, 100));
  const auto code = victim.read_until_close(&passed);
  CHECK(code == 1008);
  // All four rejects were delivered before the close (reports drain first).
  int rejects = 0;
  for (const auto &msg : passed)
    if (msg["t"] == "reject" && msg["code"] == "TICK_SIZE")
      ++rejects;
  CHECK(rejects == 4);

  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][0] == 1);
  CHECK((*closed)["args"][1] == 1008);

  // WHICH 1008 this was. The code alone cannot separate an entry-cap close from a
  // report-HWM one, so each cause carries its own record — and only the PAIR pins it: a
  // narration that labelled this close hwm_close as well satisfies every presence
  // assertion, changes nothing that fails, and tells an operator the queue overflowed when
  // the peer actually exhausted its entry budget. Lanes per the header's event table.
  const auto capped = wait_for_line(srv.telemetry_path(), event_named("entry_cap_close"));
  REQUIRE(capped.has_value());
  CHECK((*capped)["args"][0] == 1); // the victim's epoch
  CHECK((*capped)["args"][1] == 4); // entry count at the breach == cfg.max_session_entries

  // The ENGINE's own verdict is narrated, not merely answered on the wire. These four
  // rejects come back inside the engine's batch, whose event maps the wire spelling back
  // through reject_code_from_wire; the only reject event the suite asserted before
  // (MALFORMED, in the case above) reaches telemetry through push_reject with the code
  // passed directly, so that whole mapping could return nothing for every engine verdict —
  // TICK_SIZE, POST_ONLY_CROSS, UNKNOWN_CLORDID, MAX_LIVE_ORDERS, DUP_CLORDID — with the
  // suite green. FIRST reject of its code, never a COUNT: emit_reject_event coalesces per
  // (epoch, code) per window, so a count is unstable by construction while the first is
  // guaranteed (the latch opens empty).
  const auto narrated = wait_for_line(srv.telemetry_path(), [](const json &line) {
    return line.contains("event") && line["event"] == "reject" &&
           line["args"][1] == static_cast<int>(mm::RejectCode::TickSize);
  });
  REQUIRE(narrated.has_value());
  CHECK((*narrated)["args"][0] == 1);

  // The bystander session is untouched by the policy close.
  bystander.send_text(new_order(1, 2, "fine-1", 499990, 100));
  CHECK(next_of(bystander, "order_ack")["cl_id"] == "fine-1");

  // The ABSENCE half, and it has to be read from the FINAL artifact: a live poll can only
  // ever report "not yet", and "not yet" is not "never" — least of all for a line that
  // ships through a fire-and-forget try_push a full ring drops.
  // The bystander closes FIRST because the stop below JOINS: a session still open would
  // hold the shutdown through a whole closing handshake that this thread — also the
  // client's io thread — could not answer (the ServerRunner::initiate_stop note carries
  // the measured failure mode).
  bystander.close(websocket::close_code::normal);
  srv.stop();
  const auto lines = telemetry_lines(srv.telemetry_path());
  CHECK(std::none_of(lines.begin(), lines.end(), event_named("hwm_close")));
}
