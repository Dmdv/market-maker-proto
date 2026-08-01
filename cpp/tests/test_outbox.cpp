// tests: the per-session Outbox and its asymmetric backpressure policy (the design).
//
// The policy IS the deliverable here, and it is deliberately asymmetric, so every RULE gets
// its own case and every case is written to be FALSIFIABLE — deleting the rule from
// outbox.hpp must red exactly the case that names it. The rules, in the order they appear
// below: an empty outbox pops nullopt and reports nothing; reports are FIFO; EVERY report
// alternative round-trips intact; market data conflates to a slot of one with the newest
// winning; "newest" is the last ARRIVAL, not the largest md_seq; the Outbox stamps no
// envelope `seq`; reports drain BEFORE market data (all of them, not one per tick);
// conflation is COUNTED (and only on supersession); the default mark is 1024; reports are
// NEVER dropped and the high-water mark is a close signal rather than a drop; the breach
// LATCHES; reports pushed AFTER the breach are still queued (the mark is observed, never
// enforced); the mark measures PENDING DEPTH rather than lifetime report volume and counts
// reports only (including while a tick is pending); and the two programming-error guards
// cased HERE (a Tob smuggled in as a report — leaving every piece of queued state untouched,
// the smuggled message included — and a zero mark) throw. The THIRD guard (a
// valueless-by-exception message) is cased in the allocation TU below, whose probe is the
// only way to construct its fixture. The signature rules are compile-time pins rather than
// cases — the static_asserts after these comments: the noexcept trio, plus (hardgate-fix-2)
// two exact member-pointer pins that red the build the moment either push gains ANY
// overload — and one more is pinned by the call sites themselves: `push_report` binds ONLY
// a named lvalue, so every case below names the
// report it pushes and passes it without a move — the same call form the server layer's write path
// uses.
//
// The ALLOCATION rules of the set are pinned in a different TU: "a report the queue could not
// allocate room for is left with its caller, not consumed" needs a failing allocation, which
// is a WHOLE-BINARY property — the suite replaces global `operator new` exactly once, in
// `cpp/tests/alloc_probe.cpp` — so that case (and the tick path's allocation-free pin) lives
// in `cpp/tests/test_outbox_alloc.cpp`, armed on that probe. Both keep the `outbox:` name
// prefix and the `[outbox]` tag, so `ctest -R outbox` and `./mm_tests "[outbox]"` still
// select them with the cases below; that TU also pins the tick-path and supersession-burst
// zeros and the report-path churn bounds, and cases the valueless-report guard.
//
// Values are asserted on STRUCT FIELDS, never on encoded strings: the Outbox
// holds typed messages precisely so the frame — and the envelope `seq` in it — is produced at
// pop time by the session. A string-level assertion here would pass just as happily against
// the pre-encoded design rejected.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "mm/outbox.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

// Signature pins, compile-time: `pop()` and `push_tob()` are noexcept — the server
// layer calls both from asio completion handlers, where an escaping exception terminates the
// process — and `push_report()` is deliberately NOT noexcept: its programming-error guards
// throw, and that is its contract. Deleting either `noexcept` reds the build here, the same
// falsifiability discipline the runtime cases follow. (The four deleted copy/move members
// are pinned by outbox.hpp's own namespace-scope static_assert.)
static_assert(noexcept(std::declval<mm::Outbox &>().pop()));
static_assert(noexcept(std::declval<mm::Outbox &>().push_tob(std::declval<mm::Tob &&>())));
static_assert(!noexcept(std::declval<mm::Outbox &>().push_report(std::declval<mm::OutMsg &>())));

// The exclusive call forms, pinned as EXACT member-pointer types (hardgate-fix-2): the
// noexcept pins above and the named-lvalue call sites below all still hold if an OVERLOAD is
// added — a by-value or rvalue `push_report`, an lvalue `push_tob` — and any of those quietly
// restores the lossy forms this suite exists to keep uncompilable, with every existing call
// site compiling unchanged and the suite green. Taking `&mm::Outbox::push_report` is itself
// the overload-introduction trap: the moment a second overload exists the address is
// ambiguous and these lines fail to compile, which is the intent.
static_assert(
    std::is_same_v<decltype(&mm::Outbox::push_report), void (mm::Outbox::*)(mm::OutMsg &)>);
