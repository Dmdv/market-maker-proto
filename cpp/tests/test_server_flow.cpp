// Task 7 integration — the OUTBOX policy surface: TOB conflation against unbroken
// envelope sequencing, the report-HWM 1008 and its drain, the protocol close that sheds
// instead, and the M-class watermarks that must never relax. The shutdown half is in
// test_server_shutdown.cpp and the durable-artifact half in test_server_artifacts.cpp;
// the fixtures all three share are in server_flow_support.hpp.
#include "server_flow_support.hpp"

using namespace server_flow;

TEST_CASE("server: burst conflates TOBs — md_seq gaps, envelope seq never does", "[server]") {
  // Two alternating books forever at millisecond cadence; small socket buffers on both
  // ends so a non-reading client exhausts TCP's slack quickly and the pending-tick slot
  // takes over.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"set":[500005,90,500015,70]})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 1;
  cfg.loop_feed = true;
  cfg.so_sndbuf = 4096;
  ServerRunner srv{cfg};

  WsClient client{srv.port(), {.rcvbuf = 4096}};
  // Do not read: the burst piles up against the stalled socket while the outbox keeps
  // only the freshest tick.
  std::this_thread::sleep_for(std::chrono::milliseconds{1200});

  std::vector<json> tobs;
  std::uint64_t expected_seq = 1;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{400};
  while (std::chrono::steady_clock::now() < until) {
    const json msg = client.read_json();
    REQUIRE(msg["t"] == "top_of_book");
    // The envelope sequence is CONTIGUOUS across the whole burst: conflation must gap
    // only md_seq (stamp-at-pop, F-06).
    CHECK(msg["seq"].get<std::uint64_t>() == expected_seq);
    ++expected_seq;
    tobs.push_back(msg);
  }
  REQUIRE(tobs.size() >= 2);
  const auto first_md = tobs.front()["md_seq"].get<std::uint64_t>();
  const auto last_md = tobs.back()["md_seq"].get<std::uint64_t>();
  // Fewer TOBs than publishes: the md_seq span walked past more books than were
  // delivered, which is conflation made visible exactly as the client is taught to read
  // it.
  CHECK(last_md - first_md + 1 > tobs.size());

  // While the session is STILL open the engine-wide conflated counter must already
  // include this session's outbox drops. A settle that only ever reads conflated_base_
  // (skipping the live-session sum) still looks right AFTER the close fold, so the
  // final counters alone cannot kill that mutant — only a mid-session totals line can.
  const auto live_conflated = wait_for_line(srv.telemetry_path(), [](const json &j) {
    return j.contains("conflated") && j.contains("sessions") &&
           j["sessions"].get<std::uint64_t>() >= 1 && j["conflated"].get<std::uint64_t>() >= 1;
  });
  REQUIRE(live_conflated.has_value());

  // Both M-class watermarks, MID-session and on the final counters. telemetry.hpp declares
  // them "maximum since construction, NEVER reset between snapshots" — Task 13 reads them as
  // queue-buildup evidence precisely because they cannot relax — and nothing in the suite
  // read either at server level: deleting each update outright left 161/161 green. This case
  // is where they are guaranteed non-zero, because the 1200 ms park against a 4 KiB socket
  // is what puts a message in the queue and a write on a stalled socket in the first place.
  const auto marks = wait_for_line(srv.telemetry_path(), [](const json &j) {
    return j.contains("outbox_depth_hw") && j["outbox_depth_hw"].get<std::uint64_t>() >= 1 &&
           j["send_lag_max_ns"].get<std::uint64_t>() > 0;
  });
  REQUIRE(marks.has_value());

  client.close(websocket::close_code::normal);
  const auto counters = srv.counters_after_stop();
  CHECK(counters.conflated >= 1); // folded into the cumulative base at session close
  CHECK(counters.outbox_depth_hw >= 1);
  CHECK(counters.send_lag_max_ns > 0);
}

