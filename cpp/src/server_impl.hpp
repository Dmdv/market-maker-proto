// Beast/Asio-facing declarations shared by the server TUs, kept out of cpp/include/mm/ so the
// public header stays transport-free. Owner-thread only, except the telemetry ring's consumer.
#pragma once

#include "mm/bench_recorder.hpp"
#include "mm/codec.hpp"
#include "mm/engine.hpp"
#include "mm/feed.hpp"
#include "mm/outbox.hpp"
#include "mm/server.hpp"
#include "mm/telemetry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm::server_detail {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

// The negotiated application subprotocol: the ONE spelling the acceptor matches against
// the request's offer list and stamps into the 101 response.
inline constexpr std::string_view kSubprotocol = "mm.v1";

// Two-tier inbound bound: a COMPLETE message over the policy cap draws Reject{MsgTooLarge} and
// the session survives; past the transport cap Beast fails mid-reassembly and closes 1009.
inline constexpr std::size_t kMaxInboundMsgBytes = 64 * 1024;
inline constexpr std::size_t kTransportMsgBytes = 2 * kMaxInboundMsgBytes;

// Bound on a closing handshake nobody answers: a peer that never replies must not pin the
// session (nor a stop() behind it) to its idle timeout.
inline constexpr auto kCloseGrace = std::chrono::seconds{2};

// Backoff before re-arming the acceptor after a TRANSIENT accept failure (EMFILE/ENFILE, an
// aborted connection): an immediate re-arm would spin the event loop at 100% CPU.
inline constexpr auto kAcceptRetryDelay = std::chrono::milliseconds{50};

// Bounded wait for ring space when publishing the FINAL snapshot: shutdown pays for a record
// it promised rather than dropping it, and a wedged writer degrades to a counted drop.
inline constexpr auto kFinalSnapshotWait = std::chrono::milliseconds{250};

// Deliberately far longer than the bounded wait above: the fault marker is the ONE record
// distinguishing a crashed artifact from a clean one, and its loss looks like saturation.
inline constexpr auto kFaultPublishWait = std::chrono::seconds{5};

// Coalescing window for the per-reject event, which cannot be gated behind telemetry_verbose
// (a reject line is contractual): it bounds the peer-driven rate by the snapshot cadence.
inline constexpr auto kRejectCoalesceWindow = std::chrono::seconds{1};

class ServerImpl;

// One in-flight WebSocket upgrade, defined in server_accept.cpp. Declared here so ServerImpl
// can hold weak references to the set still in flight and abort them at shutdown.
struct PendingUpgrade;

// The accept path's socket options (TCP_NODELAY, SO_SNDBUF), defined in server_accept.cpp.
// Declared HERE so a signature change is a compile error in both TUs rather than a link error.
void apply_socket_options(tcp::socket &socket, const Config &cfg);

// One accepted connection. Lifetime: a shared_ptr held by ServerImpl's session map and by every
// in-flight asio handler; the object dies with its last handler after the map entry is erased.
class Session : public std::enable_shared_from_this<Session> {
public:
  Session(ServerImpl &srv, websocket::stream<beast::tcp_stream> ws);

  // Starts the read loop with the epoch the acceptor assigned at successful accept (the
  // WebSocket handshake already completed in the acceptor path).
  void start(std::uint64_t epoch);

  [[nodiscard]] Outbox &outbox() { return outbox_; }
  // The acceptor path drives the WebSocket accept on the session's own stream, so the
  // handshake failure path can drop the never-started session without unwinding state.
  [[nodiscard]] websocket::stream<beast::tcp_stream> &stream() { return ws_; }

  // Feed-driver entry points (owner thread): a routed report for this session, and the TOB
  // fan-out tick. Both take an RVALUE because both TAKE the message.
  void deliver_report(OutMsg &&msg);
  void deliver_tob(Tob &&tob);

