// The engine process's composition root (plan Task 7): the process lifecycle, the feed
// driver and the telemetry pump — everything on ONE owner thread (record D6). The
// acceptor/handshake half lives in cpp/src/server_accept.cpp, the per-connection state
// machine in cpp/src/session.cpp, the shared declarations in cpp/src/server_impl.hpp, the
// public surface in cpp/include/mm/server.hpp.
#include "server_impl.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// POSIX, in its own group: the bench artifact is held as a DESCRIPTOR for the run so its
// identity is decided between open objects rather than between pathnames (hard gate HG2-2),
// and no standard-library file type exposes one. Both supported platforms — the macOS dev
// host and the ubuntu:26.04 container that is the authoritative toolchain — are POSIX.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mm::server_detail {

namespace {

// Same-file FAST PATH for the two startup paths that must not collide. String equality
// first; weakly_canonical then catches re-spellings (`a/./b` vs `a/b`) that would open the
// same inode. Neither file exists at construction, so weakly_canonical is the only
// primitive available here — and that is also its limit: it is purely LEXICAL, so a
// case-variant re-spelling on a case-insensitive volume passes it. Identity is decided
// once the telemetry file exists, by the std::filesystem::equivalent check in run(); this
// one earns its keep by failing before any work is done. A filesystem_error degrades to
// the string verdict rather than failing construction for an unrelated reason.
[[nodiscard]] bool names_same_file(const std::string &a, const std::string &b) {
  if (a == b)
    return true;
  try {
    return std::filesystem::weakly_canonical(std::filesystem::path{a}) ==
           std::filesystem::weakly_canonical(std::filesystem::path{b});
  } catch (const std::filesystem::filesystem_error &) {
    return false;
  }
}

} // namespace

std::int64_t ServerImpl::steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

ServerImpl::ServerImpl(Config cfg, Instrument inst)
    : cfg_(std::move(cfg)), inst_(std::move(inst)), acceptor_(io_), codec_(make_codec(cfg_.codec)),
      engine_(inst_, cfg_.max_session_entries), feed_timer_(io_), snapshot_timer_(io_),
      accept_backoff_(io_), shutdown_deadline_(io_) {
  // Startup validation is the fail-loudly boundary (F-12): every nonsensical
  // configuration dies HERE with a throw main.cpp turns into stderr + exit 2, because
  // the same value failing later would fail inside an asio handler and terminate.
  if (cfg_.report_hwm == 0)
    throw std::invalid_argument("Config: report_hwm must be > 0");
  if (cfg_.feed_interval_ms < 0)
    throw std::invalid_argument("Config: feed_interval_ms must be >= 0");
  if (cfg_.idle_timeout_ms <= 0)
    throw std::invalid_argument("Config: idle_timeout_ms must be > 0");
  if (cfg_.upgrade_timeout_ms <= 0)
    throw std::invalid_argument("Config: upgrade_timeout_ms must be > 0");
  if (cfg_.so_sndbuf < 0)
    throw std::invalid_argument("Config: so_sndbuf must be >= 0");
  if (cfg_.telemetry_out.empty())
    throw std::invalid_argument("Config: telemetry_out must name a path");
  // Two writers, two formats, one path: finalize() closes the telemetry JSONL and THEN
  // dumps the recorder over it with std::ios::trunc, destroying the run's whole narration
  // while telemetry_ok() — latched from the writer's own clean close — still reads true.
  if (!cfg_.bench_out.empty() && names_same_file(cfg_.bench_out, cfg_.telemetry_out))
    throw std::invalid_argument("Config: bench_out must not name telemetry_out (the binary "
                                "sample dump would truncate the telemetry file)");
  if (cfg_.max_sessions == 0)
    throw std::invalid_argument("Config: max_sessions must be > 0");
  // The listener's interface, resolved HERE with the rest of the validation rather than at
  // the bind below: a spelling asio cannot parse is a startup failure like any other and
  // owes the same std::invalid_argument, where make_address's throwing overload would
  // surface a bare boost system_error main.cpp could only print verbatim.
  beast::error_code addr_ec;
  const boost::asio::ip::address bind_address =
      boost::asio::ip::make_address(cfg_.bind_address, addr_ec);
  if (addr_ec)
    throw std::invalid_argument("Config: bind_address is not an IP address: " + cfg_.bind_address);
  // Per-message narration is forced OFF under measurement (record A1 / §5.2): main.cpp
  // already refuses the combination, and this belt keeps the rule true for direct
  // Server embedders (the tests, Task 11's harness).
  if (!cfg_.bench_out.empty())
    cfg_.telemetry_verbose = false;
  if (!cfg_.bench_out.empty())
    recorder_.emplace();

  // The signals are claimed BEFORE the slow startup work below (feed load, bind), not in
  // run(): between construction and run() lies file IO an operator can outwait, and a
  // SIGINT arriving in that window would otherwise take the INHERITED disposition and
  // kill the process where the graceful path was asked for. async_wait only registers —
  // the completion still runs on the owner thread, inside run(), so a signal caught here
  // becomes the first thing run() does.
  if (cfg_.handle_signals) {
    signals_.emplace(io_, SIGINT, SIGTERM);
    signals_->async_wait([this](beast::error_code ec, int) {
      if (!ec)
        begin_stop();
    });
  }
  feed_ = load_feed(cfg_.feed_path);

  // Bound to Config::bind_address (loopback unless an operator said otherwise), not to
  // tcp::v4()'s any-address: an engine that authenticates nothing must not be reachable
  // from every interface by default. Taking the protocol from the endpoint is what lets a
  // v6 spelling work without a second code path.
  const tcp::endpoint endpoint{bind_address, cfg_.port};
  acceptor_.open(endpoint.protocol());
  // SO_REUSEADDR: an engine restart must not lose its port to TIME_WAIT remnants of its
  // own previous sessions (ops failure paths, F-12).
  acceptor_.set_option(boost::asio::socket_base::reuse_address{true});
  acceptor_.bind(endpoint);
  acceptor_.listen();
  bound_port_ = acceptor_.local_endpoint().port();
}

