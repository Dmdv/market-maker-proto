// Order engine implementation (plan Task 3). Contracts, check order, fill rule, unit
// conventions, complexity and retention/reclamation rules are documented in
// mm/engine.hpp; this TU keeps them mechanical.
//
// MEASURED ALLOCATION AND RESIDENCY INVENTORY
//
// The quantified exception to the plan's zero-allocation constraint (Global Constraints,
// 08 F-11) and the seed for the §6 allocation counter (Tasks 13/14). It lives HERE, next to
// the code it measures, so a change to that code and a change to its figures are one diff;
// the stdlib-independent rows are pinned by cpp/tests/test_engine_alloc.cpp.
//
// Per-call source list inside the M2 window:
//   on_new(accept):  orders_ node + its key's cl_id string, the OrderAck cl_id string, one
//                    live-index node, the result vector; PLUS the entries_by_session_ node on
//                    the session's FIRST command only. (The live index stores an orders_
//                    ITERATOR and Order carries no cl_id of its own — the key owns the only
//                    stored copy — so neither duplicates the cl_id string.)
//   on_new(reject):  tombstone node + key/Reject cl_id strings, the result vector, and
//                    Reject::reason ONLY when the fixed reason text exceeds small-string
//                    capacity — "unknown symbol" (14 B) and "qty must be positive" (20 B) fit
//                    the 22 B host cap, every other on_new reason exceeds it — PLUS the same
//                    FIRST-command entries_by_session_ node (the increment runs on this path
//                    too). The DupClOrdId reject returns EARLY, storing no tombstone: its
//                    reason + the result vector only.
//   on_cancel:       the ack/reject strings + the result vector — lookups are allocation-FREE
//                    (the key comparator is transparent, probes are string_view pairs; the
//                    plan scopes allocation to order-state INSERTION).
//   on_book:         ZERO for a sweep that fills nothing and for an ignored stale md_seq —
//                    neither the result vector nor the staging vector is ever pushed to, and
//                    an empty vector does not allocate; each fill adds growth of BOTH vectors
//                    plus the Fill's cl_id copy (read from the key) when it exceeds the
//                    small-string cap. The staging vector is the price of the sweep's strong
//                    exception guarantee (the batch is computed before any state moves — see
//                    on_book); it is charged only to sweeps that actually fill, which is why
//                    the steady-state M3 row above is still exactly zero.
//   Reject::code:    on every reject path, a std::string of the wire spelling. NEVER
//                    allocates on the host (all spellings <= 16 B fit the 22 B libc++ cap)
//                    but sits on an SSO CLIFF on the AUTHORITATIVE container (libstdc++,
//                    15-char local capacity): ALREADY_TERMINAL (16 chars) heap-allocates
//                    there, and UNKNOWN_CLORDID / MAX_LIVE_ORDERS / POST_ONLY_CROSS sit at
//                    exactly 15 with UNKNOWN_SYMBOL at 14 — a one-character wire-spelling
//                    edit silently adds one allocation per reject, so any such edit must be
//                    re-measured against the §6 counter.
//
// Measured per-call operator-new counts (global operator-new counter, host /usr/bin/c++
// libc++ SSO cap 22 B, -O0/-O2 identical; the linux/arm64 container's libstdc++ SSO is 15 B,
// so container figures can only be >= these at equal cl_id length). Declared basis: STEADY
// STATE — the correct seed for the §5.2/§6 100k-sample benchmarks, where the once-per-session
// entries_by_session_ node amortises to ~0/call; a session's FIRST command adds +1 to either
// on_new row.
//   cl_id  2 B -> accept 3; tombstone reject 2 (SSO reason) or 3 (heap reason); DupClOrdId
//                 reject 2; cancel-ack 1; cancel-reject 2; on_book 0 no-fill, 2 per one-fill
//                 sweep (4 for two fills);
//   cl_id 40 B -> accept 5; tombstone reject 4 or 5; DupClOrdId reject 3; cancel-ack 2;
//                 cancel-reject 3; on_book 0 no-fill, 3 per one-fill sweep (6 for two fills);
//   cl_id 64 B -> identical to the 40 B row (both sit past the SSO cliff; string capacity
//                 growth changes bytes, not counts).
// cl_id length is CLIENT-chosen up to the codec's 64 B cap, so the Python client's id format
// directly moves these counts — the §6 counter must seed from measured figures at the
// client's real id length, not the short-id best case. The result vector on the COMMAND entry
// points (on_new/on_cancel return >= 1 message, so their first push always allocates) is a
// consequence of the plan-pinned return-by-value signatures; on_book's steady-state empty
// return is allocation-free. (A caller-supplied output buffer / small_vector is the ranked
// follow-up if a cheaper shape is ever wanted; changing a ratified signature is not this
// task's call.)
//
// Retained-entry residency, which is what the per-session entry cap actually bounds (host
// libc++, malloc_size accounting; tombstones and accepted+cancelled entries measure
// identically): 112 B at short (SSO) cl_ids, 160 B at 40 B and 192 B at the codec's 64 B
// cl_id cap, so the cap-full worst case is up to ~192 MiB per session before the policy
// close. Order carries no cl_id of its own (the key owns the only copy), which is what keeps
// the per-entry heap-string count at one. Authoritative-container libstdc++ figures are >=
// these: its std::string is 8 B larger and the node holds one. RE-MEASURED after Order lost
// its unread `qty` member: sizeof(Order) 48 -> 40 and the node 112 -> 104 bytes, but the
// malloc size class is unchanged, so every figure above still holds exactly — a reminder that
// these are MEASURED residencies, not arithmetic on member sizes.
// Growth is ~1 entry per accepted new->cancel cycle; the tombstone path is peer-driven and
// bounded by OrderEngine::kDefaultMaxSessionEntries, so the cap-full worst case per session is
// bounded and honestly stated in the README known limitations (Task 15).
#include "mm/engine.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mm {
namespace {

// Wire side spelling -> Side, named once. A TOTAL function that GUESSES is the failure mode
// this replaces: `order.side == "B" ? Bid : Ask` maps "X" and "" to Ask and is safe only
// because validate_order returns BadSide first — a coupling nothing at the call site
// expressed. nullopt outside the acceptor set {"B", "S"} (types.hpp).
constexpr std::optional<Side> side_from_wire(std::string_view side) {
  if (side == "B")
    return Side::Bid;
  if (side == "S")
    return Side::Ask;
  return std::nullopt;
}

// The raw-wire-price <-> Ticks boundary (types.hpp: the wire carries raw integer price units,
// storage is Ticks), named once so `.v` is reached through exactly these two functions and
// nowhere else — reaching through it inline defeats the one thing the strong type is for.
Ticks to_ticks(const Instrument &inst, std::int64_t raw_px) {
  return Ticks{raw_px / inst.tick_size};
}
std::int64_t to_raw_px(const Instrument &inst, Ticks px) { return px.v * inst.tick_size; }

Reject make_reject(std::string cl_id, RejectCode code, std::string reason) {
  Reject rej;
  rej.cl_id = std::move(cl_id);
  rej.code = std::string{to_string(code)};
  rej.reason = std::move(reason);
  return rej;
}

// Human-readable reasons for validate_order codes. QtyLimit folds two conditions
// (types.hpp): the reason string is the only place they stay distinguishable.
//
// Returns a VIEW of static storage (string literals, and to_string's wire spelling) rather
// than a std::string: the heap copy is then materialized by on_new's reject lambda INSIDE
// the region that rolls back a freshly created entry-counter node, instead of in the
// argument expression that precedes the call. Same allocation count, one fewer way to
// strand engine state on a bad_alloc.
constexpr std::string_view validation_reason(RejectCode code, const NewOrder &order) {
  switch (code) {
  case RejectCode::UnknownSymbol:
    return "unknown symbol";
  case RejectCode::BadSide:
    return "side must be \"B\" or \"S\"";
  case RejectCode::TickSize:
    return "px is not a multiple of tick_size";
  case RejectCode::LotSize:
    return "qty is not a multiple of lot_size";
  case RejectCode::QtyLimit:
    return order.qty <= 0 ? "qty must be positive" : "qty exceeds max_order_qty";
  case RejectCode::PriceRange:
    return "px outside [min_price, max_price]";
  case RejectCode::UnsupportedVersion:
  case RejectCode::UnknownType:
  case RejectCode::Malformed:
  case RejectCode::MsgTooLarge:
  case RejectCode::DupClOrdId:
  case RejectCode::PostOnlyCross:
  case RejectCode::MaxLiveOrders:
  case RejectCode::UnknownClOrdId:
  case RejectCode::AlreadyTerminal:
  case RejectCode::StaleEpoch:
    break; // not produced by validate_order (its closed set is the six cases above)
  }
  // Backstop for the grouped non-validate_order codes and for out-of-range values cast
  // into RejectCode: the raw wire spelling. Deliberately NO default label (the types.hpp
  // to_string convention): -Wswitch must fire on an appended enumerator, forcing a
  // deliberate choice of client-visible reason text here instead of letting
  // Reject.reason silently degrade to the wire spelling.
  return to_string(code);
}

} // namespace

