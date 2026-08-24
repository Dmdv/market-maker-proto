// Task 7 integration — the session command path, one message at a time: service acks and
// the FIFO that pairs each measured svc_ns window with the command that cost it, report
// ordering (reports before the TOB that caused them), inbound sequencing and its 1002s,
// additive-key tolerance, and outbound seq contiguity. The EPOCH-scoped half — live-order
// cancellation, reconnects, stale epochs, cross-session isolation, the entry cap — lives in
// cpp/tests/test_server_epoch.cpp, split off under the 500-line cap along that seam.
#include "server_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

// The svc stream of a --bench-out dump: six u64 header words, then header[0] int64
// windows in processing order (the full-format reader lives in test_bench_recorder.cpp;
// this case needs only the first stream).
std::vector<std::int64_t> read_svc_stream(const std::filesystem::path &p) {
  std::ifstream in{p, std::ios::binary};
  REQUIRE(in.is_open());
  std::uint64_t header[6]{};
  in.read(reinterpret_cast<char *>(header), sizeof header);
  std::vector<std::int64_t> svc(header[0]);
  in.read(reinterpret_cast<char *>(svc.data()), static_cast<std::streamsize>(svc.size() * 8));
  REQUIRE(in.good());
  return svc;
}

// Reads until a message of tag `t` arrives (TOB heartbeats interleave with reports).
// BOUNDED, through the shared helper that owns the bound: the loop this replaced started a
// FRESH per-operation deadline on every read, so a server heartbeating forever without ever
// sending `t` tripped no deadline at all and the case hung until the blanket ctest TIMEOUT —
// which names neither the tag waited for nor the case waiting. Kept file-local rather than
// moved into server_test_support.hpp because this TU does `using namespace server_test;` and
// a same-named helper there would make every unqualified call here ambiguous.
json next_of(WsClient &client, std::string_view t) {
  return next_matching(client, [t](const json &msg) { return msg["t"] == t; }, t);
}

} // namespace

TEST_CASE("server: valid NewOrder round-trips an OrderAck with svc_ns > 0", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};
  WsClient client{srv.port()};

  const json tob = next_of(client, "top_of_book");
  client.send_text(new_order(1, 1, "ord-1", 499990, 100, tob["md_seq"].get<std::uint64_t>()));
  const json ack = next_of(client, "order_ack");
  CHECK(ack["cl_id"] == "ord-1");
  CHECK(ack["status"] == "live");
  CHECK(ack["eng_id"].get<std::uint64_t>() >= 1);
  CHECK(ack["epoch"] == 1);
  CHECK(ack["svc_ns"].get<std::int64_t>() > 0);
}

