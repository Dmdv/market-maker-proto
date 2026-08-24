// Task 6 tests, allocation part: mm::Outbox's rules on the allocation paths — four counted
// regions, an injection walk, and the one guard only this probe can arm, across four cases.
// The tick case counts the steady push-then-pop cycle and the never-popped supersession
// burst (both exactly zero, at symbols past both small-string capacities and, in the burst,
// of STRICTLY GROWING length, so a copy can hide neither inside SSO nor inside a
// destination's retained capacity); the drain case bounds the push-then-full-drain sequence
// two-sided by the push count; the enqueue-failure case bounds the push churn the same way,
// then fails every allocation of the push sequence in turn and drains the landed prefix —
// content and order, as whole report values — after each failure; and the
// valueless-guard case (hard gate) builds the one report fixture that EXISTS only by
// failing an allocation mid-emplace, then arms it on a BREACHED latch. Split out of the
// engine's allocation TU at gate P1-R5 — sharing one file had pushed it past the repo's
// 500-line cap, and the one-TU constraint that put the Outbox case there is about the
// global `operator new` REPLACEMENT (defined once, in alloc_probe.cpp), not about the cases
// that arm it. All four cases keep the `outbox:` name prefix and the `[outbox]` tag, so
// `ctest -R outbox` and `./mm_tests "[outbox]"` still select the whole rule set alongside
// `cpp/tests/test_outbox.cpp`, whose header comment points here.
#include "alloc_probe_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "mm/outbox.hpp"
#include "mm/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using alloc_probe::allocations_in;
using alloc_probe::ThrowOnNthAllocation;

namespace {

// Fixtures, built OUTSIDE every counted region (the rule every probe call site follows —
// see allocations_in): 64-byte identifiers (cl_id and Tob symbol alike) are past both
// small-string capacities (libc++ 22, libstdc++ 15), so a copy heap-allocates where a move
// does not, and a moved-from string is unmistakably empty rather than an SSO leftover. The
// tob builder takes the symbol length as a parameter because the supersession burst needs
// it GROWING (64 + i) — its region comment says why any length the destination has already
// seen re-blinds that region's copy mutant.
// Deliberately NOT shared with test_outbox.cpp's builders: the two tob builders are
// genuinely divergent — this one carries a past-SSO symbol because it measures ALLOCATIONS,
// that one carries prices because it measures POLICY — and a merge would re-blind whichever
// pin it homogenized.
mm::Tob outbox_tob(std::uint64_t md_seq, std::size_t symbol_len = 64) {
  mm::Tob t;
  t.md_seq = md_seq;
  t.symbol = std::string(symbol_len, 'S');
  t.bid_px = 100;
  t.bid_qty = 10;
  t.ask_px = 110;
  t.ask_qty = 20;
  return t;
}

// Shared by the three report cases: one fixed push count and one 64-byte cl_id SHAPE from a
// single builder, so the drain case's bound, the enqueue-failure case's injection range and
// the valueless case's fixtures derive from the same constants rather than from literals
// that drift apart silently. The id CARRIES ITS PUSH INDEX (hardgate-fix-2) — the digits
// overwrite the tail of a fixed 64-'R' run, so every id keeps the same 64-byte,
// past-both-SSO length and the allocation schedule cannot see the index — because the
// injection walk's post-failure drain compares landed content AND order against the push
// sequence, which one shared identifier could never falsify. A builder function rather than
// a namespace-scope `const std::string`, because a static initializer that can throw is
// uncatchable (bugprone-throwing-static-initialization).
std::string outbox_report_id(std::size_t index) {
  std::string id(64, 'R');
  const std::string digits = std::to_string(index);
  id.replace(id.size() - digits.size(), digits.size(), digits);
  return id;
}
constexpr std::size_t kPushes = 64;

// The indexed report fixture, EVERY field deterministic in the index (hardgate-fix-3): the
// oracles below compare landed reports as WHOLE OrderAck values, and a field the fixture
// left at the type's default was a field whose zeroing the oracle could not tell from
// health. eng_id carries the index shifted by one so element 0's stays non-zero, and
// svc_ns is fixed but non-zero for the same reason; status keeps the SSO-sized "live" and
// cl_id its 64-byte indexed shape — both strings hold their hardgate-2 lengths, and every
// field added is an integer, so the allocation schedule the bounds measure cannot move.
// The envelope (v/seq/epoch) is deliberately LEFT AT ITS DEFAULT — F-06: the Outbox writes
// to no message field and the session stamps the envelope at pop, so a fixture stamping
// its own envelope would blur the rule test_outbox.cpp's round-trip case pins — and the
// whole-value comparison still asserts those defaults, so an Outbox that WROTE a stamp
// into a landed report is caught all the same.
mm::OrderAck outbox_indexed_ack(std::size_t index) {
  mm::OrderAck a;
  a.cl_id = outbox_report_id(index);
  a.eng_id = index + 1; // non-zero at EVERY index: element 0's zeroed-eng_id must not hide
  a.status = "live";
  a.svc_ns = 777; // fixed, non-zero: a zeroed svc_ns must not pass for the default
  return a;
}

std::vector<mm::OutMsg> outbox_indexed_acks(std::size_t n) {
  std::vector<mm::OutMsg> v;
  v.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    v.emplace_back(outbox_indexed_ack(i));
  return v;
}

// Whole-object oracle (hardgate-fix-3): the walk's previous content checks read cl_id
// alone off every landed and retried report, so a failure path corrupting any OTHER field
// of a report it had already landed — envelope, eng_id, status, svc_ns — passed unseen.
// Every report assertion now compares the COMPLETE OrderAck against its own index's
// fixture through the defaulted operator== (protocol.hpp: every member, envelope
// included); a wrong alternative maps onto false, which the call site's CHECK rejects.
bool outbox_is_ack_of(const mm::OutMsg &m, std::size_t index) {
  const auto *a = std::get_if<mm::OrderAck>(&m);
  return a != nullptr && *a == outbox_indexed_ack(index);
}

// Sentinel-style pops for the drain assertions — the injection walk's landed-prefix drains
// and the valueless-guard case alike (test_outbox.cpp's helper rule): each disappointment
// maps onto a value the call site's own comparison rejects — false from the whole-object
// pop, md_seq 0 from the tick pop (every tick below carries a non-zero one) — because a
// REQUIRE's abort is invisible to clang-tidy's optional-engagement analysis, which would
// then flag every access.
bool outbox_popped_is_ack_of(mm::Outbox &o, std::size_t index) {
  const auto m = o.pop();
  return m.has_value() && outbox_is_ack_of(*m, index);
}

std::uint64_t outbox_popped_md_seq(mm::Outbox &o) {
  const auto m = o.pop();
  if (!m.has_value())
    return 0;
  const auto *t = std::get_if<mm::Tob>(&*m);
  return t != nullptr ? t->md_seq : 0;
}

} // namespace