  // Begins the session's close with `code`. POLICY closes (1008, 1001) drain the reports already
  // queued; PROTOCOL closes (1002) go out on the next free slot — framing is already broken.
  void begin_close(websocket::close_code code);

private:
  void read_next();
  void on_read(beast::error_code ec, std::size_t bytes);
  // Two stamps per inbound frame: `arrival_ns` off the socket (M3's m3) and `e1_ns` after a
  // successful decode (where M2 begins), so M2 excludes the decode the codec A/B varies.
  void on_message(std::string_view frame, std::int64_t arrival_ns);
  void on_command(InMsg &msg, std::int64_t arrival_ns, std::int64_t e1_ns);
  void push_reject(Reject reject, RejectCode code);
  // The ONLY two ways a message enters the outbox: every enqueue owes the depth watermark,
  // and every REPORT enqueue additionally owes the same-turn policy check.
  void enqueue_report(OutMsg &msg);
  void enqueue_tob(Tob &&tob);
  void mark_outbox_depth();
  // Coalesced per (epoch, RejectCode) per kRejectCoalesceWindow — see the constant.
  void emit_reject_event(RejectCode code);
  // Called on the SAME event-loop turn as any push_report that may have latched the mark;
  // also the entry-cap consumer.
  void check_policy_closes();
  // The ONE place close INTENT is latched (close code, drain policy, epoch retirement). False
  // means a close is already latched; the caller MUST still converge or the session is stranded.
  [[nodiscard]] bool latch_close(websocket::close_code code, bool drain);
  void pump();
  void on_write(beast::error_code ec, std::size_t bytes);
  void send_close_frame();
  // Post-1009 raw-socket drain: consumes the peer's in-flight oversize remainder so the close
  // frame is delivered instead of being destroyed by an RST.
  void drain_transport();
  void drain_read();
  // Cancel-on-disconnect, exactly once: end_session is DESTRUCTIVE and runs at close INTENT, not
  // at teardown — a live epoch in the map keeps drawing fills that would die with the session.
  void retire_epoch();
  void teardown(std::uint16_t recorded_code);

  ServerImpl &srv_;
  websocket::stream<beast::tcp_stream> ws_;
  std::uint64_t epoch_{0}; // assigned by start(); the session key AND the wire epoch
  Outbox outbox_;
  beast::flat_buffer read_buffer_;
  std::string encode_buffer_; // reused across writes: no steady-state allocation
  std::uint64_t out_seq_{1};  // next outbound envelope seq (per direction, from 1)
  std::uint64_t in_seq_expected_{1};
  // svc_ns for OrderAcks pushed but not yet popped, FIFO: the write path stamps svc_ns AFTER
  // pop() while the value is measured at e2. Reports never drop or reorder, so the fronts pair up.
  std::deque<std::int64_t> pending_svc_;
  // Rejects seen in the current coalescing window, one bit per RejectCode ordinal (16, pinned by
  // the static_assert in session.cpp). The window opens lazily, so a first reject always narrates.
  std::uint16_t reject_latch_{0};
  std::int64_t reject_latch_ns_{0};
  // Cancel-on-disconnect acks discarded by retire_epoch. They never enter the outbox, so
  // teardown's undelivered count cannot see them.
  std::size_t retired_acks_{0};
  bool write_inflight_{false};
  std::int64_t write_start_ns_{0};
  // Post-1009 discard sink (drain_read). 1 KiB, not the message budget: the drain re-arms until
  // EOF or the grace timer, so this sets only the syscall count. Deliberately uninitialized.
  std::array<char, 1024> drain_buffer_;
  bool closing_{false}; // a close was initiated; inbound frames are discarded
  bool drain_then_close_{false};
  bool close_frame_sent_{false};
  bool epoch_retired_{false};
  bool torn_down_{false};
  websocket::close_code close_code_{websocket::close_code::none};
  boost::asio::steady_timer grace_timer_;
};

class ServerImpl {
public:
  ServerImpl(Config cfg, Instrument inst);
  // Closes the bench descriptor bound in run(); everything else is member-owned. Declared
  // because the class holds a raw fd — the one resource here with no RAII owner of its own.
  ~ServerImpl();
  ServerImpl(const ServerImpl &) = delete;
  ServerImpl &operator=(const ServerImpl &) = delete;
  ServerImpl(ServerImpl &&) = delete;
  ServerImpl &operator=(ServerImpl &&) = delete;

  void run();
  void stop(); // thread-safe: posts begin_stop onto the owner thread

  [[nodiscard]] std::uint16_t port() const { return bound_port_; }
  [[nodiscard]] Counters counters() const { return counters_; }
  [[nodiscard]] bool telemetry_ok() const { return telemetry_ok_; }

  // --- session-facing surface (owner thread) ---
  [[nodiscard]] const Config &cfg() const { return cfg_; }
  [[nodiscard]] OrderEngine &engine() { return engine_; }
  [[nodiscard]] Counters &counters_mut() { return counters_; }
  [[nodiscard]] ICodec &codec() { return *codec_; }
  [[nodiscard]] BenchRecorder *recorder() { return recorder_ ? &*recorder_ : nullptr; }
  [[nodiscard]] static std::int64_t steady_ns();