TEST_CASE("server: each OrderAck carries the svc_ns of the command that COST it", "[server]") {
  // The case above reads svc_ns from a session with exactly ONE ack in flight, where the
  // FIFO that pairs each measured window with its ack holds one entry and its front and its
  // back are the same value — so it holds unchanged under a mispairing. svc_ns is the M2
  // service window reported in §5.2: a mispairing corrupts a SCORED number while every
  // wire assertion in this file stays green.
  //
  // Two acks must therefore be queued before EITHER is popped, and that needs the write
  // loop already stalled: the pop that stamps svc_ns otherwise runs on the same event-loop
  // turn as the command that produced it, one entry at a time, forever.
  // Leading halt = the connect window. A session is sent nothing until the NEXT publish, so
  // a client that arrives after the only book would sit silent for the whole 3600 s halt
  // and the liveness read below would expire against a perfectly healthy server.
  FeedFile feed{R"({"halt_ms":200})", R"({"set":[500000,100,500010,80]})",
                R"({"halt_ms":3600000})"};
  auto cfg = heartbeat_config(feed);
  cfg.loop_feed = false;    // one book then silence: only this case's own reports move
  cfg.report_hwm = 100'000; // out of the way — the stall is the subject, not a policy close
  cfg.so_sndbuf = 4096;
  // The recorder is the pairing oracle (see the pin at the end): its svc stream and the
  // acks' svc_ns are written from the same computed value, in processing order.
  telemetry_test::TempPath bench_path{"svcpair"};
  cfg.bench_out = bench_path.path().string();
  ServerRunner srv{cfg};

  WsClient client{srv.port(), {.rcvbuf = 4096}};
  (void)next_of(client, "top_of_book"); // the session is live, and the socket starts empty

  // Cancels of unknown ids against a client that never reads — the report-HWM case's
  // measured recipe: 4 KiB of so_sndbuf plus 4 KiB of client rcvbuf swallow ~300 reports
  // (~38 KiB) before a write blocks, so 5000 leaves thousands with nowhere to go but
  // the queue on any build. Rejects are the right filler precisely because they push
  // NOTHING into the svc FIFO — only OrderAcks do — so the flood stalls the writer without
  // adding a single window this case would then have to reason about.
  for (std::uint64_t i = 1; i <= 5000; ++i)
    client.send_text(cancel_order(i, 1, "nobody-" + std::to_string(i)));

  // Both orders land behind that stall — the client is the only reader and it has not read,
  // so nothing can leave the socket between these two sends.
  //
  // HISTORY of the discriminator, because its predecessor died silently: this case used to
  // pad the first order with ~60 KB and assert padded_svc > plain_svc, on the premise that
  // the preflight scan sat INSIDE the measured window. The M2 boundary correction (e1 is
  // stamped AFTER decode — session.cpp on_message) moved that scan out of the window, and
  // the comparison decayed into a coin toss biased only by scheduler noise: it then failed
  // twice on loaded CI runners, once under asan and once under rel, same assertion. The
  // pairing pin below is TIMING-FREE instead: the ack's svc_ns and the BenchRecorder's svc
  // stream are the same int64 written at the same line (on_command computes `svc` once,
  // then rec->svc(svc) and pending_svc_.push_back(svc)), so each ack must carry EXACTLY
  // the recorder's value at its own position — a FIFO reversal miswires the values unless
  // the two windows happen to collide to the nanosecond, in which case the scored number
  // is identical under either mapping and no corruption is expressible at all.
  client.send_text(new_order(5001, 1, "svc-first", 499990, 100));
  client.send_text(new_order(5002, 1, "svc-second", 499990, 100));

  const auto is_ack = [](const json &msg) { return msg["t"] == "order_ack"; };
  // Bounded by the flood this case itself sent plus headroom, not by a number chosen to
  // make the read work.
  const json first_ack = next_matching(client, is_ack, "the first order's ack", 8192);
  const json second_ack = next_matching(client, is_ack, "the second order's ack", 8192);
  REQUIRE(first_ack["cl_id"] == "svc-first"); // reports are FIFO: first order's ack first
  REQUIRE(second_ack["cl_id"] == "svc-second");
  CHECK(first_ack["svc_ns"].get<std::int64_t>() > 0);
  CHECK(second_ack["svc_ns"].get<std::int64_t>() > 0);

  // The PRECONDITION, asserted rather than assumed: without a stalled writer the two acks
  // never coexist in the queue and the pairing is unobservable — a green case pinning
  // nothing. A writer that keeps up holds a depth of 1; two orders of magnitude above that
  // is the stall itself, and the floor sits well under the ~700 the sizing above predicts
  // so it fails on the STALL rather than on a host's buffer arithmetic.
  const auto counters = srv.counters_after_stop();
  CHECK(counters.outbox_depth_hw >= 100);

  // The pairing pin: the dump's svc stream is written in processing order (single owner
  // thread), the 1000 reject-only cancels contribute their own windows ahead of the two
  // orders, so the LAST TWO entries are the two orders' windows — and each ack must carry
  // its own, value-identically.
  const std::vector<std::int64_t> svc = read_svc_stream(bench_path.path());
  REQUIRE(svc.size() >= 2);
  CHECK(first_ack["svc_ns"].get<std::int64_t>() == svc[svc.size() - 2]);
  CHECK(second_ack["svc_ns"].get<std::int64_t>() == svc[svc.size() - 1]);
}

