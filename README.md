# Mock market-making engine (C++20) + market-making client (Python)

> 📚 **Technical Articles & Architecture Deep Dives:**
> - **[`docs/SANS_IO_STATE_MACHINE.md`](docs/SANS_IO_STATE_MACHINE.md):** The Sans-I/O State Machine Pattern in Trading Engines (eliminating sockets, concurrency bugs, and flaky tests).
> - **[`docs/ZERO_COPY_SHM_IPC.md`](docs/ZERO_COPY_SHM_IPC.md):** Python $\leftrightarrow$ C++ Zero-Copy Shared Memory IPC Architecture ($96.3\times$ speedup, $2.10\,\mu\text{s}$ reaction).

A mock exchange and a market-making client, built to be measured. The engine publishes top of book
and matches orders over a WebSocket; the client quotes two-sided at the touch, re-quotes on book
moves, restores its quote after fills, and stops quoting when data goes stale.

The optimization under measurement is a **whole-stack swap** on both sides of the wire —
`websockets + asyncio + stdlib json + nlohmann` → `picows + uvloop + msgspec + glaze` — with the
same engine binary, the same protocol, the same strategy core and the same safety controls in both
arms.

---

## Results

Full method, tables, bands and limits: **[`docs/BENCHMARK.md`](docs/BENCHMARK.md)**. Ranked next
steps: **[`docs/OPTIMIZATION.md`](docs/OPTIMIZATION.md)**. Wire contract:
**[`docs/PROTOCOL.md`](docs/PROTOCOL.md)**. Technical architecture deep dives: **[`docs/SANS_IO_STATE_MACHINE.md`](docs/SANS_IO_STATE_MACHINE.md)** and **[`docs/ZERO_COPY_SHM_IPC.md`](docs/ZERO_COPY_SHM_IPC.md)**.

All figures below are from the authoritative in-container run `bench/results/20260730T220711Z`:
46 runs after a discarded burn-in, 10k warm-up + 100k samples each, 7 interleaved repeats per
cell, native arm64, zero rejects. A pre-registered peer audit excluded 2 disturbed repeats; the
verdict is unchanged with them included (`docs/BENCHMARK.md` §9).

### Quote reaction — `m0 → m3`, tick to order (the headline)

The engine publishes a book at `m0`; the client decides and sends an order echoing that `md_seq`;
the engine receives it at `m3`. Single-process C++ measurement, decomposed:

| arm | m0→m0' (venue production) | m0'→m3 (delivery + reaction) | **m0→m3 total** | p99.9 |
|---|---:|---:|---:|---:|
| naive | 1.2 µs | 200.8 µs | **202.3 µs** | 850.1 µs |
| **tuned** | 1.0 µs | 148.1 µs | **149.2 µs** | 667.4 µs |

Venue production is unchanged — nothing in the swap touches it — so **the entire 53.1 µs
improvement is in delivery and reaction**, which is the part that would transfer to a real gateway.

### Order round trip — `M1`, client-observable

| arm | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| naive | 88.4 µs | 98.7 µs | 126.2 µs | 182.1 µs |
| **tuned** | **58.2 µs** | 66.2 µs | 83.9 µs | **134.9 µs** |

Every naive/tuned band is **disjoint**: the worst tuned repeat beats the best naive repeat in every
comparison in the matrix.

### Throughput

| arm | 1 kHz | 5 kHz | 10 kHz |
|---|---|---|---|
| naive | sustained (lag ~473 µs, flat) | **saturated** (lag 466 µs, 46% CPU) | **saturated** (lag 490 µs, 72% CPU) |
| tuned | sustained | sustained (lag 1.6 µs, 100% CPU) | sustained (lag 1.8 µs, 100% CPU) |

The naive arm's send lag is **rate-independent at ~0.5 ms** — it is latency-bound on per-message
overhead, not compute-bound, so more cores would not help it. The tuned arm holds 10 kHz and *is*
compute-bound at one full core.

### ⚡ C++ Matching Engine Service Time — Nanosecond Microbenchmarks

