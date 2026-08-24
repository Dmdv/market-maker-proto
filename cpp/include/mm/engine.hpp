// Order engine core (plan Task 3): state machine + deterministic fill rule, SESSION-SCOPED.
//
// Pure logic: no IO, no clocks — fully unit-testable. Envelope fields (seq, epoch) and
// OrderAck.svc_ns are stamped by the SERVER SESSION at send time — never here, and never by the
// Outbox either, which holds typed messages and writes to no message field at all (F-06). The
// engine trusts the `session` argument and never reads a command's own epoch (StaleEpoch is a
// Server-side verdict).
//
// CALLER CONTRACT
//
// States (03 §3.1): Live | Filled | Cancelled | Rejected — the engine has NO pending states
// (it processes each command to completion before looking at the next, so there is nothing
// to be pending about; PendingNew/PendingCancel are CLIENT states). Terminal states are
// ABSORBING (invariant 1).
//
// Session scoping (audit F-01/F-07): orders are OWNED by the session that created them; the
// session key IS the connection epoch (record D6). cl_id namespace, duplicate detection, the
// live-order cap and cancel-on-disconnect are all PER SESSION — a cancel naming another
// session's cl_id is UnknownClOrdId.
//
// Retention: EVERY (session, cl_id) presented to on_new is consumed for the session's
// lifetime — a rejected NewOrder stores a Rejected TOMBSTONE, so a retry of that cl_id is
// DupClOrdId and a cancel of it AlreadyTerminal (invariant 4 uniform across ALL terminal
// states). A tombstone burns no eng_id and is excluded from the fill sweep, live_count and
// end_session's acks.
//
// end_session(session) is DESTRUCTIVE and valid ONLY at session close: it acks each still-live
// order, then REAPS the session's whole (session, cl_id) range. It is NOT a mid-session
// "cancel everything" — after it returns the cl_id history is gone and the key is a fresh
// namespace, which on a still-connected session would break invariant 4 live (used cl_ids
// accepted again; cancels of once-live cl_ids answered UnknownClOrdId, not AlreadyTerminal).
// The reap is contract-preserving: invariant 4 scopes cl_id uniqueness to the life of the
// session (03 §3.2) and the epoch never recurs (D6), so a reaped range is unaddressable.
//
// on_new check order (documented contract; tests pin the observable slices):
//   duplicate cl_id (incl. terminal) -> validate_order (types.hpp: symbol/side/tick/lot/
//   qty/price, in that order) -> MaxLiveOrders (per session, R-1) -> PostOnlyCross (only
//   when post_only).
// eng_id is assigned on ACCEPT only. Documented decision: a post_only=false entry that would
// cross does NOT match at accept — on_new's contract is OrderAck | Reject only (plan Task 3
// interface) — it rests and fills on the NEXT on_book evaluation. The engine therefore
// enforces the "never cross" control only for post_only=true commands (the only mode the MM
// strategy uses); for post_only=false the defense is the CLIENT's quoting rules alone.
//
// Fill rule (03 §3.3): a resting bid at P fills when ask_px <= P, a resting ask at P when
// bid_px >= P; fill qty = min(leaves, opposite top qty) with NO lot rounding (lot granularity
// is an entry-validation concern, not a fill-sizing one, so partial fills may leave a
// non-lot-multiple `leaves`); fill price is the order's OWN limit P (passive/maker). Evaluated
// once per TOB update, BIDS BEFORE ASKS (documented tie-break); within a side, in
// (session, cl_id) key order — the live indexes iterate in exactly the ratified full-map key
// order, so the determinism byte-pin is unchanged. The book is EXOGENOUS (F-30): fills never
// consume displayed quantity. A side is ABSENT when qty <= 0 whatever its px (px is not an
// absence sentinel in the feed), and a px <= 0 side with qty > 0 is treated absent too
// (defense-in-depth: a zero px must never cross every resting bid). The sweep and the
// post-only entry check share that one absence rule, which is the engine's WHOLE feed-sanity
// stance — SEMANTIC feed validation belongs to the Task 5 feed.
//
// md_seq guard: the FIRST book is installed and swept whatever its md_seq — including 0 —
// because a default-constructed sentinel cannot be told apart from a real first md_seq of 0
// (tracked explicitly via book_seen_). After that a book with md_seq <= the last PROCESSED
// one is IGNORED (no fills, book() unchanged, stale_books_ignored() incremented):
// exactly-once evaluation per TOB update even if the caller redelivers or rewinds. This
// <=-last-processed check is the engine's ONLY md_seq consumption — no gap detection, no
// reordering, no gap/decrease classification (those belong to the protocol layer's close-code
// table and the client's two-counter rule); the feed remains the sequencing AUTHORITY (F-09:
// monotonic by construction), so the guard is defense-in-depth, not a license to replay.
//
// Units (types.hpp boundary rule): commands and the Book carry RAW wire units; validate_order
// runs in raw units; px converts to Ticks for storage while Order::leaves stays RAW
// (un-rounded fills). Outbound Fill fields are raw units again.
//
// Complexity (honest bounds — these seed the §5.2 100k-sample benchmark expectations):
//   on_book    — O(L_live) per TOB update and HISTORY-BLIND: the live indexes store
//                orders_ ITERATORS (stable across insert/erase of other elements), so
//                the sweep reaches each live order directly, with no per-order tree
//                descent over retained history.
//   on_new /   — ONE O(log n) descent of orders_ per command, where n counts live +
//   on_cancel    RETAINED TERMINAL entries: on_new's duplicate probe is a lower_bound whose
//                result is reused as the insertion hint (amortized O(1) insert), on_cancel is
//                a single find. Retained history costs a log factor here (~20 key compares at
//                the 1M-entry cap); the LINEAR history walk is gone entirely.
//   live_count — O(log n_live) + a walk of that session's live keys
//                (<= max_live_orders).
// Retained terminal history therefore costs memory always, a log factor on command
// time, and ZERO sweep time.
//
// Exception safety: EVERY entry point offers the STRONG guarantee — an allocation failure
// anywhere in a call leaves the engine exactly as it was, so the command can simply be
// presented again. ONE rule buys it: each entry point builds its COMPLETE result batch first
// (that is where the allocations are) and mutates only afterwards, in a commit sequence that
// cannot throw — assignments, ++counters and associative-container erase, which the standard
// specifies as non-throwing. What each entry point would otherwise lose:
//   on_new      — a half-committed accept is Live in orders_ but absent from the live index:
//                 invisible to on_book and live_count (so the per-session cap under-counts
//                 forever) yet still cancellable through orders_. The live-index node is the
//                 one allocation that must follow the map insert, so a failure there rolls
//                 the map node back; the session's entry-counter node is reserved before the
//                 commit and erased again if this call created it.
//   on_cancel   — a cancel that commits and then loses its CancelAck is UNRECOVERABLE: the
//                 retry answers AlreadyTerminal, so the client is never told.
//   on_book     — worst of the four, because installing the book makes the retry STALE: fills
//                 lost to the failure could never be replayed, on top of stolen leaves, burnt
//                 exec_ids and a half-applied multi-fill sweep. The whole batch is therefore
//                 computed against the CANDIDATE book and applied only once it exists.
//   end_session — cancelling as it acked left Cancelled orders in the live index, where the
//                 next on_book would FILL them (terminal states are absorbing), and the
//                 teardown could not be repeated: a retry skips them and emits nothing.
// The guarantee is pinned by fault injection in cpp/tests/test_engine_alloc.cpp, which fails
// each allocation of each entry point in turn.
//
// Measured per-call allocation counts and per-entry residency — the quantified exception to
// the plan's zero-allocation constraint (plan Global Constraints) and the seed for the §6
// allocation counter — live in the TU comment of `cpp/src/engine.cpp`, where the measurement
// is taken; the stdlib-independent rows are pinned by `cpp/tests/test_engine_alloc.cpp`.
// Zero per-message logging on measured paths.
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