TEST_CASE("server: a fill is delivered before the TOB that caused it (reports first)", "[server]") {
  // Book A rests (published twice so a client connecting just after the first publish
  // still sees it), then book B's ask crosses the client's bid; the halts are the windows
  // for the connect and the order round trip.
  FeedFile feed{R"({"set":[500000,100,500010,80]})",  R"({"halt_ms":300})",
                R"({"set":[500000,100,500010,80]})",  R"({"halt_ms":800})",
                R"({"set":[499990,100,500000,120]})", R"({"halt_ms":400})"};
  auto cfg = heartbeat_config(feed);
  cfg.loop_feed = false; // A -> fill -> exhaustion stops the engine; the case is done by then
  ServerRunner srv{cfg};
  WsClient client{srv.port()};

  (void)next_of(client, "top_of_book");
  client.send_text(new_order(1, 1, "bid-1", 500000, 100, 1));
  (void)next_of(client, "order_ack");

  // The next book crosses: the SAME publish yields the fill report and the new TOB, and
  // the report must come off the wire first. Bounded for the reason next_of is: an
  // unbounded skip turns "the TOB came out first" into a deadline expiry that names nothing.
  const json first = next_matching(
      client,
      [](const json &msg) {
        return msg["t"] == "fill" || (msg["t"] == "top_of_book" && msg["ask_px"] == 500000);
      },
      "the crossing publish's first message"); // skips any residual heartbeat of book A
  REQUIRE(first["t"] == "fill");
  CHECK(first["cl_id"] == "bid-1");
  CHECK(first["px"] == 500000); // the order's own limit price (maker fill)
  CHECK(first["qty"] == 100);
  CHECK(first["leaves"] == 0);
  const json tob_after = next_of(client, "top_of_book");
  CHECK(tob_after["ask_px"] == 500000);
}

TEST_CASE("server: malformed frame draws a Reject and the session survives", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};
  WsClient client{srv.port()};

  client.send_text(R"({"garbage)");
  const json reject = next_of(client, "reject");
  CHECK(reject["code"] == "MALFORMED");
  CHECK(reject["cl_id"] == "");

  // The reject event narrates the same verdict with the lane assignment of the header's
  // event-lane table: args0 = epoch, args1 = RejectCode (Malformed).
  const auto line = wait_for_line(srv.telemetry_path(), event_named("reject"));
  REQUIRE(line.has_value());
  CHECK((*line)["args"][0] == 1);
  CHECK((*line)["args"][1] == static_cast<int>(mm::RejectCode::Malformed));

  client.send_text(new_order(1, 1, "after-bad", 499990, 100));
  CHECK(next_of(client, "order_ack")["cl_id"] == "after-bad");
}

TEST_CASE("server: an inbound envelope-seq gap closes the session 1002", "[server]") {
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};
  WsClient client{srv.port()};

  client.send_text(new_order(1, 1, "gap-1", 499990, 100));
  (void)next_of(client, "order_ack");
  client.send_text(new_order(3, 1, "gap-2", 499990, 100)); // seq 2 skipped: a client bug
  CHECK(client.read_until_close() == 1002);
}

TEST_CASE("server: a repeated or decreasing inbound seq closes the session 1002", "[server]") {
  // The gap case above only exercises seq > expected. The check is CONTIGUITY (`!=`), and
  // a weaker `>` — accepting duplicates and rewinds, the two shapes a retransmitting or
  // restarted client actually produces — passes that case unchanged. One book then
  // silence, so the close is the very next thing on the wire and a wrong verdict names
  // itself instead of expiring a deadline.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":3600000})"};
  auto cfg = heartbeat_config(feed);
  cfg.loop_feed = false;

  SECTION("a duplicate of the seq just consumed") {
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    client.send_text(new_order(1, 1, "seq-1", 499990, 100));
    CHECK(next_verdict(client)["cl_id"] == "seq-1");
    client.send_text(new_order(1, 1, "replay-1", 499990, 100));
    CHECK(expect_close(client) == 1002);
  }
  SECTION("a seq behind the first one ever expected") {
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    client.send_text(new_order(0, 1, "rewound-1", 499990, 100));
    CHECK(expect_close(client) == 1002);
  }
}

TEST_CASE("server: a command carrying keys the schema does not know is still accepted",
          "[server]") {
  // Additive forward compatibility (codec.hpp: "Unknown keys are ignored") asserted where
  // a peer actually meets it — end to end over the socket, not only in the codec's own
  // unit suite. A future client speaking v1 plus one extra field must be served, not
  // rejected.
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};
  WsClient client{srv.port()};

  (void)next_of(client, "top_of_book");
  client.send_text(new_order_with_unknown_key(1, 1, "additive-1", 499990, 100, "ignored"));
  const json verdict = next_verdict(client);
  REQUIRE(verdict["t"] == "order_ack");
  CHECK(verdict["cl_id"] == "additive-1");
  CHECK(verdict["status"] == "live");
}