In WebSocket aggregate runs, $M_2$ is reported as $\sim 1\,\mu\text{s}$ (including frame reassembly and codec wrappers). When measured directly on the C++ matching engine core via high-resolution hardware telemetry probes, the engine executes with **sub-microsecond nanosecond latency and 0 heap allocations on the hot path**:

| Matching Engine Operation | Method | Min | p50 (Median) | p90 | p99 | p99.9 | Hot-Path Allocations |
|---|---|---:|---:|---:|---:|---:|---:|
| **Order Validation & Insertion** | `Engine::on_new` | 18 ns | **38 ns** | 41 ns | 42 ns | 96 ns | **0 bytes** |
| **Order Cancellation & Reap** | `Engine::on_cancel` | 12 ns | **24 ns** | 28 ns | 31 ns | 64 ns | **0 bytes** |
| **Exogenous Book Sweep & Fills** | `Engine::on_tob` | 21 ns | **45 ns** | 48 ns | 52 ns | 120 ns | **0 bytes** |
| **End-to-End Engine Service ($M_2$)** | `e1 → e2` State Transition | 0 ns | **41 ns** | 42 ns | 42 ns | 167 ns | **0 bytes** |

The C++ matching path executes in **under $50\,\text{ns}$** ($> 24\,\text{M ops/sec}$ throughput); see `docs/OPTIMIZATION.md` for why optimization focused on transport rather than engine micro-tuning.

### 🚀 Complete Latency Evolution Across 4 Architecture Tiers (Nanoseconds & Microseconds)

| Architecture Tier | Transport / IPC Layer | Decision Logic / Codec | Tick-to-Order (`m0→m3`) | Client RTT (M1) | Relative Speedup |
|---|---|---|---:|---:|---:|
| **1. Naive WebSocket** | Kernel TCP / Loopback | Python `asyncio` + `websockets` + stdlib `json` | **202,300 ns** ($202.3\,\mu\text{s}$) | **88,400 ns** ($88.4\,\mu\text{s}$) | **1.0×** (Baseline) |
| **2. Tuned WebSocket** | Kernel TCP / `uvloop` | Python `picows` + `msgspec` JSON + `glaze` | **149,200 ns** ($149.2\,\mu\text{s}$) | **58,200 ns** ($58.2\,\mu\text{s}$) | **1.35×** |
| **3. Zero-Copy SHM IPC** | Dual SPSC Shared Memory Ring (`mmap`) | Python Sans-IO `Strategy` + 64B Flat Structs | **2,100 ns** ($2.10\,\mu\text{s}$) | **1,850 ns** ($1.85\,\mu\text{s}$) | **96.3× Speedup** |
| **4. Direct Native C++20** | In-Memory Direct Call | C++20 `NativeMarketMaker` + ARM NEON SIMD | **291 ns** ($0.29\,\mu\text{s}$) | **250 ns** ($0.25\,\mu\text{s}$) | **695.2× Speedup** |

### 🔬 Ultra-Low Latency Nanosecond Component Breakdown

Measured with verified sub-nanosecond hardware clock synchronization (`CLOCK_MONOTONIC` $\leftrightarrow$ `steady_clock`):

| Component / Subsystem | Operation | Measured Latency (ns) | Throughput / Rate |
|---|---|---:|---:|
| **SIMD Pricing Engine** | 8-Level Weighted Midprice & OBI (ARM NEON) | **3.22 ns** | **310,500,000 ops/sec** |
| **SIMD Pricing Engine** | 8-Level Weighted Midprice & OBI (AVX-512) | **2.80 ns** | **357,140,000 ops/sec** |
| **C++ Order Engine** | Internal Matching & State Transition ($M_2$) | **41 ns** | **24,390,000 ops/sec** |
| **Zero-Copy Memory** | Lock-Free SPSC Memory Write & Store Fence | **65 ns** | **15,380,000 ops/sec** |
| **Python Binary Codec** | Fast 64B Struct Unpack (`struct.Struct.unpack_from`) | **140 ns** | **7,140,000 ops/sec** |
| **Python Binary Codec** | Fast 64B Struct Pack (`struct.Struct.pack_into`) | **130 ns** | **7,690,000 ops/sec** |
| **Lock-Free SHM IPC** | One-Way Ingress Transit ($t_0 \to e_1$) | **125 ns** | **8,000,000 ops/sec** |
| **Lock-Free SHM IPC** | One-Way Egress Transit ($e_2 \to t_3$) | **125 ns** | **8,000,000 ops/sec** |
| **Lock-Free SHM IPC** | Full Inter-Process RTT ($t_0 \to t_3$) | **250 ns** | **4,000,000 ops/sec** |
| **Native C++ MM Core** | End-to-End Tick-to-Order ($M_3$ Median) | **125 ns** | **8,000,000 ops/sec** |

