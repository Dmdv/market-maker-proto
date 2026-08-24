// Task 7 integration — the DURABLE ARTIFACTS a run leaves behind: the bench dump and the
// telemetry JSONL, and the ways they can lie. A bench path that would truncate the
// telemetry file, per-message narration escaping its flag, the dump's peak-session word
// (a watermark, not a gauge), and the shutdown health verdict a dead sink must falsify.
// Split from test_server_flow.cpp under the 500-line cap; fixtures in
// server_flow_support.hpp.
#include "server_flow_support.hpp"

using namespace server_flow;

TEST_CASE("server: a bench dump aimed at the telemetry file is refused before it can truncate it",
          "[server]") {
  // The constructor is the fail-loudly boundary, and this pair had no check: finalize()
  // closes the telemetry JSONL and THEN dumps the recorder over it with ios::trunc, so
  // the run's whole narration is replaced by binary samples while telemetry_ok() — latched
  // from the writer's own clean close — still reports healthy. A config that destroys its
  // own evidence has to die before it can produce any. The property is "names the same
  // file", not "is the same string" — a re-spelling through `./` must also refuse.
  auto feed = FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
  telemetry_test::TempPath telemetry{"clash"};
  mm::Config cfg;
  cfg.port = 0;
  cfg.feed_path = feed.path().string();
  cfg.telemetry_out = telemetry.path().string();
  cfg.bench_out = cfg.telemetry_out;
  CHECK_THROWS_AS((mm::Server{cfg, mm::Instrument{.symbol = "MOCKUSDT"}}), std::invalid_argument);
  const auto parent = std::filesystem::path{cfg.telemetry_out}.parent_path();
  const auto name = std::filesystem::path{cfg.telemetry_out}.filename();
  cfg.bench_out = (parent / "." / name).string(); // same file, different spelling
  CHECK_THROWS_AS((mm::Server{cfg, mm::Instrument{.symbol = "MOCKUSDT"}}), std::invalid_argument);

  // The CASE-VARIANT re-spelling, and it is refused from run(), not from the constructor —
  // which is the whole finding. weakly_canonical is LEXICAL and does not case-fold while the
  // trailing component is missing, so on a case-insensitive volume `TELE.jsonl` and
  // `tele.jsonl` compare UNEQUAL exactly in the regime the constructor runs in ("Neither
  // file exists yet"), and the shipped engine used to exit 0, print telemetry_ok=1 and leave
  // one 40-byte binary file where the JSONL should be. Identity is decidable only once the
  // telemetry file EXISTS, which is true from the writer's emplace in run(); equivalent()
  // compares the inode, so it also catches a hard link or a symlink no spelling comparison
  // could. Guarded on a runtime probe rather than on a platform assumption: where the two
  // spellings really are two files there is no collision to refuse.
  telemetry_test::TempPath probe{"fold"};
  if (filesystem_folds_case(probe.path())) {
    telemetry_test::TempPath variant{"variant"};
    mm::Config variant_cfg;
    variant_cfg.port = 0;
    variant_cfg.feed_path = feed.path().string();
    variant_cfg.telemetry_out = variant.path().string();
    variant_cfg.bench_out = upper_spelling(variant.path()).string();
    // Construction PASSES — that is the point — and run() then refuses before io_.run(),
    // so this call returns rather than blocking. main.cpp turns the throw into one stderr
    // line and exit 2, the same F-12 contract the constructor's arms carry.
    mm::Server srv{variant_cfg, mm::Instrument{.symbol = "MOCKUSDT"}};
    CHECK_THROWS_AS(srv.run(), std::invalid_argument);
  }
}