// Engine-side order state. NOT self-describing: the cl_id lives ONLY in the owning
// container's (session, cl_id) key — duplicating it here would cost one heap string per
// accept/tombstone inside the M2 window plus ~25% of retained-entry residency at the
// 64 B cl_id cap, and both readers (the fill sweep, end_session) already hold the key.
// All members carry defaults so construction sites name only the fields they mean.
struct Order {
  std::uint64_t eng_id{};
  Side side{Side::Bid};
  Ticks px{};
  // REMAINING size, in RAW qty units — raw, not Lots, because fills are un-rounded: a
  // partial fill may leave a non-lot-multiple remainder that whole Lots cannot represent.
  // The ORIGINAL order qty is deliberately not stored: nothing in the engine reads it (the
  // client holds its own copy of the order it sent, and no report carries a cumulative
  // filled quantity), and an unread member is a contract no test can falsify. If a later
  // task needs cumulative fill, it arrives together with the accessor that exposes it.
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
  // retention means a peer can mint map entries at frame rate using only INVALID orders
  // (rejects are not gated by max_live_orders), so this cap is the only bound on that
  // growth; the resident cost it bounds is inventoried in the cpp/src/engine.cpp TU comment.
  // Overridable per instance for testability; the default is ~10x the Task 11 react-mode
  // cycle count, so the §5.2 benchmark load can never trip it.
  static constexpr std::size_t kDefaultMaxSessionEntries = 1u << 20;

