// Internal declarations shared by the Task 7 server TUs. This block is the ONE
// authoritative index of which TU owns which half — the other files defer to it instead of
// restating it, which is how two copies of the routing drifted apart across the R1 split:
//   cpp/src/server.cpp        — process lifecycle, feed driver, telemetry pump, shutdown
//   cpp/src/server_accept.cpp — the acceptor half: TCP accept, HTTP upgrade, handshake
//   cpp/src/session.cpp       — one connection's read/command/write state machine
//   cpp/src/session_close.cpp — that session's close half: close-intent latching, the
//                               drain-then-close handshake, epoch retirement, teardown
// Deliberately NOT under cpp/include/mm/: Beast/Asio are an implementation detail of
// mm::Server, and the public header stays transport-free (mm/server.hpp).
//
// THREADING: everything declared here runs on the ONE owner thread (the io_context in
// ServerImpl; record D6) except SpscTelemetryRing's consumer side, which the
// TelemetryWriter's own thread drains — the ring is the only cross-thread boundary
// (record A1). Server::stop() may be called from any thread; it only posts.
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

// Two-tier inbound message-size bound. kMaxInboundMsgBytes is the POLICY cap (Global
// Constraints' 64 KiB — the same figure the Python client enforces as its reassembly
// cap): a message that arrives COMPLETE but larger draws Reject{MsgTooLarge} on the
// message path and the session survives, because the read pipeline still has a clean
// message boundary to answer on. Beast's read_message_max is set one full budget higher
// (kTransportMsgBytes): past THAT, the frame header already announces more than will
// ever be reassembled, Beast fails the read mid-message — no clean boundary remains —
// and sends close 1009 itself (F-21). One bound alone cannot produce both mandated
// behaviors (a recoverable MsgTooLarge reject AND a 1009 mid-reassembly close): with
// read_message_max at exactly the policy cap every oversize would die as 1009. The
// two-tier resolution is queued for the plan text in docs/PENDING_AMENDMENTS.md item
// (t).
inline constexpr std::size_t kMaxInboundMsgBytes = 64 * 1024;
inline constexpr std::size_t kTransportMsgBytes = 2 * kMaxInboundMsgBytes;

// Bound on a closing handshake nobody answers: after our close frame is sent, a peer
// that never replies would otherwise pin the session (and a stop() behind it) to its
// idle timeout. Long enough for any live peer's reply round trip, short enough that
// shutdown stays prompt against a dead one.
inline constexpr auto kCloseGrace = std::chrono::seconds{2};

// Backoff before re-arming the acceptor after a TRANSIENT accept failure (EMFILE/ENFILE,
// an aborted connection). Immediate re-arm would spin the event loop at 100% CPU for as
// long as the condition lasts; this trades ~50 ms of accept latency in a regime the
// listener is already failing in.
inline constexpr auto kAcceptRetryDelay = std::chrono::milliseconds{50};

// Ceiling on how long the FINAL snapshot may wait for room in a saturated telemetry ring
// ((s)10 promises the record, so shutdown pays a bounded wait for it rather than dropping
// it). The writer's 10 ms poll empties the whole ring in one pass, so this is never
// reached by a live writer; a wedged one degrades to the ordinary counted drop.
inline constexpr auto kFinalSnapshotWait = std::chrono::milliseconds{250};

// Last-chance budget for the HELD engine-fault record, deliberately far longer than the
// ordinary bounded wait above. The two are answering different questions: a final snapshot
// that misses its slot degrades to one counted drop among many, while the fault marker is
// the ONE record distinguishing a crashed artifact from a clean one, and its loss is
// indistinguishable from ordinary saturation. So shutdown pays a much larger wait for it
// rather than reporting an abort on stderr alone — the channel a post-mortem reader and the
// Task 11 harness both ignore.
inline constexpr auto kFaultPublishWait = std::chrono::seconds{5};

// Coalescing window for the per-reject telemetry event — the ONE narration a peer drives
// directly (cmd_in/tob_out are gated behind telemetry_verbose; the reject line cannot be,
// because a malformed frame's reject event is a pinned contract). Tracks the 1 Hz snapshot
// cadence of ServerImpl::schedule_snapshot: the invariant is that the peer-controlled
// emission rate is bounded by the SNAPSHOT cadence, never by the peer. Measured
// unbounded: a 7-byte-frame flood produced 4.24x write amplification into the telemetry
// file at 1.20 MB/s, against a documented ring-drop onset of 300k records/s.
inline constexpr auto kRejectCoalesceWindow = std::chrono::seconds{1};

