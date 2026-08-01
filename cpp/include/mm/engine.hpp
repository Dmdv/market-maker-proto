// Order engine core: pure state machine and deterministic fill rule, scoped per session.
// No IO and no clocks; envelope fields (seq, epoch) and OrderAck.svc_ns are stamped by the server.
//
// States: Live | Filled | Cancelled | Rejected, terminal states absorbing. No pending states —
// each command runs to completion before the next (PendingNew/PendingCancel are client states).
//
// Session scoping: orders are owned by the session that created them; the cl_id namespace,
// duplicate detection, the live-order cap and cancel-on-disconnect are all per session.
//
// Retention: every (session, cl_id) presented to on_new is consumed for the session's lifetime;
// a rejected NewOrder leaves a Rejected tombstone (retry -> DupClOrdId, cancel -> AlreadyTerminal).
//
// end_session is destructive and valid only at session close: it acks the still-live orders, then
// reaps the session's whole cl_id range. It is not a mid-session "cancel everything".
//
// on_new check order: duplicate cl_id (incl. terminal) -> validate_order (types.hpp) ->
// MaxLiveOrders -> PostOnlyCross. eng_id is assigned on accept only.
//
// A crossing post_only=false order is not matched at entry: on_new answers OrderAck | Reject only,
// so it rests and fills on the next on_book. "Never cross" is enforced for post_only alone.
//
// Fill rule: a resting bid at P fills when ask_px <= P, a resting ask at P when bid_px >= P;
// fill qty = min(leaves, opposite top qty), no lot rounding; fill price is the order's own P.
//
// Sweep: once per TOB update, bids before asks, then in (session, cl_id) key order. The book is
// exogenous — fills never consume displayed quantity.
//
// A book side is absent when qty <= 0 whatever its px, and when px <= 0 with qty > 0: a zero px
// must never cross every resting bid. Semantic feed validation belongs to the feed.
//
// md_seq guard: the first book is installed whatever its md_seq (book_seen_ tells it from a
// sentinel); afterwards md_seq <= the last processed one is ignored — exactly-once per update.
//
// Units (types.hpp boundary rule): commands and Book carry raw wire units; px converts to Ticks
// for storage while Order::leaves stays raw. Outbound Fill fields are raw units again.
//
// Complexity: on_book is O(L_live) and history-blind — the live indexes hold orders_ iterators;
// on_new and on_cancel are one O(log n) descent each, n = live + retained terminal entries.
//
// Exception safety: every entry point offers the STRONG guarantee — each builds its complete
// result batch first, then commits with operations that cannot throw.
//
#pragma once

#include "mm/protocol.hpp"
#include "mm/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm {

enum class OrdState : std::uint8_t { Live, Filled, Cancelled, Rejected }; // NO pending states

// Engine-side order state, deliberately not self-describing: the cl_id lives only in the owning
// container's (session, cl_id) key, and both readers of an Order already hold that key.
struct Order {
  std::uint64_t eng_id{};
  Side side{Side::Bid};
  Ticks px{};
  // REMAINING size in RAW qty units, not Lots: fills are un-rounded, so a partial fill can leave
  // a non-lot-multiple remainder. The original order qty is not stored — nothing here reads it.
  std::int64_t leaves{};
  OrdState st{OrdState::Live};
};

struct Book {
  std::int64_t bid_px{}, bid_qty{}, ask_px{}, ask_qty{};
  std::uint64_t md_seq{};
};

// A report plus the session it belongs to: on_book sweeps ALL sessions' orders, and the
// Server routes each fill report to its owning session by .session.
struct Routed {
  std::uint64_t session;
  OutMsg msg;
};

class OrderEngine {
public:
  // Default per-session cap on TOTAL (session, cl_id) entries, live + terminal. Tombstone
  // retention lets a peer mint entries from invalid orders alone, so this is the only bound.
  static constexpr std::size_t kDefaultMaxSessionEntries = 1u << 20;

  // Throws std::invalid_argument unless all six hold: tick_size > 0, lot_size > 0,
  // max_live_orders >= 0, max_order_qty > 0, min_price <= max_price, max_session_entries > 0.
  explicit OrderEngine(Instrument inst,
                       std::size_t max_session_entries = kDefaultMaxSessionEntries);

  // MOVE-ONLY: the live indexes hold iterators into orders_, so a copy would index the SOURCE's
  // nodes. Move stays declared and is safe: std::map/std::set move transfers the nodes themselves.
  OrderEngine(const OrderEngine &) = delete;
  OrderEngine &operator=(const OrderEngine &) = delete;
  OrderEngine(OrderEngine &&) = default;
  OrderEngine &operator=(OrderEngine &&) = default;

  // [[nodiscard]] across the API: these vectors are the ONLY channel for client-visible reports,
  // so a discarded result is a silently lost ACK/CancelAck/Fill/Reject batch.

  // -> OrderAck | Reject (post-only rejects at entry; check order documented above).
  [[nodiscard]] std::vector<OutMsg> on_new(std::uint64_t session, const NewOrder &order);
  // -> CancelAck | Reject{UnknownClOrdId | AlreadyTerminal}.
  [[nodiscard]] std::vector<OutMsg> on_cancel(std::uint64_t session, const CancelOrder &cancel);
  // Fill sweep over the LIVE orders only; bids before asks; Server routes by .session. A book
  // with md_seq <= the last processed one is ignored (empty result, book() unchanged).
  [[nodiscard]] std::vector<Routed> on_book(const Book &book);