TEST_CASE("server: a reject consumes the inbound seq only past the sequencer", "[server]") {
  // The client obligation stated in mm/server.hpp ("INBOUND SEQ CONSUMPTION AFTER A
  // REJECT") and mirrored in mm/codec.hpp, asserted from the peer's side because nothing
  // on the wire carries it: the Reject envelope stamps the OUTBOUND seq, and the
  // pre-sequencer rejects carry an empty cl_id. Getting it wrong costs the connection, so
  // the rule needs a case that fails when either half moves.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":3600000})"};
  auto cfg = heartbeat_config(feed);
  cfg.loop_feed = false;

  // Each pre-sequencer code is its own SECTION so a mutant that special-cases one of
  // them to advance the seq fails that SECTION alone — sampling only MALFORMED left
  // UNKNOWN_TYPE / UNSUPPORTED_VERSION unpinned against the client-obligation rule.
  const auto assert_seq_unconsumed = [](WsClient &client, std::string_view frame,
                                        std::string_view code, std::string_view cl_id) {
    client.send_text(frame);
    REQUIRE(next_verdict(client)["code"] == code);
    client.send_text(new_order(1, 1, cl_id, 499990, 100)); // the SAME seq, reused
    // next_verdict OUTSIDE CHECK so a 1002 surfaces as PeerClosed (not Catch2's
    // "Unknown exception") and the FAIL names the code that ate the seq.
    json verdict;
    try {
      verdict = next_verdict(client);
    } catch (const PeerClosed &closed) {
      FAIL("the reused seq after " << code << " drew close " << closed.code
                                   << " instead of an ack");
    }
    CHECK(verdict["cl_id"] == cl_id);
  };
  SECTION("MALFORMED leaves the seq unconsumed") {
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    assert_seq_unconsumed(client, R"({"garbage)", "MALFORMED", "reuse-malformed");
  }
  SECTION("UNKNOWN_TYPE leaves the seq unconsumed") {
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    assert_seq_unconsumed(
        client,
        R"({"t":"no_such_type","v":1,"seq":1,"epoch":1,"md_seq":1,"cl_id":"x","symbol":"MOCKUSDT","side":"B","px":1,"qty":1,"post_only":true})",
        "UNKNOWN_TYPE", "reuse-unknown");
  }
  SECTION("UNSUPPORTED_VERSION leaves the seq unconsumed") {
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    assert_seq_unconsumed(
        client,
        R"({"t":"new_order","v":7,"seq":1,"epoch":1,"md_seq":1,"cl_id":"x","symbol":"MOCKUSDT","side":"B","px":499990,"qty":100,"post_only":true})",
        "UNSUPPORTED_VERSION", "reuse-version");
  }
  SECTION("an engine reject has already consumed it") {
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    client.send_text(new_order(1, 1, "bad-tick", 499991, 100)); // TICK_SIZE, past the sequencer
    REQUIRE(next_verdict(client)["code"] == "TICK_SIZE");
    client.send_text(new_order(1, 1, "reuse-2", 499990, 100)); // reusing it is a duplicate
    CHECK(expect_close(client) == 1002);
  }
}

