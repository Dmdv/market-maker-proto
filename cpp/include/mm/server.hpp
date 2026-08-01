// Beast WebSocket server: the engine process's composition root. Owns the single-threaded asio
// io_context and, through it, all order/session/feed state; Asio types sit behind a pimpl.
//
// LIFECYCLE CONTRACT
//   Construction — loads and validates the feed script, then binds and listens (failures throw;
//                  main.cpp maps them to exit 2). Binding here makes port() race-free.
//   run()        — starts the telemetry writer, the timers and the acceptor, then blocks on the
//                  io_context until stop(), a signal, or feed exhaustion. Call it exactly once.
//   stop()       — thread-safe and idempotent: closes the acceptor, closes every open session
//                  1001, publishes the final snapshot, dumps the bench recorder. One shutdown
//                  deadline bounds the drain, so never-sheds is scoped to a RUNNING engine.
//   counters(), telemetry_ok() — valid only after run() returned.
//
// INBOUND SEQ AFTER A REJECT (contiguous from 1; a break closes 1002): pre-sequencer rejects
// (MSG_TOO_LARGE, codec DecodeErrors) leave the seq UNCONSUMED — reuse it; all others advance.
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

// The wire-visible configuration. Timeouts and caps are Config fields rather than constants
// because a test can only assert a bound it is able to size down.
struct Config {
  std::uint16_t port{8765}; // 0 = ephemeral; Server::port() reports the bound port
  // LOOPBACK by default, and that is a security decision: this engine authenticates nothing, so
  // reaching it from another host must be an explicit operator act, never the shipped default.
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
  // Bound on the PRE-upgrade read — a peer that opens a socket and then sends nothing. It is
  // the only thing that recycles that peer's admission slot. Then idle_timeout_ms takes over.
  std::int64_t upgrade_timeout_ms{10'000};
  std::size_t max_session_entries{OrderEngine::kDefaultMaxSessionEntries};
  int so_sndbuf{0}; // >0: SO_SNDBUF on each session socket; 0 = OS default
  bool handle_signals{false};
  // Admission bound on CONCURRENT connections — sessions plus in-flight upgrades, in two tiers:
  // a presented upgrade is refused HTTP 503; a mute socket is closed at accept, not left to sit.
  //
  // It also bounds a PRODUCT: worst-case memory is max_sessions x max_session_entries x ~195 B,
  // so the default of 4 caps it near 820 MB where 64 would mean ~13 GB. A <= 1 GB budget fixes it.
  std::size_t max_sessions{4};
};

class Server {
public:
  Server(Config cfg, Instrument inst);
  ~Server();

  // A lifecycle owner, not a value: run() BLOCKS on an io_context the impl owns, so a moved-from
  // Server would still be run()-able and the moved-to one could not adopt an already-blocked run.
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  void run();
  void stop();

  [[nodiscard]] std::uint16_t port() const;

  // Final counters for the shutdown line (orders/fills/conflated). Valid after run() returns.
  [[nodiscard]] Counters counters() const;
  // Run health verdict (valid after run() returns), a one-way latch: false if telemetry output
  // failed, the final snapshot was lost, a handler threw, or signals could not be restored.
  [[nodiscard]] bool telemetry_ok() const;

private:
  std::unique_ptr<server_detail::ServerImpl> impl_;
};

} // namespace mm