OrderEngine::OrderEngine(Instrument inst, std::size_t max_session_entries)
    : inst_{std::move(inst)}, max_session_entries_{max_session_entries} {
  // Header contract: fail loudly at construction on any nonsensical configuration —
  // never reach `% 0` / signed-overflow UB in the entry conversion or the fill sweep,
  // never silently reject every order (a negative max_live_orders, a non-positive
  // max_order_qty or an empty [min_price, max_price] range each would), and never
  // report entry_cap_breached for a session that has sent nothing.
  if (inst_.tick_size <= 0 || inst_.lot_size <= 0)
    throw std::invalid_argument{"OrderEngine: Instrument tick_size and lot_size must be > 0"};
  if (inst_.max_live_orders < 0)
    throw std::invalid_argument{"OrderEngine: Instrument max_live_orders must be >= 0"};
  if (inst_.max_order_qty <= 0)
    throw std::invalid_argument{"OrderEngine: Instrument max_order_qty must be > 0"};
  if (inst_.min_price > inst_.max_price)
    throw std::invalid_argument{"OrderEngine: Instrument min_price must be <= max_price"};
  if (max_session_entries_ == 0)
    throw std::invalid_argument{"OrderEngine: max_session_entries must be > 0"};
}

std::vector<OutMsg> OrderEngine::on_new(std::uint64_t session, const NewOrder &order) {
  std::vector<OutMsg> out;
  // 1. Duplicate cl_id — per-session namespace, terminal orders included (invariant 4).
  // ONE descent of orders_ serves the whole call: lower_bound answers the duplicate
  // question AND names the exact position any insertion below goes in front of, which
  // emplace_hint then reuses (amortized O(1) — a second independent descent would double the
  // ~20 key compares this path pays at the 1M-entry cap). The probe is a string_view pair:
  // the lookup allocates nothing. Nothing between here and the inserts touches orders_, so
  // the hint stays valid.
  const auto lb = orders_.lower_bound(OrderKeyProbe{session, order.cl_id});
  if (lb != orders_.end() && lb->first.first == session && lb->first.second == order.cl_id) {
    out.emplace_back(
        make_reject(order.cl_id, RejectCode::DupClOrdId, "cl_id already used on this session"));
    return out;
  }
  // The session's entry-counter node, reserved here because BOTH outcomes below record an
  // entry — reserving it up front is what makes the ++ that commits it non-throwing.
  // try_emplace rather than operator[] so a node THIS call created can be erased again when
  // a later allocation throws: a zero-valued node left behind is state the engine did not
  // have before, which is exactly what the strong guarantee promises never happens, and it
  // would silently retire the inventory's "+1 on a session's FIRST command" row for that
  // session (the retry would find the node already there). Reserved AFTER the duplicate
  // check above, which returns early and records nothing. Spelled as two named variables
  // rather than a structured binding because `undo_counter` below CAPTURES them.
  const auto reserved = entries_by_session_.try_emplace(session, 0);
  const auto counter = reserved.first;
  const bool counter_is_new = reserved.second;
  const auto undo_counter = [&] {
    if (counter_is_new)
      entries_by_session_.erase(counter);
  };
  // Every non-duplicate reject stores a Rejected TOMBSTONE: the cl_id is consumed for
  // the session's lifetime, so a retry is DupClOrdId (invariant 4 uniform across
  // terminal states). Tombstones burn no eng_id; their side/px fields are never read
  // (st != Live excludes them from sweep/live_count/end_session acks) — the designated
  // initializer names the ONE field that means anything on a tombstone. They DO count
  // toward the per-session entry cap: the cap — surfaced via entry_cap_breached — is the
  // only backpressure on this path. Ordered for the strong guarantee: the reject reaches the
  // result vector BEFORE the tombstone lands in orders_, and a throw from either undoes the
  // counter node this call may have created.
  const auto reject = [&](RejectCode code, std::string_view reason) {
    try {
      out.emplace_back(make_reject(order.cl_id, code, std::string{reason}));
      orders_.emplace_hint(lb, OrderKey{session, order.cl_id}, Order{.st = OrdState::Rejected});
    } catch (...) {
      undo_counter();
      throw;
    }
    ++counter->second;
  };
  // 2. Semantic validation, in raw wire units (types.hpp pins the check order).
  if (const auto code = validate_order(inst_, order.symbol, order.side, order.px, order.qty)) {
    reject(*code, validation_reason(*code, order));
    return out;
  }
  // 3. Per-session live-order cap (R-1; max_live_orders >= 0 is a constructor invariant).
  if (live_count(session) >= static_cast<std::size_t>(inst_.max_live_orders)) {
    reject(RejectCode::MaxLiveOrders, "session already holds max_live_orders live orders");
    return out;
  }
  // 4. Post-only entry cross (fill rule mirrored at entry; a side with qty <= 0 or
  // px <= 0 is absent — same absence rule as the sweep).
  const std::optional<Side> parsed_side = side_from_wire(order.side);
  // PRECONDITION, established by check 2 above: validate_order rejects BadSide for every
  // spelling outside {"B", "S"}, so this optional is always engaged here. Asserted rather
  // than defaulted — a silent fallback to one side is exactly the guess side_from_wire
  // exists to prevent.
  assert(parsed_side.has_value() && "validate_order must reject any side outside {B, S}");
  const Side side = *parsed_side;
  const bool crosses = side == Side::Bid
                           ? (book_.ask_qty > 0 && book_.ask_px > 0 && book_.ask_px <= order.px)
                           : (book_.bid_qty > 0 && book_.bid_px > 0 && book_.bid_px >= order.px);
  if (order.post_only && crosses) {
    reject(RejectCode::PostOnlyCross, "post-only order would cross the book");
    return out;
  }
  // Accept: convert px to Ticks at the boundary (leaves stays RAW — un-rounded fills). The
  // key's cl_id copy in the orders_ node is the ONLY stored copy (header contract on Order);
  // the live index stores the new node's ITERATOR, duplicating nothing.
  const Order ord{.eng_id = next_eng_id_,
                  .side = side,
                  .px = to_ticks(inst_, order.px),
                  .leaves = order.qty,
                  .st = OrdState::Live};
  // Commit, ordered for the STRONG guarantee (header contract). The ack reaches the result
  // vector FIRST — an accept the caller is never told about is as bad as a state change it
  // never asked for — and every mutation that can still fail is undone on the way out: the
  // live-index node takes the orders_ node with it, and both take the counter node when this
  // call created it. A half-committed accept — Live in orders_ but absent from the index —
  // would be invisible to on_book and live_count (so the per-session max_live_orders cap
  // would under-count forever) while still cancellable through orders_: two divergent views
  // of one order, the worst available outcome.
  // The ack is BUILT here rather than above so that its cl_id copy — an allocation like any
  // other — is inside the rollback region too.
  auto it = orders_.end(); // doubles as the "did the map insert happen" flag for the rollback
  try {
    OrderAck ack;
    ack.cl_id = order.cl_id;
    ack.eng_id = ord.eng_id;
    ack.status = "live";
    ack.svc_ns = 0; // stamped by the Server at send time (engine has no clock)
    out.emplace_back(std::move(ack));
    it = orders_.emplace_hint(lb, OrderKey{session, order.cl_id}, ord);
    live_index(side).insert(it);
  } catch (...) {
    if (it != orders_.end())
      orders_.erase(it);
    undo_counter();
    throw;
  }
  ++counter->second;
  ++next_eng_id_; // burned only on a fully committed accept
  return out;
}