static_assert(
    std::is_same_v<decltype(&mm::Outbox::push_tob), void (mm::Outbox::*)(mm::Tob &&) noexcept>);

namespace {

// A top of book carrying its own md_seq: it is assigned per publish by the DRIVER (feed.hpp),
// so a conflated tick is visible to the client as an md_seq GAP — the whole point of the
// counter.
mm::Tob tob(std::uint64_t md_seq, std::int64_t bid_px, std::int64_t ask_px) {
  mm::Tob t;
  t.md_seq = md_seq;
  t.symbol = "MOCKUSDT";
  t.bid_px = bid_px;
  t.bid_qty = 10;
  t.ask_px = ask_px;
  t.ask_qty = 20;
  return t;
}

mm::OutMsg ack(std::string cl_id) {
  mm::OrderAck a;
  a.cl_id = std::move(cl_id);
  a.eng_id = 1;
  a.status = "live";
  return a;
}

// One builder per NON-Tob alternative — all four are reports, and "never dropped" is a claim
// about every one of them, not just the acknowledgement the FIFO cases happen to use. Each
// field is set to a distinct non-default value so a round trip that loses or rewrites one is
// visible; the envelope (`v`, `seq`, `epoch`) is deliberately LEFT AT ITS DEFAULT, because
// says the Outbox writes to no message field and the session stamps `seq` at pop.
mm::CancelAck cancel_ack() {
  mm::CancelAck m;
  m.cl_id = "c-cancel";
  m.eng_id = 12;
  m.status = "cancelled";
  return m;
}

mm::Reject reject() {
  mm::Reject m;
  m.cl_id = "c-reject";
  m.code = "UnknownClOrdId";
  m.reason = "no such order";
  return m;
}

mm::Fill fill() {
  mm::Fill m;
  m.cl_id = "c-fill";
  m.eng_id = 14;
  m.px = 1005;
  m.qty = 30;
  m.leaves = 70;
  m.exec_id = 99;
  return m;
}

// The cl_id an OrderAck carries, read off the STRUCT.
std::string cl_id_of(const mm::OutMsg &m) {
  const auto *a = std::get_if<mm::OrderAck>(&m);
  return a != nullptr ? a->cl_id : "<not-an-order-ack>";
}

// Pop-and-identify helpers, each mapping the two ways a pop can disappoint onto a value the
// CALL SITE's own comparison rejects — an empty pop and a wrong-alternative pop both red the
// assertion that named the message, with the reason in the failure text. Deliberately no
// REQUIRE in here: an abort inside a helper reports the helper's line, and it is also
// invisible to clang-tidy's optional-engagement analysis, which then flags every access.
std::string popped_ack(mm::Outbox &o) {
  const auto m = o.pop();
  if (!m.has_value())
    return "<empty>";
  return cl_id_of(*m);
}

// A default Tob (md_seq 0, empty symbol) on either disappointment: every call site asserts on
// md_seq and every tick BELOW carries a non-zero one, so the sentinel can never pass for a
// real tick. Deliberately not justified by "the producer numbers from 1" — engine.hpp says the
// opposite (a first md_seq of 0 is legal), so the guarantee has to come from this file.
mm::Tob popped_tob(mm::Outbox &o) {
  const auto m = o.pop();
  if (!m.has_value())
    return {};
  const auto *t = std::get_if<mm::Tob>(&*m);
  return t != nullptr ? *t : mm::Tob{};
}

// Same contract for an arbitrary alternative: a DEFAULT-CONSTRUCTED T on either disappointment.
// Every call site compares against a value with non-default fields, so the sentinel cannot pass.
template <class T> T popped(mm::Outbox &o) {
  const auto m = o.pop();
  if (!m.has_value())
    return T{};
  const auto *v = std::get_if<T>(&*m);
  return v != nullptr ? *v : T{};
}

} // namespace