TEST_CASE("outbox: the tick path allocates nothing", "[outbox]") {
  // outbox.hpp documents its allocation exception (report-queue deque node churn) and claims
  // "the tick path ... is allocation-free by construction". The churn RATE and sizeof(OutMsg)
  // halves of that comment are stdlib-dependent and cannot be pinned portably (the same rule
  // that limits the engine's inventory pins to ZERO rows and past-both-SSO rows), but zero is
  // zero on any stdlib — so the portable half is pinned here, in two counted regions. The
  // fixture symbols are PAST both small-string capacities (64 bytes here; region two
  // grows from 64) because the zero must be falsifiable: at the SSO-sized "MOCKUSDT" a
  // copy allocates nothing, so a Tob copy materialized on the push or pop path measured the
  // very same zero and the pin was blind to exactly the rot it exists to catch.
  constexpr std::size_t kTicks = 1000;
  std::vector<mm::Tob> ticks;
  ticks.reserve(kTicks);
  for (std::size_t i = 0; i < kTicks; ++i)
    ticks.push_back(outbox_tob(i + 1));

  // Region one: the steady state — every push lands in a freed slot, every tick is popped.
  mm::Outbox o;
  CHECK(allocations_in([&] {
          for (auto &t : ticks) {
            o.push_tob(std::move(t));
            (void)o.pop();
          }
        }) == 0);
  CHECK(o.depth() == 0);
  CHECK(o.conflated() == 0); // every push landed in a freed slot: the steady-state tick path

  // Region two: the SUPERSESSION branch — push over a pending tick, never popping. Built
  // from FRESH ticks: region one moved from `ticks`, a moved-from symbol is SSO-shrunk, and
  // a copy of an SSO string allocates nothing — reuse would re-blind the copy mutants. The
  // symbol length GROWS STRICTLY (64 + i) for the same falsifiability reason one level up:
  // this branch commits into an ENGAGED slot, so a copy here is a copy-ASSIGNMENT, and a
  // copy-assign hides inside the destination's retained capacity whenever the destination
  // has already seen an equal-or-longer symbol — SSO is only the FIRST place a copy can
  // hide, and even an alternating 64/128 pays only at its first 64→128 supersession, only
  // when it starts short (a phase flip re-blinds it silently). Growing the length through
  // the burst forces the reallocations the zero can see and removes that start-short
  // ordering dependency.
  std::vector<mm::Tob> burst;
  burst.reserve(kTicks);
  for (std::size_t i = 0; i < kTicks; ++i)
    burst.push_back(outbox_tob(i + 1, 64 + i));

  mm::Outbox o2;
  CHECK(allocations_in([&] {
          for (auto &t : burst)
            o2.push_tob(std::move(t));
        }) == 0);
  CHECK(o2.depth() == 1);
  CHECK(o2.conflated() == kTicks - 1); // every push but the first superseded a pending tick
}