TEST_CASE("server: report-HWM flood closes 1008 after draining queued reports", "[server]") {
  // A single book then silence: nothing but the flood's own reports moves on the wire.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":3600000})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.report_hwm = 8; // test-sized mark
  cfg.so_sndbuf = 4096;
  ServerRunner srv{cfg};

  WsClient client{srv.port(), {.rcvbuf = 4096}};
  // Cancels of unknown ids -> that many undroppable Reject reports, against a client that
  // never reads: the queue can only climb once the write loop stalls on a full socket.
  // The flood must DWARF what the kernel absorbs, not merely exceed the buffers asked
  // for — measured here, 4 KiB of so_sndbuf plus 4 KiB of client rcvbuf swallow ~300
  // reports (~38 KiB) before a write blocks. Sized near that ceiling the breach becomes a
  // function of OPTIMIZATION LEVEL rather than of policy: at 100 it fired under the dev
  // preset 10/10 and under rel 2/10, where the faster write loop kept the depth at 1 and
  // the case then hung waiting for a close that policy never had cause to send. 1000
  // leaves ~700 reports with nowhere to go but the queue, on any build.
  for (std::uint64_t i = 1; i <= 1000; ++i)
    client.send_text(cancel_order(i, 1, "nobody-" + std::to_string(i)));

  std::vector<json> delivered;
  const auto code = client.read_until_close(&delivered);
  CHECK(code == 1008);
  // Every report the engine COUNTED reached the wire ahead of the close frame — the
  // policy closes loudly after the drain, never by shedding. Counted-vs-delivered is the
  // only form that can fail on a shed: the envelope seq is stamped at POP time (F-06), so
  // a report dropped before pop leaves no gap behind for a contiguity check to find.
  const auto counters = srv.counters_after_stop();
  const auto rejects = std::count_if(delivered.begin(), delivered.end(), [](const json &j) {
    return j["t"] == "reject" && j["code"] == "UNKNOWN_CLORDID";
  });
  CHECK(static_cast<std::uint64_t>(rejects) == counters.rejects);

  // The hwm_close narration, pinned against the header's event-lane table:
  // args0 = epoch, args1 = report queue depth at the breach.
  const auto hwm = wait_for_line(srv.telemetry_path(), event_named("hwm_close"));
  REQUIRE(hwm.has_value());
  CHECK((*hwm)["args"][0] == 1);
  CHECK((*hwm)["args"][1].get<std::uint64_t>() >= 9);
  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][1] == 1008);
  // ...and NOT the other 1008's cause. The presence half alone cannot tell the two policy
  // closes apart — a build that narrated an entry-cap close as a report-HWM close (or the
  // mirror of it) satisfies every assertion above — so the ABSENCE is the mutant-killer, and
  // it is read from the FINAL artifact because absence is only decidable once the run's last
  // byte is on disk. counters_after_stop() above already joined, so it is.
  const auto lines = telemetry_lines(srv.telemetry_path());
  CHECK(std::none_of(lines.begin(), lines.end(), event_named("entry_cap_close")));
}

TEST_CASE("server: a PROTOCOL close sheds queued reports instead of draining them", "[server]") {
  // The mirror of the case above, and the half the suite could not tell apart: policy
  // closes (1008/1001) drain, protocol closes (1002) do not. A build that drained on
  // EVERY close passes every 1002 case in this suite, because they all assert only the
  // code. The distinguishing observable is counted-vs-delivered, read the other way
  // round: with the queue deep, strictly FEWER rejects reach the wire than the engine
  // counted, because the close supersedes them.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":3600000})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.report_hwm = 100'000; // out of the way: the 1008 path is the OTHER case
  cfg.so_sndbuf = 4096;
  ServerRunner srv{cfg};

  WsClient client{srv.port(), {.rcvbuf = 4096}};
  // Sized to DWARF the kernel's appetite, not merely exceed the buffers asked for:
  // under sanitizer+load the kernel has been observed to swallow the entire former
  // 1000-frame flood (queue empty at the close, nothing left to shed, and the
  // counted-vs-delivered assertion below collapses to equality). 5000 (~450 KiB) leaves
  // thousands queued on any build and any buffer autotuning. All sends complete BEFORE
  // the close latches, so unlike the HWM case above this flood can scale freely.
  for (std::uint64_t i = 1; i <= 5000; ++i)
    client.send_text(cancel_order(i, 1, "nobody-" + std::to_string(i)));
  client.send_binary(std::string_view{"\x01\x02\x03", 3}); // the 1002: framing, not policy

  std::vector<json> delivered;
  const auto code = client.read_until_close(&delivered);
  CHECK(code == 1002);

  const auto counters = srv.counters_after_stop();
  const auto rejects = std::count_if(delivered.begin(), delivered.end(), [](const json &j) {
    return j["t"] == "reject" && j["code"] == "UNKNOWN_CLORDID";
  });
  CHECK(counters.rejects == 5000);                               // every one was counted
  CHECK(static_cast<std::uint64_t>(rejects) < counters.rejects); // ...and not every one sent
}

