# PROTOCOL — `mm.v1`

The wire contract between the C++ engine and the Python client. This document is normative: where
it and a comment disagree, this document is the specification and the comment is the bug.

Transport is a WebSocket carrying **UTF-8 text frames**, one JSON object per frame, subprotocol
`mm.v1`. All prices and quantities are **integers in raw wire units** — never floats, at any layer.

---

## 1. Envelope

Every message in both directions carries the same four leading fields.

| field | type | semantics |
|---|---|---|
| `t` | string | Message type tag. The discriminator; see §2 and §3. |
| `v` | integer | Protocol major version. **Must be `1`.** Anything else is rejected — see §6. |
| `seq` | uint64 | Per-direction envelope counter. Starts at **1**, increments by exactly 1 per message sent **on that connection, in that direction** — with one exception after a reject, in §1.2. |
| `epoch` | uint64 | The engine-assigned session generation. See §7. |

### 1.1 The two-counter scheme, and why there are two

`seq` and `md_seq` count different things and a gap in each means something different. Conflating
them is the mistake this scheme exists to prevent.

| counter | scope | assigned by | may it gap? | what a gap means |
|---|---|---|---|---|
| `seq` | per connection, per direction | the sender | **No, ever** | Message loss or reordering on a stream that cannot lose or reorder. It is a **protocol violation** and the receiver closes **1002**. |
| `md_seq` | market data only | the engine, per TOB publish | **Yes, legitimately** | Conflation: the engine dropped intermediate book states under load and published only the newest. This is the designed backpressure policy for market data (§8), not an error. |

**The gap-classification table** — how a receiver must react:

| observation | classification | required action |
|---|---|---|
| inbound `seq` ≠ expected | protocol violation | close **1002**, do not process the message |
| inbound `md_seq` > last + 1 | conflation, expected under load | accept; the book is current, intermediate states are gone by design |
| inbound `md_seq` **==** last | the same book **restated** — a heartbeat | accept; refresh the freshness clock, but make **no quoting decision** — an unchanged book has no answer |
| inbound `md_seq` **<** last | regression — must never happen | stop quoting; client-side guard only (see below) |
| inbound `epoch` ≠ current | stale command from a previous session | engine replies `reject{STALE_EPOCH}`; the session stays up |

**Equality and regression are different events, and collapsing them breaks a live client.** A
restated book is how a quiet feed says "still here": the freshness clock must be refreshed on it,
because the stale rule asks whether the FEED is alive, not whether the price moved. Treating
equality as a regression would stop quoting on a healthy feed; treating it as a *change* would
re-quote both sides for no reason. So it refreshes freshness and returns no commands — and the
freshness update happens **before** the duplicate short-circuit, because doing it after once
declared a heartbeating feed stale and cancelled everything resting on it.

**`md_seq` regression is client-unit-tested only.** The engine assigns `md_seq` monotonically per
publish on a single owner thread, so a regression is impossible by construction on the producing
side and there is nothing to test there. The client still guards against it because a client is
obliged to distrust its feed, and that guard is covered by unit tests rather than by an integration
test — there is no way to make the real engine emit one.

### 1.2 Inbound `seq` consumption after a reject — the one exception, and it costs the connection

**A reject does not always consume the `seq` of the command it answers.** Get this wrong and the
next command looks like a gap, which is a protocol violation and closes **1002**. Nothing on the
wire states it directly, so it is stated here.

| the reject was raised… | did it consume the inbound `seq`? | what the client must do next |
|---|---|---|
| **before** the sequencer | **No** | **Reuse the same `seq`** |
| **after** the sequencer | Yes | Advance to `seq + 1`, as normal |

**The rule is decidable from `code` alone**, which is what makes it usable — and it has to be,
because a pre-sequencer reject carries an empty `cl_id` and the `reject` envelope stamps the
*outbound* counter, so nothing else in the message identifies the inbound command it answers.