class ServerImpl;

// One in-flight WebSocket upgrade, defined in server_accept.cpp. Declared here only so
// ServerImpl can hold weak references to the set still in flight and abort them at
// shutdown.
struct PendingUpgrade;

// The accept path's socket options (TCP_NODELAY, SO_SNDBUF), defined in
// server_accept.cpp. Declared HERE rather than forward-declared inside the test that pins
// them: this header is the ONE declaration both TUs compile against, so a signature change
// is a compile error on both sides at once. A test-side declaration would make the same
// change a LINK error instead — found at the end of the build, with nothing naming which
// side drifted — and silent drift is exactly what a white-box pin exists to catch. The pin
// needs a seam at all because no peer can observe the far end's socket options, so no
// black-box case can see the option go missing (deleting the TCP_NODELAY line leaves the
// whole suite green). cpp/src is on mm_tests' include path for this; mm_core's PUBLIC
// interface stays cpp/include, so Beast still does not reach the shipped headers.
void apply_socket_options(tcp::socket &socket, const Config &cfg);

// One accepted connection. Lifetime: shared_ptr held by ServerImpl's session map and by
// every in-flight asio handler; the map entry is erased at teardown and the object dies
// with its last handler.
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
  // fan-out tick (rvalue per (r)13 — the caller decides copy vs move per call site). Both
  // take an RVALUE because both TAKE the message: a non-const lvalue reference says "I may
  // modify this" where the contract is "this is mine now", which left the transfer to be
  // carried by a comment at the call site instead of by the type.
  void deliver_report(OutMsg &&msg);
  void deliver_tob(Tob &&tob);

  // Begins the session's close with `code`. Reports already queued drain first for the
  // POLICY closes (1008, 1001 — record D6's "after draining reports"); the PROTOCOL
  // closes (1002) send the close frame on the next free write slot, because the peer
  // already broke framing and its reports no longer have a reader worth waiting on.
  void begin_close(websocket::close_code code);