TEST_CASE("outbox: an empty outbox pops nullopt and reports nothing", "[outbox]") {
  mm::Outbox o;
  CHECK(o.depth() == 0);
  CHECK(o.conflated() == 0);
  CHECK_FALSE(o.report_hwm_breached());
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: reports pop in FIFO order", "[outbox]") {
  mm::Outbox o;
  mm::OutMsg a = ack("A");
  mm::OutMsg b = ack("B");
  mm::OutMsg c = ack("C");
  o.push_report(a);
  o.push_report(b);
  o.push_report(c);
  CHECK(o.depth() == 3);

  CHECK(popped_ack(o) == "A");
  CHECK(popped_ack(o) == "B");
  CHECK(popped_ack(o) == "C");
  CHECK(o.depth() == 0);
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: every report alternative round-trips field for field", "[outbox]") {
  // "Reports are never dropped" covers ALL FOUR non-Tob alternatives, not just the OrderAck
  // the ordering cases use — an implementation that swallowed or rewrote a Fill would still
  // satisfy every FIFO assertion in this file. Full-struct equality also pins on the
  // REPORT path (the conflation case pins it on the tick path): each original leaves the
  // envelope at its default, so a push_report that stamped `seq` would break equality here.
  const mm::OrderAck a = std::get<mm::OrderAck>(ack("c-ack"));
  const mm::CancelAck c = cancel_ack();
  const mm::Reject r = reject();
  const mm::Fill f = fill();

  mm::Outbox o;
  // Named OutMsg copies of the four originals: push_report binds only a named lvalue, and
  // the originals must survive intact to be compared against after the round trip.
  mm::OutMsg qa{a};
  mm::OutMsg qc{c};
  mm::OutMsg qr{r};
  mm::OutMsg qf{f};
  o.push_report(qa);
  o.push_report(qc);
  o.push_report(qr);
  o.push_report(qf);
  CHECK(o.depth() == 4);

  CHECK(popped<mm::OrderAck>(o) == a);
  CHECK(popped<mm::CancelAck>(o) == c);
  CHECK(popped<mm::Reject>(o) == r);
  // Spelled out as well as compared: the envelope is the session's to stamp at pop time, and
  // the Outbox never touches it — the report-path half of .
  const auto popped_fill = popped<mm::Fill>(o);
  CHECK(popped_fill == f);
  CHECK(popped_fill.v == 1);
  CHECK(popped_fill.seq == 0);
  CHECK(popped_fill.epoch == 0);
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: top of book conflates to a slot of one and the newest wins", "[outbox]") {
  mm::Outbox o;
  o.push_tob(tob(7, 100, 110));
  o.push_tob(tob(8, 101, 111));
  o.push_tob(tob(9, 102, 112));

  CHECK(o.depth() == 1);     // three pushes, one pending message
  CHECK(o.conflated() == 2); // and two attributable drops

  const auto t = popped_tob(o);
  CHECK(t.md_seq == 9);
  CHECK(t.bid_px == 102);
  CHECK(t.ask_px == 112);
  CHECK(t.bid_qty == 10);
  CHECK(t.ask_qty == 20);
  CHECK(t.symbol == "MOCKUSDT");
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: the newest tick is the last arrival, not the largest md_seq", "[outbox]") {
  // "Newest wins" is ARRIVAL order. Every other conflation case pushes ascending md_seqs, so
  // a keep-the-larger-md_seq slot satisfies all of them while quietly ceasing to be a queue
  // discipline at all. A decreasing md_seq is a producer fault whose CLASSIFICATION belongs
  // to the protocol layer (the client treats a decrease as fatal); an Outbox that "repaired"
  // it by keeping the larger would swallow the very evidence that rule exists to surface.
  mm::Outbox o;
  o.push_tob(tob(9, 102, 112));
  o.push_tob(tob(7, 100, 110));
  CHECK(o.depth() == 1);
  CHECK(o.conflated() == 1);

  const auto t = popped_tob(o);
  CHECK(t.md_seq == 7); // the later push, though it carries the smaller md_seq
  CHECK(t.bid_px == 100);
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: conflation gaps md_seq and never the envelope seq", "[outbox]") {
  // The client's own rules make these two gaps mean OPPOSITE things: an md_seq gap is
  // deliberate conflation (benign), an envelope-seq gap is transport loss (fatal, close
  // 1002). That distinction survives only because the Outbox holds a TYPED message and the
  // session stamps `seq` at pop — a pre-encoded frame would have burnt a seq on each
  // superseded tick and manufactured the fatal gap.
  mm::Outbox o;
  o.push_tob(tob(7, 100, 110));
  o.push_tob(tob(9, 102, 112)); // md_seq 8 was never produced; 7 is conflated away here
  mm::OutMsg a = ack("A");
  o.push_report(a);

  CHECK(popped_ack(o) == "A");
  const auto t = popped_tob(o);
  CHECK(t.md_seq == 9); // the client sees md_seq jump — that IS the conflation signal
  CHECK(t.seq == 0);    // untouched: the session stamps the envelope seq at pop time
  CHECK(t.v == 1);
  CHECK(t.epoch == 0);
}

