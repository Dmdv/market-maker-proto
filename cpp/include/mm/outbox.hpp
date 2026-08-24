// Per-session outbound queue (plan Task 6): record D6's asymmetric backpressure policy,
// verbatim. Pure container — no IO, no clocks, no locks (a single thread owns each session's
// Outbox, so the counters are plain uint64 by telemetry rule A1) — and therefore fully
// unit-testable without a socket.
//
// WHY ASYMMETRIC. One session carries two producers, and their failure modes are opposites:
//   * ORDER REPORTS ARE NEVER DROPPED. They are state transitions; discarding one leaves the
//     order state undefined. When the report queue breaches its high-water mark the session
//     must CLOSE (1008 policy_violation) — a market maker vastly prefers a known disconnect
//     to an unknown position, so the queue never lies about state by shedding.
//   * MARKET DATA IS CONFLATED to depth 1. A newer top of book SUPERSEDES an older one;
//     only the freshest price has value, so a pending tick is replaced rather than queued.
//   * REPORTS DRAIN FIRST, so a report can never sit behind a burst of ticks.
//
// TYPED MESSAGES, NOT ENCODED FRAMES (audit F-06). The Outbox holds `OutMsg`/`Tob` VALUES;
// the envelope `seq` is stamped and the frame encoded at POP time by the session. Encoding at
// push would burn a `seq` on every tick that is later conflated away and open a gap in the
// envelope sequence — which the client's own rules classify as transport loss and answer with
// StopQuoting + close 1002. Conflation must gap ONLY `md_seq` (assigned per publish by the
// DRIVER — feed.hpp), and that asymmetry is precisely how the client tells deliberate
// conflation from a lost frame. The Outbox therefore writes to no message field, ever.
//
// The two signals are only signals if their consumer checks them: `report_hwm_breached()` is
// the session's close trigger and `conflated()` is the exported drop-attribution metric
// (record §6 patch) that keeps market-data drops countable rather than silent.
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

// A plain class, deliberately: "reports are never dropped" is a claim about the
// allocation-failure path too, but testing it needs no seam in the production API — the suite
// already owns global `operator new` in exactly one TU (cpp/tests/alloc_probe.cpp), and the
// Outbox's failed-enqueue case (cpp/tests/test_outbox_alloc.cpp) is armed from there. A
// template parameter here would have widened the shipped API for a job that whole-binary
// probe already does.
class Outbox {
public:
  // Record D6's mark. Overridable per instance for testability.
  static constexpr std::size_t kDefaultReportHwm = 1024;

  // Throws std::invalid_argument on a zero mark: it would flag a breach on the session's
  // FIRST report, closing it 1008 before it can act (same fail-loudly policy the engine
  // applies to its own nonsensical configurations).
  explicit Outbox(std::size_t report_hwm = kDefaultReportHwm) : report_hwm_{report_hwm} {
    if (report_hwm == 0)
      throw std::invalid_argument("Outbox: report_hwm must be > 0");
  }

  // A copy would duplicate queued UNDROPPABLE state transitions and the breach latch — two
  // owners each delivering "the only copy" of every pending report. IMMOVABLE too (hard
  // gate; supersedes the earlier defaulted moves): move-ASSIGNMENT destroys the
  // DESTINATION's queued reports and overwrites its latch and conflation counter — a
  // never-drop violation by another name — and move-CONSTRUCTION leaves the source's
  // `optional<Tob>` engaged, holding a moved-from ghost tick a later pop would deliver.
  // Task 7 constructs each session's Outbox in place, so transfer semantics buy nothing.
  Outbox(const Outbox &) = delete;
  Outbox &operator=(const Outbox &) = delete;
  Outbox(Outbox &&) = delete;
  Outbox &operator=(Outbox &&) = delete;

