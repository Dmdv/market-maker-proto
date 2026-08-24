# Historical Performance & Regression Ledger

This document records benchmark latency trends (M1, M2, M3) and test suite metrics across Git commits and architectural iterations.

## 1. Summary of Benchmarked Milestones

| Date (UTC) | Commit | Branch | Description | C++ Tests | Py Tests | Py Cover | M1 RTT p50 (Tuned) | M2 Svc p50 (Tuned) | M3 React p50 (Tuned) |
|---|---|---|---|---|---|---|---|---|---|
| 2026-08-24 | `1afc6c3` | `main` | Baseline: post-audit master state | 179 | 580 | 100.0% | 34.4µs | 625ns | 130.8µs |
| 2026-08-24 | `1afc6c3` | `main` | Verification of automated perf tracker agent | 179 | 580 | 100.0% | 36.7µs | 667ns | 156.4µs |
| 2026-08-24 | `4bacb61` | `main` | perf: single-pass fast-path tag probe in C++ Glaze codec | 179 | 580 | 100.0% | 34.5µs | 666ns | 132.0µs |
| 2026-08-24 | `754e320` | `feat/cxx-code-coverage` | perf: combined C++ glaze fast-path + Python preflight scanner zero-slice fast-path | 179 | 580 | 100.0% | 33.8µs | 666ns | 133.3µs |
| 2026-08-24 | `b2428ca` | `main` | perf: enable TCP_NODELAY on client-side WebSocket connections in ws_picows and ws_naive | 179 | 580 | 100.0% | 34.4µs | 625ns | 157.7µs |
| 2026-08-24 | `b2428ca` | `feat/shm-spsc-ring-ipc` | milestone: pre-SHM baseline with client TCP_NODELAY and optimized codecs | 179 | 581 | 100.0% | 34.2µs | 625ns | 136.0µs |

## 2. Regression Protection Gate Guidelines
- **M1 (ACK RTT p50):** Alert if $p50 > +5\%$ over baseline without justification.
- **M2 (Engine Service Time p50):** Hard limit $< 1.5\,\mu\text{s}$.
- **Python Coverage:** Hard ratchet $\ge 100\%$ branch coverage enforced by CI.
- **C++ Test Pass Rate:** 100% required across all 4 build presets.

