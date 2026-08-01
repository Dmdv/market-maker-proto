// OrderEngine implementation. The contracts, check order, fill rule, unit conventions and
// retention rules are documented in mm/engine.hpp; this TU keeps them mechanical.
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

// Wire side spelling -> Side. A total function that guesses (`side == "B" ? Bid : Ask`) maps
// every unknown spelling to Ask; nullopt outside the acceptor set {"B", "S"} (types.hpp).
constexpr std::optional<Side> side_from_wire(std::string_view side) {
  if (side == "B")
    return Side::Bid;
  if (side == "S")
    return Side::Ask;
  return std::nullopt;
}

// The raw-wire-price <-> Ticks boundary (types.hpp): `.v` is reached through exactly these two
// functions and nowhere else, which is the one thing the strong type is for.
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

// Reasons for validate_order codes; QtyLimit folds two conditions (types.hpp) that only this
// text distinguishes. Returns a view: the heap copy is made inside on_new's rollback region.
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
  // Backstop for the non-validate_order codes and out-of-range casts: the raw wire spelling.
  // No default label (types.hpp convention) so -Wswitch fires on an appended enumerator.
  return to_string(code);
}

} // namespace

OrderEngine::OrderEngine(Instrument inst, std::size_t max_session_entries)
    : inst_{std::move(inst)}, max_session_entries_{max_session_entries} {
  // Fail loudly at construction (header contract): these values would otherwise reach `% 0` or
  // signed-overflow UB in the entry conversion and the fill sweep, or reject every order.
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
  // 1. Duplicate cl_id — per-session namespace, terminal orders included (invariant 4). ONE
  // descent answers it and hints every insert below; nothing in between touches orders_.
  const auto lb = orders_.lower_bound(OrderKeyProbe{session, order.cl_id});
  if (lb != orders_.end() && lb->first.first == session && lb->first.second == order.cl_id) {
    out.emplace_back(
        make_reject(order.cl_id, RejectCode::DupClOrdId, "cl_id already used on this session"));
    return out;
  }
  // The entry-counter node is reserved up front (both outcomes record an entry), which makes the
  // committing ++ non-throwing; try_emplace so a node THIS call created is erased on a throw.
  const auto reserved = entries_by_session_.try_emplace(session, 0);
  const auto counter = reserved.first;
  const bool counter_is_new = reserved.second;
  const auto undo_counter = [&] {
    if (counter_is_new)
      entries_by_session_.erase(counter);
  };
  // Every non-duplicate reject stores a Rejected tombstone: the cl_id stays consumed for the
  // session (invariant 4) and counts toward the cap. The reject reaches `out` before state moves.
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
  // PRECONDITION from check 2: validate_order rejects BadSide for every spelling outside
  // {"B", "S"}, so this optional is always engaged. Asserted rather than defaulted to a side.
  assert(parsed_side.has_value() && "validate_order must reject any side outside {B, S}");
  const Side side = *parsed_side;
  const bool crosses = side == Side::Bid
                           ? (book_.ask_qty > 0 && book_.ask_px > 0 && book_.ask_px <= order.px)
                           : (book_.bid_qty > 0 && book_.bid_px > 0 && book_.bid_px >= order.px);
  if (order.post_only && crosses) {
    reject(RejectCode::PostOnlyCross, "post-only order would cross the book");
    return out;
  }
  // Accept: px converts to Ticks at the boundary, leaves stays RAW (un-rounded fills). The key's
  // cl_id copy is the ONLY stored one; the live index stores the node's iterator.
  const Order ord{.eng_id = next_eng_id_,
                  .side = side,
                  .px = to_ticks(inst_, order.px),
                  .leaves = order.qty,
                  .st = OrdState::Live};
  // Commit, ordered for the STRONG guarantee: the ack (with its cl_id copy) reaches `out` first
  // and every mutation below is rolled back — Live but unindexed would be two divergent views.
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
  // The ack is in `out` BEFORE the order leaves Live: a cancel that commits and then loses its
  // report is UNRECOVERABLE — the retry answers AlreadyTerminal, so the client is never told.
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
  // Idempotency guard: the sweep runs once per TOB update — a redelivered or rewound md_seq is
  // ignored (counted) and not installed. book_seen_ lets a real first update carry md_seq 0.
  if (book_seen_ && book.md_seq <= book_.md_seq) {
    ++stale_books_ignored_;
    return out;
  }
  // Staged against the CANDIDATE book before any state moves: that is what makes the sweep
  // strongly exception-safe — committing first installs book_, so the retry is ignored as stale.
  struct StagedFill {
    LiveIndex::iterator lit;
    Side side; // the index lit belongs to — the one a completed order must leave
    std::int64_t leaves_after;
  };
  std::vector<StagedFill> staged;        // stays empty (hence allocation-free) on a no-fill sweep
  std::uint64_t exec_id = next_exec_id_; // burned only by the commit below
  // Bids before asks (documented tie-break); within a side the live index iterates in
  // (session, cl_id) order over LIVE orders only. The book is exogenous: fills never consume it.
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
      // Un-rounded min(leaves, opposite top), no lot flooring: lot granularity is an
      // entry-validation concern. Each live order is visited once per sweep.
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
  // Commit: the batch exists, so nothing below may throw (assignments and set::erase, specified
  // non-throwing; erasing one element leaves every other staged iterator valid).
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
  // O(log n_live) + a walk of the session's LIVE entries only (<= max_live_orders), never a scan
  // of retained history. Derived from the live indexes, so it cannot drift from the sweep.
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
  // The WHOLE ack batch is built before a single order is touched: cancelling as we went would
  // leave Cancelled orders in the live index on a throw — fillable, and the retry emits nothing.
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
  // Reap (erase is non-throwing): invariant 4 scopes cl_id to the session and epochs never recur.
  // ORDER: erase the live indexes BEFORE the orders_ nodes LiveLess dereferences (use-after-free).
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