TEST_CASE("outbox: draining the report queue allocates nothing per report", "[outbox]") {
  // The pop side of the report path, its own case because it is its own rule (this file's
  // discipline: deleting a rule must red exactly the case that NAMES it). Push kPushes
  // reports, then pop the queue dry: a pop that COPIED the report out instead of moving it
  // would add one allocation per 64-byte cl_id — kPushes of them — which the bound's upper
  // side kills. Two-sided against the loop's own constant (two literals for one quantity
  // drift apart silently): >= 1 says the sequence reached the deque's node churn at all,
  // and <= kPushes caps push-plus-drain at the push count, so the drain itself contributes
  // no per-report allocation on any stdlib. Kept OUT of the enqueue-failure case's measured
  // push figure: that figure aims its injection walk at the push path alone, and widening
  // it with the drain would aim injections at pops.
  auto pending = outbox_indexed_acks(kPushes);
  mm::Outbox o;
  const std::size_t push_drain_allocations = allocations_in([&] {
    for (auto &r : pending)
      o.push_report(r); // named lvalue, no move at the call site — the push consumes on success
    while (o.pop().has_value()) {
    }
  });
  REQUIRE(push_drain_allocations >= 1);
  REQUIRE(push_drain_allocations <= kPushes);
}

TEST_CASE("outbox: an enqueue that fails to allocate never consumes the caller's report",
          "[outbox]") {
  // Task 6's headline rule — "order reports are NEVER dropped" — has to hold on the FAILURE
  // path too: a report the caller can no longer retry IS a dropped report, and a dropped
  // report leaves the order's state undefined. The deque's own strong guarantee covers the
  // CONTAINER and says nothing about the ARGUMENT, which is why `push_report` binds the
  // caller's NAMED report — a non-const lvalue reference, so the lossy call forms (a
  // prvalue, a `std::move`d handoff, an implicit alternative conversion) do not compile at
  // all (hard gate) — and consumes it only in the noexcept commit AFTER the allocation.
  // Under the previous `OutMsg &&` binding this walk could prove the guarantee only for the
  // named-object call form while those other forms compiled and died in the unwind; now the
  // object that survives the throw IS the caller's own, and the retry below re-pushes the
  // SAME object rather than a copy captured on the way out.
  //
  // Pinned from this TU rather than from test_outbox.cpp because failing an allocation is a
  // whole-binary property — the probe alloc_probe.cpp owns for the entire binary. The
  // alternative was a template parameter on the shipped `mm::Outbox` whose only caller was
  // one test — a seam widening the shipped API for a job this whole-binary probe already
  // does.
  //
  // Every allocation of a fixed 64-push sequence is failed IN TURN (the engine cases' rule):
  // WHICH push allocates is a stdlib detail — libc++ allocates on the first, libstdc++ ships a
  // preallocated first node — so the sequence is walked rather than a failing index assumed,
  // and a guarantee that held only for the first node would still have to survive the later
  // ones. The bound is MEASURED on a clean run of the same sequence, never hand-copied.
  //
  // Each report's cl_id carries its push index at the same 64-byte length (hardgate-fix-2;
  // the builder comment has the how), and after every injected failure the landed prefix is
  // DRAINED — outside every counted region, so the two-sided bound and the measured figure
  // keep their meaning — and compared, content and order, against the push sequence. With
  // one shared identifier the walk asserted only the failed argument and the depth, so a
  // failure path that corrupted, reordered or replaced a report it had ALREADY landed
  // passed unseen; the indexed drain is what makes those depth-preserving mutants visible.
  // The drain, the failed survivor and the retried landing are all compared as WHOLE
  // OrderAck values (hardgate-fix-3): the fix-2 drain read cl_id alone, so a failure path
  // corrupting any OTHER field of an already-landed report — envelope, eng_id, status,
  // svc_ns — still passed; the whole-object oracle over the builder's per-index,
  // non-default fields closes exactly that blindness.
  const std::size_t path_allocations = [&] {
    auto pending = outbox_indexed_acks(kPushes);
    mm::Outbox o;
    return allocations_in([&] {
      for (auto &r : pending)
        o.push_report(r);
    });
  }();
  // Two-sided, against the loop's own constant (two literals for one quantity drift apart
  // silently): >= 1 says the sequence reached the deque's node churn at all, and <= kPushes
  // bounds it from above — node churn amortizes to well under one allocation per pushed
  // report on any stdlib, so a figure past the push count would mean the enqueue had grown
  // per-report allocations (a string copy, a re-encoded frame) that no inventory documents.
  REQUIRE(path_allocations >= 1);
  REQUIRE(path_allocations <= kPushes);

  for (std::size_t nth = 1; nth <= path_allocations; ++nth) {
    INFO("std::bad_alloc injected at allocation #" << nth);
    auto pending = outbox_indexed_acks(kPushes); // built outside the armed region
    mm::Outbox o;
    mm::OutMsg *failed = nullptr;
    std::size_t landed = 0;
    {
      const ThrowOnNthAllocation arm{nth};
      for (auto &r : pending) {
        try {
          o.push_report(r);
        } catch (const std::bad_alloc &) {
          failed = &r; // point at the survivor in place — nothing is captured or moved
          break;
        }
        ++landed;
      }
    }
    REQUIRE(failed != nullptr); // the injection actually fired on this sequence
    // Untouched: still the complete report of its own index, EVERY field — not moved-from,
    // not rewritten.
    CHECK(outbox_is_ack_of(*failed, landed));
    CHECK(o.depth() == landed); // and the failed push queued nothing

    // The landed prefix, drained and compared report by report — content and order both,
    // every field of every element.
    for (std::size_t i = 0; i < landed; ++i)
      CHECK(outbox_popped_is_ack_of(o, i));
    CHECK(o.depth() == 0);

    o.push_report(*failed); // the SAME named object retries — the throw consumed nothing
    CHECK(o.depth() == 1);
    CHECK(outbox_popped_is_ack_of(o, landed)); // and it lands, intact in every field
    CHECK_FALSE(o.pop().has_value());
  }
}