- **Pre-sequencer (reuse the `seq`):** `MSG_TOO_LARGE`, and every decode failure —
  `MALFORMED`, `UNKNOWN_TYPE`, `UNSUPPORTED_VERSION`.
- **Post-sequencer (advance):** `STALE_EPOCH` and every engine verdict — `TICK_SIZE`,
  `LOT_SIZE`, `QTY_LIMIT`, `PRICE_RANGE`, `BAD_SIDE`, `UNKNOWN_SYMBOL`, `DUP_CLORDID`,
  `POST_ONLY_CROSS`, `MAX_LIVE_ORDERS`, `UNKNOWN_CLORDID`, `ALREADY_TERMINAL`.

**Why the split is real rather than an inconsistency to be tidied away.** The sequencer runs *after*
decode, so a frame that never decoded never reached it — the engine cannot consume a sequence
number it was never able to read, and a frame rejected for size never reached the codec at all.
Advancing anyway would mean guessing what the unparseable frame claimed its `seq` was.

The shipped Python client cannot normally trigger this: its encoder validates outbound messages, so
it does not emit frames that fail to decode. That makes the rule latent for `mmclient` and load-
bearing for **any other producer** — a test harness, a fuzzer, a reimplementation in another
language. It is pinned by one C++ case per pre-sequencer code, so a change that special-cases a
single code fails on its own.

---

## 2. Engine → client messages

### 2.1 `top_of_book`

The market data publication. Exogenous — see §5.3.

| field | type | units | semantics |
|---|---|---|---|
| `md_seq` | uint64 | — | Market-data sequence. Gaps mean conflation (§1.1). |
| `symbol` | string | — | Instrument. `MOCKUSDT` throughout. |
| `bid_px` | int64 | price units ($0.01) | Best bid price. |
| `bid_qty` | int64 | qty units (0.001 base) | Aggregate size at the best bid. |
| `ask_px` | int64 | price units | Best ask price. |
| `ask_qty` | int64 | qty units | Aggregate size at the best ask. |

### 2.2 `order_ack`

| field | type | semantics |
|---|---|---|
| `cl_id` | string | Echoes the client's order id. |
| `eng_id` | uint64 | The engine's own id for the order. Unique per session. |
| `status` | string | `"live"`. The order is resting. |
| `svc_ns` | int64 | **M2** — engine service time in nanoseconds for this command (§9). |

### 2.3 `cancel_ack`

`cl_id`, `eng_id`, and `status` = `"cancelled"`. The order is no longer resting and will produce no
further fills.

### 2.4 `fill`

| field | type | semantics |
|---|---|---|
| `cl_id` / `eng_id` | string / uint64 | The order that filled. |
| `px` | int64 | Execution price. **Always the order's own limit price** (§5.2). |
| `qty` | int64 | Quantity filled by this execution. |
| `leaves` | int64 | Quantity still resting **after** this fill. `leaves == 0` means fully filled and no longer live. |
| `exec_id` | uint64 | Unique execution id. |

### 2.5 `reject`

| field | type | semantics |
|---|---|---|
| `cl_id` | string | The rejected command's id, or `""` when the command could not be parsed far enough to recover one. |
| `code` | string | A value from §4, in **SCREAMING_SNAKE** exactly as listed there. Machine-readable and the stable part — match on this. The C++ `RejectCode` enum spells the same values in CamelCase (`TickSize`); those are **C++ identifiers and never appear on the wire**. |
| `reason` | string | Human-readable detail. **Never parse this** — it is for logs and humans. |

---

## 3. Client → engine messages

### 3.1 `new_order`

