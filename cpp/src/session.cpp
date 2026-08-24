// One WebSocket session (plan Task 7): the read -> decode -> engine -> outbox -> write
// state machine for a single connection, on the owner thread. The WebSocket discipline
// items (close-code table, frame policy, size bounds) are enforced HERE. Shared
// declarations — and the authoritative index of which TU owns which half of the server —
// are in cpp/src/server_impl.hpp; this file does not restate the routing, because two
// copies of it are what drifted apart last time.
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

// The scan bound below assumes StaleEpoch is the TERMINAL enumerator; a code appended
// after it would be unmapped, and every engine verdict carrying it would go silently
// unnarrated. to_string's backstop is the falsifiable form of that assumption: every
// in-range enumerator returns a normative spelling from its exhaustive switch, so the
// first ordinal past the end must fall through to "UNKNOWN" — and stops doing so the
// moment a code is appended. The same count is the width of Session::reject_latch_.
constexpr int kRejectCodeCount = static_cast<int>(RejectCode::StaleEpoch) + 1;
static_assert(to_string(static_cast<RejectCode>(kRejectCodeCount)) == "UNKNOWN",
              "RejectCode::StaleEpoch is no longer the last enumerator: extend the scan in "
              "reject_code_from_wire, or an appended code goes unmapped with nothing failing");
static_assert(kRejectCodeCount <= 16, "Session::reject_latch_ is a 16-bit coalescing latch");

// Engine rejects come back with the WIRE spelling in Reject::code; the reject telemetry
// event's lane 1 carries the numeric RejectCode (telemetry_ring.hpp lane table), so the
// spelling maps back through the one normative to_string. Rejects are off the measured
// path, so a 16-entry scan is the simple, obviously-right shape.
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
  // NO control_callback, in ANY build. The debug-only fprintf that used to sit here put one
  // unbuffered write syscall on the OWNER thread — which also runs the acceptor, every other
  // session, the feed timer and the telemetry pump — per ping/pong/close, at a rate the PEER
  // chooses: measured under the dev preset (the asan/tsan gates' own configuration) at 84,769
  // lines and 2.4 MB of stderr from 3 s of one client's empty pings. Beast sends its automatic
  // pong whether or not a callback is installed, and the frame kinds stay observable in the
  // close-code telemetry, so nothing but the syscalls is lost.
}

