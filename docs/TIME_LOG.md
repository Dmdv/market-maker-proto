# Time log

Actual hands-on time per task (assignment deliverable 7). Review ran in an automated
multi-agent pipeline — independent code reviewers, adversarial verification, mutation
testing — largely unattended and often with several reviewers in parallel; machine review
wall-clock is not a human-effort figure and is deliberately not reported as one.

| Task | Description | Coding (h) |
|---|---|---|
| 0 | Repo scaffold, build skeleton, container | 0.2 |
| 1 | Domain types and validation | 0.1 |
| 2 | Protocol structs + dual codec + golden fixtures | 0.15 |
| 3 | Order engine core — state machine + fill rule, session-scoped | 0.1 |
| 4 | Python protocol module + cross-language golden test | 0.4 |
| 5 | Deterministic scripted feed | 0.35 |
| 6 | Outbox with asymmetric policy | 0.25 |
| 6.5 | Telemetry — in-process SPSC ring + RAII writer thread + counters | 0.85 |
| 7 | Beast server — WS discipline, sessions, sequencing, backpressure, lifecycle | 1.1 |
| 8 | Python sans-IO strategy core | 0.3 |
| 9 | Transport adapters (naive websockets + tuned picows) | 0.4 |
| 10 | Seven-step demo as an integration test | 0.5 |
| 11 | Benchmark harness + Makefile pipeline | 1.9 |
| 12 | Sanitizers, container, CI, floors | 0.6 |
| 13 | E-3 — deliverable benchmark matrix + BENCHMARK.md + symbolised profile | 4.6 ‡ |
| 14 | Optimization report + ranked proposals | ‡ |
| 15 | README, PROTOCOL.md, packaging, demo wiring | ‡ |
| Z | Contract validation — codex 22 + grok 5 findings, all closed with tests | 1.2 |
| 13b | E-2 stability — contamination controls, peer audit, platform audit | — |

‡ **One sitting, three rows.** Tasks 13–15 were executed as one interleaved 4.6 h working
session; a per-row split would be invented precision. The total is entered once, on row 13,
and counted once in the totals.
## Totals

| | Hours |
|---|---:|
| Development — code, tests, harness, scripts, packaging | **≈9.6** |
| Document authoring — BENCHMARK, OPTIMIZATION, README, PROTOCOL | ≈3.4 |
| Total hands-on | ≈13.0 |

**Development ≈9.6 h is the figure comparable to the assignment's "suggested effort: 10–12
focused hours"** — document authoring is reported separately, not folded into that comparison.
The split of the combined 13–15 session between execution/scripts and document authoring is
estimated from the plan's own per-task pricing, because the interleaved work does not separate
cleanly; every other figure is measured.
