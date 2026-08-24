# OPTIMIZATION — the implemented change, measured, and what to do next

Every number here comes from the §5.2 matrix in `bench/results/20260730T220711Z` and is reproduced in
`docs/BENCHMARK.md` with its full tables, bands and methodology. Nothing in this document is an
estimate unless it is labelled one.

---

## 1. The baseline

The **naive arm**: `websockets` + `asyncio` + stdlib `json` on the client, `nlohmann` on the
engine. It is not a strawman — it is a correct, complete implementation of the same protocol
running the same strategy core, the same envelope stamping, the same sequencing and the same
§2.3 safety controls. The only things that differ from the tuned arm are the four stack
components in §2.

Measured (medians of 7 audit-clean interleaved repeats, CO-corrected, 10k warm-up + 100k samples
each — protocol and exclusions in BENCHMARK.md §2/§9):

| interval | naive |
|---|---:|
| M1 order round trip, p50 | 88.4 µs |
| M1 order round trip, p99.9 | 182.1 µs |
| M3 tick-to-order, p50 | 202.3 µs |
| M3 tick-to-order, p99.9 | 850.1 µs |
| M2 engine service, p50 | 1.0 µs |
| sustainable paced rate | **between 1 kHz and 5 kHz** (sustains 1 kHz; saturated at 5 kHz) |

## 2. The implemented change — one variable per layer

| layer | before | after | what it changes |
|---|---|---|---|
| WebSocket | `websockets` | `picows` | Cython parser with a callback surface: no per-frame coroutine, no queue hop between parser and handler |
| event loop | `asyncio` | `uvloop` | libuv-backed loop; cheaper timer and I/O dispatch |
| codec (Python) | stdlib `json` | `msgspec` | schema-typed decode straight into structs |
| codec (C++) | `nlohmann` | `glaze` | same engine binary, selected by `--codec` |

**This is an aggregate stack comparison, and the report says so.** Four things move at once, so the
end-to-end delta cannot be attributed to any single layer from these runs. The per-layer evidence
below comes from component microbenchmarks taken during design, and is labelled as such.

### Measured before → after

| interval | naive | tuned | improvement |
|---|---:|---:|---|
| M1 round trip p50 | 88.4 | **58.2** | −30.2 µs (−34%) |
| M1 round trip p99.9 | 182.1 | **134.9** | −47.2 µs (−26%) |
| M3 tick-to-order p50 | 202.3 | **149.2** | −53.1 µs (−26%) |
| M3 tick-to-order p99.9 | 850.1 | **667.4** | −182.7 µs (−21%) |
| paced 1 kHz actual-send p50 | 160.2 | **38.1** | −122.1 µs (−76%) |

All naive/tuned bands are **disjoint** — the worst tuned repeat beats the best naive repeat in
every comparison.

Two results matter more than the headline:

1. **The tick-to-order improvement is entirely in delivery and reaction.** Decomposed, `m0→m0'`
   (the mock’s own market-data production) is 1.2 µs
   naive against 1.0 µs tuned — unchanged, as it
   should be, since nothing in the swap touches it. The whole 53.1 µs sits in `m0'→m3`.
2. **The naive arm's throughput ceiling moved.** It saturates at 5 kHz while only ~45% CPU-busy;
   the tuned arm sustains 10 kHz with ~1.6 µs of median schedule lag. That is a change in kind,
   not degree: the naive stack is latency-bound on per-message overhead, so it could not be fixed
   by giving it more cores.

### What did NOT change, and why that is the important finding

**M2 — the engine's own order handling — is ~1 µs in every scenario**, against an end-to-end of
40–170 µs. The C++ side was never the bottleneck. The symbolised profile
(`docs/BENCHMARK.md` §8) says the same thing by name: **46.8%** of the engine's on-CPU time is the
kernel network path, led by `sock_def_readable` at 17.4%, while the largest first-party costs are
WebSocket frame preflight (4.6%) and the glaze tag probe (3.7%). **Order matching does not appear
in the top symbols at all.**

This is why the ranked list below leads with transport rather than with engine micro-optimization:
there is roughly 1 µs of engine work available to win, and ~59 µs of everything else.

---

## 3. Ranked next steps

Each carries expected benefit / implementation cost / correctness-and-operational risk. Benefits
are labelled **[measured]** where a probe exists and **[estimate]** where one does not.

### #1 — Shared-memory SPSC seqlock ring (proposal, deliberately not implemented)

**Benefit [measured — transport probe, not end-to-end]:** container transport RTT p50 **0.375 µs**
against **37.5 µs** for TCP loopback (~100×), 0 torn payloads in 450k echoes; independently
reproduced at p50 250 ns with 0 tears in 200k. Against an M1 p50 of 58.2 µs of which the
engine accounts for ~1 µs, transport is the only term large enough to matter.

**Cost:** ~3.5 h ring + ~3 h POD payload + a forced-overrun stress suite.

**Risk: the highest of anything here, which is why it is a proposal and not code.** Cross-process
lock-free code cannot be validated by TSan (it does not see cross-process races), so the design
carries a mandatory writer-stress tear-injection suite. A silent torn read in an order path is
strictly worse than being 37 µs slower.

