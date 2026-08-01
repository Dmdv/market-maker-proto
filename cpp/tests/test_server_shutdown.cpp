// integration — process SHUTDOWN: the signal disposition handed back on the way
// out, stop() refusing to be pinned by a peer (never-upgraded, or parked mid-frame), the
// 1001 fan-out and the final snapshot that must agree with the counters, and the feed's
// own two terminal events. Split from test_server_flow.cpp under the 500-line cap; the
// shared fixtures are in server_flow_support.hpp.
#include "server_flow_support.hpp"

using namespace server_flow;

TEST_CASE("server: shutdown hands SIGINT/SIGTERM back to their default disposition", "[server]") {
  // begin_stop cancelled the signal wait, and asio's cancel does NOT deregister: the
  // process-wide handler stayed installed with no waiter behind it, so a second Ctrl-C
  // during a shutdown bounded by the stream idle timeout was caught and discarded and
  // SIGKILL was the operator's only escalation. The DISPOSITION is what this case can
  // read — the behavior it decides (that second signal killing this process) cannot be
  // asserted from inside the process it would kill.
  //
  // Asio's deregistration restores SIG_DFL, not the handler it displaced (Catch2's own
  // fatal-condition handler here), so this case has to put back what it borrows — and the
  // guard is what makes that true on EVERY exit path, the REQUIREs below included. A restore
  // reachable only by falling off the end changes the disposition process-wide for every
  // case that follows the first time one of them fails.
  const SigactionGuard sigint{SIGINT};
  auto feed = FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;      // so feed exhaustion cannot stop the engine ahead of stop()
  cfg.handle_signals = true; // only the mm_engine binary claims them; this case borrows it
  {
    HealthRunner srv{cfg};
    struct sigaction claimed{};
    REQUIRE(::sigaction(SIGINT, nullptr, &claimed) == 0);
    // the engine really took the signal
    REQUIRE(claimed.sa_handler != sigint.saved().sa_handler);
    srv.stop(); // joins: begin_stop has finished
    struct sigaction after{};
    REQUIRE(::sigaction(SIGINT, nullptr, &after) == 0);
    CHECK(after.sa_handler == SIG_DFL);
    // The (s)9 verdict's TRUE arm, and this is the ONE case in the suite where all four of
    // the latch's falsifiers are ARMED and none of them fires: the writer closed cleanly,
    // the promised final snapshot published, no handler unwound out of io_.run(), and the
    // dispositions asserted above really were handed back. The fourth is only reachable
    // with handle_signals set, which is why the assertion lives here rather than in the
    // clean-shutdown case below — it also catches a build that latched the run unhealthy
    // whenever it restored a signal, not only when it failed to.
    CHECK(srv.telemetry_ok_after_stop());
  }
}

TEST_CASE("server: stop() does not wait on a connection that never sent its upgrade", "[server]") {
  // A socket accepted but not yet upgraded is invisible to the session map, so the
  // graceful path has nothing to close it with — and its ten-second pre-upgrade read
  // timeout would then be the shutdown's real duration. The bound asserted here is a
  // small multiple of a prompt stop and a small fraction of that timeout, so it can only
  // fail on the difference between the two.
  auto feed = FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  ServerRunner srv{cfg};

  WsClient silent{srv.port(), {.handshake = false}}; // TCP only: no request will ever come
  // connect() returns as soon as the kernel completes the handshake into the LISTEN
  // backlog, which can be before the engine has accepted the socket at all — and a
  // connection still in the backlog is not an in-flight upgrade, so without this settle
  // the case measures a shutdown with nothing to shut down. There is no wire-visible
  // event for "accepted": the accept loop is unconditional and re-arms per connection,
  // so this is orders of magnitude more than it needs on an idle loopback listener.
  std::this_thread::sleep_for(std::chrono::milliseconds{250});
  const auto began = std::chrono::steady_clock::now();
  srv.stop();
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds{3});
}

