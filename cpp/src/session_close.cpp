// The close half of one WebSocket session: close-intent latching, the drain-then-close
// handshake, epoch retirement and teardown. The read -> write path stays in session.cpp.
#include "server_impl.hpp"

#include <boost/asio/buffer.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace mm::server_detail {

bool Session::latch_close(websocket::close_code code, bool drain) {
  if (closing_ || torn_down_)
    return false; // first latch keeps its code; a second must not rewrite it
  closing_ = true;
  close_code_ = code;
  drain_then_close_ = drain;
  // Retire the epoch at close INTENT, not at teardown: the session stays in the server's map for
  // the whole drain, and a live epoch there keeps drawing fills that are counted and then dropped.
  retire_epoch();
  return true;
}

void Session::begin_close(websocket::close_code code) {
  // POLICY closes (1008 report-HWM / entry-cap, 1001 going_away) drain the queued reports first —
  // never silent loss; PROTOCOL closes (1002) go out on the next free write slot instead.
  if (!latch_close(code, code == websocket::close_code::policy_error ||
                             code == websocket::close_code::going_away))
    return;
  pump();
}

void Session::drain_transport() {
  if (torn_down_)
    return;
  // Raw-socket drain (the WebSocket stream is failed and unreadable at this point): consume the
  // peer's in-flight bytes until it acknowledges the close or the grace expires.
  grace_timer_.expires_after(kCloseGrace);
  grace_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (!ec && !self->torn_down_)
      beast::get_lowest_layer(self->ws_).close();
  });
  drain_read();
}

void Session::drain_read() {
  beast::get_lowest_layer(ws_).socket().async_read_some(
      boost::asio::buffer(drain_buffer_),
      [self = shared_from_this()](beast::error_code ec, std::size_t) {
        if (ec) {
          self->teardown(static_cast<std::uint16_t>(self->close_code_));
          return;
        }
        self->drain_read();
      });
}

void Session::send_close_frame() {
  if (close_frame_sent_ || torn_down_ || write_inflight_)
    return;
  close_frame_sent_ = true;
  ws_.async_close(websocket::close_reason{close_code_},
                  [self = shared_from_this()](beast::error_code ec) {
                    if (ec)
                      self->teardown(static_cast<std::uint16_t>(self->close_code_));
                    // else: the read loop observes the peer's reply as error::closed.
                  });
  // A peer that never answers the close frame must not pin this session (nor a stop() behind
  // it) to the idle timeout: past the grace, sever the transport and let teardown run.
  grace_timer_.expires_after(kCloseGrace);
  grace_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (!ec && !self->torn_down_)
      beast::get_lowest_layer(self->ws_).close();
  });
}

void Session::retire_epoch() {
  if (epoch_retired_)
    return;
  // Cancel-on-disconnect: DESTRUCTIVE, and valid exactly once — the acks are dropped with the
  // session. They never enter the outbox, so the count is carried out to teardown from here.
  const auto acks = srv_.engine().end_session(epoch_);
  retired_acks_ = acks.size();
  // The latch is set AFTER the call: end_session stages its whole ack batch before it touches an
  // order, so a throw leaves every order Live and the call RETRYABLE. Latching first spends that.
  epoch_retired_ = true;
}

void Session::teardown(std::uint16_t recorded_code) {
  if (torn_down_)
    return;
  torn_down_ = true;
  if (write_inflight_) {
    // Fold the IN-FLIGHT write into the watermark before it is lost: on_write's torn_down_
    // guard returns without sampling, and by then finalize() has already exported the counters.
    Counters &counters = srv_.counters_mut();
    counters.send_lag_max_ns =
        std::max(counters.send_lag_max_ns,
                 static_cast<std::uint64_t>(ServerImpl::steady_ns() - write_start_ns_));
  }
  // The paths that never went through begin_close land here directly — an idle-timeout
  // reap, a peer's own close, a dead transport.
  retire_epoch();
  grace_timer_.cancel();
  beast::get_lowest_layer(ws_).close();
#ifndef NDEBUG
  if (outbox_.depth() > 0 || retired_acks_ > 0)
    std::fprintf(stderr,
                 "mm: session %llu: %zu undelivered message(s) and %zu discarded "
                 "cancel-on-disconnect ack(s) died with it\n",
                 static_cast<unsigned long long>(epoch_), outbox_.depth(), retired_acks_);
#endif
  srv_.session_closed(epoch_, recorded_code, outbox_.conflated());
}

} // namespace mm::server_detail