  // Throws std::invalid_argument unless ALL six hold: tick_size > 0, lot_size > 0,
  // max_live_orders >= 0, max_order_qty > 0, min_price <= max_price and
  // max_session_entries > 0 (the fill sweep and entry conversion divide/multiply by
  // tick/lot — an invalid Instrument must fail loudly at construction, never reach
  // signed-division UB mid-sweep; a negative max_live_orders, a non-positive
  // max_order_qty or min_price > max_price would each silently reject every order
  // (MaxLiveOrders / QtyLimit / PriceRange respectively), and max_session_entries == 0
  // would flag entry_cap_breached for a session that has never sent anything, closing
  // it 1008 before it can act — all are nonsensical configurations, same fail-loudly
  // policy).
  explicit OrderEngine(Instrument inst,
                       std::size_t max_session_entries = kDefaultMaxSessionEntries);

  // MOVE-ONLY, and both halves are load-bearing. The live indexes hold iterators into
  // orders_ (see LiveLess below), so an implicitly copied engine would carry indexes
  // pointing at the SOURCE's map nodes — a use-after-free the moment either engine is
  // destroyed or reindexes. Copying is therefore deleted rather than made deep. Move is
  // safe and MUST stay declared: std::map/std::set move-construction transfers nodes, so
  // the stored iterators keep referring to the same (now moved-in) nodes; and a
  // user-declared copy constructor suppresses the implicit move, which would break every
  // by-value factory (the tests' primed_engine() returns an engine by value, and copy
  // elision is permitted, not guaranteed).
  OrderEngine(const OrderEngine &) = delete;
  OrderEngine &operator=(const OrderEngine &) = delete;
  OrderEngine(OrderEngine &&) = default;
  OrderEngine &operator=(OrderEngine &&) = default;

  // [[nodiscard]] across the API: the returned vectors are the ONLY channel through
  // which client-visible reports leave the engine — a discarded result is a silently
  // lost ACK/CancelAck/Fill/Reject batch that would compile clean otherwise.

  // -> OrderAck | Reject (post-only rejects at entry; check order documented above).
  [[nodiscard]] std::vector<OutMsg> on_new(std::uint64_t session, const NewOrder &order);
  // -> CancelAck | Reject{UnknownClOrdId | AlreadyTerminal}.
  [[nodiscard]] std::vector<OutMsg> on_cancel(std::uint64_t session, const CancelOrder &cancel);
  // Fill sweep over the LIVE orders only; bids before asks; Server routes by .session.
  // The first book ever seen is installed unconditionally; after that a book with
  // md_seq <= the last processed one is IGNORED (empty result, book() unchanged,
  // stale_books_ignored() incremented) — exactly-once evaluation per TOB update
  // (header contract above).
  [[nodiscard]] std::vector<Routed> on_book(const Book &book);

  [[nodiscard]] const Book &book() const;
  [[nodiscard]] std::size_t live_count(std::uint64_t session) const;