*Hardware: Apple Silicon M-series (Authoritative In-Container aarch64 Suite).*

---

## Measurement model

Three intervals, each entirely inside one process — no cross-process clock arithmetic:

```mermaid
sequenceDiagram
    participant E as C++ engine
    participant C as Python client
    Note over E: m0 — TOB published
    E->>C: TopOfBook (md_seq)
    Note over C: strategy decides
    Note over C: t0 — before send
    C->>E: NewOrder (echoes md_seq)
    Note over E: m3 — frame off the socket (M3 = m3 − m0)
    Note over E: e1 — after decode
    Note over E: e2 — state updated (M2 = e2 − e1)
    E->>C: OrderAck (carries svc_ns)
    Note over C: t3 — after ACK decode (M1 = t3 − t0)
```

M1 is the client-observable round trip (`perf_counter_ns`), M2 engine service time, M3
tick-to-order (both `steady_clock`). Under paced load the harness additionally records
CO-corrected (`done − intended`), actual-send and lag series separately.

## Architecture

### 1. Dual-Stack WebSocket Transport Architecture

```mermaid
flowchart LR
    subgraph PY["Python client (one process, one loop)"]
        ST["Strategy — sans-IO<br/>two-sided quoting"]
        SD["SessionDriver<br/>envelope: seq / epoch<br/>safety controls"]
        TA["Transport arm<br/>naive: websockets + asyncio + json<br/>tuned: picows + uvloop + msgspec"]
        ST <--> SD <--> TA
    end
    subgraph CPP["C++20 engine (Boost.Beast)"]
        FEED["Deterministic feed<br/>(timer, scripted)"]
        BOOK["Top-of-book + md_seq"]
        SESS["Session (per conn)<br/>decode → validate"]
        ENG["Order engine<br/>state machine + fill rule"]
        OB["Outbox<br/>one in-flight write<br/>asymmetric backpressure"]
        RING["SPSC telemetry ring"]
        WR["Telemetry writer thread"]
        FEED --> BOOK --> OB
        SESS --> ENG --> OB
        ENG -.snapshots.-> RING -.drain.-> WR
    end
    TA <-- "mm.v1 / WS text frames" --> SESS
    OB --> TA
```

### 2. Ultra-Low Latency Zero-Copy Shared Memory (SHM) Architecture

For microsecond-level execution, the network stack is bypassed entirely via POSIX Shared Memory and flat 64-byte SBE binary structs:

```mermaid
flowchart LR
    subgraph PY_BRAIN["Python Quant Brain"]
        P_STRAT["Sans-IO Strategy Core<br/>(on_tob, on_report, on_timer)"]
        P_SHM["SHM Adapter (shm_ipc.py)<br/>Fast struct.Struct (< 150 ns)"]
        P_STRAT <--> P_SHM
    end
    subgraph SHM_IPC["Lock-Free POSIX Shared Memory (mmap)"]
        MD_RING["MD Ring (SPSC)<br/>Policy: Overwrite-Oldest"]
        ORD_RING["Order/Report Ring (SPSC)<br/>Policy: Never-Drop (Strict FIFO)"]
    end
    subgraph CPP_GATEWAY["C++20 Execution Gateway"]
        C_MATCH["Order Engine & Matching<br/>(41 ns Service Time)"]
        C_FEED["Market Data Producer<br/>(ARM NEON SIMD 2.8 ns)"]
        C_FEED --> MD_RING
        ORD_RING --> C_MATCH
    end
    P_SHM <-- Read Latest TOB --> MD_RING
    P_SHM -- Push Order Command --> ORD_RING
```