TEST_CASE("server: a policy close whose peer never answers still records 1008", "[server]") {
  // Every other close case drains to the close frame against a live Beast client, which
  // replies — so no case had ever let a closing handshake go UNANSWERED, the one shape in
  // which a timer rather than the peer ends the session. The peer that earns a 1008 is by
  // construction the peer that stopped reading, so this is the shape that matters most:
  // an operator debugging "why was my client dropped" must not read the SHUTDOWN code for
  // a close their client earned.
  //
  // Scope, stated because the name could over-reach: this pins the recorded OUTCOME, not
  // which branch produces it. Measured on this build, the session's read completes
  // operation_aborted rather than beast::error::timeout — Beast's own close op consumes
  // the one-shot timeout first ("Deliver the timeout to the first caller",
  // stream_impl.hpp) — so it is the generic transport branch that records the code here.
  // The timeout branch's own closing_ arm is repaired but unreachable from the wire;
  // docs/PENDING_AMENDMENTS.md item (t)13 carries it with the seam that would falsify it.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":3600000})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.max_session_entries = 3; // test-sized: the deterministic 1008, no socket stall needed
  cfg.idle_timeout_ms = 500;   // test-sized, and well inside the 2 s close grace
  ServerRunner srv{cfg};

  WsClient client{srv.port()};
  client.send_text(new_order(1, 1, "bad-1", 499991, 100));
  REQUIRE(next_verdict(client)["code"] == "TICK_SIZE");
  client.send_text(new_order(2, 1, "bad-2", 499991, 100));
  REQUIRE(next_verdict(client)["code"] == "TICK_SIZE");
  // The third breaches the entry cap: 1008, drained and sent. From here the client reads
  // NOTHING, so its close reply never comes and only a timer can end the session.
  client.send_text(new_order(3, 1, "bad-3", 499991, 100));

  const auto closed = wait_for_line(srv.telemetry_path(), event_named("session_close"));
  REQUIRE(closed.has_value());
  CHECK((*closed)["args"][0] == 1);
  CHECK((*closed)["args"][1] == 1008); // the close's own code, not the reap's 1001

  // WHICH 1008, narrated in its own lane: args0 = epoch, args1 = the entry count at the
  // breach (three rejected orders, three tombstones, the test-sized cap). The close code
  // alone cannot separate this from the report-HWM 1008 — a build that emitted hwm_close
  // here survived the whole suite — so the pair is presence AND absence, and the absence is
  // the half that kills it. Read from the final artifact: absence is decidable only once
  // every byte of the run is on disk.
  const auto entry_cap = wait_for_line(srv.telemetry_path(), event_named("entry_cap_close"));
  REQUIRE(entry_cap.has_value());
  CHECK((*entry_cap)["args"][0] == 1);
  CHECK((*entry_cap)["args"][1] == 3);
  srv.stop();
  const auto lines = telemetry_lines(srv.telemetry_path());
  CHECK(std::none_of(lines.begin(), lines.end(), event_named("hwm_close")));
}

TEST_CASE("server: the M-class watermarks never relax once the queue drains", "[server]") {
  // The half a GAUGE passes. Both marks are declared lifetime maxima that are never reset
  // between snapshots (telemetry.hpp), and turning either std::max into a plain assignment
  // left the whole suite green — because every reader in it looks while the queue is still
  // deep. So this case looks afterwards: build a deep queue behind a stalled socket, record
  // the peak from the live artifact, DRAIN it completely, then keep the session running long
  // enough that shallow enqueues and fast writes would overwrite an assignment, and assert
  // the final counters still carry the peak the run actually reached.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"set":[500005,90,500015,70]})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;     // the post-drain ticks an assignment would report from
  cfg.report_hwm = 100'000; // out of the way: the 1008 is the report-HWM case's subject
  cfg.so_sndbuf = 4096;
  ServerRunner srv{cfg};

  WsClient client{srv.port(), {.rcvbuf = 4096}};
  // Same sizing rationale as the report-HWM case above: ~8 KiB of socket buffers swallow a
  // few hundred reports, so 1000 leaves several hundred with nowhere to go but the queue.
  constexpr std::uint64_t kRejects = 1000;
  for (std::uint64_t i = 1; i <= kRejects; ++i)
    client.send_text(cancel_order(i, 1, "nobody-" + std::to_string(i)));

  // The depth reference, read while the queue is still deep — and the wait for that snapshot
  // IS the stall, because the client reads nothing until it returns. Only the DEPTH is taken
  // from the artifact: under an assignment the mid-session send-lag reading is whatever the
  // last write happened to cost, so pinning the lag half against it would make the kill a
  // coin flip on where the 1 Hz snapshot fell (observed: it can land on a fast write).
  const auto peak = wait_for_line(srv.telemetry_path(), [](const json &j) {
    return j.contains("outbox_depth_hw") && j["outbox_depth_hw"].get<std::uint64_t>() >= 2;
  });
  REQUIRE(peak.has_value());
  const auto depth_peak = (*peak)["outbox_depth_hw"].get<std::uint64_t>();

  // A policy close never sheds on a RUNNING engine, so every counted reject reaches the
  // wire: having read all of them is proof the report queue is EMPTY, not merely shallow —
  // which is what "drained to zero" has to mean for the assertions below to say anything.
  for (std::uint64_t seen = 0; seen < kRejects;)
    if (client.read_json()["t"] == "reject")
      ++seen;
  // ...and now the drained session enqueues one conflated tick at a time and completes its
  // writes against a socket nobody is stalling. THIS window is what an assignment reports.
  for (int tick = 0; tick < 5; ++tick)
    (void)next_matching(client, is_tob, "a post-drain TOB");

  client.close(websocket::close_code::normal);
  const auto counters = srv.counters_after_stop();
  CHECK(counters.outbox_depth_hw >= depth_peak);
  // The lag half is pinned against an ABSOLUTE floor rather than an earlier reading, because
  // that is the only form an assignment cannot satisfy: what it reports at the end is the
  // last post-drain write, and those are microseconds. The floor is measurement-derived —
  // every stalled write in this shape completes in ~101 ms on this host, a non-reading peer's
  // receive window reopening on a ~100 ms cadence rather than staying shut — so it sits at
  // half the observed stall and three orders of magnitude above an unstalled loopback write.
  CHECK(counters.send_lag_max_ns >= 50'000'000);
}