TEST_CASE("server: stop() is not pinned by a peer that parks an oversize frame header",
          "[server]") {
  // A peer that sends only the frame header announcing > transport ceiling and holds
  // the socket parks Beast's read op on wr_block. The graceful 1001 close write cannot
  // progress, and without a shutdown-scoped deadline stop() waited for the stream idle
  // timeout (measured 27 s against the 30 s default). The bound here is a small multiple
  // of kCloseGrace (2 s) and a small fraction of the idle timeout below, so it fails on
  // the difference between the two.
  auto feed = FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  cfg.idle_timeout_ms = 15'000; // well above the close-grace bound; the mutant waits here
  ServerRunner srv{cfg};

  WsClient client{srv.port()};
  (void)client.read_json(); // first TOB: session is up and reading
  // Masked text frame header announcing 200000 bytes (> 128 KiB transport ceiling), no
  // payload: Beast parks mid-reassembly until the peer sends more or the idle timer ends.
  const std::array<unsigned char, 14> oversize_header = {
      0x81, 0xFF,                                     // FIN+text, masked, 64-bit length
      0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0D, 0x40, // 200000
      0x11, 0x22, 0x33, 0x44                          // mask key
  };
  boost::asio::write(beast::get_lowest_layer(client.stream()).socket(),
                     boost::asio::buffer(oversize_header));
  // Beast must process the header and enter its teardown park before stop() runs:
  // a stop that races ahead of the park takes the ordinary close-grace path (~2 s)
  // and does not exercise the shutdown deadline. The park leaves no session_close
  // (and no 1009 upcall) until the socket is severed — so "still open after a
  // short settle" is the precondition, not a wire event.
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  REQUIRE_FALSE(wait_for_line(srv.telemetry_path(), event_named("session_close"),
                              std::chrono::milliseconds{50})
                    .has_value()); // still parked, not already reaped
  const auto began = std::chrono::steady_clock::now();
  srv.stop();
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds{5});

  // The stall the teardown fold exists for, and the only place in the suite it is visible.
  // Every other write in this suite COMPLETES, so its lag is sampled by on_write; here the
  // graceful close cannot progress against the parked read, so the write still in flight at
  // teardown is the largest send lag the suite ever produces (measured 2.43 s — 24x the next
  // biggest, and floored by the 2 s shutdown grace above). on_write's torn_down_ guard
  // discards exactly that sample, which is what degrades a documented lifetime maximum into
  // an instantaneous gauge; folding it in Session::teardown, before session_closed() runs
  // finalize(), is what puts it somewhere anyone exports.
  const auto counters = srv.counters_after_stop();
  CHECK(counters.send_lag_max_ns > 1'000'000'000);
}