  [[nodiscard]] const Book &book() const;
  [[nodiscard]] std::size_t live_count(std::uint64_t session) const;

  // Count of TOB updates discarded by the md_seq guard. The feed is monotonic by construction,
  // so any nonzero value is a producer regression. Single-writer plain uint64 (telemetry rule A1).
  [[nodiscard]] std::uint64_t stale_books_ignored() const;

  // Total (session, cl_id) entries for the session — live + terminal, tombstones
  // included (the quantity the entry cap bounds).
  [[nodiscard]] std::size_t entry_count(std::uint64_t session) const;
  // Policy signal: true once entry_count reaches max_session_entries. The engine keeps RECORDING
  // past the cap (cl_id history must outlive it), so it is not a reject: the caller must check it.
  [[nodiscard]] bool entry_cap_breached(std::uint64_t session) const;

  // Cancel-on-disconnect: cancels ONLY that session's live orders, one CancelAck each, then
  // reaps the session's whole entry range. Destructive — it ends the session's lifetime.
  [[nodiscard]] std::vector<OutMsg> end_session(std::uint64_t session);

private:
  // The engine's order key, named ONCE for the three containers and every
  // session-range walk: .first is the session, .second the cl_id.
  using OrderKey = std::pair<std::uint64_t, std::string>;
  // Heterogeneous probe built straight over a command's cl_id: lookups never
  // materialize a heap key copy on the hot path.
  using OrderKeyProbe = std::pair<std::uint64_t, std::string_view>;

  // Transparent (session, cl_id) ordering, identical to std::less over OrderKey, so iteration
  // order is the plain-map key order. Private on purpose: the ordering is not public API.
  struct KeyLess {
    using is_transparent = void;
    template <class A, class B> bool operator()(const A &a, const B &b) const {
      if (a.first != b.first)
        return a.first < b.first;
      return std::string_view{a.second} < std::string_view{b.second};
    }
  };
  using OrdersMap = std::map<OrderKey, Order, KeyLess>;

  // Live-index ordering: elements are orders_ ITERATORS, compared by the referenced key. HAZARD:
  // the comparator DEREFERENCES them, so a bulk erase from orders_ must clear this index first.
  struct LiveLess {
    using is_transparent = void;
    bool operator()(OrdersMap::const_iterator a, OrdersMap::const_iterator b) const {
      return KeyLess{}(a->first, b->first);
    }
    bool operator()(OrdersMap::const_iterator a, const OrderKeyProbe &b) const {
      return KeyLess{}(a->first, b);
    }
    bool operator()(const OrderKeyProbe &a, OrdersMap::const_iterator b) const {
      return KeyLess{}(a, b->first);
    }
  };
  using LiveIndex = std::set<OrdersMap::iterator, LiveLess>;

  // The session component of an element of ANY of the containers above — the ONE
  // spelling every walk's termination predicate uses, whatever the container shape.
  static std::uint64_t session_of(const OrdersMap::value_type &entry) { return entry.first.first; }
  static std::uint64_t session_of(OrdersMap::const_iterator it) { return it->first.first; }

  // First possible key of `session`: the empty cl_id sorts before every other string.
  static OrderKeyProbe session_floor(std::uint64_t session) {
    return {session, std::string_view{}};
  }

  // [lo, hi) of `session`'s elements in any (session, cl_id)-ordered container above.
  // Deliberately a forward walk, not upper_bound(session + 1): session + 1 overflows at UINT64_MAX.
  template <class Container> static auto session_range(Container &c, std::uint64_t session) {
    const auto lo = c.lower_bound(session_floor(session));
    auto hi = lo;
    while (hi != c.end() && session_of(*hi) == session)
      ++hi;
    return std::pair{lo, hi};
  }

  [[nodiscard]] LiveIndex &live_index(Side side) {
    return side == Side::Bid ? live_bids_ : live_asks_;
  }
  [[nodiscard]] const LiveIndex &live_index(Side side) const {
    return side == Side::Bid ? live_bids_ : live_asks_;
  }

  Instrument inst_;
  Book book_;
  bool book_seen_{false}; // distinguishes "no book yet" from a first md_seq of 0
  std::uint64_t next_eng_id_{1}, next_exec_id_{1};
  std::uint64_t stale_books_ignored_{0}; // md_seq-guard drops (single-writer, rule A1)
  std::size_t max_session_entries_;
  // Keyed (session, cl_id); entries are kept incl. terminal for the session's lifetime and
  // reaped by end_session. KeyLess is transparent: string_view probes, allocation-free lookups.
  OrdersMap orders_;
  // Live-order indexes: iterators to the Live entries only, in (session, cl_id) key order —
  // that is the sweep order. Maintained on accept and on every Live -> terminal transition.
  LiveIndex live_bids_, live_asks_;
  // Total entries per session (live + terminal): O(log n) entry_count for the cap.
  std::map<std::uint64_t, std::size_t> entries_by_session_;
};

} // namespace mm