ServerImpl::~ServerImpl() {
  if (bench_fd_ >= 0)
    ::close(bench_fd_);
}

void ServerImpl::run() {
  // Writer first (its constructor emits the run's open marker as line one), then the
  // zeroed baseline snapshot ((s)10): even an instant shutdown ships one snapshot.
  writer_.emplace(ring_, std::filesystem::path{cfg_.telemetry_out});
  // The telemetry inode, captured the INSTANT the writer has it open. Every later identity
  // question is asked against this, not against whatever cfg_.telemetry_out resolves to by
  // then: the name can be re-pointed after the writer holds its stream, and a check that
  // re-resolved it would compare the bench descriptor against a file the writer is not
  // writing. This is as close to the writer's own descriptor as the engine can get —
  // TelemetryWriter holds a std::ofstream, which exposes none. The residual window is NOT
  // negligible and is not described as such: between that open and this stat the writer's
  // constructor imbues a locale, WRITES AND FLUSHES the open marker, checks the stream, and
  // spawns its thread — file IO and a thread creation, either of which can block. Closing it
  // properly needs a descriptor-based sink in TelemetryWriter; recorded as (u.2).
  struct ::stat telemetry_st{};
  const bool telemetry_identity_known = ::stat(cfg_.telemetry_out.c_str(), &telemetry_st) == 0;
  // The constructor's names_same_file() guard is LEXICAL and runs while neither file
  // exists — the regime where weakly_canonical does NOT case-fold, so on a case-insensitive
  // volume `TELE.jsonl` and `tele.jsonl` compare unequal there and the guard is blind
  // exactly where it is armed (measured: the run then exits 0, prints telemetry_ok=1, and
  // leaves one 40-byte binary file where the JSONL should be). From THIS line the telemetry
  // file exists, so identity is decidable rather than guessable: equivalent() compares the
  // real inode — which case-folds, and also catches a hard link or a symlink that no
  // spelling comparison ever could. The error_code overload returns false when bench_out
  // does not exist, which is the pass case. Reset the writer first so the artifact just
  // opened closes cleanly instead of being left as an unterminated open marker; main.cpp
  // turns the throw into one stderr line + exit 2 (F-12).
  std::error_code fs_ec;
  if (!cfg_.bench_out.empty() &&
      std::filesystem::equivalent(cfg_.bench_out, cfg_.telemetry_out, fs_ec)) {
    writer_.reset();
    throw std::invalid_argument("Config: bench_out must not name telemetry_out (the binary "
                                "sample dump would truncate the telemetry file)");
  }
  // Open the bench destination NOW and keep the handle for the whole run (hard gate HG-2).
  // The identity check above answers "is this the telemetry file?" about a PATHNAME; the
  // dump happens at shutdown, so re-resolving the name there would re-answer it against
  // whatever the name points at by then — a symlink or hard link swapped in mid-run
  // redirects a truncating open onto the telemetry artifact, after the writer has already
  // reported success. Binding the descriptor here makes the identity that was checked the
  // identity that gets written. Failing fast is the second benefit: an unwritable bench
  // path is now a startup error (F-12's one stderr line + exit 2) instead of a surprise
  // after the measured run is already over and its samples unrecoverable.
  if (!cfg_.bench_out.empty()) {
    // O_CREAT WITHOUT O_TRUNC, deliberately. Truncating here would destroy a pre-existing
    // bench artifact at STARTUP, before the run that is supposed to replace it has produced
    // a single sample — and server.hpp documents this output as written on shutdown. The
    // file is emptied at dump time instead, through this same descriptor.
    bench_fd_ = ::open(cfg_.bench_out.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (bench_fd_ < 0) {
      writer_.reset();
      throw std::invalid_argument("Config: cannot open bench_out for writing: " + cfg_.bench_out);
    }
    // Identity is now decided between OPEN OBJECTS, not between pathnames. The
    // equivalent() check above compares names, and a name can be re-pointed in the window
    // between checking it and opening it — so a symlink or hard link swapped into that
    // interval would leave a descriptor bound to the telemetry inode that every later check
    // considers already cleared. fstat answers the question about the thing actually opened,
    // which is the thing that will be written.
    // FAIL CLOSED. An identity question that cannot be answered is not an answer of "no":
    // the previous shape treated a failed stat as a pass, so the one case where the engine
    // could not tell the two files apart was also the case where it proceeded to truncate.
    struct ::stat bench_st{};
    const bool bench_identity_known = ::fstat(bench_fd_, &bench_st) == 0;
    if (!bench_identity_known || !telemetry_identity_known ||
        (bench_st.st_dev == telemetry_st.st_dev && bench_st.st_ino == telemetry_st.st_ino)) {
      ::close(bench_fd_);
      bench_fd_ = -1;
      writer_.reset();
      throw std::invalid_argument(
          bench_identity_known && telemetry_identity_known
              ? "Config: bench_out resolves to the same file as telemetry_out (the binary "
                "sample dump would truncate the telemetry file)"
              : "Config: cannot establish that bench_out and telemetry_out are different "
                "files; refusing to run rather than risk truncating the telemetry artifact");
    }
  }
  push_snapshot();
  schedule_snapshot();
  schedule_feed(std::chrono::milliseconds{0});
  accept_next();
  // An exception escaping a handler unwinds straight OUT of io_.run(), past every path
  // that reaches finalize(): the run would lose its final snapshot, its close marker and
  // the bench dump, and the writer thread would be joined only by ~ServerImpl. finalize()
  // allocates nothing before the writer is drained, so a std::bad_alloc reaches the same
  // file state as any other failure. The rethrow is main.cpp's exit-2 path — a crashed
  // engine must not report success.
  try {
    io_.run();
  } catch (...) {
    stopping_ = true;
    // Say, IN THE ARTIFACT, that this run ABORTED. The fail-safe above already gets the
    // final snapshot and the close marker to disk, which is exactly the problem: without
    // this line the file is shaped line-for-line like a graceful stop and telemetry_ok()
    // reads true, so the durable record affirms health for a crashed engine while the only
    // truth — main.cpp's stderr line and exit 2 — lives on a channel no post-mortem reader
    // or harness consumes. ORDERING CONSTRAINT: the event must be pushed BEFORE finalize(),
    // which stops and joins the writer; pushed after, it would never reach the file. The
    // health latch is the same statement in the other channel, so the read cannot
    // contradict the exit code the rethrow produces (finalize() ANDs, so it survives).
    //
    // MANDATORY, not fire-and-forget (hard gate HG-1). Every other event ships through
    // emit_event's bare try_push, where a drop is one narration lost out of many and the
    // counter still carries the volume. This record is the ONLY thing that distinguishes a
    // crashed run's artifact from a clean one, and on THIS path stdout never gets a second
    // chance: main.cpp prints telemetry_ok only after run() returns, so a rethrow skips it
    // and leaves stderr — which no post-mortem reader or harness consumes. A ring saturated
    // at the instant of the fault would therefore erase the abort itself, indistinguishably
    // from ordinary telemetry loss. publish_record waits for a free slot on the same bounded
    // deadline the final snapshot uses.
    // LATCHED, not merely attempted (hard gate HG2-1). publish_record's bounded wait is the
    // first try; if the ring is still full when it expires, the record is held here and
    // finalize() re-offers it after the writer's own drain has made room. Degrading to a
    // stderr line instead would answer a finding about the ARTIFACT with a message on the
    // channel main.cpp already writes — the durable record would still imply a graceful stop
    // by omission, which is the whole defect. stderr stays as the last resort for a sink
    // that never recovers at all.
    pending_fault_ = event_record(TelemetryEvent::EngineFault, steady_ns(), 0, 0);
    if (publish_record(*pending_fault_))
      pending_fault_.reset();
    telemetry_ok_ = false;
    finalize();
    throw;
  }
}

void ServerImpl::stop() {
  boost::asio::post(io_, [this] { begin_stop(); });
}

void ServerImpl::begin_stop() {
  if (stopping_)
    return;
  stopping_ = true;
  beast::error_code ec;
  acceptor_.close(ec); // NOLINT(bugprone-unused-return-value) — best-effort teardown
  feed_timer_.cancel();
  snapshot_timer_.cancel();
  accept_backoff_.cancel();
  if (signals_) {
    signals_->cancel(ec); // completes the pending wait; leaves the handler INSTALLED
    // ...which is why clear() follows: asio's cancel does not deregister, so a second
    // Ctrl-C during a shutdown bounded by the stream idle timeout would be caught and
    // discarded with no waiter, leaving SIGKILL as the operator's only escalation.
    // Handing the signals back to their default disposition makes the second one kill —
    // the conventional behavior, and the mirror of claiming them before the slow startup.
    signals_->clear(ec);
    if (ec) {
      // The paragraph above is an OPERATIONAL GUARANTEE — the escalation an operator
      // reaches for when a shutdown outlasts their patience — so its failure cannot be the
      // one verdict here nobody reads. Name what actually holds now, and latch the run
      // unhealthy so the shutdown line cannot report a clean stop while that path is gone.
      std::fprintf(stderr,
                   "mm: could not restore the default SIGINT/SIGTERM disposition (%s): a "
                   "second termination signal will still be caught and discarded — SIGKILL "
                   "is the escalation\n",
                   ec.message().c_str());
      telemetry_ok_ = false;
    }
  }
  abort_pending_upgrades(); // the acceptor half's half of shutdown
  if (sessions_.empty()) {
    finalize();
    return;
  }
  // Graceful path: every session closes 1001 going_away; the LAST teardown's
  // session_closed call runs finalize(). Iterate over a copy — begin_close never erases
  // synchronously, but the shutdown path must not depend on that staying true.
  std::vector<std::shared_ptr<Session>> open;
  open.reserve(sessions_.size());
  for (const auto &[epoch, session] : sessions_)
    open.push_back(session);
  for (const auto &session : open)
    session->begin_close(websocket::close_code::going_away);
  // Bound shutdown independently of each stream's idle timeout. A peer that sends an
  // oversize frame header and holds the socket parks Beast's read op on wr_block; the
  // graceful close write cannot progress, and without this deadline stop() would wait
  // for the idle timeout (the only Beast timer still armed on that path). Same grace as
  // the per-session unanswered-handshake bound — long enough for a live peer, short
  // enough that one mute peer cannot pin the process.
  shutdown_deadline_.expires_after(kCloseGrace);
  shutdown_deadline_.async_wait([this](beast::error_code ec) {
    if (ec || finalized_)
      return;
    std::vector<std::shared_ptr<Session>> still_open;
    still_open.reserve(sessions_.size());
    for (const auto &[epoch, session] : sessions_)
      still_open.push_back(session);
    for (const auto &session : still_open)
      beast::get_lowest_layer(session->stream()).close();
  });
}

void ServerImpl::finalize() {
  if (finalized_)
    return;
  finalized_ = true;
  shutdown_deadline_.cancel();
  // The deterministic final snapshot ((s)10): every session has folded its close by now,
  // the acceptor and the in-flight upgrades are retired, and no timer can re-arm — so
  // nothing pushes after this pair, and the writer's shutdown drain owes exactly it.
  const bool final_snapshot_ok = push_final_snapshot();
  // The shutdown health read ((s)9). stop_and_join FIRST: the writer's final drain and
  // its close marker are where a dying sink most often surfaces, and a latch sampled
  // ahead of them would report healthy on precisely the run whose last records were
  // lost. The writer exists on every path that reaches finalize (run() emplaces it
  // before the io_context turns); the guard keeps that true under future code motion
  // instead of assuming it.
  bool writer_ok = true;
  if (writer_) {
    writer_->stop_and_join();
    writer_ok = !writer_->output_failed();
    writer_.reset();
  }
  // ANDed with the running value, never assigned over it: telemetry_ok_ is a one-way LATCH
  // (mm/server.hpp states the contract). A failure recorded earlier in the run — a shutdown
  // that could not restore the signal dispositions, a run that unwound out of io_.run() —
  // must not be erased by a writer that then closed cleanly. It starts true, so a run with
  // nothing to latch reads exactly as before.
  telemetry_ok_ = final_snapshot_ok && writer_ok && telemetry_ok_;
  if (recorder_ && !cfg_.bench_out.empty()) {
    // The dump header carries saturated() so a harness that reads only the artifact can
    // see a truncated run (BenchRecorder::dump). stderr is the ops-visible line for a
    // human watching the process — not the harness's channel.
    if (const std::uint64_t refused = recorder_->saturated(); refused > 0)
      std::fprintf(stderr,
                   "mm: bench recorder refused %llu sample(s) at capacity: the dumped "
                   "streams are TRUNCATED\n",
                   static_cast<unsigned long long>(refused));
    try {
      // The peak concurrent session count is a DUMP HEADER field, not a diagnostic: the
      // recorded m0->m0' is the true venue-production window only at one session, and above
      // it a reader could not tell a fan-out minimum from a correct dump.
      // Emptied and written through the descriptor bound at startup — never by reopening
      // the pathname, which is what a swap could redirect. This is also where the file is
      // first truncated: startup only CREATED it.
      const std::vector<char> bytes = recorder_->serialize(peak_sessions());
      // ftruncate is interruptible on the same terms as the write below — same shutdown
      // path, same signals — so it is retried on EINTR rather than losing a complete run.
      int trunc_rc = 0;
      do {
        trunc_rc = ::ftruncate(bench_fd_, 0);
      } while (trunc_rc != 0 && errno == EINTR);
      if (trunc_rc != 0 || ::lseek(bench_fd_, 0, SEEK_SET) != 0)
        throw std::runtime_error("bench dump: cannot rewind the bound output");
      // EINTR is RETRIED, not fatal: this engine installs SIGINT/SIGTERM handlers and the
      // dump runs on the shutdown path, so a second Ctrl-C landing mid-write is an ordinary
      // event here — treating it as a failure would discard a complete measured run for a
      // signal that changed nothing. Any other error, and a zero-byte write, still throw:
      // a partial artifact must not be mistaken for a whole one.
      std::size_t written = 0;
      while (written < bytes.size()) {
        const ::ssize_t n = ::write(bench_fd_, bytes.data() + written, bytes.size() - written);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          throw std::runtime_error("bench dump: short write to the bound output");
        written += static_cast<std::size_t>(n);
      }
    } catch (const std::exception &e) {
      // A lost benchmark dump must not abort an otherwise clean shutdown; a lost dump is
      // loud anyway — the harness fails on the missing file.
      std::fprintf(stderr, "mm: bench recorder dump failed: %s\n", e.what());
    }
  }
  // io_.run() returns once the last handler (this one) completes: nothing re-arms.
}

void ServerImpl::schedule_feed(std::chrono::milliseconds delay) {
  feed_timer_.expires_after(delay);
  feed_timer_.async_wait([this](beast::error_code ec) {
    if (!ec)
      on_feed_event();
  });
}

void ServerImpl::on_feed_event() {
  if (stopping_)
    return;
  if (feed_index_ >= feed_.size()) {
    // Looping is the DRIVER's decision (feed.hpp): a loop-worthy scenario simply has no
    // end marker, and the driver wraps; exhaustion without --loop is a graceful stop.
    if (cfg_.loop_feed && !feed_.empty()) {
      feed_index_ = 0;
    } else {
      begin_stop();
      return;
    }
  }
  const FeedEvent &event = feed_[feed_index_++];
  std::chrono::milliseconds next{cfg_.feed_interval_ms};
  if (const auto *set = std::get_if<FeedSet>(&event)) {
    publish_book(*set);
  } else if (const auto *halt = std::get_if<FeedHalt>(&event)) {
    next = std::chrono::milliseconds{halt->ms}; // publish nothing for the halt window
  } else if (std::holds_alternative<FeedDrop>(event)) {
    // Force-close every session (disconnect demo): same graceful 1001 as shutdown, but
    // the engine keeps running and later connects get fresh epochs.
    std::vector<std::shared_ptr<Session>> open;
    open.reserve(sessions_.size());
    for (const auto &[epoch, session] : sessions_)
      open.push_back(session);
    for (const auto &session : open)
      session->begin_close(websocket::close_code::going_away);
  } else { // FeedEnd
    begin_stop();
    return;
  }
  schedule_feed(next);
}

void ServerImpl::publish_book(const FeedSet &set) {
  const Book book{.bid_px = set.bid_px,
                  .bid_qty = set.bid_qty,
                  .ask_px = set.ask_px,
                  .ask_qty = set.ask_qty,
                  .md_seq = ++next_md_seq_};
  const std::int64_t m0 = steady_ns();
  if (recorder_)
    recorder_->md_published(book.md_seq, m0); // m0: book applied on the owner thread
  auto routed = engine_.on_book(book);
  for (Routed &report : routed) {
    if (std::holds_alternative<Fill>(report.msg))
      ++counters_.fills;
    const auto it = sessions_.find(report.session);
    if (it != sessions_.end())
      // Ownership transfers AT the call ((r)5): `report` is a routed element this loop owns
      // and never revisits, so the move is visible where it is paid for rather than hidden
      // behind a by-reference signature that leaves the caller unsure what it still holds.
      it->second->deliver_report(std::move(report.msg));
  }
  if (sessions_.empty())
    return;
  Tob tob{.md_seq = book.md_seq,
          .symbol = inst_.symbol,
          .bid_px = book.bid_px,
          .bid_qty = book.bid_qty,
          .ask_px = book.ask_px,
          .ask_qty = book.ask_qty};
  // TOB fan-out per (r)13: an explicit copy for every session but the last, a move for
  // the last — each copy is visible at exactly the call site that pays for it. Two loops
  // rather than a branch on the last iterator, so the move is structurally the final use.
  // The successor is taken BEFORE each hand-off, never derived after it. begin_stop copies
  // the whole map to survive a synchronous erase; this loop cannot pay that allocation on
  // the measured path, so it buys the same safety by structure. deliver_tob can only ever
  // erase its OWN entry (session_closed erases by the session's epoch, and every close
  // path reachable from pump is an asio round trip), and a walk that never holds the node
  // it is about to hand off therefore keeps both `it` and `last` valid.
  const auto last = std::prev(sessions_.end());
  for (auto it = sessions_.begin(); it != last;) {
    const auto next = std::next(it);
    it->second->deliver_tob(Tob{tob});
    it = next;
  }
  last->second->deliver_tob(std::move(tob));
}

void ServerImpl::schedule_snapshot() {
  // The 1 Hz counters snapshot rides its OWN timer, deliberately independent of the feed
  // timer: FeedHalt pauses the feed, and telemetry must keep narrating exactly when the
  // feed goes quiet (F-26).
  snapshot_timer_.expires_after(std::chrono::seconds{1});
  snapshot_timer_.async_wait([this](beast::error_code ec) {
    if (ec || stopping_)
      return;
    push_snapshot();
    schedule_snapshot();
  });
}

void ServerImpl::push_snapshot() {
  settle_snapshot_counters();
  // BOTH records of the two-record snapshot ((s)1) — pushing one would silently halve
  // the exported counter set. Fire-and-forget: a full ring counts the drop itself.
  for (const TelemetryRecord &record : snapshot_records(counters_, steady_ns()))
    ring_.try_push(record);
  sample_ring_pending();
}

void ServerImpl::settle_snapshot_counters() {
  counters_.sessions = sessions_.size();
  std::uint64_t live = 0;
  std::uint64_t conflated = conflated_base_;
  for (const auto &[epoch, session] : sessions_) {
    live += engine_.live_count(epoch);
    conflated += session->outbox().conflated();
  }
  counters_.live_orders = live;
  counters_.conflated = conflated; // cumulative base + live sessions ((s)12)
  counters_.stale_books_ignored = engine_.stale_books_ignored(); // item (k)'s seat
}

bool ServerImpl::publish_pending_fault() {
  if (!pending_fault_.has_value())
    return true;
  // Second and last offer, taken AFTER the writer has had the ring to itself for the whole
  // shutdown path — the state that made the first attempt fail is exactly the state a drain
  // clears. Only a sink that never recovers reaches the stderr fallback.
  const auto giveup = std::chrono::steady_clock::now() + kFaultPublishWait;
  do {
    if (publish_record(*pending_fault_)) {
      pending_fault_.reset();
      return true;
    }
  } while (std::chrono::steady_clock::now() < giveup);
  std::fprintf(stderr, "mm: engine fault could not be recorded — the telemetry sink never "
                       "drained; this run ABORTED\n");
  pending_fault_.reset();
  return false;
}

bool ServerImpl::push_final_snapshot() {
  publish_pending_fault();
  settle_snapshot_counters();
  // The pending high-water mark is sampled BEFORE the records are materialized, unlike
  // the periodic path: the final snapshot is the last thing anyone reads, so a watermark
  // stamped from a sample taken after it would never be exported at all.
  sample_ring_pending();
  bool published = true;
  for (const TelemetryRecord &record : snapshot_records(counters_, steady_ns()))
    published = publish_record(record) && published; // both records, whatever the first did
  return published;
}

bool ServerImpl::publish_record(const TelemetryRecord &record) {
  // pending() is a producer-side OVERESTIMATE, so "< capacity" PROVES a free slot — which
  // is why the wait polls that instead of retrying try_push: a retry loop would spend the
  // ring's drop counter on every attempt and report a saturation that never happened.
  const auto giveup = std::chrono::steady_clock::now() + kFinalSnapshotWait;
  while (ring_.pending() >= ring_.capacity() && std::chrono::steady_clock::now() < giveup)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  return ring_.try_push(record); // a writer wedged past the deadline: counted like any drop
}

void ServerImpl::sample_ring_pending() {
  // Producer-side pending() is a safe overestimate — the right direction for a
  // high-water mark (it rounds the alarm up, never hides it).
  counters_.telemetry_pending_hw =
      std::max(counters_.telemetry_pending_hw, static_cast<std::uint64_t>(ring_.pending()));
}

void ServerImpl::emit_event(TelemetryEvent event, std::uint64_t a0, std::uint64_t a1) {
  ring_.try_push(event_record(event, steady_ns(), a0, a1));
  sample_ring_pending();
}

void ServerImpl::session_closed(std::uint64_t epoch, std::uint16_t close_code,
                                std::uint64_t session_conflated) {
  // (s)12: fold the closing session's lifetime conflation count into the cumulative base
  // so the engine-wide aggregate never decreases when the session leaves the sum.
  conflated_base_ += session_conflated;
  emit_event(TelemetryEvent::SessionClose, epoch, close_code);
  sessions_.erase(epoch);
  if (stopping_ && sessions_.empty())
    finalize();
}

} // namespace mm::server_detail

namespace mm {

Server::Server(Config cfg, Instrument inst)
    : impl_(std::make_unique<server_detail::ServerImpl>(std::move(cfg), std::move(inst))) {}
Server::~Server() = default;

void Server::run() { impl_->run(); }
void Server::stop() { impl_->stop(); }
std::uint16_t Server::port() const { return impl_->port(); }
Counters Server::counters() const { return impl_->counters(); }
bool Server::telemetry_ok() const { return impl_->telemetry_ok(); }

} // namespace mm