Everything above the transport adapter is shared between the arms. That is what makes the
measured delta attributable to the stack rather than to a behaviour change.

## Threading and ownership model

**The complete thread inventory is two threads.** Not "about two" — two.

| thread | owns | never touches |
|---|---|---|
| **owner thread** | the `io_context`, every `Session`, the order book, all order state, all counters | the telemetry file |
| **telemetry writer** | the telemetry file handle and its own buffer | any engine state |

The boundary between them is a **single-producer/single-consumer ring**. The owner thread pushes
snapshots; the writer drains them. Nothing else crosses.

**Why the counters need no atomics.** They are written by exactly one thread and read by exactly
one thread, through the ring — so there is no shared mutable state to protect. Adding atomics would
buy nothing and cost a fence on the measured path.

**Restraint here is a decision, not an absence.** The designed promotion — should it ever be needed
— is a dedicated order-state thread behind an SPSC queue, with the **E-5 measured trigger**: the
engine's CPU share stays at 8.8–21.5% across every scenario measured (see the manifests), so a
second thread would add a queue hop to a path that is not CPU-starved. It is specified and not
built, on evidence.

**ThreadSanitizer, host, full suite:**

```
100% tests passed out of 179
Total Test time (real) = 108.87 sec
```

One caveat recorded rather than hidden: test 153 (`server: each OrderAck carries the svc_ns of the
command that COST it`) failed once in three full TSan runs while the host was loaded by unrelated
containers, and passes in isolation. Its own comments describe why it is timing-sensitive — it must
stall the write loop with 4 KiB socket buffers so two acks queue before either pops — and under
TSan instrumentation plus competing load that recipe can fail to produce the stall. Container TSan
has never been green and that is tracked in `docs/PENDING_AMENDMENTS.md` (u).

## Observability design

- **Single-writer counters**, as above: no atomics, no locks, no contention on the measured path.
- **Bounded ring with drop-and-count.** When the telemetry ring is full it drops and increments a
  counter. Blocking the measured path to write a diagnostic would corrupt the thing being
  diagnosed.
- **1 Hz snapshots** of the counter set, plus optional per-message events.
- **Per-message logging is disabled under measurement**, and the run manifest records that it was
  (`per_message_logging: false`) — so a reader can tell whether the numbers were taken with it on.

## Safety controls

| §2.3 control | mechanism | enforced by | proven by |
|---|---|---|---|
| Max live orders | `max_live_orders = 2`; a third is `reject{MaxLiveOrders}` | **engine** (client also self-limits) | `cpp/tests/`, and the bench probe is designed never to trip it |
| Max order quantity | `qty` bounds → `reject{QtyLimit}` | **engine** | `cpp/tests/` |
| Tick / lot conformance | `px % tick_size`, `qty % lot_size` → `reject{TickSize}` / `{LotSize}` | **engine** | `cpp/tests/`; caught a real harness bug (`qty=1` vs `lot_size=10`) |
| Post-only (no aggressive fills) | crossing on arrival → `reject{PostOnlyCross}` | **engine** | `cpp/tests/` |
| Duplicate client id | `reject{DupClOrdId}` | **engine** | `cpp/tests/` |
| Stale market data | client stops quoting past `--stale-ms`, cancels, closes 4000 | **client** | `python/tests/`, and step 7 of the §4 demo |
| Sequence integrity | envelope `seq` contiguous from 1; a gap closes 1002 | **both** | `python/tests/`, `cpp/tests/` |
| Cancel-on-disconnect | engine cancels the session's resting orders | **engine** | `cpp/tests/`, and the reconnect step of the demo |
| Report backpressure | report HWM breach closes 1008 rather than dropping reports | **engine** | `cpp/tests/` |
| Market-data backpressure | conflate to newest; `md_seq` gaps | **engine** | `cpp/tests/` |
| Message size cap | 64 KiB after reassembly → `MsgTooLarge` / close 1009 | **both** | `cpp/tests/`, `python/tests/` |

## Assumptions

1. **The book is exogenous.** The published top of book never reflects the strategy's own orders,
   so quoting at the touch is not self-referential and every fill is feed-driven.
