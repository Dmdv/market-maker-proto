// The engine process's composition root: process lifecycle, feed driver and telemetry pump, all
// on ONE owner thread. Acceptor: server_accept.cpp. Sessions: session.cpp. Decls: server_impl.hpp.
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

// POSIX, in its own group: the bench artifact is held as a DESCRIPTOR for the run so its identity
// is decided between open objects rather than pathnames, and no standard-library type exposes one.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mm::server_detail {

namespace {

// Same-file fast path. Neither file exists at construction, so this can only be LEXICAL: it
// misses case-variants and links. Real identity is decided by equivalent() in run().
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
  // The fail-loudly boundary: a nonsensical config dies HERE with a throw main.cpp turns into
  // stderr + exit 2, because the same value failing later would do so inside an asio handler.
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
  // Two writers, two formats, one path: finalize() would dump the recorder over the telemetry
  // JSONL with std::ios::trunc, destroying it while telemetry_ok() still reads true.
  if (!cfg_.bench_out.empty() && names_same_file(cfg_.bench_out, cfg_.telemetry_out))
    throw std::invalid_argument("Config: bench_out must not name telemetry_out (the binary "
                                "sample dump would truncate the telemetry file)");
  if (cfg_.max_sessions == 0)
    throw std::invalid_argument("Config: max_sessions must be > 0");
  // Resolved HERE with the rest of the validation: a spelling asio cannot parse owes the same
  // std::invalid_argument, not the bare boost system_error the throwing overload raises.
  beast::error_code addr_ec;
  const boost::asio::ip::address bind_address =
      boost::asio::ip::make_address(cfg_.bind_address, addr_ec);
  if (addr_ec)
    throw std::invalid_argument("Config: bind_address is not an IP address: " + cfg_.bind_address);
  // Per-message narration is forced OFF under measurement: main.cpp already refuses the
  // combination, and this keeps the rule true for direct embedders (tests, benchmark harness).
  if (!cfg_.bench_out.empty())
    cfg_.telemetry_verbose = false;
  if (!cfg_.bench_out.empty())
    recorder_.emplace();

  // Claimed BEFORE the slow startup work below: a SIGINT in that window would otherwise take
  // the inherited disposition and kill the process. async_wait only registers; run() completes it.
  if (cfg_.handle_signals) {
    signals_.emplace(io_, SIGINT, SIGTERM);
    signals_->async_wait([this](beast::error_code ec, int) {
      if (!ec)
        begin_stop();
    });
  }
  feed_ = load_feed(cfg_.feed_path);

