# bench/probes — design-time probes and their provenance

Design-time probes, re-runnable, **never deliverable numbers**: the deliverable latency
matrix is `bench/results/`, produced by `scripts/run_bench.sh` inside the pinned
`ubuntu:26.04` container. A probe answers one narrow question on the dev host and prints
its own provenance line (basis, interpreter, library versions, platform, repo SHA), so
every absolute figure quoted in a source comment or a document can be re-taken rather than
believed.

| Probe | Question it answers | Cited by |
|---|---|---|
| `py_codec.py` | Python codec cost on the canonical frame, the preflight/library split, the worst case the resource caps permit, and the `_TOKEN` accept-set differential | `python/mmclient/protocol.py`, `python/mmclient/_preflight.py`, `the limitations backlog` (p)1b/(p)7/(p)12 |
| `tsan_struct_copy_probe.cpp` | Whether a toolchain's TSan race-checks struct-copy (memcpy-shaped) accesses — i.e. WHERE the telemetry ring's memory-ordering contract is enforceable by TSan (measured: host Apple runtime blind to the shape; container g++/libtsan reports it) | `cpp/include/mm/telemetry_ring.hpp`, `cpp/tests/test_telemetry.cpp`, `the limitations backlog` (s)7 |
| `ring_push_probe.cpp` | What the telemetry ring's design refusals cost or save: L1-hot push cost, the declined cached-head amortization under saturation, the notify floor vs the real parked-waiter wake, the coherence-granule calibration behind alignas(64), and the capacity/poll drop-onset threshold | `cpp/include/mm/telemetry_ring.hpp`, `the limitations backlog` (s)6 |

Run: `.venv/bin/python bench/probes/py_codec.py` (add `--fuzz` for the ~40 s accept-set
differential, which must print `0 differences`).

Run: `c++ -std=c++20 -O1 -g -fsanitize=thread bench/probes/tsan_struct_copy_probe.cpp -o
/tmp/tsan_probe && /tmp/tsan_probe relaxed-scalar` (then `relaxed-copy`, `release-copy`;
the file's header states which mode must report on which toolchain).

Run: `c++ -std=c++20 -O3 -Icpp/include bench/probes/ring_push_probe.cpp -o
/tmp/ring_push_probe && /tmp/ring_push_probe` (all modes; or name one — `push`,
`push-cached`, `notify`, `granule`, `saturation`; measured figures for both toolchains
in the file's header).

Step 1b re-homes the remaining design-time probes (ring, UDS, ctypes microbenches)
here with the same discipline.