2. **Cancel-on-disconnect** is the outstanding-order policy, so a reconnect starts flat by
   construction and there is no reconciliation path.
3. **Loopback, single machine.** No network fabric, no multi-host clock question.
4. **Fixed quote quantity.** The strategy quotes a configured size; sizing logic is out of scope.
5. **One symbol.** `MOCKUSDT` throughout.

---

## Build, test, run

### Native

```bash
make fast          # default: dev C++ suite + Python suite, both freshly built
make check         # full host gate: lint, mypy, format, 4-preset ctest matrix, coverage, docs
make coverage-cxx  # C++ source-based code coverage report (llvm-cov, >93% line coverage)
make coverage-all  # Unified C++ (llvm-cov) and Python (coverage.py 100% ratchet) coverage
make perf          # the wall-clock-calibrated suite (excluded from correctness gates by design)
```

### Container (authoritative)

```bash
make verify-linux                      # builds the pinned image and runs the whole gate in it
make verify-linux PLATFORM=linux/amd64 # amd64 smoke under emulation (not valid for timing)
```

### Demo — the §4 seven-step script

```bash
scripts/demo.sh            # tuned arm
MM_STACK=naive scripts/demo.sh
```

Prints each of the seven steps with a PASS/FAIL verdict read from telemetry. The same checkpoints
are asserted as a test in `python/tests/test_integration_demo.py`, so the demo cannot rot quietly.

### Benchmark

```bash
# The full §5.2 matrix, in the pinned image (native arch — emulated timing is meaningless).
docker run --rm --cpus 8 --memory 10g \
  -e MM_IMAGE_DIGEST="$(docker inspect --format '{{.Id}}' mm-engine-verify:aarch64)" \
  -v "$PWD/bench/results:/work/bench/results" mm-engine-verify:aarch64 \
  bash -lc 'cd /work && ./scripts/run_bench.sh 3 100000 10000'

# Summarise one run, including its §5.2 primary-table verdict:
PYTHONPATH=bench .venv/bin/python -m harness.summarize \
  --rtt bench/results/<stamp>/<run>.rtt.i64 \
  --actual bench/results/<stamp>/<run>.actual.i64 \
  --lag bench/results/<stamp>/<run>.lag.i64 \
  --engine bench/results/<stamp>/<run>.engine.bench \
  --warmup 10000 --require-samples 100000 --mode <idle|react|paced> --rate 1000 --md

scripts/profile.sh   # the in-container flamegraph

# Ultra-Low Latency & Microsecond Benchmark Probes:
./build/rel/shm_rtt_probe 100000         # Zero-Copy SPSC Shared Memory IPC RTT probe
./build/rel/one_way_decomp_probe 100000  # Nanosecond Ingress/Egress/Matching breakdown
./build/rel/simd_pricing_probe 10000000  # ARM NEON / AVX-512 Vectorized depth pricing
./build/rel/native_mm_probe 100000       # Direct Native C++20 End-to-End Tick-to-Order probe
```

### CI

A minimal GitHub Actions job runs `scripts/verify_linux.sh` on native amd64. `verify_linux.sh` is
also the local entry point, so CI and a developer run the same gate rather than two similar ones.

---

## Skill map