TEST_CASE("server: per-message events appear only under telemetry_verbose", "[server]") {
  auto make_feed = [] {
    return FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
  };

  auto run_one = [&](bool verbose) {
    auto feed = make_feed();
    mm::Config cfg;
    cfg.feed_path = feed.path().string();
    cfg.feed_interval_ms = 50;
    cfg.loop_feed = true;
    cfg.telemetry_verbose = verbose;
    ServerRunner srv{cfg};
    WsClient client{srv.port()};
    json msg = client.read_json();
    while (msg["t"] != "top_of_book")
      msg = client.read_json();
    client.send_text(new_order(1, 1, "v-1", 499990, 100));
    while (msg["t"] != "order_ack")
      msg = client.read_json();
    if (verbose) {
      // cmd_in: args0 = epoch, args1 = svc_ns; tob_out: args0 = epoch, args1 = md_seq.
      const auto cmd = wait_for_line(srv.telemetry_path(), event_named("cmd_in"));
      REQUIRE(cmd.has_value());
      CHECK((*cmd)["args"][0] == 1);
      CHECK((*cmd)["args"][1].get<std::int64_t>() > 0);
      const auto tob = wait_for_line(srv.telemetry_path(), event_named("tob_out"));
      REQUIRE(tob.has_value());
      CHECK((*tob)["args"][1].get<std::uint64_t>() >= 1);
    }
    client.close(websocket::close_code::normal);
    srv.stop();
    return telemetry_lines(srv.telemetry_path());
  };

  const auto quiet = run_one(false);
  CHECK(std::none_of(quiet.begin(), quiet.end(), event_named("cmd_in")));
  CHECK(std::none_of(quiet.begin(), quiet.end(), event_named("tob_out")));
  (void)run_one(true); // the verbose assertions live inside
}

TEST_CASE("server: bench_out forces telemetry_verbose off at construction", "[server]") {
  // Per-message narration is measurement noise (record A1 / §5.2): main.cpp refuses the
  // combination on the CLI, and Server's constructor forces the flag off for direct
  // embedders. Without that belt a Config that sets both still emits cmd_in/tob_out and
  // contaminates the measured windows; the quiet half of the verbose case above never
  // sets bench_out, so it cannot kill this mutant.
  auto feed = FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":100})"};
  telemetry_test::TempPath bench{"bench"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  cfg.telemetry_verbose = true; // asked for — must be forced OFF under measurement
  cfg.bench_out = bench.path().string();
  ServerRunner srv{cfg};

  WsClient client{srv.port()};
  json msg = client.read_json();
  while (msg["t"] != "top_of_book")
    msg = client.read_json();
  client.send_text(new_order(1, 1, "bench-v", 499990, 100));
  while (msg["t"] != "order_ack")
    msg = client.read_json();
  client.close(websocket::close_code::normal);
  srv.stop();

  const auto lines = telemetry_lines(srv.telemetry_path());
  CHECK(std::none_of(lines.begin(), lines.end(), event_named("cmd_in")));
  CHECK(std::none_of(lines.begin(), lines.end(), event_named("tob_out")));

  // The ARTIFACT, which nothing in the suite had ever opened: this case ran a full server
  // with --bench-out, produced a dump, and asserted only the verbose gate — so `if (false)
  // recorder_->dump(...)`, deleting the dump outright, and making ServerImpl::recorder()
  // return nullptr (killing every md_published/md_written/order_for/svc call site) ALL
  // survived 161/161. The plan makes this file Task 11's sole input, so both halves are
  // pinned here: that the samples were captured at all, and the (t)10 format they land in.
  const BenchDump dump = read_bench_dump(bench.path());
  CHECK(dump.svc >= 1);    // one command serviced
  CHECK(dump.m0_m0p >= 1); // one TOB handed to async_write
  CHECK(dump.m0_m3 >= 1);  // the order echoed a published md_seq
  CHECK(dump.saturated == 0);
  CHECK(dump.peak_sessions == 1);
  // Six header words then the four counted streams and NOTHING ELSE — the arithmetic is what
  // makes "nothing else" falsifiable, and it is the assertion the sixth word moved.
  CHECK(dump.file_size == 8 * 6 + 8 * (dump.svc + dump.m0_m0p + dump.m0p_m3 + dump.m0_m3));
}