private:
  void read_next();
  void on_read(beast::error_code ec, std::size_t bytes);
  // TWO STAMPS, because §5.1 asks two different questions of one inbound frame:
  //   * `arrival_ns` — when the frame came off the socket. This is M3's `m3` ("receipt of the
  //     NewOrder echoing that md_seq"), so it MUST be arrival and not anything later.
  //   * `e1_ns` — taken AFTER a successful decode, which is where §5.1 defines M2 to begin
  //     ("C++ e1 after message decode") and what the spec's table calls post-decode.
  // A single stamp served both, so M2 silently absorbed the JSON decode — the exact work the
  // codec swap changes — and reported it as engine service time. That inflated M2 on both arms
  // and made the naive/tuned M2 difference partly a measurement of the decoder it was supposed
  // to hold constant.
  void on_message(std::string_view frame, std::int64_t arrival_ns);
  void on_command(InMsg &msg, std::int64_t arrival_ns, std::int64_t e1_ns);
  void push_reject(Reject reject, RejectCode code);
  // The ONLY two ways a message enters the outbox. Every enqueue owes the depth
  // watermark, and every REPORT enqueue additionally owes the same-turn policy check
  // ((r)6). Four hand-copied sites were four chances for a fifth to omit one — on the
  // exact counter the in-flight fold in Session::teardown makes load-bearing.
  void enqueue_report(OutMsg &msg);
  void enqueue_tob(Tob &&tob);
  void mark_outbox_depth();
  // Coalesced per (epoch, RejectCode) per kRejectCoalesceWindow — see the constant.
  void emit_reject_event(RejectCode code);
  // (r)6: called on the SAME event-loop turn as any push_report that may have latched
  // the mark; also the entry-cap consumer (item (i)).
  void check_policy_closes();
  // The ONE place close INTENT is latched — the close code, the drain policy and the
  // epoch retirement below, together. begin_close is its ordinary caller; the two sites
  // that cannot use begin_close (the 1009 mid-reassembly close owns its own transport
  // drain, the 1011 empty-encode close runs inside pump's own pop loop) call it directly,
  // because they owe the same retirement and setting the flags inline once skipped it.
  // Returns false when a close is already under way or the session is torn down — the
  // first latch keeps its code, and the caller must not re-arm a close frame.
  //
  // A refusal is not permission to do nothing: the caller's OBLIGATION on false is to
  // converge, either to teardown() or to the FIRST latch's close frame. This session is
  // reaped only by an asio completion, so an arm that returns bare after abandoning its
  // own operation strands the session in ServerImpl::sessions_ forever — no teardown, no
  // session_closed(), no finalize(), and the run silently loses its final snapshot and
  // its whole bench dump while telemetry_ok() still reads its true initializer.
  [[nodiscard]] bool latch_close(websocket::close_code code, bool drain);
  void pump();
  void on_write(beast::error_code ec, std::size_t bytes);
  void send_close_frame();
  // Post-1009 raw-socket drain: consumes the peer's in-flight oversize remainder so the
  // close frame is delivered instead of being destroyed by an RST (rationale at the
  // message_too_big branch in session.cpp).
  void drain_transport();
  void drain_read();
  // Cancel-on-disconnect ((h)5b), exactly once per session: OrderEngine::end_session is
  // DESTRUCTIVE, and it runs the moment this session can no longer deliver — at close
  // INTENT, not only at teardown. A session whose close frame is already queued still
  // sits in the server's map, so an epoch left live there would keep drawing fills out
  // of the feed sweep that are counted and then die with the session.
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
  // svc_ns values for OrderAcks pushed but not yet popped, FIFO — the mechanism that
  // lets the session stamp svc_ns AFTER pop() ((r)7: the write path is the sole writer
  // of seq/epoch/svc_ns) while the value is measured at e2 on the command path. Reports
  // are never dropped or reordered, so the fronts pair up by construction. The deque's
  // node churn joins the outbox deque's documented allocation exception ((r)8).
  std::deque<std::int64_t> pending_svc_;
  // Rejects seen in the current coalescing window, one bit per RejectCode ordinal (the
  // enum has 16, pinned by the static_assert in session.cpp), plus the window's start.
  // The window opens lazily: the latch starts empty, so a session's first reject of any
  // code always narrates.
  std::uint16_t reject_latch_{0};
  std::int64_t reject_latch_ns_{0};
  // Cancel-on-disconnect acks discarded by retire_epoch. They never enter the outbox, so
  // teardown's undelivered count cannot see them — this is what makes the debug line
  // cover the retired epoch's acks rather than merely claim to.
  std::size_t retired_acks_{0};
  bool write_inflight_{false};
  std::int64_t write_start_ns_{0};
  // Post-1009 discard sink (drain_read). 1 KiB, not the connection's message budget: the
  // drain re-arms until EOF or the grace timer, so this size sets only the syscall count
  // on a path already bounded by kCloseGrace — while an 8 KiB array was 94% of every
  // Session object and was value-initialized on every accept, for a branch a conforming
  // client never reaches. Deliberately uninitialized: the sink is written before it is
  // read, by async_read_some itself.
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

  // Peak concurrent sessions for the run, called by the acceptor at every map insert.
  // A WATERMARK, not a gauge: Counters::sessions is the gauge and is the WRONG source
  // here, because a run that peaked at 8 and ended at 0 must still report 8 — the whole
  // point is to describe the run the bench dump came from. It cannot relax for the same
  // reason the M-class counters cannot: above one session the recorder's first-hand-off
  // rule degrades md_written's m0' from "this session's write" to a fan-out MINIMUM, so
  // the artifact has to carry its own session count. Without it a 79x-understated dump is
  // indistinguishable from a correct one.
  void note_session_added() noexcept {
    peak_sessions_ = std::max(peak_sessions_, static_cast<std::uint64_t>(sessions_.size()));
  }
  [[nodiscard]] std::uint64_t peak_sessions() const noexcept { return peak_sessions_; }

  void emit_event(TelemetryEvent event, std::uint64_t a0, std::uint64_t a1 = 0);
  // Teardown bookkeeping, exactly once per session: the conflated fold ((s)12), the
  // session_close event, the map erase, and — when stopping — the shutdown finalizer once
  // the last session is gone. The engine-side reap is the SESSION's (Session::retire_epoch),
  // because it must be able to run earlier than this.
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
  // Shutdown's claim on the upgrades still in flight: PendingUpgrade is complete only in
  // that TU, so begin_stop reaches them through this rather than by dereferencing.
  void abort_pending_upgrades();
  void schedule_feed(std::chrono::milliseconds delay);
  void on_feed_event();
  void publish_book(const FeedSet &set);
  void schedule_snapshot();
  void push_snapshot();
  // The derived fields both snapshot paths must settle before a record is materialized.
  void settle_snapshot_counters();
  // The final snapshot of (s)10, which is a PROMISE rather than a sample: counters and
  // watermarks are settled first, then both records are published with a bounded wait for
  // ring space. False if either record could not be published.
  // Re-offers a held engine-fault record after the writer has drained; see pending_fault_.
  bool publish_pending_fault();
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
  // lifetime counts here so the aggregate never decreases ((s)12).
  std::uint64_t conflated_base_{0};
  SpscTelemetryRing ring_;
  std::optional<BenchRecorder> recorder_;
  // Opened at startup beside the bench_out/telemetry_out identity check and held for the
  // whole run, so the destination the check cleared is the destination the shutdown dump
  // writes — see the open site in server.cpp for why re-resolving the pathname at shutdown
  // is not equivalent.
  // Bound at startup (created, NOT truncated) and closed by ~ServerImpl: the identity the
  // startup check cleared is the identity the shutdown dump writes. A descriptor rather than
  // a stream because the dump must be able to empty its own destination without naming it.
  int bench_fd_{-1};
  // An engine-fault record whose first publication attempt found the ring full. Held so
  // finalize() can re-offer it once the writer has drained, because this record's whole
  // purpose is to be IN the artifact — a dropped one leaves a crashed run shaped exactly
  // like a clean one.
  std::optional<TelemetryRecord> pending_fault_;
  std::map<std::uint64_t, std::shared_ptr<Session>> sessions_; // keyed by epoch
  // Upgrades past the TCP accept and not yet a Session. WEAK on purpose: the chain owns
  // itself through its handlers, and this view exists only so shutdown can abort what is
  // still in flight — an owning list would keep aborted upgrades alive past their last
  // handler. Compacted by live_upgrades().
  std::vector<std::weak_ptr<PendingUpgrade>> upgrades_;
  // Sessions whose WebSocket handshake is in flight: past the point where the upgrade's
  // socket was MOVED into the Session, and not yet in sessions_. Beast's move leaves the
  // PendingUpgrade's stream not-open, so closing that aborts nothing — this is the only
  // handle shutdown has on the live socket in that window. Compacted (and the entry
  // dropped) by the accept completion, so it is bounded by concurrent handshakes.
  std::vector<std::weak_ptr<Session>> handshaking_;
  std::uint64_t next_epoch_{0};    // last assigned; accept assigns from 1
  std::uint64_t peak_sessions_{0}; // run high-water mark of sessions_.size()
  bool stopping_{false};
  bool finalized_{false};
  bool telemetry_ok_{true};
  // LAST member, deliberately, and the only member with a cross-thread neighbour problem
  // to solve. TelemetryWriter's writer-thread-dirtied state (mutex_/cv_/stop_/thread_) sits
  // at the END of its own layout, so putting writer_ last puts that state at the end of
  // ServerImpl with nothing after it: the ~100 dirties/second the writer's 10 ms poll
  // drives can no longer land in a granule shared with an owner-thread member. It
  // previously shared one with recorder_'s vector end-pointers, which the owner thread
  // bumps on every recorded sample — measured on this host as writer_ tail at [2232,2360)
  // against recorder_ at 2368, isolated at a 64-byte granule and NOT isolated at the
  // 128-byte granule of the ubuntu:26.04-on-M3 surface telemetry_ring.hpp already names
  // authoritative for exactly this calibration. Nothing before writer_ is touched off the
  // owner thread, so its head has no neighbour to lose either.
  //
  // DESTRUCTION ORDER is the one thing this move could get wrong, so state it: members
  // destruct in REVERSE declaration order, so last-declared destructs FIRST — the writer
  // joins its thread before ring_ (declared above) is destroyed, which is the
  // ring-outlives-the-writer contract, preserved rather than merely unbroken. writer_ is
  // absent from ServerImpl's mem-init-list (it is emplaced in run()), so moving it changes
  // no initialization order.
  std::optional<TelemetryWriter> writer_;
};

} // namespace mm::server_detail