| field | type | units | semantics |
|---|---|---|---|
| `cl_id` | string | — | Client order id. Must be unique within the session; a repeat is `DUP_CLORDID`. |
| `symbol` | string | — | Must be a known instrument or `UNKNOWN_SYMBOL`. |
| `side` | string | — | `"B"` or `"S"`. Anything else is `BAD_SIDE`. |
| `px` | int64 | price units | Must be a positive multiple of `tick_size` (**5**) or `TICK_SIZE`. |
| `qty` | int64 | qty units | Must be a positive multiple of `lot_size` (**10**) or `LOT_SIZE`. |
| `post_only` | bool | — | **Required on the wire** — omitting it is `MALFORMED`, not an implied `true`. If true, an order that would cross the touch on arrival is rejected `POST_ONLY_CROSS` rather than filled. (Both language bindings default it to `true` at *construction*, which is a convenience for building a message in-process; the encoder always emits the key, and both C++ codecs reject a frame that lacks it.) |
| `md_seq` | uint64 | — | The market-data sequence this decision was made from. Used by the engine for the **M3** tick-to-order measurement (§9). |

### 3.2 `cancel_order`

`cl_id` identifies the order to cancel. An id never presented on this session gets
`UNKNOWN_CLORDID`; an id that IS known but whose order is already terminal gets
`ALREADY_TERMINAL` — see §4, which sets out why the two are different answers.

---

## 4. Reject codes

Rejects are **recoverable**: the command is refused, the session stays up. Contrast §6, where the
session is closed.

| code | raised when |
|---|---|
| `UNSUPPORTED_VERSION` | `v` is not 1 |
| `UNKNOWN_TYPE` | `t` is not a known client message |
| `MALFORMED` | the frame is not valid JSON, or a field has the wrong type |
| `MSG_TOO_LARGE` | the frame exceeds the 64 KiB cap (§8.3). Transport-layer policy — the codec never sees it |
| `UNKNOWN_SYMBOL` | `symbol` is not a configured instrument |
| `BAD_SIDE` | `side` is not `"B"` or `"S"` |
| `TICK_SIZE` | `px` is not a positive multiple of `tick_size` |
| `LOT_SIZE` | `qty` is not a positive multiple of `lot_size` |
| `QTY_LIMIT` | `qty` exceeds the per-order maximum |
| `PRICE_RANGE` | `px` is outside the accepted band |
| `DUP_CLORDID` | `cl_id` is already in use in this session |
| `POST_ONLY_CROSS` | a `post_only` order would have crossed on arrival |
| `MAX_LIVE_ORDERS` | accepting would exceed `max_live_orders` (**2**) |
| `UNKNOWN_CLORDID` | cancel names a `cl_id` that has **never been presented on this session** |
| `ALREADY_TERMINAL` | cancel names a `cl_id` that **is** known but whose order is terminal — filled, cancelled, or a rejected tombstone |
| `STALE_EPOCH` | the command's `epoch` is not the session's current epoch (§7) |

**`UNKNOWN_CLORDID` vs `ALREADY_TERMINAL` is a real distinction, not two names for one condition.** Every
`cl_id` presented to the engine is consumed for the **session's lifetime**, including one whose order
was rejected — a rejected `new_order` leaves a *tombstone*. So for any given `cl_id` a client sees
exactly one of:

| the `cl_id` was… | `new_order` again | `cancel_order` |
|---|---|---|
| never used | accepted | `UNKNOWN_CLORDID` |
| used and still live | `DUP_CLORDID` | `CancelAck` |
| used and now terminal (incl. rejected) | `DUP_CLORDID` | `ALREADY_TERMINAL` |

The consequence worth knowing: **terminal states are absorbing, so a cancel is not idempotent in its
reply.** The first cancel of a live order gets `CancelAck`; a retry of that same cancel gets
`ALREADY_TERMINAL`. A client that retries a cancel after a lost reply therefore cannot distinguish
"my cancel worked and I missed the ack" from "it was already gone" — which is exactly why order
reports are never dropped (§10): if they could be, that ambiguity would be unrecoverable.

---

## 5. Order lifecycle

### 5.1 Two state machines, deliberately different

The brief's state model names `PendingNew` and `PendingCancel`. Where those states *exist* differs
between the two sides, and that difference is a design consequence rather than an omission.

**Engine — commands are processed to completion in arrival order on one owner thread:**