void Session::start(std::uint64_t epoch) {
  epoch_ = epoch;
  read_next();
  // Deliberately nothing is SENT here: the first outbound message of any session is the
  // next feed publish's TOB (plan: "send nothing until first feed publish").
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
    // Either the peer's clean close (record ITS code — Beast already replied) or the
    // reply completing a close WE initiated (record ours).
    //
    // The peer's code is NORMALISED exactly as Beast normalises the reply it sends: an
    // EMPTY close body is legal and conformant (a browser's argument-less
    // WebSocket.close() sends one), read_close leaves reason().code at close_code::none =
    // 0, and Beast answers it 1000. Recording the raw 0 would put a value that is not a
    // close code at all into session_close args[1], breaking the same invariant the
    // 1006-vs-1002/1007 fix exists for: the record must be the code that went out.
    // reason().code is a std::uint16_t, so both arms are widened to it rather than compared
    // as close_code: mixing the enumerator with the raw field is a -Wextra diagnostic on
    // g++ (the authoritative container toolchain) that Apple clang does not emit.
    const std::uint16_t peer = ws_.reason().code;
    constexpr auto kNone = static_cast<std::uint16_t>(websocket::close_code::none);
    constexpr auto kNormal = static_cast<std::uint16_t>(websocket::close_code::normal);
    teardown(closing_ ? static_cast<std::uint16_t>(close_code_) : (peer == kNone ? kNormal : peer));
    return;
  }
  if (ec == beast::error::timeout || (ec == boost::asio::error::operation_aborted && !closing_)) {
    // Idle timeout: keep-alive pings went unanswered, so the peer is dead. 1001 cannot
    // be DELIVERED to it — Beast has already severed the transport — only recorded.
    // The second disjunct is the same event by another spelling: Beast delivers the
    // timeout "to the first caller" (stream_impl.hpp check_stop_now), and when an
    // internal keep-alive op consumes it first, this read completes operation_aborted.
    // No OTHER source of operation_aborted reaches here: this session cancels reads
    // only through teardown (guarded above) or while closing_ (grace-timer severs).
    // A close already under way keeps ITS code, exactly as the generic branch below
    // does. Measured against Boost 1.90 this arm is the BELT: while closing_ is set there
    // is always another Beast op in play — the close op after send_close_frame, or the
    // stalled write during a drain — and it takes the one-shot timeout first, so the
    // read lands on the generic branch instead. The two must still agree, because the
    // moment they do not, an operator reading a policy close's telemetry is told "the
    // server shut down" for a close their client earned.
    teardown(closing_ ? static_cast<std::uint16_t>(close_code_) : 1001);
    return;
  }
  if (ec == websocket::error::message_too_big) {
    // Past the TRANSPORT ceiling mid-reassembly (server_impl.hpp size-bound note): no
    // clean message boundary remains, so this is the 1009 close. Beast reaches this
    // completion only AFTER running that close itself — the close frame, then its TCP
    // teardown of shutdown(send), drain to EOF, close (websocket/impl/read.hpp's `close:`
    // path into websocket/impl/teardown.hpp) — so the async_close and the raw drain below
    // are belts over work already done, covering only a socket that outlived it. The
    // epoch retirement is NOT a belt: it is this session's only claim on the epoch, and
    // this branch used to declare close intent without making it.
    if (!latch_close(websocket::close_code::too_big, /*drain=*/false)) {
      // An in-flight close keeps ITS code and this arm must not re-arm async_close — but it
      // still owes CONVERGENCE (latch_close's caller contract). Returning bare left the
      // session with no re-armed operation on a transport Beast has already severed, so
      // nothing would ever reap it. Teardown is the honest action for exactly that reason.
      teardown(static_cast<std::uint16_t>(close_code_));
      return;
    }
    close_frame_sent_ = true;
    ws_.async_close(websocket::close_reason{websocket::close_code::too_big},
                    [self = shared_from_this()](beast::error_code) { self->drain_transport(); });
    return;
  }
  if (!closing_ && ec == websocket::condition::protocol_violation) {
    // Malformed RFC 6455 framing (illegal opcode, reserved bits, an unmasked client
    // frame, a non-canonical size): Beast fails the read AND sends the close itself, so
    // the RECORD has to be the code that actually went out — 1007 where the payload was
    // not valid UTF-8 (Beast's close_code::bad_payload), 1002 for every other violation.
    // Falling through to the 1006 branch below would narrate an abnormal closure to the
    // telemetry file while the peer held a well-formed protocol close in its hand.
    teardown(static_cast<std::uint16_t>(ec == websocket::error::bad_frame_payload
                                            ? websocket::close_code::bad_payload
                                            : websocket::close_code::protocol_error));
    return;
  }
  if (ec) {
    // Transport death without a close handshake: record our own intent code if a close
    // was already under way, else 1006 (the reserved abnormal-closure code — never sent
    // on the wire, exactly why it is the honest record here).
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
  // M2 STARTS HERE — after a successful decode, which is what §5.1 specifies ("C++ e1 after
  // message decode"). Stamped only on the path that reaches the engine: a frame rejected above
  // for size or a decode error never enters engine handling, so it has no service time to report.
  const std::int64_t e1_ns = ServerImpl::steady_ns();
  on_command(std::get<InMsg>(decoded), arrival_ns, e1_ns);
  pump();
}

void Session::on_command(InMsg &msg, std::int64_t arrival_ns, std::int64_t e1_ns) {
  // Envelope checks first, in the plan's order: seq contiguity (a gap is a CLIENT bug —
  // the engine never gaps, so close 1002), then epoch (a stale epoch is a WELL-FORMED
  // command from a previous life: Reject{StaleEpoch}, session stays up — F-12).
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
      // engine, not when the engine finished decoding or serving it. Decode is part of the
      // tick-to-order path M3 measures, so excluding it here would hide real reaction latency —
      // which is the opposite of the M2 boundary rule two lines above, and deliberately so.
      // First echo only (recorder rule).
      rec->order_for(order->md_seq, arrival_ns);
    }
    for (OutMsg &report : batch) {
      if (std::holds_alternative<OrderAck>(report)) {
        ++counters.orders;
        pending_svc_.push_back(svc); // stamped into the ack at pop time ((r)7)
      } else if (const auto *reject = std::get_if<Reject>(&report)) {
        ++counters.rejects;
        if (const auto code = reject_code_from_wire(reject->code))
          emit_reject_event(*code);
      }
      enqueue_report(report); // named lvalue, no move at the call site ((r)5)
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
  check_policy_closes(); // (r)6: the SAME turn as the push that may have latched the mark
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
  // COALESCED, never gated. The telemetry_verbose gate that cmd_in and tob_out use is not
  // available here — "a malformed frame draws a reject line" is a pinned contract, and a
  // build that gated it fails that case — so the RATE is bounded instead: one event per
  // (epoch, code) per kRejectCoalesceWindow, tying a peer-controlled emission to the
  // snapshot cadence rather than to the peer's frame rate. Nothing is lost: counters.rejects
  // carries the volume, and a session's FIRST reject of any code always narrates, which is
  // the form every assertion on this event takes.
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
  // (r)6: the mark is OBSERVED, not enforced — this check runs on the SAME event-loop
  // turn as every push that could have latched it, or the deque grows unbounded behind a
  // socket that is already too slow.
  if (closing_)
    return;
  if (outbox_.report_hwm_breached()) {
    // Lane 1 is the queue depth at the breach; the pending tick can inflate it by at
    // most one over the pure report count (the Outbox exposes no report-only depth).
    srv_.emit_event(TelemetryEvent::HwmClose, epoch_, static_cast<std::uint64_t>(outbox_.depth()));
    begin_close(websocket::close_code::policy_error);
    return;
  }
  // Item (i): rejected-order tombstones are not gated by max_live_orders, so this check
  // is the ONLY backpressure on peer-driven entry growth; the engine keeps recording
  // past the cap and the signal is only a signal because this consumer checks it.
  if (srv_.engine().entry_cap_breached(epoch_)) {
    // Its OWN record, because the two 1008 causes are otherwise separable only by the
    // ABSENCE of an hwm_close line — and absence is not evidence: that line ships through
    // a fire-and-forget try_push that a full ring drops. Lane 1 is the entry count at the
    // breach (telemetry_ring.hpp lane table).
    srv_.emit_event(TelemetryEvent::EntryCapClose, epoch_,
                    static_cast<std::uint64_t>(srv_.engine().entry_count(epoch_)));
    begin_close(websocket::close_code::policy_error);
  }
}