TEST_CASE("outbox: reports drain before market data", "[outbox]") {
  // A report must never queue behind a burst of ticks, so the interleave pops the report
  // first and then the LATEST tick — not the tick that arrived before it.
  mm::Outbox o;
  o.push_tob(tob(1, 100, 110));
  mm::OutMsg r = ack("R");
  o.push_report(r);
  o.push_tob(tob(2, 101, 111));
  CHECK(o.depth() == 2);

  CHECK(popped_ack(o) == "R");
  CHECK(popped_tob(o).md_seq == 2);
  CHECK(o.conflated() == 1);
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: EVERY report drains before the newest tick", "[outbox]") {
  // The single-report interleave above is also satisfied by a round-robin drain — one report,
  // then the pending tick, then the rest — which is exactly the policy failure the rule
  // exists to prevent (a report queueing behind market data). Three reports interleaved with
  // three ticks separate the two: reports-first must yield A, B, C and only then the tick.
  mm::Outbox o;
  mm::OutMsg a = ack("A");
  mm::OutMsg b = ack("B");
  mm::OutMsg c = ack("C");
  o.push_tob(tob(1, 100, 110));
  o.push_report(a);
  o.push_tob(tob(2, 101, 111));
  o.push_report(b);
  o.push_tob(tob(3, 102, 112));
  o.push_report(c);
  CHECK(o.depth() == 4); // three reports and the one pending tick

  CHECK(popped_ack(o) == "A");
  CHECK(popped_ack(o) == "B");
  CHECK(popped_ack(o) == "C");
  const auto t = popped_tob(o);
  CHECK(t.md_seq == 3); // the newest, not the one that arrived before the first report
  CHECK(t.bid_px == 102);
  CHECK(o.conflated() == 2);
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: a tick pushed into a freed slot is not a conflation", "[outbox]") {
  mm::Outbox o;
  o.push_tob(tob(1, 100, 110));
  CHECK(popped_tob(o).md_seq == 1);
  o.push_tob(tob(2, 101, 111)); // slot was empty — nothing was superseded
  CHECK(o.conflated() == 0);
  CHECK(o.depth() == 1);
}

TEST_CASE("outbox: the default report mark is the 1024", "[outbox]") {
  // Pinned against the LITERAL and on its own, because every behavioural case below derives
  // its oracle from the constant: without this, moving the mark off the record's value would
  // leave the entire file green while the shipped session closed at the wrong depth. A
  // runtime CHECK rather than a STATIC_REQUIRE on purpose — the file's discipline is that
  // breaking a rule REDS THE CASE THAT NAMES IT, and a failed static_assert reds the build
  // instead, before any case can name anything.
  CHECK(mm::Outbox::kDefaultReportHwm == 1024);
}

TEST_CASE("outbox: the report mark breaches at 1025 and drops nothing", "[outbox]") {
  // The three observations that fix the boundary exactly — 1023 below it, 1024 ON it, 1025
  // past it — spelled as literals against the mark. The breach is a CLOSE signal
  // (1008), never a drop: all 1025 reports are still there, in order, afterwards.
  mm::Outbox o;
  for (std::size_t i = 1; i <= 1023; ++i) {
    mm::OutMsg r = ack("R" + std::to_string(i));
    o.push_report(r);
  }
  CHECK(o.depth() == 1023);
  CHECK_FALSE(o.report_hwm_breached()); // 1023: one below the mark

  mm::OutMsg r1024 = ack("R1024");
  o.push_report(r1024);
  CHECK(o.depth() == 1024);
  CHECK_FALSE(o.report_hwm_breached()); // 1024: exactly AT the mark is still within policy

  mm::OutMsg r1025 = ack("R1025");
  o.push_report(r1025);
  CHECK(o.report_hwm_breached()); // 1025: the first report PAST the mark
  CHECK(o.depth() == 1025);

  for (std::size_t i = 1; i <= 1025; ++i)
    CHECK(popped_ack(o) == "R" + std::to_string(i));
  CHECK(o.depth() == 0);
}