  // Peak concurrent sessions, folded in at every map insert. A WATERMARK, not a gauge: a run
  // that peaked at 8 and ended at 0 must still report 8, or its bench dump cannot be read.
  void note_session_added() noexcept {
    peak_sessions_ = std::max(peak_sessions_, static_cast<std::uint64_t>(sessions_.size()));
  }
  [[nodiscard]] std::uint64_t peak_sessions() const noexcept { return peak_sessions_; }

  void emit_event(TelemetryEvent event, std::uint64_t a0, std::uint64_t a1 = 0);
  // Teardown bookkeeping, exactly once per session: the conflated fold, the session_close event,
  // the map erase, and — when stopping — the finalizer once the last session is gone.
  void session_closed(std::uint64_t epoch, std::uint16_t close_code,
                      std::uint64_t session_conflated);

private:
  void begin_stop();
  void finalize();
  // --- the acceptor half, defined in cpp/src/server_accept.cpp ---
  void accept_next();
  void on_upgrade_request(tcp::socket socket);
  // In-flight upgrades still alive, pruning the expired entries as it counts — which is
  // what keeps `upgrades_` bounded by peak concurrency rather than by lifetime connects.
  std::size_t live_upgrades();
  // Shutdown's claim on the upgrades still in flight: PendingUpgrade is complete only in that
  // TU, so begin_stop reaches them through this rather than by dereferencing.
  void abort_pending_upgrades();
  void schedule_feed(std::chrono::milliseconds delay);
  void on_feed_event();
  void publish_book(const FeedSet &set);
  void schedule_snapshot();
  void push_snapshot();
  // The derived fields both snapshot paths must settle before a record is materialized.
  void settle_snapshot_counters();
  // Re-offers a held engine-fault record after the writer has drained; see pending_fault_.
  bool publish_pending_fault();
  // The final snapshot is a PROMISE rather than a sample: counters and watermarks are settled
  // first, then both records are published with a bounded wait for ring space.
  [[nodiscard]] bool push_final_snapshot();
  [[nodiscard]] bool publish_record(const TelemetryRecord &record);
  void sample_ring_pending();

  Config cfg_;
  Instrument inst_;
  boost::asio::io_context io_;
  tcp::acceptor acceptor_;
  std::uint16_t bound_port_{0};
  std::unique_ptr<ICodec> codec_;
  OrderEngine engine_;
  std::vector<FeedEvent> feed_;
  std::size_t feed_index_{0};
  std::uint64_t next_md_seq_{0}; // last assigned; the driver assigns from 1
  boost::asio::steady_timer feed_timer_;
  boost::asio::steady_timer snapshot_timer_;
  boost::asio::steady_timer accept_backoff_;
  // Shutdown-scoped force-close: independent of each stream's idle timeout so a peer
  // that parks Beast's read (oversize header, socket held open) cannot pin stop().
  boost::asio::steady_timer shutdown_deadline_;
  std::optional<boost::asio::signal_set> signals_;
  Counters counters_;
  // Cumulative base for the engine-wide conflated count: closing sessions fold their
  // lifetime counts here so the aggregate never decreases.
  std::uint64_t conflated_base_{0};
  SpscTelemetryRing ring_;
  std::optional<BenchRecorder> recorder_;
  // Bound at startup (created, NOT truncated) and closed by ~ServerImpl: the identity the
  // startup check cleared is the one the shutdown dump writes, without re-resolving the path.
  int bench_fd_{-1};
  // An engine-fault record whose first publication found the ring full, held so finalize() can
  // re-offer it: a dropped fault record leaves a crashed run shaped exactly like a clean one.
  std::optional<TelemetryRecord> pending_fault_;
  std::map<std::uint64_t, std::shared_ptr<Session>> sessions_; // keyed by epoch
  // Upgrades past the TCP accept and not yet a Session. WEAK on purpose — the chain owns itself
  // through its handlers — so aborting at shutdown cannot extend a lifetime past its handlers.
  std::vector<std::weak_ptr<PendingUpgrade>> upgrades_;
  // Sessions whose WebSocket handshake is in flight: the socket has already been MOVED out of
  // the PendingUpgrade, so this is shutdown's only handle on the live socket in that window.
  std::vector<std::weak_ptr<Session>> handshaking_;
  std::uint64_t next_epoch_{0};    // last assigned; accept assigns from 1
  std::uint64_t peak_sessions_{0}; // run high-water mark of sessions_.size()
  bool stopping_{false};
  bool finalized_{false};
  bool telemetry_ok_{true};
  // LAST member deliberately: it puts TelemetryWriter's writer-thread-dirtied tail at the end of
  // ServerImpl (no shared granule) and joins its thread before ring_, above, is destroyed.
  std::optional<TelemetryWriter> writer_;
};

} // namespace mm::server_detail