**Design, as specified:** two rings encoding the asymmetric policy — market data overwrite-oldest
with seq-gap overrun detection, order reports never-drop with a full ring treated as fatal
backpressure. 60 B POD slots read in place via cached `struct.Struct.unpack_from` — explicitly
**not** ctypes, which measured 447 ns against msgspec's 250–340 ns. Bounded spin-then-park with a
portable UDS doorbell, `eventfd`/futex behind a Linux `#ifdef`, because C++20's `atomic::wait`/
`notify` provably does not cross processes.

**Promotion gate (binding, verbatim):** the ring is implemented only if ALL of — (a) the composed
tuned baseline lands by ~hour 8; (b) its task-clock flamegraph attributes **≥50% of m0→m3 wall
time to kernel/transport**; (c) tests and docs are on schedule at the gate.

**Status against that gate today: (b) is NOT met, so the ring stays a proposal.** (a) and (c)
hold. The now-symbolised profile (`docs/BENCHMARK.md` §8) attributes **46.8%** of the engine's
on-CPU time to the kernel network path — below the ≥50% the gate requires. Adding libc's 17.4%
would clear it, but much of that is unresolved offsets not attributed to transport, so counting it
would assume the conclusion. And the capture is the **idle** arm while the gate names **m0→m3**,
which is the react path — so the gate has not even been evaluated on its own terms yet.

This is the gate doing its job rather than a disappointment. It exists to stop the highest-risk
proposal in the project from being built on a hunch, and the number came in under the line. **The
next step for the ring is a react-mode profile**, not an implementation.

### #2 — WebSocket over a Unix domain socket

**Benefit [measured]:** −7.8 µs transport delta in-container. Roughly 13% of the tuned M1 p50 (58.2 µs).

**Cost:** ~1 h. `websockets.unix_connect` exists; **picows has no UDS connector**, so this arm runs
on `websockets` by design — which means it is a decomposition experiment rather than a drop-in
product change.

**Risk: low.** Same protocol, same framing, no shared-memory hazards. It also has diagnostic value
beyond its own delta: it separates the kernel TCP path from the rest, and would sharpen the ring's
promotion case considerably.

### #3 — Binary/POD wire format over WebSocket

**Benefit [measured microbench]: ~0.3 µs per cycle. Effectively immaterial**, and *saying so with
numbers is the finding.* On this message set — all-integer fields and short strings — a POD binary
encoding does not repay its cost at the JSON tuned baseline; the schema-typed `msgspec` decode has
already taken most of what was available.

**Cost:** ~1 h for an A/B against the existing golden-bytes tests.

**Risk: low**, but the honest recommendation is **do not adopt it for its own sake.** Its real
payoff is as the ring's slot payload (proposal #1), where the 60 B POD struct with SBE-style header
discipline is already specified.

### #4 — uWebSockets server swap

**Benefit [estimate]:** single-digit-µs parity at best. Never measured, because the arm was cut.

**Cost:** ~6 h of bespoke FetchContent glue — no brew or apt package and no upstream CMake — for
the worst information-per-hour ratio of any option considered.

**Risk: a specific, verified integrity hazard.** uWS defaults to
`closeOnBackpressureLimit=false`, and sends above `maxBackpressure` are silently reported as
`DROPPED` (`WebSocket.h:119`). In an order-report path that is a ready-made correctness bug. **Any
future adoption requires `closeOnBackpressureLimit=true` or explicit `getBufferedAmount()`
gating** — this condition is not optional.

### #5 — Free-threaded CPython (3.14t) re-run

**Benefit [measured]: expected null.** 3.14t was run in the authoritative container and RTT was
statistically identical across all scenarios. It is retained only as a 1 h conditional arm whose
one decisive outcome would be a >10% p50 win, which would reopen the runtime decision.

**Cost:** ~1 h. **Risk: none** — it is a measurement, not a change.

---

## 4. The summary question: highest expected value, and greatest risk

**Highest EV per hour: the stack swap — already implemented.** ~1–1.5 h of marginal cost, because
both stacks had to ship anyway (the naive arm is the "before" half of the comparison and the
tuned arm is the product), for 34% off M1 p50, 26% off tick-to-order p50, and a
throughput ceiling that moves from below 5 kHz to at least 10 kHz. Every layer it touches is
independently revertible: picows → websockets is a 0.5 h swap through the sans-IO adapter, glaze →
yyjson is a 30-minute timeboxed fallback, uvloop is one line.

**Greatest correctness risk: the shared-memory ring** — which is also the proposal with the largest
absolute number. That is precisely why it is gated on a profile rather than coded on a hunch, and
why its design carries a tear-injection stress suite rather than a promise. The most expensive
mistake available in this codebase is not being 37 µs slow; it is silently mis-reading an order
report.

## 5. What would change these conclusions

- **A symbolised profile.** The ranked list rests on "the engine is ~1 µs and the rest is kernel
  and Python". M2 establishes the first half directly. The second half is currently inferred from
  a kernel/user split without symbols; a symbolised profile could redistribute effort sharply — for
  example onto the Python legs if the kernel share turns out smaller than the `[k]` count suggests.
- **The UDS arm (#2).** At ~1 h it is the cheapest way to convert an inference about transport into
  a measurement, and it directly informs whether #1 is worth its risk.
- **A clock-identity proof.** If C++ `steady_clock` and Python `perf_counter_ns` could be shown to
  share an origin, M1 could be decomposed into one-way `t0→e1` and `e2→t3` legs. That would locate
  the remaining ~58 µs far more precisely than anything else on this list, and it is measurement
  rather than optimization — no production risk at all.