std::vector<OutMsg> OrderEngine::on_cancel(std::uint64_t session, const CancelOrder &cancel) {
  std::vector<OutMsg> out;
  // string_view probe: a read-only lookup materializes no std::string.
  const auto it = orders_.find(OrderKeyProbe{session, cancel.cl_id});
  if (it == orders_.end()) {
    out.emplace_back(
        make_reject(cancel.cl_id, RejectCode::UnknownClOrdId, "unknown cl_id on this session"));
    return out;
  }
  Order &ord = it->second;
  if (ord.st != OrdState::Live) { // terminal states are absorbing (invariants 1 and 3)
    out.emplace_back(
        make_reject(cancel.cl_id, RejectCode::AlreadyTerminal, "order is already terminal"));
    return out;
  }
  // The ack is in the result vector BEFORE the order leaves Live: a cancel that commits and
  // then loses its report to a failed allocation is UNRECOVERABLE — the retry answers
  // AlreadyTerminal, so the client can never be told about a cancellation that did happen.
  CancelAck ack;
  ack.cl_id = cancel.cl_id;
  ack.eng_id = ord.eng_id;
  ack.status = "cancelled";
  out.emplace_back(std::move(ack));
  // Commit: neither line can throw (a plain assignment, and associative-container erase is
  // specified not to — LiveLess compares a uint64 and a string_view).
  ord.st = OrdState::Cancelled;
  live_index(ord.side).erase(it); // leave the live index (erases the stored iterator)
  return out;
}