TEST_CASE("outbox: the breach latches across a drain", "[outbox]") {
  // A level reading would be undone by the very drain the breach proves is too slow, so a
  // consumer that checks in its write-completion handler rather than at push time would
  // silently miss it. The verdict is session-fatal, so it is recorded, not sampled.
  mm::Outbox o{4};
  for (int i = 0; i < 5; ++i) {
    mm::OutMsg r = ack("R");
    o.push_report(r);
  }
  REQUIRE(o.report_hwm_breached());

  while (o.pop().has_value()) {
  }
  CHECK(o.depth() == 0);
  CHECK(o.report_hwm_breached());
}

TEST_CASE("outbox: reports pushed AFTER the breach are still queued, in order", "[outbox]") {
  // The headline rule — "order reports are NEVER dropped" — is a claim about what happens PAST
  // the mark, and every other high-water case above stops ON the breaching push, so none of
  // them ever exercises an enqueue while the latch is already set. That leaves the shed-once-
  // breached mutant (`if (report_hwm_breached_) return;` at the top of push_report) alive: it
  // satisfies the boundary case, the latch case and the drop-nothing case, while silently
  // destroying every state transition the session produces between the breach and the close.
  // Reports 6..64 land here precisely because the mark is OBSERVED, never ENFORCED: the
  // SESSION, not the Outbox, must bound the report queue by acting on the latch, so the queue
  // keeps growing and it is never the Outbox's job to shed. Sampled to SIXTEEN TIMES the mark,
  // not merely past it: the case's first cut stopped at depth 8 — exactly 2x the mark — so a
  // shed that started past `2 * report_hwm_` satisfied it while silently destroying every
  // report from the 9th on; the depth a slow socket reaches is unbounded, so the sample must
  // sit far past any plausible multiple-of-the-mark shed point. A dropped report leaves the
  // order's state undefined, which is the one outcome the design spends a whole close code
  // (1008) to avoid.
  mm::Outbox o{4};
  for (std::size_t i = 1; i <= 5; ++i) {
    mm::OutMsg r = ack("R" + std::to_string(i));
    o.push_report(r);
  }
  REQUIRE(o.report_hwm_breached()); // the latch is set BEFORE the pushes under test

  for (std::size_t i = 6; i <= 64; ++i) {
    mm::OutMsg r = ack("R" + std::to_string(i));
    o.push_report(r);
  }
  CHECK(o.depth() == 64); // all 59 post-breach reports queued, none shed

  for (std::size_t i = 1; i <= 64; ++i)
    CHECK(popped_ack(o) == "R" + std::to_string(i)); // and FIFO still holds across the mark
  CHECK(o.depth() == 0);
}

TEST_CASE("outbox: the mark measures pending depth, not lifetime report volume", "[outbox]") {
  // Every OTHER high-water case pushes onto a queue that is never drained below the mark and
  // then refilled, so a mark counting LIFETIME reports pushed — `if (++pushed_total_ >
  // report_hwm_)` — satisfies all of them: the boundary case, the latch case, the post-breach
  // case and the pending-tick case each breach at the same push either way. In production that
  // mutant closes EVERY session 1008 on its 1025th lifetime report no matter how promptly the
  // socket drained, which is a session-killing bug with a green suite. The discriminator is a
  // full drain followed by a refill: what the mark measures is how far the writer has fallen
  // BEHIND, not how much work it has done.
  mm::Outbox o{4};
  for (std::size_t i = 1; i <= 4; ++i) {
    mm::OutMsg r = ack("R" + std::to_string(i));
    o.push_report(r);
  }
  CHECK(o.depth() == 4);
  CHECK_FALSE(o.report_hwm_breached()); // 4 pending, 4 lifetime: at the mark, within policy

  for (std::size_t i = 1; i <= 4; ++i)
    CHECK(popped_ack(o) == "R" + std::to_string(i));
  CHECK(o.depth() == 0);

  for (std::size_t i = 5; i <= 8; ++i) {
    mm::OutMsg r = ack("R" + std::to_string(i));
    o.push_report(r);
  }
  CHECK(o.depth() == 4);
  CHECK_FALSE(o.report_hwm_breached()); // 4 pending but EIGHT lifetime: still within policy

  mm::OutMsg r9 = ack("R9");
  o.push_report(r9); // the FIFTH pending report — the depth is what breaches
  CHECK(o.report_hwm_breached());
  CHECK(o.depth() == 5);
}