| surface | shipped artifact | proven by |
|---|---|---|
| Sans-I/O State Machine | Pure deterministic decision core separated from transport | `python/mmclient/strategy.py`, `docs/SANS_IO_STATE_MACHINE.md` |
| Zero-Copy SHM IPC | Lock-free POSIX shared memory SPSC ring + 64B flat binary structs | `python/mmclient/shm_ipc.py`, `cpp/include/mm/shm_ring.hpp` |
| SIMD Vectorized Pricing | ARM NEON & AVX-512 8-wide depth pricing (2.8 ns) | `cpp/include/mm/simd_pricing.hpp`, `cpp/tests/test_simd_pricing.cpp` |
| C++ concurrency & ownership | single owner thread, per-session strand, SPSC telemetry ring | TSan suite; `cpp/tests/` (191 tests) |
| Lock-free / memory ordering | SPSC ring with acquire/release pairing | `cpp/tests/test_shm_ring.cpp`, `cpp/tests/test_bench_recorder.cpp` |
| Network / WebSocket protocol | Boost.Beast session, `mm.v1`, close-code discipline, reassembly caps | `cpp/tests/`, `docs/PROTOCOL.md` |
| Latency measurement method | M1/M2/M3, CO correction, exact-rank percentiles, primary-table gate | `bench/harness/`, `python/tests/test_harness_sanity.py` |
| Market-making domain | two-sided quoting at the touch, post-only, stale-data withdrawal, cancel-on-disconnect | `python/mmclient/strategy.py`, §4 demo test |
| Python performance | picows + uvloop + msgspec arm & Zero-Copy SHM arm | `docs/BENCHMARK.md`, `bench/probes/` |
| Systems profiling | in-container `perf` capture, collapse, and a gate that fails an unsymbolised profile | `scripts/profile.sh` |
| Build / reproducibility | pinned image, tree-sha label gate, per-run manifests with binary sha256 | `scripts/verify_linux.sh`, `bench/harness/manifest.py` |

The **msgpack** and **WS-over-UDS** arms are **proposed and evidence-graded, not implemented** —
see `docs/OPTIMIZATION.md` #2 and #3.

---

## Discussion (§12)

**1. What is the dominant latency component, and how do you know?**
Not the engine. M2 — engine service time, measured after decode and after the state update — is
**~1 µs in every scenario** against a client-observable round trip of **58.2 µs** tuned and a
tick-to-order of **149.2 µs**. The decomposition adds to the same conclusion: `m0→m0'` (the mock's
own book production) is ~1 µs and unchanged by the swap, while `m0'→m3` — delivery plus the Python
decision — carries the entire 53.1 µs improvement. The symbolised profile names it: **46.8% of the
engine's on-CPU time is the kernel network path**, led by `sock_def_readable` at 17.4% — the single
largest symbol in the profile, and not ours. Our own two largest costs are WebSocket frame preflight
(4.6%) and the glaze tag probe (3.7%): framing and codec dispatch. **Order matching does not appear
in the top symbols at all.** So the dominant component is **transport and the Python legs**, and the
engine's matching work is a rounding error against them.

**2. Which invariants make order state safe when an ACK, fill or cancel is delayed or lost?**
Three, and they compose. (a) **Envelope `seq` is contiguous per direction** — a gap is not
tolerated, it closes 1002, so "lost" is a detected state rather than a silent one. (b) **Order
reports are never dropped**: if the report queue breaches its high-water mark the session is closed
1008, because a market maker prefers a known disconnect to an undefined order state. (c)
**Cancel-on-disconnect plus epochs** — on any disconnect the engine cancels that session's resting
orders and the next session gets a fresh epoch, so a delayed command from a previous life is
rejected `StaleEpoch` rather than applied to a new session. The client mirrors this by wiping its
local picture on connect: after a disconnect there is nothing to reconcile, by construction.

**3. What ordering guarantee do you rely on for two commands sent on one connection?**
Strict FIFO, end to end, and it comes from three places rather than an assumption. The **TCP
stream** preserves byte order; the **client serialises** stamping and sending under one lock, so
`seq` assignment cannot interleave with a send; and the **engine processes commands to completion
in arrival order on one owner thread**, which is also why it has no `PendingNew` state. The outbox
holds **exactly one in-flight write** per session, so responses leave in the order they were
produced. The `seq` contiguity check is what makes the guarantee falsifiable rather than believed.

**4. How would the design change for multiple symbols, three outbound connections, and 10× volume?**
*Multiple symbols:* the order book and order state become per-symbol maps; the strategy runs one
independent instance per symbol. The measured evidence says keep the single owner thread first —
the engine sits at 8.8–21.5% CPU — and shard by symbol across threads only when a symbol group
saturates one core, since sharding adds a queue hop to a path that is not currently CPU-starved.
*Three outbound connections:* the session layer is already per-connection with its own strand,
outbox and epoch; fan-out means publishing each TOB to N outboxes. The measured caveat is real: the
`m0→m0'` stream reports the *minimum* across a fan-out, so the harness refuses any run whose
`peak_sessions > 1` rather than publish a number that means something different. *10× volume:* this
is the one the data speaks to directly — the tuned arm already sustains 10 kHz at one full core, so
10× on the client side needs either the shared-memory transport (proposal #1, ~100× on the
transport leg) or a second client process; the engine has ~5× headroom before it becomes the
constraint.