TEST_CASE("server: the dump's peak-session word is a watermark, not a live gauge", "[server]") {
  // The sixth header word exists because the recorded m0->m0' is the true venue-production
  // boundary only at ONE session — above it, it is the fan-out MINIMUM (measured 34-79x
  // low) — so a harness reading only the artifact has to be able to refuse the dump. The
  // recorder round-trips whatever the caller passes, which is all its unit file can see: a
  // ServerImpl that read sessions_.size() at stop() instead of the run's high-water mark
  // would write ~0 here and stay green everywhere. Peak at three, then disconnect ALL of
  // them before stopping — that disconnect is the entire distinction between the two.
  constexpr std::uint64_t kPeak = 3; // inside the default max_sessions of 4
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"set":[500005,90,500015,70]})"};
  telemetry_test::TempPath bench{"peak"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  cfg.bench_out = bench.path().string();
  ServerRunner srv{cfg};

  {
    WsClient a{srv.port()};
    WsClient b{srv.port()};
    WsClient c{srv.port()};
    // A TOB apiece, because the watermark is folded at the session map INSERT: a completed
    // client handshake is not yet evidence that three sessions were ever concurrent.
    (void)next_matching(a, is_tob, "a's first TOB");
    (void)next_matching(b, is_tob, "b's first TOB");
    (void)next_matching(c, is_tob, "c's first TOB");
    a.close(websocket::close_code::normal);
    b.close(websocket::close_code::normal);
    c.close(websocket::close_code::normal);
  }
  // Every session must have FOLDED its close before the dump is written: with even one still
  // in the map a gauge and a watermark read alike, and the case would pass either way.
  REQUIRE(wait_for_count(srv.telemetry_path(), event_named("session_close"), kPeak));

  // Then a FOURTH connection, and it is the whole kill. The fold happens at the session-map
  // INSERT, so peaking-then-disconnecting does not by itself separate a watermark from a
  // gauge: the three inserts write 1, 2, 3 either way and nothing writes again while they
  // close. Only a later insert against an EMPTIER map makes the two disagree — a gauge
  // reports this one session, a watermark still reports the three that once overlapped.
  {
    WsClient d{srv.port()};
    (void)next_matching(d, is_tob, "d's first TOB");
    d.close(websocket::close_code::normal);
  }
  REQUIRE(wait_for_count(srv.telemetry_path(), event_named("session_close"), kPeak + 1));
  srv.stop();

  CHECK(read_bench_dump(bench.path()).peak_sessions == kPeak);
}

TEST_CASE("server: a telemetry sink that dies mid-run latches the shutdown verdict false",
          "[server]") {
  // The (s)9 verdict's FALSE arm, and the mutant it kills is the dangerous one: a constant
  // TRUE telemetry_ok() survived the whole suite, so a run whose narration was lost still
  // told its operator — on main.cpp's shutdown line — that the narration was trustworthy.
  // RLIMIT_FSIZE is how cpp/tests/test_telemetry_errors.cpp forces that loss deterministically
  // rather than hoping for a full disk; verbose narration is the cheapest way to cross a
  // small cap, one cmd_in line per command. The feed file is written BEFORE the cap, because
  // the cap is process-wide and does not care whose file it is.
  auto feed = FeedFile{R"({"set":[500000,100,500010,80]})", R"({"halt_ms":3600000})"};
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  cfg.telemetry_verbose = true;
  constexpr rlim_t kCap = 1024;
  {
    const FileSizeCapGuard cap{kCap};
    const IgnoreSigxfszGuard no_sigxfsz;
    HealthRunner srv{cfg};
    {
      WsClient client{srv.port()};
      for (std::uint64_t seq = 1; seq <= 60; ++seq)
        client.send_text(new_order(seq, 1, "cap-" + std::to_string(seq), 499990, 100));
      // The kernel fills the file to the cap exactly and fails the remainder with EFBIG, so
      // reaching the cap is the observable that the crossing write has happened; waiting for
      // it here rather than stopping blind is what keeps the case from passing on a run that
      // simply never wrote enough.
      const auto giveup = std::chrono::steady_clock::now() + std::chrono::seconds{10};
      while (std::filesystem::file_size(srv.telemetry_path()) < kCap &&
             std::chrono::steady_clock::now() < giveup)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
      REQUIRE(std::filesystem::file_size(srv.telemetry_path()) == kCap);
    }
    CHECK_FALSE(srv.telemetry_ok_after_stop());
  }
}

