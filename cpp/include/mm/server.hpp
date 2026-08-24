// Beast WebSocket server (plan Task 7): the engine process's composition root. Owns the
// single-threaded asio io_context and, through it, ALL order/session/feed state (record
// D6); the process's whole thread inventory is the owner thread running io_context plus
// the one TelemetryWriter thread (record A1) — stated here because this class is where
// both are created.
//
// The Beast/Asio types live behind a pimpl in cpp/src/server_impl.hpp: consumers of this
// header (main.cpp, the integration tests) need the lifecycle and the wire-visible
// configuration, not the transport internals.
//
// LIFECYCLE CONTRACT
//   Construction  — loads and validates the feed script (Task 5 load_feed, which throws
//                   naming the offending line) and binds+listens on
//                   Config::bind_address:Config::port with SO_REUSEADDR. Failures throw
//                   std::runtime_error /
//                   std::invalid_argument: main.cpp maps them to one stderr line + exit 2
//                   (ops failure paths, F-12). Binding in the constructor is what makes
//                   port() race-free for a caller that starts run() on another thread.
//   run()         — starts the telemetry writer, pushes the zeroed baseline snapshot
//                   (PENDING item (s)10), starts the feed/telemetry timers and the
//                   acceptor, then blocks running the io_context until stop() (or
//                   SIGINT/SIGTERM with handle_signals, or feed exhaustion without
//                   loop_feed) completes shutdown. Call it exactly once.
//   stop()        — thread-safe and idempotent: posts the graceful shutdown onto the
//                   owner thread — the acceptor closes, in-flight upgrades are aborted,
//                   and every session then OPEN is closed 1001 going_away (a session
//                   already closing keeps the code it began with — 1002/1008/1009 are
//                   not rewritten). The final counters snapshot is pushed BEFORE the
//                   writer is destroyed ((s)10) and the bench recorder dumped, after
//                   which run() returns. It returns PROMPTLY, not instantly: a policy
//                   close still drains its queued reports first and an unanswered closing
//                   handshake still waits for its reply — but under stop() BOTH are
//                   bounded by one shutdown-scoped deadline of the session close grace,
//                   which then force-closes the lowest layer of EVERY session still in the
//                   map, drained or not. So a peer too slow to take its queued reports
//                   inside that window LOSES them: the never-sheds guarantee of a policy
//                   close is scoped to a RUNNING engine, not to shutdown. That sweep is
//                   deliberately unconditional rather than scoped to sessions that never
//                   reached a close frame — being unconditional is precisely what makes
//                   stop() unpinnable, where the narrower predicate would hand the pin
//                   back to a peer that HAS taken its close frame and then stopped
//                   reading (or that parks Beast's read op on an oversize frame header).
//   counters() / telemetry_ok() — the shutdown-line reads; valid only after run()
//                   returned (the caller's join is the happens-before edge).
//
// INBOUND SEQ CONSUMPTION AFTER A REJECT — the client obligation nothing on the wire
// states. Inbound envelope seq must be contiguous from 1 and a break closes 1002, so a
// client that guesses wrong here loses its connection. A Reject raised BEFORE the
// sequencer leaves the seq UNCONSUMED and the client must reuse it: that is MSG_TOO_LARGE
// plus every codec DecodeError (MALFORMED, UNKNOWN_TYPE, UNSUPPORTED_VERSION) — every
// pre-sequencer reject, none of which the sequencer ever saw. Every OTHER reject —
// STALE_EPOCH and all engine verdicts (UNKNOWN_CLORDID, TICK_SIZE, POST_ONLY_CROSS, ...)
// — is raised after the seq was consumed, so the client must ADVANCE. The rule is
// decidable from the reject's `code` alone, which is what makes it usable: the Reject
// envelope stamps the OUTBOUND seq and every pre-sequencer reject carries an empty
// cl_id, so nothing else in the message identifies the inbound command it answers.
#pragma once

#include "mm/codec.hpp"
#include "mm/engine.hpp"
#include "mm/outbox.hpp"
#include "mm/telemetry.hpp"
#include "mm/types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace mm {

namespace server_detail {
class ServerImpl;
}