std::vector<Routed> OrderEngine::on_book(const Book &book) {
  std::vector<Routed> out;
  // Idempotency guard: the fill sweep runs exactly once per TOB update — a redelivered
  // or rewound md_seq is ignored (counted, never silent) and the book is not
  // installed. book_seen_ keeps the guard off the very FIRST book: a real first update
  // carrying md_seq 0 must install and sweep, not vanish into the default-constructed
  // sentinel (header contract).
  if (book_seen_ && book.md_seq <= book_.md_seq) {
    ++stale_books_ignored_;
    return out;
  }
  // One fill computed against the CANDIDATE book but NOT yet applied: where the order sits
  // in its live index (*lit is that order's orders_ iterator) and the leaves the fill would
  // leave behind. The whole batch is staged before ANY state moves, which is what makes the
  // sweep strongly exception-safe: every allocation it performs (this vector, the result
  // vector, each Fill's cl_id copy) happens while the engine is still untouched, so a
  // std::bad_alloc leaves the same book presentable again. Committing first was
  // unrecoverable in a way the other entry points are not — `book_` would already be
  // installed, so the retry is IGNORED as stale and the fills lost to the failure could
  // never be replayed, on top of leaving stolen leaves, burnt exec_ids and possibly a Live
  // order with leaves == 0 behind.
  struct StagedFill {
    LiveIndex::iterator lit;
    Side side; // the index lit belongs to — the one a completed order must leave
    std::int64_t leaves_after;
  };
  std::vector<StagedFill> staged;        // stays empty (hence allocation-free) on a no-fill sweep
  std::uint64_t exec_id = next_exec_id_; // burned only by the commit below
  // Bids before asks (documented tie-break); within a side, the live index iterates in
  // (session, cl_id) key order — only LIVE orders are ever touched, each reached
  // through its stored orders_ iterator (retained terminal history is memory, never
  // sweep time). The book is exogenous (F-30): fills do not consume displayed
  // quantity.
  for (const Side side : {Side::Bid, Side::Ask}) {
    auto &live = live_index(side);
    const std::int64_t opp_px = side == Side::Bid ? book.ask_px : book.bid_px;
    const std::int64_t opp_qty = side == Side::Bid ? book.ask_qty : book.bid_qty;
    if (opp_qty <= 0 || opp_px <= 0)
      continue; // side absent (qty rule), or corrupt px <= 0 (defense-in-depth): no fill
    for (auto it = live.begin(); it != live.end(); ++it) {
      const auto oit = *it;           // an orders_ iterator: the indexed order IS its map entry
      const Order &ord = oit->second; // read-only: nothing is applied before the commit
      const std::int64_t px_raw = to_raw_px(inst_, ord.px);
      const bool crossed = side == Side::Bid ? opp_px <= px_raw : opp_px >= px_raw;
      if (!crossed)
        continue;
      // Un-rounded: min(leaves, opposite top), no lot flooring — lot granularity is an
      // entry-validation concern, not a fill-sizing one. Each live order is visited once
      // per sweep, so the staged view never has to account for an earlier fill of its own.
      const std::int64_t fill_qty = std::min(ord.leaves, opp_qty);
      Fill fill;
      fill.cl_id = oit->first.second; // the key owns the only stored cl_id copy
      fill.eng_id = ord.eng_id;
      fill.px = px_raw; // the order's own limit price (passive/maker semantics)
      fill.qty = fill_qty;
      fill.leaves = ord.leaves - fill_qty;
      fill.exec_id = exec_id++;
      staged.push_back(StagedFill{.lit = it, .side = side, .leaves_after = fill.leaves});
      out.push_back(Routed{.session = session_of(oit), .msg = OutMsg{std::move(fill)}});
    }
  }
  // Commit: the batch exists, so nothing below may throw (assignments and set::erase, which
  // the standard specifies as non-throwing; erasing one element leaves every other staged
  // live-index iterator valid).
  book_seen_ = true;
  book_ = book;
  next_exec_id_ = exec_id;
  for (const auto &[lit, side, leaves_after] : staged) {
    Order &ord = (*lit)->second;
    ord.leaves = leaves_after;
    if (leaves_after == 0) {
      ord.st = OrdState::Filled; // terminal: leaves the live index right here
      live_index(side).erase(lit);
    }
  }
  return out;
}