```
                  new_order (valid)              fill (leaves == 0)
   (none) ──────────────────────────► Live ──────────────────────────► Filled
      │                                 │
      │ new_order (invalid)             │ cancel_order          fill (leaves > 0)
      ▼                                 ▼                        ↺ stays Live
   Rejected                         Cancelled
```

There is **no `PendingNew` or `PendingCancel` state in the engine.** A command is validated,
applied and acknowledged within a single synchronous step on the owner thread, so the pending
states have zero duration — nothing can observe them and no code can be in them. Materialising
them would be inventing a state to match a diagram.

**Client — every command has real in-flight time, because the network is in the middle:**

```
                send new_order              order_ack
   (none) ──────────────────────► PendingNew ─────────► Live
                                      │                   │
                                      │ reject            │ send cancel_order
                                      ▼                   ▼
                                  Rejected           PendingCancel
                                                          │
                                        cancel_ack        │        fill (leaves == 0)
                                  Cancelled ◄─────────────┴──────────────► Filled
```

**Mapping the brief's states to where they actually live:**

| brief state | engine | client | note |
|---|---|---|---|
| `New` / `PendingNew` | zero-duration | **real state** | The uncertainty is the round trip, which only the client experiences |
| `Live` | **real state** | **real state** | Both track it |
| `PendingCancel` | zero-duration | **real state** | Same reason as `PendingNew` |
| `Cancelled` | **real state** | **real state** | Terminal |
| `Filled` | **real state** | **real state** | Terminal at `leaves == 0` |
| `Rejected` | **real state** | **real state** | Terminal |

### 5.2 Fill model

Deterministic and fully specified — no randomness, no partial-fill lottery.

- A resting **bid** at price *P* fills when the published **`ask_px` ≤ *P***.
- A resting **ask** at price *P* fills when the published **`bid_px` ≥ *P***.
- **Quantity** is `min(order.leaves, opposite_top_qty)` — the resting remainder or the size
  available at the opposite touch, whichever is smaller. A fill may therefore be partial, and
  `leaves > 0` leaves the order live.
- **Price** is the order's **own limit price**, never the opposite touch. The order does not get
  price improvement.
- **Tie-break:** when a single book update crosses both a resting bid and a resting ask,
  **bids are evaluated before asks**.
- Fills are evaluated **once per `md_seq`** — one book publication produces at most one fill
  evaluation per resting order, so a single tick cannot cascade.

### 5.3 The book is exogenous

**The published top of book never reflects the strategy's own orders.** It is produced entirely by
the feed scenario. Two consequences worth stating plainly, because both are load-bearing for how
the results should be read:

1. Quoting at the touch is **not self-referential** — the client cannot move the market it is
   quoting into, so there is no feedback loop to reason about.
2. Every fill is **feed-driven**. A fill happens because the scenario moved the book across a
   resting order, not because another participant traded with it. The engine is a market
   *simulator*, not a matching engine with counterparties.

---

## 6. Version gate

`v` is checked before anything else. A message with `v != 1` is rejected `UNSUPPORTED_VERSION` and
the session stays up — a version mismatch is a client bug worth reporting, not a reason to drop a
market maker's connection.

The **subprotocol** is a separate gate at a lower layer: the client offers `mm.v1` in the WebSocket
handshake and a server that does not select it fails the handshake (§8.1). That failure is
deterministic and is **not** retried — dialling the same server again asks the same question and
gets the same answer.

---

## 7. Epochs, reconnect, and cancel-on-disconnect

The engine assigns an `epoch` when a session opens and includes it in every message.

**Cancel-on-disconnect is the outstanding-order policy.** When a session drops, the engine cancels
every order that session had resting. There is therefore **no state to reconcile on reconnect** — a
new session starts flat, by construction, which is why the client wipes its local picture in
`on_connect` rather than attempting recovery.

A command arriving with a *stale* epoch is a well-formed command from a previous life. It gets
`reject{STALE_EPOCH}` and the session survives; it is not a protocol violation.