// The plan's Config block plus the fields the shipped session/telemetry contracts
// require (each addition queued for the plan text in docs/PENDING_AMENDMENTS.md item
// (t)): telemetry output/verbosity (record A1 wiring), the idle timeout (test-sizable —
// the 30 s default is the Global Constraints value), the per-session entry cap (item
// (i)'s test-sized acceptance row), a send-buffer bound (bounded kernel queueing; also
// what makes backpressure behavior testable), the signal-handling opt-in (only the
// mm_engine binary wants SIGINT/SIGTERM ownership — a test harness does not), the bind
// address (the listener's reach is a decision, not a default) and the pre-upgrade read
// timeout (the pre-upgrade bound has to be test-sizable to be assertable at all).
struct Config {
  std::uint16_t port{8765}; // 0 = ephemeral; Server::port() reports the bound port
  // The interface the listener binds. LOOPBACK by default, and that is a security
  // decision, not a convenience: this engine authenticates nothing, so every bound below
  // is the only thing between an arbitrary peer and the process — reaching it from another
  // host has to be an explicit operator act ("0.0.0.0", or a specific address), never the
  // shipped default. A spelling asio cannot parse throws with the other Config validations
  // at construction.
  std::string bind_address{"127.0.0.1"};
  std::string feed_path;
  CodecKind codec{CodecKind::Tuned};
  std::int64_t feed_interval_ms{100};
  bool loop_feed{false};
  std::size_t report_hwm{Outbox::kDefaultReportHwm};
  std::string bench_out; // if non-empty, write m0m3/svc samples here on shutdown
  std::string telemetry_out{"telemetry.jsonl"};
  bool telemetry_verbose{false}; // per-message events; forced OFF when bench_out is set
  std::int64_t idle_timeout_ms{30'000};
  // Bound on the PRE-upgrade read — a peer that opens a socket and then sends nothing.
  // Config-sized rather than a constant because it is the only thing that ever recycles an
  // accept-tier admission slot held by such a peer, and the sibling control's own case
  // passes only BECAUSE it does: sizing it lets a case assert this half directly instead of
  // inheriting it. Replaced by idle_timeout_ms the moment the WebSocket layer takes over.
  std::int64_t upgrade_timeout_ms{10'000};
  std::size_t max_session_entries{OrderEngine::kDefaultMaxSessionEntries};
  int so_sndbuf{0}; // >0: SO_SNDBUF on each session socket; 0 = OS default
  bool handle_signals{false};
  // Admission bound on CONCURRENT connections — open sessions plus in-flight upgrades.
  // Every accepted connection costs an epoch, two timers and the session's buffers, and
  // nothing else caps that: the entry cap and the report HWM bound one session's growth,
  // not their number. Enforced in TWO tiers, because only one of them can be answered in
  // words: a connection that has PRESENTED an upgrade request is refused HTTP 503 (a
  // refusal the peer can read, not a silent drop), while one that has only connected is
  // closed at accept once this many upgrades are already in flight — it has sent nothing
  // to answer, and without that tier a peer that connects and stays mute holds a
  // descriptor per socket for the whole upgrade_timeout_ms read with no bound at all. The
  // descriptor ceiling is therefore this figure for sessions plus this figure for
  // not-yet-upgraded sockets.
  //
  // It is also — and this is the binding half — a bound on a PRODUCT. max_session_entries
  // above caps ONE session's retained entries, so worst-case resident memory is
  // max_sessions x max_session_entries x ~195 B/entry (measured on this engine: 120k
  // tombstone entries moved RSS by 195 B each, matching engine.cpp's own "192 B at the
  // 64 B cl_id cap"). At the shipped 1<<20 entries that is ~205 MB per session, so the
  // default of 4 sets the worst case at ~820 MB while 64 would set it at ~13 GB — reachable
  // by ONE unauthenticated peer before any 1008 close fires, which is not a bound at all.
  // A memory budget of <= 1 GB is therefore what fixes this figure, not a capacity plan:
  // the demo and the §5.2 react run use a single session. Raise it with --max-sessions when
  // a run genuinely needs the descriptors, and lower it, not max_session_entries, when the
  // budget is tighter — the entry cap is sized at ~10x the Task 11 react-mode cycle count.
  std::size_t max_sessions{4};
};

class Server {
public:
  Server(Config cfg, Instrument inst);
  ~Server();

  // A lifecycle owner, not a value. run() BLOCKS on an io_context the impl owns, and that
  // impl also owns the acceptor, the writer thread and every session's executor: a Server
  // whose ownership moved mid-life would be a trap at both ends — the moved-from one is
  // still run()-able and the moved-to one cannot adopt a run already blocked on the other
  // side of the move. The pimpl is NOT the obstacle (transferring the unique_ptr would not
  // relocate the pointee, so nothing would be left pointing into a husk); if movability is
  // ever wanted, the honest route is out-of-line move operations defined after ServerImpl
  // is complete, gated on the lifecycle rule above rather than on representation.
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  void run();
  void stop();

  [[nodiscard]] std::uint16_t port() const;

  // Final counters for the shutdown line (orders/fills/conflated) and the tests' exact
  // final-snapshot match ((s)10). Valid after run() returns.
  [[nodiscard]] Counters counters() const;
  // The RUN's health verdict — the shutdown health read of PENDING item (s)9, and the
  // figure mm_engine's shutdown line prints. A one-way latch: once any of its falsifiers
  // fires, a later clean result cannot erase it. False if the writer's output stream ever
  // failed (TelemetryWriter::output_failed, sampled after its final drain and close
  // marker); if the promised final snapshot could not be published; if the run ABORTED on
  // an exception escaping an asio handler (which also emits engine_fault, because the
  // durable artifact of a crashed run must not read line-for-line like a graceful one);
  // or if shutdown could not hand SIGINT/SIGTERM back to their default disposition (the
  // operator's second-Ctrl-C escalation is then gone, and the shutdown line must not
  // claim a clean stop while it is). Valid after run() returns.
  [[nodiscard]] bool telemetry_ok() const;

private:
  std::unique_ptr<server_detail::ServerImpl> impl_;
};

} // namespace mm