  // Bound to Config::bind_address (loopback by default), not tcp::v4()'s any-address: an engine
  // that authenticates nothing must not be reachable from every interface.
  const tcp::endpoint endpoint{bind_address, cfg_.port};
  acceptor_.open(endpoint.protocol());
  // SO_REUSEADDR: an engine restart must not lose its port to TIME_WAIT remnants of its
  // own previous sessions (ops failure paths).
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
  // The telemetry inode, captured the INSTANT the writer holds it open: the pathname can be
  // re-pointed afterwards, so every later identity check must use this, not a fresh resolution.
  struct ::stat telemetry_st{};
  const bool telemetry_identity_known = ::stat(cfg_.telemetry_out.c_str(), &telemetry_st) == 0;
  // The constructor's guard was lexical; from here the telemetry file exists, so equivalent()
  // compares real inodes — catching case-folding, hard links and symlinks. Reset the writer first.
  std::error_code fs_ec;
  if (!cfg_.bench_out.empty() &&
      std::filesystem::equivalent(cfg_.bench_out, cfg_.telemetry_out, fs_ec)) {
    writer_.reset();
    throw std::invalid_argument("Config: bench_out must not name telemetry_out (the binary "
                                "sample dump would truncate the telemetry file)");
  }
  // Bind the fd at startup: the identity checked is the identity written (a path re-resolved
  // at shutdown could be swapped mid-run), and an unwritable path fails fast.
  if (!cfg_.bench_out.empty()) {
    // O_CREAT WITHOUT O_TRUNC, deliberately: truncating here would destroy a pre-existing bench
    // artifact before the run produced a sample. The file is emptied at dump time instead.
    bench_fd_ = ::open(cfg_.bench_out.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (bench_fd_ < 0) {
      writer_.reset();
      throw std::invalid_argument("Config: cannot open bench_out for writing: " + cfg_.bench_out);
    }
    // fstat decides identity between OPEN OBJECTS: the name checked above could be re-pointed
    // between the check and the open. FAIL CLOSED — an unanswerable identity question is not "no".
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
  // An exception escaping a handler unwinds straight OUT of io_.run(), past every path that
  // reaches finalize() — hence the catch. The rethrow is main.cpp's exit-2 path.
  try {
    io_.run();
  } catch (...) {
    stopping_ = true;
    // Record the abort IN THE ARTIFACT: without it the file is shaped like a graceful stop and
    // telemetry_ok() reads true. Push BEFORE finalize() (it joins the writer); latched for retry.
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
    // ...which is why clear() follows: asio's cancel does not deregister, so a second Ctrl-C
    // would be caught and discarded. Restoring the default disposition makes it kill.
    signals_->clear(ec);
    if (ec) {
      // That escalation is an operational guarantee, so its failure must be named and must latch
      // the run unhealthy — the shutdown line cannot report a clean stop while the path is gone.
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
  // Graceful path: every session closes 1001 going_away and the LAST teardown's session_closed
  // runs finalize(). Iterate over a copy — the shutdown path must not assume begin_close defers.
  std::vector<std::shared_ptr<Session>> open;
  open.reserve(sessions_.size());
  for (const auto &[epoch, session] : sessions_)
    open.push_back(session);
  for (const auto &session : open)
    session->begin_close(websocket::close_code::going_away);
  // Bound shutdown independently of each stream's idle timeout: a peer that parks Beast's read
  // op on wr_block blocks the graceful close write, and only that timeout would end the wait.
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
  // The deterministic final snapshot ((s)10): sessions are folded, the acceptor is retired and
  // no timer can re-arm, so nothing pushes after this pair and the writer's drain owes exactly it.
  const bool final_snapshot_ok = push_final_snapshot();
  // The shutdown health read ((s)9). stop_and_join FIRST: the writer's final drain and close
  // marker are where a dying sink surfaces, so a latch sampled ahead of them reports healthy.
  bool writer_ok = true;
  if (writer_) {
    writer_->stop_and_join();
    writer_ok = !writer_->output_failed();
    writer_.reset();
  }
  // ANDed, never assigned over: telemetry_ok_ is a one-way LATCH (mm/server.hpp), so a failure
  // recorded earlier in the run must not be erased by a writer that then closed cleanly.
  telemetry_ok_ = final_snapshot_ok && writer_ok && telemetry_ok_;
  if (recorder_ && !cfg_.bench_out.empty()) {
    // The dump header carries saturated() so an artifact-only reader sees a truncated run
    // (BenchRecorder::dump); stderr is the ops-visible line, not the harness's channel.
    if (const std::uint64_t refused = recorder_->saturated(); refused > 0)
      std::fprintf(stderr,
                   "mm: bench recorder refused %llu sample(s) at capacity: the dumped "
                   "streams are TRUNCATED\n",
                   static_cast<unsigned long long>(refused));
    try {
      // peak_sessions rides in the dump header: the recorded m0->m0' is the true venue-production
      // window only at one session. Written through the fd bound at startup, truncated here first.
      const std::vector<char> bytes = recorder_->serialize(peak_sessions());
      // ftruncate is interruptible on the same terms as the write below — same shutdown
      // path, same signals — so it is retried on EINTR rather than losing a complete run.
      int trunc_rc = 0;
      do {
        trunc_rc = ::ftruncate(bench_fd_, 0);
      } while (trunc_rc != 0 && errno == EINTR);
      if (trunc_rc != 0 || ::lseek(bench_fd_, 0, SEEK_SET) != 0)
        throw std::runtime_error("bench dump: cannot rewind the bound output");
      // EINTR is RETRIED: the dump runs on the shutdown path of a process that handles
      // SIGINT/SIGTERM. Any other error, and a zero-byte write, throw: no partial artifact.
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
      // Ownership transfers AT the call ((r)5): `report` is an element this loop owns and never
      // revisits, so the move is visible where it is paid for.
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
  // TOB fan-out ((r)13): a copy for every session but the last, a move for the last. The
  // successor is taken BEFORE each hand-off, so deliver_tob erasing its OWN entry keeps it valid.
  const auto last = std::prev(sessions_.end());
  for (auto it = sessions_.begin(); it != last;) {
    const auto next = std::next(it);
    it->second->deliver_tob(Tob{tob});
    it = next;
  }
  last->second->deliver_tob(std::move(tob));
}

void ServerImpl::schedule_snapshot() {
  // The 1 Hz counters snapshot rides its OWN timer, independent of the feed timer: FeedHalt
  // pauses the feed, and telemetry must keep narrating exactly when the feed goes quiet.
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
  // Second and last offer, AFTER the writer has had the ring to itself for the whole shutdown
  // path — the state that made the first attempt fail is exactly what a drain clears.
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
  // The pending high-water mark is sampled BEFORE the records are materialized: the final
  // snapshot is the last thing anyone reads, so a watermark stamped after it is never exported.
  sample_ring_pending();
  bool published = true;
  for (const TelemetryRecord &record : snapshot_records(counters_, steady_ns()))
    published = publish_record(record) && published; // both records, whatever the first did
  return published;
}

bool ServerImpl::publish_record(const TelemetryRecord &record) {
  // pending() is a producer-side OVERESTIMATE, so "< capacity" PROVES a free slot — polling it
  // beats retrying try_push, which would spend the ring's drop counter on every attempt.
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