const Book &OrderEngine::book() const { return book_; }

std::size_t OrderEngine::live_count(std::uint64_t session) const {
  // O(log n_live) + a walk of the session's LIVE entries only (<= max_live_orders) —
  // never a scan of retained history. Derived from the live indexes, so it cannot
  // drift from what the sweep would touch.
  std::size_t count = 0;
  for (const Side side : {Side::Bid, Side::Ask}) {
    const auto [lo, hi] = session_range(live_index(side), session);
    count += static_cast<std::size_t>(std::distance(lo, hi));
  }
  return count;
}

std::uint64_t OrderEngine::stale_books_ignored() const { return stale_books_ignored_; }

std::size_t OrderEngine::entry_count(std::uint64_t session) const {
  const auto it = entries_by_session_.find(session);
  return it == entries_by_session_.end() ? 0 : it->second;
}

bool OrderEngine::entry_cap_breached(std::uint64_t session) const {
  return entry_count(session) >= max_session_entries_;
}

std::vector<OutMsg> OrderEngine::end_session(std::uint64_t session) {
  std::vector<OutMsg> out;
  const auto [lo, hi] = session_range(orders_, session);
  // The WHOLE ack batch is built before a single order is touched. Cancelling as we went
  // meant a failed allocation mid-batch left orders marked Cancelled but still in the live
  // index — where the next on_book would FILL them, breaking the one invariant the state
  // machine rests on (terminal states are absorbing) — while the teardown could never be
  // repeated, since a retry skips the already-Cancelled orders and emits nothing. Staging
  // first makes the whole call retryable: a throw here leaves every order Live, exactly as
  // the session left it.
  for (auto it = lo; it != hi; ++it) {
    const Order &ord = it->second;
    if (ord.st != OrdState::Live)
      continue; // exactly one CancelAck per LIVE order; terminals are absorbing
    CancelAck ack;
    ack.cl_id = it->first.second; // the key owns the only stored cl_id copy
    ack.eng_id = ord.eng_id;
    ack.status = "cancelled";
    out.emplace_back(std::move(ack));
  }
  // Commit — non-throwing (associative-container erase is specified not to throw, and these
  // comparators compare a uint64 and a string_view). No order is marked Cancelled first:
  // every entry the loop above acked is destroyed by the reap below, so the state write would
  // be to memory that is about to be freed.
  //
  // Reap: this call ENDS the session's lifetime. Invariant 4 scopes cl_id uniqueness
  // to the life of the session (03 §3.2) and the epoch never recurs (D6), so the whole
  // key range — live, tombstones, filled, cancelled — is unaddressable afterwards and
  // is reclaimed here, along with its index and cap-count entries. ORDER MATTERS: the
  // live-index comparator dereferences the stored orders_ iterators, so the index
  // ranges MUST be erased BEFORE the map nodes they point into are freed (header
  // HAZARD note on LiveLess) — the reverse order is use-after-free.
  for (const Side side : {Side::Bid, Side::Ask}) {
    auto &live = live_index(side);
    const auto [first, last] = session_range(live, session);
    live.erase(first, last);
  }
  orders_.erase(lo, hi);
  entries_by_session_.erase(session);
  return out;
}

} // namespace mm
