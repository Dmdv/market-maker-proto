// One WebSocket session: the read -> decode -> engine -> outbox -> write state machine for a
// single connection, on the owner thread. Close-code and frame policy are enforced HERE.
#include "server_impl.hpp"

#include <boost/asio/buffer.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <utility>
#include <variant>

namespace mm::server_detail {

namespace {

// The scan below assumes StaleEpoch is the TERMINAL enumerator; the static_assert is that
// assumption in falsifiable form — the first ordinal past the end must fall through to "UNKNOWN".
constexpr int kRejectCodeCount = static_cast<int>(RejectCode::StaleEpoch) + 1;
static_assert(to_string(static_cast<RejectCode>(kRejectCodeCount)) == "UNKNOWN",
              "RejectCode::StaleEpoch is no longer the last enumerator: extend the scan in "
              "reject_code_from_wire, or an appended code goes unmapped with nothing failing");
static_assert(kRejectCodeCount <= 16, "Session::reject_latch_ is a 16-bit coalescing latch");

// Engine rejects come back with the WIRE spelling in Reject::code, while the reject event's
// lane 1 carries the numeric RejectCode. Rejects are off the measured path, so a scan will do.
std::optional<RejectCode> reject_code_from_wire(std::string_view wire) {
  for (int i = 0; i < kRejectCodeCount; ++i) {
    const auto code = static_cast<RejectCode>(i);
    if (to_string(code) == wire)
      return code;
  }
  return std::nullopt;
}

} // namespace

Session::Session(ServerImpl &srv, websocket::stream<beast::tcp_stream> ws)
    : srv_(srv), ws_(std::move(ws)), outbox_(srv.cfg().report_hwm),
      grace_timer_(ws_.get_executor()) {
  ws_.text(true); // all application frames are text (JSON v1)
  // NO control_callback, in ANY build: a per-frame log line puts an unbuffered write syscall on
  // the OWNER thread at a rate the PEER chooses. Beast sends its automatic pong regardless.
}

void Session::start(std::uint64_t epoch) {
  epoch_ = epoch;
  read_next();
  // Deliberately nothing is SENT here: the first outbound message of any session is the
  // next feed publish's TOB.
}

void Session::read_next() {
  read_buffer_.consume(read_buffer_.size());
  ws_.async_read(read_buffer_,
                 [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
                   self->on_read(ec, bytes);
                 });
}

void Session::on_read(beast::error_code ec, std::size_t) {
  if (torn_down_)
    return;
  if (ec == websocket::error::closed) {
    // Record the code that actually went out: the peer's own close, or ours if we initiated it.
    // An EMPTY close body is legal (reason().code stays none), and Beast answers it 1000, so do we.
    const std::uint16_t peer = ws_.reason().code;
    constexpr auto kNone = static_cast<std::uint16_t>(websocket::close_code::none);
    constexpr auto kNormal = static_cast<std::uint16_t>(websocket::close_code::normal);
    teardown(closing_ ? static_cast<std::uint16_t>(close_code_) : (peer == kNone ? kNormal : peer));
    return;
  }
  if (ec == beast::error::timeout || (ec == boost::asio::error::operation_aborted && !closing_)) {
    // Idle timeout: pings went unanswered, so 1001 can only be RECORDED — Beast already severed
    // the transport. operation_aborted is the same event; a close under way keeps ITS code.
    teardown(closing_ ? static_cast<std::uint16_t>(close_code_) : 1001);
    return;
  }
  if (ec == websocket::error::message_too_big) {
    // Past the TRANSPORT ceiling mid-reassembly: no clean message boundary remains, so this is
    // the 1009 close. Beast has already sent it; the epoch retirement below is not redundant.
    if (!latch_close(websocket::close_code::too_big, /*drain=*/false)) {
      // An in-flight close keeps ITS code and must not re-arm async_close, but this arm still
      // owes convergence: Beast severed the transport, so nothing else would reap the session.
      teardown(static_cast<std::uint16_t>(close_code_));
      return;
    }
    close_frame_sent_ = true;
    ws_.async_close(websocket::close_reason{websocket::close_code::too_big},
                    [self = shared_from_this()](beast::error_code) { self->drain_transport(); });
    return;
  }
  if (!closing_ && ec == websocket::condition::protocol_violation) {
    // Malformed RFC 6455 framing: Beast fails the read AND sends the close itself, so the RECORD
    // must be the code that went out — 1007 for a bad UTF-8 payload, 1002 for anything else.
    teardown(static_cast<std::uint16_t>(ec == websocket::error::bad_frame_payload
                                            ? websocket::close_code::bad_payload
                                            : websocket::close_code::protocol_error));
    return;
  }
  if (ec) {
    // Transport death without a close handshake: our own intent code if a close was already
    // under way, else 1006 — the reserved abnormal-closure code, never sent on the wire.
    teardown(closing_ ? static_cast<std::uint16_t>(close_code_) : 1006);
    return;
  }
  // FRAME ARRIVAL, which is M3's m3. M2's own start is stamped after decode in on_message.
  const std::int64_t arrival = ServerImpl::steady_ns();
  if (closing_) {
    read_next(); // draining toward the close handshake: frames no longer have a reader
    return;
  }
  if (ws_.got_binary()) {
    begin_close(websocket::close_code::protocol_error); // binary frame: 1002
    read_next();
    return;
  }
  // flat_buffer's readable region is one contiguous span: the decode path gets a view,
  // never a copy (the inbound frame is inside the measured M2 window).
  const boost::asio::const_buffer frame = read_buffer_.data();
  on_message(std::string_view{static_cast<const char *>(frame.data()), frame.size()}, arrival);
  read_next();
}

void Session::on_message(std::string_view frame, std::int64_t arrival_ns) {
  if (frame.size() > kMaxInboundMsgBytes) {
    // Fully reassembled but over the POLICY cap: recoverable, so reject-and-survive
    // (the codec never sees it — MsgTooLarge is transport-layer policy by contract).
    push_reject(Reject{.cl_id = "",
                       .code = std::string{to_string(RejectCode::MsgTooLarge)},
                       .reason = "message exceeds the 65536-byte cap"},
                RejectCode::MsgTooLarge);
    pump();
    return;
  }
  auto decoded = srv_.codec().decode(frame);
  if (auto *err = std::get_if<DecodeError>(&decoded)) {
    push_reject(Reject{.cl_id = "",
                       .code = std::string{to_string(err->code)},
                       .reason = std::move(err->detail)},
                err->code);
    pump();
    return;
  }
  // M2 STARTS HERE — after a successful decode. Stamped only on the path that reaches the engine:
  // a frame rejected above never enters engine handling, so it has no service time to report.
  const std::int64_t e1_ns = ServerImpl::steady_ns();
  on_command(std::get<InMsg>(decoded), arrival_ns, e1_ns);
  pump();
}

void Session::on_command(InMsg &msg, std::int64_t arrival_ns, std::int64_t e1_ns) {
  // Envelope checks first: seq contiguity (a gap is a CLIENT bug — the engine never gaps, so
  // close 1002), then epoch (a stale epoch is well-formed: Reject{StaleEpoch}, session stays up).
  const auto [seq, cmd_epoch] =
      std::visit([](const auto &m) { return std::pair{m.seq, m.epoch}; }, msg);
  if (seq != in_seq_expected_) {
    begin_close(websocket::close_code::protocol_error);
    return;
  }
  ++in_seq_expected_;
  if (cmd_epoch != epoch_) {
    push_reject(Reject{.cl_id = std::visit([](const auto &m) { return m.cl_id; }, msg),
                       .code = std::string{to_string(RejectCode::StaleEpoch)},
                       .reason = "command epoch does not match this session's epoch"},
                RejectCode::StaleEpoch);
    return;
  }

  Counters &counters = srv_.counters_mut();
  std::vector<OutMsg> batch;
  if (const auto *order = std::get_if<NewOrder>(&msg)) {
    batch = srv_.engine().on_new(epoch_, *order);
    const std::int64_t svc = ServerImpl::steady_ns() - e1_ns;
    if (BenchRecorder *rec = srv_.recorder()) {
      rec->svc(svc);
      // m3 uses ARRIVAL, not the post-decode e1: the reaction ended when the order REACHED the
      // engine, and decode is part of the tick-to-order path M3 measures. First echo only.
      rec->order_for(order->md_seq, arrival_ns);
    }
    for (OutMsg &report : batch) {
      if (std::holds_alternative<OrderAck>(report)) {
        ++counters.orders;
        pending_svc_.push_back(svc); // stamped into the ack at pop time
      } else if (const auto *reject = std::get_if<Reject>(&report)) {
        ++counters.rejects;
        if (const auto code = reject_code_from_wire(reject->code))
          emit_reject_event(*code);
      }
      enqueue_report(report); // named lvalue, no move at the call site
    }
    if (srv_.cfg().telemetry_verbose)
      srv_.emit_event(TelemetryEvent::CmdIn, epoch_, static_cast<std::uint64_t>(svc));
  } else {
    const auto &cancel = std::get<CancelOrder>(msg);
    batch = srv_.engine().on_cancel(epoch_, cancel);
    const std::int64_t svc = ServerImpl::steady_ns() - e1_ns;
    if (BenchRecorder *rec = srv_.recorder())
      rec->svc(svc);
    for (OutMsg &report : batch) {
      if (std::holds_alternative<CancelAck>(report)) {
        ++counters.cancels;
      } else if (const auto *reject = std::get_if<Reject>(&report)) {
        ++counters.rejects;
        if (const auto code = reject_code_from_wire(reject->code))
          emit_reject_event(*code);
      }
      enqueue_report(report);
    }
    if (srv_.cfg().telemetry_verbose)
      srv_.emit_event(TelemetryEvent::CmdIn, epoch_, static_cast<std::uint64_t>(svc));
  }
}

void Session::push_reject(Reject reject, RejectCode code) {
  ++srv_.counters_mut().rejects;
  emit_reject_event(code);
  OutMsg msg{std::move(reject)};
  enqueue_report(msg);
}

void Session::enqueue_report(OutMsg &msg) {
  outbox_.push_report(msg);
  mark_outbox_depth();
  check_policy_closes(); // the SAME turn as the push that may have latched the mark
}

void Session::enqueue_tob(Tob &&tob) {
  // No policy check here, and that is not an omission: the TOB slot is CONFLATED, so it
  // cannot grow the queue and can never be what latches the report high-water mark.
  outbox_.push_tob(std::move(tob));
  mark_outbox_depth();
}

void Session::mark_outbox_depth() {
  Counters &counters = srv_.counters_mut();
  counters.outbox_depth_hw =
      std::max(counters.outbox_depth_hw, static_cast<std::uint64_t>(outbox_.depth()));
}

void Session::emit_reject_event(RejectCode code) {
  // COALESCED, never gated: "a malformed frame draws a reject line" is a pinned contract, so the
  // RATE is bounded instead: one event per (epoch, code) per window, and counters carry volume.
  static constexpr std::int64_t kWindowNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(kRejectCoalesceWindow).count();
  const std::int64_t now = ServerImpl::steady_ns();
  if (now - reject_latch_ns_ >= kWindowNs) {
    reject_latch_ = 0;
    reject_latch_ns_ = now;
  }
  const auto bit = static_cast<std::uint16_t>(1U << static_cast<unsigned>(code));
  if ((reject_latch_ & bit) != 0)
    return;
  reject_latch_ = static_cast<std::uint16_t>(reject_latch_ | bit);
  srv_.emit_event(TelemetryEvent::Reject, epoch_, static_cast<std::uint64_t>(code));
}

void Session::check_policy_closes() {
  // The mark is OBSERVED, not enforced: this check runs on the SAME event-loop turn as every
  // push that could have latched it, or the deque grows behind a socket that is already slow.
  if (closing_)
    return;
  if (outbox_.report_hwm_breached()) {
    // Lane 1 is the queue depth at the breach; the pending tick can inflate it by at
    // most one over the pure report count (the Outbox exposes no report-only depth).
    srv_.emit_event(TelemetryEvent::HwmClose, epoch_, static_cast<std::uint64_t>(outbox_.depth()));
    begin_close(websocket::close_code::policy_error);
    return;
  }
  // Rejected-order tombstones are not gated by max_live_orders, so this check is the ONLY
  // backpressure on peer-driven entry growth: the engine keeps recording past the cap and signals.
  if (srv_.engine().entry_cap_breached(epoch_)) {
    // Its OWN record, because the two 1008 causes are otherwise separable only by the ABSENCE
    // of an hwm_close line — and that line ships through a try_push a full ring may drop.
    srv_.emit_event(TelemetryEvent::EntryCapClose, epoch_,
                    static_cast<std::uint64_t>(srv_.engine().entry_count(epoch_)));
    begin_close(websocket::close_code::policy_error);
  }
}

void Session::deliver_report(OutMsg &&msg) {
  if (torn_down_)
    return; // the engine reaped this session's orders at teardown; nothing routes here
  // Feed-routed reports are FILLS only, which is what pending_svc_'s FIFO pairing rests on: each
  // measured svc_ns window belongs to the ack of the command that COST it. The assert pins that.
  assert(!std::holds_alternative<OrderAck>(msg));
  enqueue_report(msg);
  pump();
}

void Session::deliver_tob(Tob &&tob) {
  if (torn_down_ || closing_)
    return; // a dying session has no use for fresher market data
  enqueue_tob(std::move(tob));
  pump();
}

void Session::pump() {
  if (write_inflight_ || torn_down_ || close_frame_sent_)
    return;
  if (closing_ && !drain_then_close_) {
    send_close_frame();
    return;
  }
  while (auto popped = outbox_.pop()) {
    // A REFERENCE, not a move out of the optional: `popped` is the while-condition variable and
    // lives for the whole iteration, so the lifetimes are identical and a move would be pure cost.
    OutMsg &msg = *popped;
    if (closing_ && std::holds_alternative<Tob>(msg))
      continue; // explicit discard: the close supersedes the pending tick
    // The write path is the SOLE writer of the wire-stamped fields: envelope seq and epoch on
    // every message, svc_ns on OrderAcks — after pop, never before push.
    std::visit(
        [&](auto &m) {
          m.seq = out_seq_++;
          m.epoch = epoch_;
        },
        msg);
    if (auto *ack = std::get_if<OrderAck>(&msg)) {
      assert(!pending_svc_.empty()); // reports are FIFO and never dropped: fronts pair up
      if (!pending_svc_.empty()) {
        ack->svc_ns = pending_svc_.front();
        pending_svc_.pop_front();
      }
    }
    encode_buffer_.clear();
    srv_.codec().encode(msg, encode_buffer_);
    if (encode_buffer_.empty()) {
      // The codec's internal-error signal: never write an empty frame — drop the message and
      // close 1011. The frame is owed even on a refused latch; send_close_frame is idempotent.
      (void)latch_close(websocket::close_code::internal_error, /*drain=*/false);
      send_close_frame();
      return;
    }
    // ONE clock read for both consumers below: m0' and the send-lag start are the SAME instant,
    // immediately before async_write.
    const std::int64_t now = ServerImpl::steady_ns();
    if (const auto *tob = std::get_if<Tob>(&msg)) {
      // m0': the venue-production/delivery boundary. First hand-off per md_seq only; the
      // recorder enforces that.
      if (BenchRecorder *rec = srv_.recorder())
        rec->md_written(tob->md_seq, now);
      if (srv_.cfg().telemetry_verbose)
        srv_.emit_event(TelemetryEvent::TobOut, epoch_, tob->md_seq);
    }
    write_inflight_ = true;
    write_start_ns_ = now;
    ws_.async_write(boost::asio::buffer(encode_buffer_),
                    [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
                      self->on_write(ec, bytes);
                    });
    return;
  }
  if (closing_)
    send_close_frame(); // queued reports drained: the policy close can now go out
}

void Session::on_write(beast::error_code ec, std::size_t) {
  write_inflight_ = false;
  if (torn_down_)
    return;
  Counters &counters = srv_.counters_mut();
  counters.send_lag_max_ns =
      std::max(counters.send_lag_max_ns,
               static_cast<std::uint64_t>(ServerImpl::steady_ns() - write_start_ns_));
  if (ec) {
    // The one-shot idle timeout goes to the FIRST completing op; with a stalled write in
    // flight that op is this write — record 1001 exactly as the read path does.
    const bool idle_timeout =
        ec == beast::error::timeout || (ec == boost::asio::error::operation_aborted && !closing_);
    teardown(closing_ ? static_cast<std::uint16_t>(close_code_) : (idle_timeout ? 1001 : 1006));
    return;
  }
  pump();
}

} // namespace mm::server_detail