TEST_CASE("server: stop() closes 1001 and the final snapshot matches the counters", "[server]") {
  // One resting book, then a cross (one deterministic fill), then heartbeats.
  FeedFile feed{R"({"set":[500000,100,500010,80]})",  R"({"halt_ms":600})",
                R"({"set":[499990,100,500000,120]})", R"({"halt_ms":200})",
                R"({"set":[499990,100,500005,120]})", R"({"halt_ms":200})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  ServerRunner srv{cfg};

  WsClient client{srv.port()};
  // Wait for the RESTING book before quoting at its bid: connecting mid-cycle could
  // otherwise place the bid against the crossed book and draw POST_ONLY_CROSS.
  json tob;
  for (;;) {
    tob = client.read_json();
    if (tob["t"] == "top_of_book" && tob["ask_px"] == 500010)
      break;
  }
  client.send_text(new_order(1, 1, "will-fill", 500000, 100, tob["md_seq"].get<std::uint64_t>()));
  for (bool filled = false; !filled;) {
    const json msg = client.read_json();
    REQUIRE(msg["t"] != "reject");
    filled = msg["t"] == "fill";
  }

  // Initiate stop from another thread mid-session, WITHOUT joining: this thread is also
  // the client's io thread, and the graceful 1001 needs a live peer in the closing
  // handshake (the ServerRunner::initiate_stop note has the measured failure mode).
  srv.initiate_stop();
  CHECK(client.read_until_close() == 1001);

  const auto counters = srv.counters_after_stop();
  CHECK(counters.orders == 1);
  CHECK(counters.fills == 1);

  const auto lines = telemetry_lines(srv.telemetry_path());
  // session_open / session_close lane pins (header event-lane table).
  const auto open_line = std::find_if(lines.begin(), lines.end(), event_named("session_open"));
  REQUIRE(open_line != lines.end());
  CHECK((*open_line)["args"][0] == 1);
  const auto close_line = std::find_if(lines.begin(), lines.end(), event_named("session_close"));
  REQUIRE(close_line != lines.end());
  CHECK((*close_line)["args"][0] == 1);
  CHECK((*close_line)["args"][1] == 1001);

  // The run()-start baseline snapshot: the FIRST totals line is all zeroes, so even an
  // instant shutdown ships one; and every totals line reports telemetry_dropped.
  json first_totals;
  for (const auto &j : lines)
    if (j.contains("orders")) {
      first_totals = j;
      break;
    }
  REQUIRE(!first_totals.is_null());
  CHECK(first_totals["orders"] == 0);
  CHECK(first_totals["fills"] == 0);
  CHECK(first_totals.contains("telemetry_dropped"));

  // The LAST snapshot is the deterministic stop() push — it matches the shutdown-line
  // counters exactly, not up-to-a-second stale ((s)10).
  const json final_totals = last_totals(lines);
  REQUIRE(!final_totals.is_null());
  CHECK(final_totals["orders"] == counters.orders);
  CHECK(final_totals["fills"] == counters.fills);
  CHECK(final_totals["sessions"] == 0); // pushed after every session folded its close

  // The watermarks line carries the engine's stale-book drop counter (a backlog item
  // lane 2 seated; the feed is monotonic by construction, so it reads zero).
  const auto watermarks = std::find_if(
      lines.begin(), lines.end(), [](const json &j) { return j.contains("stale_books_ignored"); });
  REQUIRE(watermarks != lines.end());
  CHECK((*watermarks)["stale_books_ignored"] == 0);
}

TEST_CASE("server: a feed drop closes every session 1001 and the engine keeps running",
          "[server]") {
  // plan L777 and the normative close-code table: FeedDrop closes ALL sessions 1001 going
  // away, the engine survives it, and later connects get FRESH epochs. Nothing asserted any
  // of that — a no-op drop, a drop whose predicate is forced false, and a drop replaced by
  // begin_stop() all passed the whole suite — while bench/scenarios/demo.feed walks the path
  // on every Step-5 smoke run, so under the begin_stop() mutant the demo silently ENDS here.
  //
  // The opening halt is what makes the case deterministic rather than a race: the first book
  // must publish after both clients are attached (publish_book returns early on an empty
  // session map), and the trailing run of books gives the reconnect a window that does not
  // depend on how fast a sanitizer build completes two closing handshakes.
  FeedFile feed{R"({"halt_ms":600})",
                R"({"set":[500000,100,500010,80]})",
                R"({"halt_ms":500})",
                R"({"drop":true})",
                R"({"halt_ms":400})",
                R"({"set":[499990,100,500005,120]})",
                R"({"set":[499990,100,500005,120]})",
                R"({"set":[499990,100,500005,120]})",
                R"({"set":[499990,100,500005,120]})",
                R"({"set":[499990,100,500005,120]})",
                R"({"set":[499990,100,500005,120]})",
                R"({"halt_ms":3600000})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 100;
  cfg.loop_feed = false; // the drop is reached exactly once
  ServerRunner srv{cfg};

  WsClient a{srv.port()};
  WsClient b{srv.port()};
  (void)next_matching(a, is_tob, "a's first TOB");
  (void)next_matching(b, is_tob, "b's first TOB");
  // BOTH, not either: a one-client case cannot tell a fan-out from a branch that closes only
  // the first session it walks.
  CHECK(a.read_until_close() == 1001);
  CHECK(b.read_until_close() == 1001);

  // ...and the engine is still minting epochs. This is what separates a drop from the
  // begin_stop() mutant, which also delivers two 1001s and then serves nobody: the third
  // connection is admitted, gets the NEXT epoch rather than a reused one, and is still being
  // fed books from a driver that never stopped.
  WsClient c{srv.port()};
  CHECK(next_matching(c, is_tob, "c's first TOB")["epoch"].get<std::uint64_t>() == 3);
}

TEST_CASE("server: a feed end closes every session 1001 on its way to a graceful stop",
          "[server]") {
  // The other arm of the same normative row. `end` is the script's terminator — the loader
  // refuses content after it — so it gets its own scenario, and today it is reachable only
  // through demo.feed's trailing {"end":true}, with nothing asserting behind it either.
  FeedFile feed{R"({"halt_ms":600})", R"({"set":[500000,100,500010,80]})", R"({"halt_ms":300})",
                R"({"end":true})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 100;
  cfg.loop_feed = false;
  ServerRunner srv{cfg};

  WsClient a{srv.port()};
  WsClient b{srv.port()};
  (void)next_matching(a, is_tob, "a's first TOB");
  (void)next_matching(b, is_tob, "b's first TOB");
  CHECK(a.read_until_close() == 1001);
  CHECK(b.read_until_close() == 1001);
}