TEST_CASE("outbox: a valueless message pushed as a report is a programming error", "[outbox]") {
  // The THIRD programming-error guard (hard gate; the other two are cased in
  // test_outbox.cpp): a valueless-by-exception OutMsg answers false to EVERY
  // holds_alternative, so it would slip the Tob guard, count against the report mark, and
  // hand the session's pop-time visitor a std::bad_variant_access. The case lives in THIS
  // TU because only the injection probe can build the fixture: every OutMsg alternative is
  // nothrow-movable (asserted in outbox.hpp), which closes the MOVE routes to
  // valuelessness, but variant::emplace destroys its old value before constructing the new
  // one — so failing an allocation inside the new alternative's string copy leaves the
  // variant holding nothing. Both stdlibs this project ships on (libc++ dev host, libstdc++
  // container) take that route to valueless; a stdlib with a stronger emplace would fail
  // the REQUIRE below loudly rather than skip the rule silently.
  // Index kPushes: outside every id this case queues (0..4), so the valueless fixture's
  // identity can never pass for a landed report's.
  mm::OutMsg valueless = outbox_indexed_ack(kPushes);
  {
    const mm::OrderAck big = outbox_indexed_ack(kPushes);
    bool threw = false;
    try {
      const ThrowOnNthAllocation arm{1};
      valueless.emplace<mm::OrderAck>(big); // the 64-byte cl_id copy is the failing allocation
    } catch (const std::bad_alloc &) {
      threw = true;
    }
    REQUIRE(threw);
  }
  REQUIRE(valueless.valueless_by_exception());

  // Armed on a POPULATED outbox with the latch BREACHED (hardgate-fix-2; the populated-arming
  // discipline is test_outbox.cpp's Tob-guard case's): "the throw leaves the Outbox exactly
  // as it was" is a claim about ALL the state, and an empty outbox cannot see a guard that
  // resets the pending tick or sheds a report on its way out — while an UNBREACHED latch
  // cannot see the worst latch regression of all, a guard that CLEARS an already-true breach
  // verdict and with it the 1008 close the session owes. Five reports against a mark of 4 set
  // the latch BEFORE the guarded call; it must still read true after the throw.
  mm::Outbox o{4};
  o.push_tob(outbox_tob(1));
  o.push_tob(outbox_tob(2)); // one supersession: conflated() must still read 1 after
  auto pending = outbox_indexed_acks(5);
  for (auto &r : pending)
    o.push_report(r);
  REQUIRE(o.depth() == 6); // five queued reports plus the pending tick
  REQUIRE(o.conflated() == 1);
  REQUIRE(o.report_hwm_breached()); // the latch is set BEFORE the guarded call

  CHECK_THROWS_MATCHES(
      o.push_report(valueless), std::invalid_argument,
      Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("valueless")));

  CHECK(valueless.valueless_by_exception()); // the guard consumed nothing either
  CHECK(o.depth() == 6);
  CHECK(o.conflated() == 1);
  CHECK(o.report_hwm_breached()); // STILL latched: the guard cleared nothing on its way out
  for (std::size_t i = 0; i < 5; ++i)
    CHECK(outbox_popped_is_ack_of(o, i)); // the FIFO drain — whole reports — is untouched
  CHECK(outbox_popped_md_seq(o) == 2);    // and so is the pending tick
  CHECK_FALSE(o.pop().has_value());
}