  // Count of TOB updates discarded by the md_seq idempotency guard. The feed is
  // monotonic by construction (F-09), so ANY nonzero value in production is a Task 5/7
  // producer regression — without a counter such a regression would make the engine
  // silently deaf (no fills, no M3 samples, no diagnostic), breaking the
  // count-every-drop discipline every other drop path follows (docs/assignment.md
  // makes visibility the stated purpose of the sequence controls). Single-writer plain
  // uint64 (telemetry rule A1).
  [[nodiscard]] std::uint64_t stale_books_ignored() const;

  // Total (session, cl_id) entries for the session — live + terminal, tombstones
  // included (the quantity the entry cap bounds).
  [[nodiscard]] std::size_t entry_count(std::uint64_t session) const;
  // Policy signal: true once entry_count has reached max_session_entries. The engine keeps
  // RECORDING past the cap — invariant 4 must hold until the session dies — so this is not a
  // reject: no §2.3 reject code fits, and reusing MaxLiveOrders would lie about state. The
  // signal is only a signal if its consumer checks it.
  [[nodiscard]] bool entry_cap_breached(std::uint64_t session) const;

  // Cancel-on-disconnect: cancels ONLY that session's live orders, one CancelAck each,
  // then REAPS the session's whole entry range — a DESTRUCTIVE call that ENDS the
  // session's lifetime (caller contract above). Named for what it does: this is NOT a
  // mid-session "cancel everything" — after it returns, the session's cl_id history is
  // gone and its key is a fresh namespace.
  [[nodiscard]] std::vector<OutMsg> end_session(std::uint64_t session);

private:
  // The engine's order key, named ONCE for the three containers and every
  // session-range walk: .first is the session, .second the cl_id.
  using OrderKey = std::pair<std::uint64_t, std::string>;
  // Heterogeneous probe built straight over a command's cl_id: lookups never
  // materialize a heap key copy on the M2-measured path.
  using OrderKeyProbe = std::pair<std::uint64_t, std::string_view>;

  // Transparent (session, cl_id) ordering — same lexicographic order as the default
  // std::less over OrderKey, so iteration order (and the determinism byte-pin) matches
  // the plan's ratified plain-map keying. An implementation detail, deliberately NOT
  // at namespace scope: it is not plan-normative API, and public visibility would
  // freeze the internal ordering decision into the contract.
  struct KeyLess {
    using is_transparent = void;
    template <class A, class B> bool operator()(const A &a, const B &b) const {
      if (a.first != b.first)
        return a.first < b.first;
      return std::string_view{a.second} < std::string_view{b.second};
    }
  };
  using OrdersMap = std::map<OrderKey, Order, KeyLess>;

  // Live-index ordering: elements are orders_ ITERATORS (stable across insert/erase
  // of OTHER elements), compared by the referenced key — the index invariant is
  // structural (an indexed order IS its map entry; no by-key re-find to drift) and
  // the sweep touches no retained history. HAZARD, load-bearing for end_session: this
  // comparator DEREFERENCES the stored iterators, so any bulk erase from orders_ must
  // remove the corresponding live-index range FIRST — comparing an iterator into a
  // freed node is use-after-free. Same reason the class is MOVE-ONLY (see the deleted
  // copy operations above): a copied index would alias the source engine's nodes, while
  // moving is safe because std::map/std::set move-construction transfers the nodes
  // themselves.
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
  // Deliberately lower_bound + a forward walk, NOT upper_bound(session_floor(session
  // + 1)): session + 1 overflows at UINT64_MAX and would turn the range into the whole
  // container. Recorded once HERE — every caller shares this one definition of "this
  // session's elements".
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
  // Keyed (session, cl_id); kept incl. terminal for the session's lifetime (invariant
  // 4); reaped by end_session. (The member-list delta vs the plan's ratified listing
  // is queued in docs/PENDING_AMENDMENTS.md item (h).) KeyLess is transparent:
  // lookups take string_view probes, allocation-free.
  OrdersMap orders_;
  // Live-order indexes: iterators to the Live entries only, ordered by the referenced
  // (session, cl_id) key — sweep order, and the determinism byte-pin, match the
  // ratified full-map key order. Maintained on accept and on every Live -> terminal
  // transition.
  LiveIndex live_bids_, live_asks_;
  // Total entries per session (live + terminal): O(log n) entry_count for the cap.
  std::map<std::uint64_t, std::size_t> entries_by_session_;
};

} // namespace mm