TEST_CASE("server: a bench path swapped mid-run cannot redirect the dump onto telemetry",
          "[server]") {
  // Hard gate HG-2. The bench-vs-telemetry identity guard runs at startup, but the dump used
  // to re-resolve the PATHNAME at shutdown — so the question the guard answered was
  // re-decided at the end of the run, against whatever the name pointed at by then and after
  // the writer had already reported success. Replacing the bench path mid-run with a symlink
  // to the telemetry file redirected a truncating write onto the run's whole narration. The
  // engine now binds a DESCRIPTOR at startup and empties and writes through that.
  //
  // The bound file is RENAMED aside before the symlink takes its place (HG2-3): unlinking it
  // would leave the descriptor's inode with no name to read the dump back through, and the
  // case would then only be able to re-assert that telemetry survived — which the swap alone
  // cannot disprove. With the alias, both halves are observable: telemetry intact, AND a
  // complete well-formed dump in the file the engine actually bound.
  FeedFile feed{R"({"set":[500000,100,500010,80]})", R"({"set":[500005,90,500015,70]})"};
  telemetry_test::TempPath bench{"swap"};
  // RAII, per the suite's destructor-cleanup rule (telemetry_test_support.hpp): the alias
  // must go away on EVERY exit, and this case has REQUIREs after it that unwind past any
  // trailing cleanup. A leaked artifact from a failing run is a second failure to diagnose.
  struct AliasPath {
    explicit AliasPath(std::filesystem::path p) : path(std::move(p)) {}
    ~AliasPath() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
    AliasPath(const AliasPath &) = delete;
    AliasPath &operator=(const AliasPath &) = delete;
    std::filesystem::path path;
  } alias_guard{bench.path().string() + ".bound"};
  const std::filesystem::path &alias = alias_guard.path;
  mm::Config cfg;
  cfg.feed_path = feed.path().string();
  cfg.feed_interval_ms = 50;
  cfg.loop_feed = true;
  cfg.bench_out = bench.path().string();
  ServerRunner srv{cfg};
  {
    WsClient client{srv.port()};
    (void)next_matching(client, is_tob, "a TOB, so the run has samples to dump");
    std::error_code ec;
    std::filesystem::rename(bench.path(), alias, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_symlink(srv.telemetry_path(), bench.path(), ec);
    if (ec) {
      WARN("symlink unavailable on this filesystem — swap arm skipped");
      std::error_code restore_ec;
      std::filesystem::rename(alias, bench.path(), restore_ec);
      CHECK_FALSE(restore_ec); // a silent failure here would hide the skip behind a pass
      return;
    }
    client.close(websocket::close_code::normal);
  }
  srv.stop();

  // (1) The telemetry artifact survived intact. telemetry_lines FAILS the case on any
  // unparseable line, so a binary header written over it cannot pass here quietly.
  const auto lines = telemetry_lines(srv.telemetry_path());
  CHECK(!lines.empty());
  CHECK(std::any_of(lines.begin(), lines.end(), event_named("session_close")));

  // (2) ...and the dump landed in the file the engine BOUND, not the one the name now
  // resolves to. Read through the alias and check the format, not merely the size: a
  // well-formed six-word header whose counts account for the whole file is what proves the
  // write went somewhere complete rather than somewhere plausible.
  REQUIRE(std::filesystem::exists(alias));
  std::ifstream in{alias, std::ios::binary};
  REQUIRE(in.is_open());
  std::uint64_t header[6]{};
  in.read(reinterpret_cast<char *>(header), sizeof header);
  REQUIRE(in.gcount() == static_cast<std::streamsize>(sizeof header));
  const std::uint64_t samples = header[0] + header[1] + header[2] + header[3];
  CHECK(header[1] >= 1); // at least the TOB this case waited for
  CHECK(header[5] == 1); // one session peaked
  CHECK(std::filesystem::file_size(alias) == sizeof header + samples * sizeof(std::int64_t));

  // The symlink at the original name is not TempPath's to remove (it replaced the file
  // TempPath created), so it is cleaned explicitly; the alias is the guard's.
  std::error_code cleanup;
  std::filesystem::remove(bench.path(), cleanup);
}