**The client retries exactly once**, then exits non-zero. A retry loop would turn a genuinely dead
engine into a client that looks alive forever, and a supervisor that cannot tell "engine is down"
from "client is patient" is worse than one that exits.

---

### 7.1 Counter exhaustion

`seq`, `md_seq` and `epoch` are uint64. The C++ side increments without an overflow check and would
wrap; Python integers do not wrap, and the encoder refuses a value above the uint64 range — so the
two sides fail *differently* at exhaustion rather than agreeing.

No guard is placed on the increment, deliberately: it sits in the measured path, and exhausting a
uint64 needs on the order of 1.8 × 10^19 messages — at the 10 kHz this system sustains, about 58
million years on a single connection. The honest statement is that **exhaustion is undefined
behaviour in this protocol version** and that a production successor should define it (close and
reconnect before the boundary), not that it is handled.

## 8. WebSocket layer

### 8.1 Subprotocol negotiation

The client offers exactly `mm.v1`. The engine selects it. A server that selects nothing, or
something else, is not speaking this protocol and the handshake fails.

### 8.2 Close codes — normative

| code | meaning here | sent by |
|---|---|---|
| **1000** | Normal closure — a clean, intended shutdown | either |
| **1001** | `going_away` — the engine is shutting down; every open session is closed 1001. A session already closing keeps the code it began with | engine |
| **1002** | Protocol error — an envelope `seq` gap, a **binary frame** (this wire is text-only), or a reserved-bit violation | either |
| **1008** | Policy violation — the report queue breached its high-water mark. **A market maker vastly prefers a known disconnect to silently dropped order reports** | engine |
| **1009** | Message too big — a frame exceeded the cap after reassembly | either |
| **4000** | Application: market data went stale beyond the client's tolerance, so the client stopped quoting and disconnected | client |

### 8.3 Size limits and fragmentation

**There are TWO size limits, and they have different consequences.** Documenting only one made
the recoverable case look fatal:

| tier | bound | what happens |
|---|---|---|
| **policy cap** | **64 KiB** | The frame reassembles fully, then the engine refuses it: `MSG_TOO_LARGE`, **session survives**. This is the recoverable case. |
| **transport ceiling** | **128 KiB** (`read_message_max`) | Beast never completes reassembly at all: close **1009**, session ends. |

So a 65,537-byte message is answered with a reject and the connection continues; only past 128 KiB
does the transport tier fire. `read_message_max` is explicitly lowered from Boost.Beast's 16 MiB
default, which is a denial-of-service surface rather than a feature.
- **Continuation frames are reassembled against the same cap.** Enforcing a per-frame limit alone
  is not a limit at all — a peer can send unlimited small fragments and defeat it entirely.

### 8.4 Ping/pong and idle policy

The engine bounds how long an un-upgraded connection may sit before the handshake completes
(`--upgrade-timeout-ms`); an unauthenticated peer that never upgrades cannot hold a slot
indefinitely. On the client side, auto-pong is **disabled** in the tuned adapter and pings are
answered by hand, so that every inbound frame — control frames included — passes through the same
validation path rather than being answered beneath it.

### 8.5 Compression

**`permessage-deflate` is disabled in both arms**, and both arms negotiate no extensions. Two
reasons: it puts a variable-cost, data-dependent CPU step directly in the measured path, which
would make the latency distribution a function of message content; and the messages here are small
enough that framing dominates any compression win. It is also a fairness requirement for the §6
comparison — an optimization measured with compression on in one arm and off in the other is not
measuring the optimization.

### 8.5b Where the three implementations disagree at the raw framing edge

Three WebSocket implementations meet on this wire — Boost.Beast (engine), `websockets` (naive
arm), `picows` (tuned arm) — and at the **raw framing edge** they do not agree. None of these is
reachable through `mm.v1` traffic: every one needs a hand-built frame that neither shipped client
emits. They are recorded because the two Python arms are the §6 A/B, so a behavioural difference
between them is a measurement caveat and not merely a curiosity.