  // Queue an order report. NEVER dropped and never conflated — past the mark the queue keeps
  // growing and `report_hwm_breached()` becomes the session's close verdict.
  //
  // TAKEN BY NON-CONST LVALUE REFERENCE, AND CONSUMED LAST, because never-dropped must hold
  // under `bad_alloc` for EVERY call form that compiles. By value, the caller's only copy is
  // moved-from at the CALL, so an allocation failure unwinds it into a destructor. By rvalue
  // reference (`OutMsg &&`, this function's previous shape), a NAMED argument survives the
  // throw — but a prvalue (`push_report(make_ack())`) and an alternative handed over by
  // implicit conversion (`push_report(std::move(ack))`, materializing a temporary OutMsg)
  // also compiled, and there the value dies in the unwind: the caller has nothing left to
  // retry. `OutMsg &` makes those lossy forms UNCOMPILABLE — no temporary can bind to it —
  // so the retryable object is the caller's own named message, by construction (hard gate).
  // Consumption contract: on success `msg` is moved-from; on ANY throw `msg` is untouched
  // and the SAME object can be retried. The one throwing step (the slot reserve) runs while
  // the caller still owns the value, and the enqueue then commits by a move that cannot
  // throw (asserted below).
  //
  // Throws std::invalid_argument on a valueless-by-exception message — every
  // `holds_alternative` answers false on one, so it would pass the Tob guard, count against
  // the report mark, and hand the session's pop-time visitor a std::bad_variant_access —
  // and on a Tob: an `OutMsg` alternative, so the call compiles, and it would defeat the
  // whole policy by queueing an unconflatable, undroppable tick against the report mark.
  // Both guards run before anything is queued, so a throw leaves the Outbox — and `msg` —
  // exactly as they were.
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
  // counts as a conflation. Pushing into a freed slot supersedes nothing and is not counted.
  //
  // TAKEN BY RVALUE REFERENCE (hard gate; supersedes the earlier by-value shape, whose
  // parameter copy made the tick path's allocation-freedom CALL-SITE CONDITIONAL), so THIS
  // FUNCTION never allocates on any call: the slot steals the argument's buffers by nothrow
  // move, and no parameter copy can occur inside the call. The fan-out copy the by-value
  // shape hid in its parameter is now an EXPLICIT call-site decision: Task 7's TOB fan-out
  // to N sessions writes `push_tob(Tob{tob})` for the first N−1 sessions and
  // `push_tob(std::move(tob))` for the last, so every copy — one heap allocation per copied
  // tick once `symbol` outgrows the small-string capacity (libstdc++ 15 / libc++ 22 bytes;
  // the shipped 8-byte "MOCKUSDT" sits inside both) — is visible at exactly the call site
  // that pays for it, and its bad_alloc is that caller's, outside this noexcept boundary.
  //
  // noexcept AT THE SIGNATURE (hard gate): the body is a counter increment and a nothrow
  // move into the slot (asserted below), and the rvalue binding is what removes the one
  // operation — the parameter copy — that could throw inside the previous shape's call.
  // Task 7 calls this from asio handlers, where an escaping exception terminates the
  // process; the signature now states what "non-throwing in effect" only promised.
  void push_tob(Tob &&msg) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Tob> &&
                      std::is_nothrow_move_assignable_v<Tob>,
                  "the slot install is the noexcept signature's one real operation: it must "
                  "move, into an engaged slot or an empty one, without throwing");
    if (tob_.has_value())
      ++conflated_;
    tob_ = std::move(msg);
  }

  // Next message for the session to stamp, encode and write. Reports first — all of them —
  // then the pending tick. Under sustained report flow the tick therefore waits; that is the
  // policy, and conflation bounds what waiting costs (at most one tick is ever pending).
  //
  // One local, engaged in place: the two-local form — an OutMsg local returned from each
  // branch — always costs TWO moves, because no elision is possible across the OutMsg →
  // optional<OutMsg> return (the types differ; what remains is the guaranteed implicit move
  // into the optional's converting constructor). The emplace form costs one move under NRVO
  // and two without, so it is never worse. noexcept AT THE SIGNATURE (hard gate): the
  // commits are moves of nothrow-movable messages — asserted in push_report for the report
  // branch and below for the tick branch's Tob → OutMsg emplacement — and the
  // pop_front/reset mutation runs strictly AFTER the move. Task 7 calls pop() from asio
  // completion handlers, where an escaping exception terminates the process; the signature
  // now states what "non-throwing in effect" only promised.
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

  // True once the report queue has EXCEEDED the mark; the session must then close 1008.
  // The mark bounds PENDING DEPTH, not lifetime report volume: a session that drains promptly
  // may emit unboundedly many reports and never breach, because what the mark measures is how
  // far the writer has fallen behind — not how much work it has done.
  // LATCHING, deliberately: the verdict is never LOST to a subsequent drain (a level reading
  // would be cleared by the very drain the breach proves is too slow), so it cannot be missed.
  // It must STILL be tested on the SAME event-loop turn as the push that set it — the mark is
  // OBSERVED, not enforced (`push_report` keeps enqueueing past it), so the deque is unbounded
  // until the session acts. The latch guarantees detection, not promptness.
  [[nodiscard]] bool report_hwm_breached() const { return report_hwm_breached_; }

  // Ticks superseded by a newer one — the exported metric that makes every market-data drop
  // attributable (the project's count-every-drop discipline).
  [[nodiscard]] std::uint64_t conflated() const { return conflated_; }

  // Messages pending: queued reports plus the pending tick, i.e. exactly how many pops
  // remain before pop() yields nullopt.
  [[nodiscard]] std::size_t depth() const { return reports_.size() + (tob_.has_value() ? 1U : 0U); }

private:
  std::size_t report_hwm_;
  bool report_hwm_breached_{false};
  std::uint64_t conflated_{0}; // single-writer plain uint64 (telemetry rule A1)
  // DOCUMENTED ALLOCATION EXCEPTION (Global Constraints, "allocation discipline, honestly
  // scoped"): this deque allocates and frees one node per `__deque_buf_size` reports in steady
  // state — ≈1 malloc + 1 free per 4 reports on libstdc++, where `sizeof(OutMsg)` is 128 (104
  // on the libc++ dev host) and the node holds 512/128 elements. Construction itself
  // allocates twice on libstdc++ — the deque's map array plus its preallocated first node —
  // and zero times on the libc++ dev host (the same global-operator-new-counter measurement):
  // a PER-SESSION cost, not per-message, since Task 7 constructs one Outbox per connection.
  // The tick path below is allocation-free by construction. The portable halves of this
  // paragraph are PINNED by cpp/tests/test_outbox_alloc.cpp (tick push+pop and tick
  // supersession exactly zero, at a symbol past both SSO capacities; report churn — push
  // alone and push-then-full-drain — bounded above by the push count); the per-node and
  // construction figures are stdlib-dependent and remain measurement, not oracle. The §6
  // allocation counter mandated by that same Global-Constraints bullet must attribute this.
  std::deque<OutMsg> reports_;
  std::optional<Tob> tob_; // the slot of one
};

// Outside the class only because the type is incomplete inside its own definition. All four
// transfer members pinned DELETED (hard gate — the earlier form pinned the moves PRESENT;
// the rationale for deleting them sits at the members).
static_assert(!std::is_copy_constructible_v<Outbox> && !std::is_copy_assignable_v<Outbox> &&
              !std::is_move_constructible_v<Outbox> && !std::is_move_assignable_v<Outbox>);

} // namespace mm
