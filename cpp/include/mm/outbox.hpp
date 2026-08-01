// Per-session outbound queue implementing the asymmetric backpressure policy. Pure container:
// no IO, no clocks, no locks — a single thread owns each session's Outbox.
//
// WHY ASYMMETRIC: reports are state transitions — NEVER dropped; at the mark the session closes
// 1008, a known disconnect beating an unknown position. Market data conflates to depth 1.
//
// Holds typed VALUES, not encoded frames: `seq` is stamped at POP time, so conflation gaps only
// `md_seq` and never the envelope seq, which the client would read as transport loss.
#pragma once

#include "mm/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace mm {

// Not a template: "reports are never dropped" covers the allocation-failure path, but the
// suite's global operator-new probe tests that without a seam in the shipped API.
class Outbox {
public:
  // Default high-water mark; overridable per instance for testability.
  static constexpr std::size_t kDefaultReportHwm = 1024;

  // Throws on a zero mark: it would breach on the session's FIRST report, closing it 1008
  // before it can act.
  explicit Outbox(std::size_t report_hwm = kDefaultReportHwm) : report_hwm_{report_hwm} {
    if (report_hwm == 0)
      throw std::invalid_argument("Outbox: report_hwm must be > 0");
  }

  // Non-copyable AND immovable: copying or move-assigning would duplicate or destroy queued
  // UNDROPPABLE reports and the breach latch, and move-construction leaves a ghost tick behind.
  Outbox(const Outbox &) = delete;
  Outbox &operator=(const Outbox &) = delete;
  Outbox(Outbox &&) = delete;
  Outbox &operator=(Outbox &&) = delete;

  // Queue an order report: never dropped, never conflated. Past the mark the queue keeps
  // growing and `report_hwm_breached()` becomes the session's close verdict.
  //
  // TAKEN BY NON-CONST LVALUE REFERENCE so no temporary can bind: on ANY throw the caller still
  // owns `msg` and can retry it; on success it is moved-from. The reserve is the only throw site.
  //
  // Throws std::invalid_argument on a valueless message (it would pass the guards and hand the
  // pop-time visitor a bad_variant_access) and on a Tob (an undroppable, unconflatable tick).
  void push_report(OutMsg &msg) {
    static_assert(std::is_nothrow_move_assignable_v<OutMsg>,
                  "the commit step must not throw, or a failed enqueue could leave a phantom "
                  "default-constructed report in the queue");
    static_assert(std::is_nothrow_move_constructible_v<OutMsg>,
                  "pop() commits by moving the message into its returned optional; a throwing "
                  "move could leave a half-moved report in the queue");
    if (msg.valueless_by_exception())
      throw std::invalid_argument(
          "Outbox::push_report: message is valueless_by_exception (no report to queue)");
    if (std::holds_alternative<Tob>(msg))
      throw std::invalid_argument(
          "Outbox::push_report: top of book must go through push_tob (it is conflated)");
    reports_.emplace_back();          // reserve: the only step that can throw, `msg` untouched
    reports_.back() = std::move(msg); // commit: noexcept, so the slot is never left empty
    if (reports_.size() > report_hwm_)
      report_hwm_breached_ = true;
  }

  // Install the pending top of book. Slot of one: a newer tick REPLACES the pending one and
  // counts as a conflation; pushing into a freed slot supersedes nothing and is not counted.
  //
  // RVALUE REFERENCE, noexcept: the slot steals the argument's buffers by nothrow move, so the
  // call never allocates and cannot throw in an asio handler; fan-out copies stay call-site.
  void push_tob(Tob &&msg) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Tob> &&
                      std::is_nothrow_move_assignable_v<Tob>,
                  "the slot install is the noexcept signature's one real operation: it must "
                  "move, into an engaged slot or an empty one, without throwing");
    if (tob_.has_value())
      ++conflated_;
    tob_ = std::move(msg);
  }

  // Next message for the session to stamp, encode and write: all pending reports first, then
  // the tick. Under sustained report flow the tick waits — conflation bounds that to one tick.
  //
  // noexcept: both commits are moves of nothrow-movable messages (asserted here and in
  // push_report) and the pop_front/reset runs strictly AFTER the move; asio handlers call this.
  [[nodiscard]] std::optional<OutMsg> pop() noexcept {
    static_assert(std::is_nothrow_constructible_v<OutMsg, Tob &&>,
                  "the tick branch emplaces the returned OutMsg from the pending Tob; a "
                  "throwing conversion would break the noexcept signature");
    std::optional<OutMsg> out;
    if (!reports_.empty()) {
      out.emplace(std::move(reports_.front()));
      reports_.pop_front();
    } else if (tob_.has_value()) {
      out.emplace(std::move(*tob_));
      tob_.reset();
    }
    return out;
  }

  // True once PENDING reports EXCEED the mark (writer backlog, not lifetime volume): the session
  // must then close 1008. Latching, so a drain cannot clear it — but pushes continue past it.
  [[nodiscard]] bool report_hwm_breached() const { return report_hwm_breached_; }

  // Ticks superseded by a newer one — the exported metric that keeps every market-data drop
  // attributable rather than silent.
  [[nodiscard]] std::uint64_t conflated() const { return conflated_; }

  // Messages pending: queued reports plus the pending tick — the pops remaining before
  // pop() yields nullopt.
  [[nodiscard]] std::size_t depth() const { return reports_.size() + (tob_.has_value() ? 1U : 0U); }

private:
  std::size_t report_hwm_;
  bool report_hwm_breached_{false};
  std::uint64_t conflated_{0}; // single-writer, one thread
  // DOCUMENTED ALLOCATION EXCEPTION: the deque allocates a node per few reports plus one or two
  // at construction — per session, not per message. The tick path is allocation-free, and pinned.
  std::deque<OutMsg> reports_;
  std::optional<Tob> tob_; // the slot of one
};

// Outside the class only because the type is incomplete inside its own definition.
static_assert(!std::is_copy_constructible_v<Outbox> && !std::is_copy_assignable_v<Outbox> &&
              !std::is_move_constructible_v<Outbox> && !std::is_move_assignable_v<Outbox>);

} // namespace mm