| edge case | Beast (engine) | `websockets` (naive) | `picows` (tuned) |
|---|---|---|---|
| invalid UTF-8 in a TEXT frame | close **1007** | close **1007** | reaches the decoder, becomes **1002** |
| encoded close code `0` | rejected | not in its allowed set | exposed as `NO_INFO`, read as an empty close |
| close code `1014` | reserved, rejected | accepted | accepted |
| CLOSE arriving mid-fragmentation | handled independently | `ProtocolError` | control frames dispatched before fragment state |
| non-canonical length (e.g. 116 bytes encoded with marker 126) | rejected **1002** | accepted | accepted |
| PING after we sent CLOSE | n/a | answered | send is dropped by the library |

**What this does and does not mean.** The engine is the strictest of the three, so nothing here
lets a malformed frame through to order state — the divergences are in *which close code* a
non-conforming peer gets, and in whether a client answers a control frame it is about to stop
talking on. It does mean that a conformance suite written against one arm would not transfer
unchanged to the other, and that a raw-framing fuzzer would find different edges depending on
which arm it pointed at. Closing them means patching or pinning third-party parsers, which is
recorded as a known limitation rather than attempted here.

### 8.6 Text frames, not binary

The wire is **text**, and a binary frame is a protocol error (close 1002). The choice is
deliberate: the protocol is JSON, text framing makes it inspectable in any standard tool, and the
measured cost of a binary POD encoding over this message set is ~0.3 µs per cycle — immaterial
against a ~59 µs round trip. See `docs/OPTIMIZATION.md` #3, where saying so *with numbers* is the
finding rather than an excuse.

This is enforced rather than assumed, and it caught a real defect: the naive client was sending
every command as a binary frame (Python's `websockets` selects the opcode from the argument type,
and both codecs return `bytes`). No unit test could see it, because the test double accepted binary
frames the real engine closes 1002 on.

---

## 9. Timestamps on the wire

`svc_ns` on every `order_ack` is the engine's **M2** — its own service time for that command, in
nanoseconds, measured after decode and after the state update. It is a *duration*, not a point in
time.

**No absolute timestamp ever rides the wire, and no interval subtracts a C++ stamp from a Python
one.** Every measured interval is single-process: M1 is `perf_counter_ns` differences inside the
client, M2 and M3 are `steady_clock` differences inside the engine. This is what the brief requires
when it says not to require absolute timestamps from different processes to share a clock origin.

`svc_ns` rides every ACK permanently rather than being toggled for measurement. Its wire cost is
upper-bounded from the codec microbenchmark at ≈20 B/msg.

---

## 10. Backpressure — stated verbatim

The policy differs by message class, and the asymmetry is the point.

**Market data — conflate, never block.** When the outbound queue cannot keep up, the engine
publishes only the newest book and drops the intermediate states. `md_seq` gaps accordingly (§1.1).
A stale book is worthless to a market maker; the newest one is what matters.

**Order reports — never drop.** They are the client's only account of its own state. When the
report queue breaches its high-water mark the session is **closed 1008** rather than allowed to
continue with reports missing: *a market maker vastly prefers a known disconnect to silently
dropped order reports, because dropping one leaves its order state undefined.*

**Telemetry — drop and count, never block.** Telemetry is diagnostic. It uses a bounded ring, and
when the ring is full it drops and increments a counter. Blocking the measured path to write a
diagnostic would corrupt the thing being diagnosed.

---

## 11. Normative examples

The golden fixtures under `tests/golden/` are the normative byte-level examples, shared by BOTH suites — CMake passes the directory as `MM_GOLDEN_DIR` and the Python side reads it through `golden_support`. One directory, two readers, which is what makes them a cross-language contract rather than two copies. Where this
document describes a field and a fixture shows it, the fixture is the tie-breaker for encoding
details — it is executed on every test run, and prose is not.