TEST_CASE("server: outbound envelope seq is contiguous across rejects, acks, cancels, fills",
          "[server]") {
  auto track = [](std::uint64_t &last, const json &msg) {
    const auto s = msg["seq"].get<std::uint64_t>();
    REQUIRE((last == 0 ? s >= 1 : s == last + 1));
    last = s;
  };
  SECTION("reject then order_ack then cancel_ack share one seq space") {
    FeedFile feed{R"({"halt_ms":200})", R"({"set":[500000,100,500010,80]})",
                  R"({"halt_ms":3600000})"}; // leading halt = connect window
    auto cfg = heartbeat_config(feed);
    cfg.loop_feed = false;
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    std::uint64_t last = 0;
    track(last, next_of(client, "top_of_book"));
    client.send_text(R"({"garbage)");
    auto m = next_verdict(client);
    REQUIRE((m["t"] == "reject" && m["code"] == "MALFORMED"));
    track(last, m);
    client.send_text(new_order(1, 1, "bad-tick", 499991, 100));
    m = next_verdict(client);
    REQUIRE((m["t"] == "reject" && m["code"] == "TICK_SIZE" && m["cl_id"] == "bad-tick"));
    track(last, m);
    client.send_text(new_order(2, 1, "live-1", 499990, 100));
    m = next_verdict(client);
    REQUIRE((m["t"] == "order_ack" && m["cl_id"] == "live-1"));
    track(last, m);
    client.send_text(cancel_order(3, 1, "live-1"));
    m = next_verdict(client);
    REQUIRE((m["t"] == "cancel_ack" && m["cl_id"] == "live-1"));
    track(last, m);
  }
  SECTION("a fill is stamped into the same contiguity as its preceding ack") {
    FeedFile feed{R"({"halt_ms":200})", R"({"set":[500000,100,500010,80]})", R"({"halt_ms":600})",
                  R"({"set":[499990,100,500000,120]})", R"({"halt_ms":400})"};
    auto cfg = heartbeat_config(feed);
    cfg.loop_feed = false;
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    std::uint64_t last = 0;
    bool saw_ack = false, saw_fill = false;
    for (int i = 0; i < 64 && !(saw_ack && saw_fill); ++i) {
      const json msg = client.read_json();
      track(last, msg);
      if (msg["t"] == "top_of_book" && !saw_ack && msg["ask_px"] == 500010)
        client.send_text(
            new_order(1, 1, "bid-fill", 500000, 100, msg["md_seq"].get<std::uint64_t>()));
      else if (msg["t"] == "order_ack") {
        CHECK(msg["cl_id"] == "bid-fill");
        saw_ack = true;
      } else if (msg["t"] == "fill") {
        CHECK(msg["cl_id"] == "bid-fill");
        saw_fill = true;
      }
    }
    REQUIRE((saw_ack && saw_fill));
  }
}

TEST_CASE("server: reject narration is bounded by the snapshot cadence, not by the peer",
          "[server]") {
  // Task 7 gate G-B, findings GB-3/GB-4. The coalescing landed in Phase 4 against a MEASURED
  // amplification — 121,600 malformed frames drew 3.6 MB of telemetry, 4.24x the bytes the
  // peer sent, at the CLIENT's rate — but nothing asserted the bound, so an unbounded emit
  // and a one-shot latch that silences every LATER code both passed the whole suite. The two
  // halves need each other: a rate assertion alone accepts the one-shot latch, and a
  // distinct-code assertion alone accepts unbounded emission.
  auto feed = heartbeat_feed();
  ServerRunner srv{heartbeat_config(feed)};
  WsClient client{srv.port()};
  (void)next_of(client, "top_of_book");

  // One code, many frames, comfortably inside ONE kRejectCoalesceWindow (1 s).
  constexpr int kFlood = 200;
  for (int i = 1; i <= kFlood; ++i) {
    client.send_text(cancel_order(static_cast<std::uint64_t>(i), 1, "nobody-" + std::to_string(i)));
    (void)next_of(client, "reject"); // every one is answered ON THE WIRE — volume is not lost
  }
  // A DIFFERENT code in the same window must still narrate: the latch is per (epoch, code),
  // and a one-shot latch would have spent itself on UNKNOWN_CLORDID above.
  client.send_text("{not json");
  (void)next_of(client, "reject");

  client.close(websocket::close_code::normal);
  const auto counters = srv.counters_after_stop();
  CHECK(counters.rejects >= kFlood + 1); // the COUNTER carries the volume, as designed

  const auto lines = telemetry_lines(srv.telemetry_path());
  const auto reject_events = std::count_if(lines.begin(), lines.end(), event_named("reject"));
  // Bounded by (codes x windows), never by frames. Two codes, and the flood is well inside
  // one window — a couple of window rollovers under a slow sanitizer build is fine, 200 is
  // not. The bound is what kills an uncoalesced emit; it is deliberately loose in the
  // direction that cannot hide the defect.
  CHECK(reject_events <= 8);
  CHECK(std::any_of(lines.begin(), lines.end(), [](const json &j) {
    return j.contains("event") && j["event"] == "reject" &&
           j["args"][1] == static_cast<int>(mm::RejectCode::UnknownClOrdId);
  }));
  CHECK(std::any_of(lines.begin(), lines.end(), [](const json &j) {
    return j.contains("event") && j["event"] == "reject" &&
           j["args"][1] == static_cast<int>(mm::RejectCode::Malformed);
  }));
}