TEST_CASE("outbox: market data never breaches the report mark", "[outbox]") {
  // The mark bounds REPORTS — the queue that cannot drop. Market data is bounded by
  // conflation instead, so no burst of ticks can ever close a session.
  mm::Outbox o{4};
  for (int i = 0; i < 100; ++i)
    o.push_tob(tob(static_cast<std::uint64_t>(i) + 1, 100, 110));
  CHECK(o.depth() == 1);
  CHECK(o.conflated() == 99);
  CHECK_FALSE(o.report_hwm_breached());
}

TEST_CASE("outbox: the report mark counts reports only while a tick is pending", "[outbox]") {
  // The tick-flood case above never holds market data AND a nearly-full report queue at the
  // same time, so it cannot see a mark computed from depth() — reports PLUS the pending tick.
  // That mutant closes the session 1008 one report early whenever market data happens to be
  // queued, i.e. precisely when the session is busiest, and reports a policy violation the
  // client did not commit. Here depth() is over the mark while the report count is not.
  mm::Outbox o{4};
  o.push_tob(tob(1, 100, 110));
  for (int i = 1; i <= 4; ++i) {
    mm::OutMsg r = ack("R" + std::to_string(i));
    o.push_report(r);
  }
  CHECK(o.depth() == 5);                // four reports and the pending tick
  CHECK_FALSE(o.report_hwm_breached()); // but only the four count against the mark

  mm::OutMsg r5 = ack("R5");
  o.push_report(r5);
  CHECK(o.report_hwm_breached()); // the FIFTH report is what breaches it
}

TEST_CASE("outbox: a top of book pushed as a report is a programming error", "[outbox]") {
  // Tob is an OutMsg alternative, so this compiles — and it would defeat the one policy the
  // class exists for: an unconflatable, undroppable tick queued against the report mark.
  // Armed on a POPULATED outbox: "the throw leaves the Outbox exactly as it was" is a claim
  // about ALL the state, and an empty outbox cannot see a guard that resets the pending
  // tick, sheds a report or clears the latch on its way out.
  mm::Outbox o{4};
  o.push_tob(tob(1, 100, 110));
  o.push_tob(tob(2, 101, 111)); // one supersession: conflated() must still read 1 after
  for (std::size_t i = 1; i <= 5; ++i) {
    mm::OutMsg r = ack("R" + std::to_string(i));
    o.push_report(r);
  }
  REQUIRE(o.report_hwm_breached()); // the latch is set BEFORE the guarded call
  REQUIRE(o.depth() == 6);          // five queued reports plus the pending tick

  mm::OutMsg smuggled = tob(3, 102, 112);
  CHECK_THROWS_MATCHES(
      o.push_report(smuggled), std::invalid_argument,
      Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("push_tob")));

  // On ANY throw the ARGUMENT is untouched too (the consumption contract): the smuggled
  // message still holds the very tick it arrived with, so even this programming error
  // destroys nothing.
  const auto *smuggled_tick = std::get_if<mm::Tob>(&smuggled);
  REQUIRE(smuggled_tick != nullptr);
  CHECK(smuggled_tick->md_seq == 3);

  CHECK(o.depth() == 6); // nothing was queued on the way out — and nothing was lost
  CHECK(o.conflated() == 1);
  CHECK(o.report_hwm_breached());
  for (std::size_t i = 1; i <= 5; ++i)
    CHECK(popped_ack(o) == "R" + std::to_string(i)); // the FIFO drain order is untouched
  CHECK(popped_tob(o).md_seq == 2);                  // and so is the pending tick
  CHECK_FALSE(o.pop().has_value());
}

TEST_CASE("outbox: a zero report mark is rejected at construction", "[outbox]") {
  // It would flag a breach on a session's FIRST report — an instant 1008 close before the
  // session can act. Same fail-loudly policy as the engine's nonsensical-config checks.
  CHECK_THROWS_AS(mm::Outbox{std::size_t{0}}, std::invalid_argument);
  CHECK_NOTHROW(mm::Outbox{std::size_t{1}});
}