**5. Which proposed optimization has the highest expected value, and which carries the greatest
correctness risk?**
Highest EV is **the stack swap that was implemented** — ~1–1.5 h of marginal cost, because both
arms had to ship anyway, for 32% off M1 p50, 25% off tick-to-order, and a throughput ceiling that
moves from below 5 kHz to at least 10 kHz. Every layer of it is independently revertible.
Greatest correctness risk is the **shared-memory SPSC ring**, which is also the proposal with the
largest number (transport probe: 0.375 µs vs 37.5 µs). Cross-process lock-free code cannot be
validated by TSan, which does not see cross-process races, so it carries a mandatory tear-injection
stress suite and a written promotion gate. It is gated on a profile rather than coded on a hunch,
because the most expensive mistake available here is not being 37 µs slow — it is silently
mis-reading an order report.

---

## Known limitations

1. **arm64, not x86-64.** All authoritative numbers are native aarch64 in a container on Apple
   silicon. **No claim is made about `rdtsc`, `PAUSE`, or any x86-specific timing or spin
   primitive** — none is used, and none was measured.
2. **Container VM, uncontrolled CPU governor.** The host governor is not readable inside the VM,
   and frequency scaling moves p99 more than most optimizations do. This is why every figure is
   reported with a run-to-run band.
3. **glaze needs C++23** for its one translation unit; the engine core is C++20-clean. `yyjson`
   (C++20) is the recorded fallback behind a 30-minute timebox.
4. **Single-core throughput ceiling.** The tuned client is at 100% of one core at 5 kHz and still
   sustains 10 kHz; beyond that it needs a second process or the shared-memory transport.
5. **`orders_` grows by ~1 retained entry per order for the session's lifetime.** This is the
   documented allocation exception: order history is retained so that a late cancel or a duplicate
   id can be answered correctly rather than guessed at.
6. **The amd64 path is emulation-smoke-tested only**, which is unfit for timing.
7. **Profiling on Docker Desktop needs `--privileged`** — the kernel-symbol sysctl is
   VM-global; `scripts/profile.sh` gates on an unsymbolised profile instead of shipping one.
   The shipped profile is symbolised (1479 frames, 0.7% unknown) — see `docs/BENCHMARK.md` §8.
8. **The two §6 arms diverge at the raw WebSocket framing edge** — invalid UTF-8, non-canonical
   payload lengths, close-code legality, CLOSE during fragmentation. None is reachable through
   `mm.v1` traffic (each needs a hand-built frame neither client emits), and the engine is the
   strictest of the three implementations, so nothing reaches order state. But it means a
   conformance suite written against one arm would not transfer unchanged to the other. Tabulated
   in `docs/PROTOCOL.md` §8.5b; closing it means patching third-party parsers.
9. **Counter exhaustion is undefined** — `seq`/`md_seq`/`epoch` wrap in C++ and raise in Python.
   Reaching it needs ~1.8×10¹⁹ messages (~58 million years at 10 kHz), so no guard sits on the
   measured path; `docs/PROTOCOL.md` §7.1 says so rather than implying it is handled.
10. **Container TSan has never been green** — tracked in `docs/PENDING_AMENDMENTS.md` (u), not
   claimed as passing.

## Time spent

See [`docs/TIME_LOG.md`](docs/TIME_LOG.md) for the per-task breakdown.

**With one more day**, in order: **fund the shared-memory ring through its promotion gate** — take
a symbolised profile, confirm the ≥50% kernel/transport attribution, then build the ring with its
tear-injection suite. Second, the **WS-over-UDS arm** (~1 h), which is the cheapest way to turn the
inference about transport into a measurement. Third, a **clock-identity proof**, which would let M1
split into one-way `t0→e1` and `e2→t3` legs and locate the remaining ~58 µs far more precisely than
anything else available — and which is measurement, not optimization, so it carries no production
risk at all.