void Session::deliver_report(OutMsg &&msg) {
  if (torn_down_)
    return; // the engine reaped this session's orders at teardown; nothing routes here
  // Feed-routed reports are FILLS, only: OrderEngine::on_book's sweep is the sole producer
  // of Routed and emits nothing else. That matters to pending_svc_, whose FIFO pairs each
  // measured svc_ns window with the ack of the command that COST it — the placeholder push
  // that used to sit here served an ack this path cannot carry, and a placeholder for an
  // impossible path is a false claim about the invariant. The assert is that claim in
  // falsifiable form: a later engine change routing acks here fires it instead of silently
  // mispairing every subsequent svc_ns.
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
    // A REFERENCE, not a move out of the optional: `popped` is the while-condition variable
    // and lives for the whole iteration, so the lifetimes are identical — while the move was
    // real work on the measured path, OutMsg being 104 bytes whose active alternative is
    // known only at runtime (9.4 -> 6.8 ns/message, min-of-7, three repeats).
    OutMsg &msg = *popped;
    if (closing_ && std::holds_alternative<Tob>(msg))
      continue; // explicit discard ((r)10): the close supersedes the pending tick
    // The write path is the SOLE writer of the wire-stamped fields ((r)7): envelope seq
    // and epoch on every message, svc_ns on OrderAcks — after pop, never before push.
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
      // The codec's internal-error signal (amendment (e)): never write an empty frame —
      // drop the message and close. 1011 (internal error) is the honest code: the peer
      // did nothing wrong. The popped report dies WITH the session, which is (r)12's
      // required consequence — no error branch between pop() and the socket may discard
      // the message yet keep the session alive.
      //
      // The latch may be REFUSED (a close already under way), and the frame is owed either
      // way — on a refusal the first latch keeps its code but still needs the frame this
      // arm was about to send, and returning bare would strand a session whose only pending
      // operation was the write just abandoned (latch_close's caller contract).
      // send_close_frame is idempotent under close_frame_sent_/write_inflight_, so the
      // unconditional call cannot re-arm a close already going out.
      (void)latch_close(websocket::close_code::internal_error, /*drain=*/false);
      send_close_frame();
      return;
    }
    // ONE clock read for both consumers below: m0' and the send-lag start are the SAME
    // instant — "immediately before async_write", which is what both comments already
    // claimed while taking two reads (~14 ns each) five statements apart.
    const std::int64_t now = ServerImpl::steady_ns();
    if (const auto *tob = std::get_if<Tob>(&msg)) {
      // m0': the venue-production/delivery boundary (A3 addendum). First hand-off per
      // md_seq only; the recorder enforces that.
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
    // The one-shot idle timeout is delivered to the FIRST completing op (stream_impl.hpp
    // check_stop_now) — and with a peer that stopped reading, the op in flight is this
    // WRITE, not the read: under sanitizer slowdown the write consumes the timeout
    // routinely, on a fast host the read does. The two arms must record the same verdict
    // for the same event, or the close code becomes a scheduling artifact (observed in
    // CI: the idle-reap case recorded 1006 or 1001 by runner load). Mirror the read
    // path's mapping exactly, both disjuncts and for the same reasons stated there.
    const bool idle_timeout =
        ec == beast::error::timeout || (ec == boost::asio::error::operation_aborted && !closing_);
    teardown(closing_ ? static_cast<std::uint16_t>(close_code_) : (idle_timeout ? 1001 : 1006));
    return;
  }
  pump();
}

} // namespace mm::server_detail
