# mm_engine / mmclient verification pipeline.
#
# THE RULE THIS FILE EXISTS TO ENFORCE: any target that reports a test result
# built the thing it tested, in the same invocation, and fails loudly when
# that build fails. `ctest` runs whatever binary is present — running it
# without building once produced a 3-of-4-presets-stale "ALL GREEN", caught
# only because the edit had renamed a test. Every test-* target below reaches
# ctest exclusively THROUGH its build-* edge; there is no other path in here.
#
# Written for GNU make 3.81, the macOS default. Anything newer is avoided:
# no .SHELLFLAGS (3.81 ignores it SILENTLY — verified on this host), no
# .ONESHELL, no grouped targets, no ::=.
#
# Facts this file relies on (verified 2026-07-30):
#   * Presets: configure/build dev rel asan tsan; test presets dev rel asan
#     tsan (perf label excluded via correctness-base) + perf (rel binary).
#   * ninja's regen rule tracks CMakeLists.txt and every included *.cmake, so
#     `cmake --build` re-configures on those by itself — but it does NOT
#     track CMakePresets.json (zero mentions in build/dev/build.ninja). That
#     edge is owned by the cache rules below.
#   * ruff and mypy take their config from the directory they run in; the
#     config is python/pyproject.toml (per-file-ignores are relative paths,
#     mypy_path=../bench), so both ALWAYS run from python/. The checked scope
#     is $(PY_SCOPE) — python/ + bench/probes + bench/harness. Defined once
#     near the tool paths; PENDING_AMENDMENTS (p)8 landed it.
#   * python/tests' integration suite execs build/rel/mm_engine (or
#     $MM_ENGINE), so pytest has a C++ build edge: build-rel.
#   * The compile DB is 107/141 third-party _deps entries — clang-tidy gets
#     the plan's explicit first-party scope (Task 12 Step 1b), never the raw
#     DB.
#   * Recipes assume cwd == repo root (paths are $(CURDIR)-relative): invoke
#     as plain `make` from the root, or `make -C <repo>` from elsewhere.

# ----------------------------------------------------------------------------
# Failure semantics. make 3.81 ignores .SHELLFLAGS, so the flags ride on
# SHELL itself (GNU make word-splits it; identical behaviour on 4.x).
# Verified on this host: with these flags a recipe line `false | true` fails
# the build; with .SHELLFLAGS instead it "passes". -e fails a line at its
# first failing command; -u makes unset shell expansions fatal; pipefail
# stops `cmd | tail` reporting tail's status — the exact trap that has bitten
# this repo twice. Belt and braces: no recipe below pipes a command whose
# status matters (the only pipeline in this file is `help`).
SHELL := /bin/bash -eu -o pipefail

# A target whose recipe died must not survive to look up to date. This is
# what forces a clean re-configure after an aborted `cmake --preset`.
.DELETE_ON_ERROR:

.DEFAULT_GOAL := fast

# ----------------------------------------------------------------------------
# Tools and layout.
# VENV IS DISCOVERED, not assumed. The dev host keeps its venv at `.venv`; the authoritative
# ubuntu:26.04 image builds one at `/opt/venv` (docker/Dockerfile). Measured with `VENV ?= .venv`:
# inside the container `make pytest` and `make lint` both died `FATAL: .venv/bin/python not
# found` while `make test-rel` passed 179 tests — so the C++ half of this pipeline was
# Linux-ready and the Python half was not.
#
# `ifndef` rather than `?=`, deliberately: `?=` is defeated by an EMPTY environment override, and
# `VENV= make lint` then expands to `/bin/python` with no prefix at all. Measured on Make 3.81 —
# `?=` yields `[]`, `ifndef` yields the fallback. `firstword $(wildcard …)` is valid on 3.81
# (the macOS default) and on 4.4.1 (the image), which are the two makes this file must satisfy.
#
# `.venv` is preferred when BOTH exist: a local tree beats an image layout, so a developer who
# has both gets the one they installed.
# Bounds the ASan build's parallelism. An unbounded ASan build twice preceded a wedged Docker
# daemon on this host; the MECHANISM WAS NEVER ESTABLISHED (no OOM lines in the window, zero
# jetsam events, and the change that fixed it moved three variables at once), so this is a
# precaution against a correlated failure, not a fix for a diagnosed one. Retracted from the
# earlier "exhausts Docker Desktop's VM" wording, which asserted a cause nobody measured.
ASAN_BUILD_JOBS ?= 4

ifndef VENV
VENV := $(firstword $(wildcard $(CURDIR)/.venv /opt/venv) $(CURDIR)/.venv)
endif
PY   := $(VENV)/bin/python
# Absolute forms for recipes that cd into python/ first. The `filter /%` guard matters because
# discovery yields an ABSOLUTE path: prefixing $(CURDIR) unconditionally would produce
# `/work//opt/venv/bin/python`. A relative override still gets the prefix it needs.
VENV_ABS := $(if $(filter /%,$(VENV)),$(VENV),$(CURDIR)/$(VENV))
PY_ABS   := $(VENV_ABS)/bin/python
RUFF_ABS := $(VENV_ABS)/bin/ruff
MYPY_ABS := $(VENV_ABS)/bin/mypy

# THE PYTHON CHECK SCOPE, in one place. It was spelled out in four recipes (lint x2, mypy,
# format-py, format-check-py) and `bench/harness` was missing from every one of them — the whole
# measurement toolchain, unlinted and untyped, while `bench/probes` beside it was covered. That is
# PENDING_AMENDMENTS (p)8, and the cost of leaving it open was concrete: `qualifies_as_primary`
# and `gaps_over` sat unused in `bench/harness` for the life of the project, and edits to the
# summarizer reached a benchmark matrix without ever passing a checker.
PY_SCOPE := . ../bench/probes ../bench/harness

# clang tooling: PATH first, else Homebrew LLVM (Apple's toolchain ships
# neither clang-format nor clang-tidy; on this host they exist only under
# /opt/homebrew/opt/llvm/bin). Override per-run if needed:
#   make format-check CLANG_FORMAT=/some/other/clang-format
LLVM_BIN       ?= /opt/homebrew/opt/llvm/bin
# PATH first; the Homebrew fallback is used only if that binary ACTUALLY EXISTS. Naming an
# absolute Homebrew path unconditionally produced `FATAL: no clang-format at
# /opt/homebrew/opt/llvm/bin/clang-format (brew install llvm...)` inside a Linux container —
# an accurate exit code attached to advice that cannot be followed there.
CLANG_FORMAT   ?= $(or $(shell command -v clang-format 2>/dev/null),$(wildcard $(LLVM_BIN)/clang-format))
# The install hint depends on the platform, for the same reason.
CLANG_HINT := $(if $(filter Darwin,$(shell uname -s)),brew install llvm,apt install clang-format)
RUN_CLANG_TIDY ?= $(or $(shell command -v run-clang-tidy 2>/dev/null),$(wildcard $(LLVM_BIN)/run-clang-tidy))
# The run-clang-tidy wrapper does NOT search beside itself for the clang-tidy
# binary — off PATH it dies with "failed to find clang-tidy in $PATH or at
# build/dev/bin/clang-tidy" (measured), so the binary is passed explicitly.
CLANG_TIDY     ?= $(or $(shell command -v clang-tidy 2>/dev/null),$(LLVM_BIN)/clang-tidy)
ifeq ($(shell uname -s),Darwin)
# Homebrew clang-tidy does not find the Apple SDK on its own ('cstdint' file
# not found — verified). Recursive `=`: xcrun runs only when tidy does.
TIDY_EXTRA = -extra-arg=-isysroot -extra-arg=$(shell xcrun --show-sdk-path)
endif

# All first-party C++ (cpp/ holds only .cpp/.hpp — verified, no .h).
CXX_SOURCES := $(shell find cpp -type f \( -name '*.cpp' -o -name '*.hpp' \) | LC_ALL=C sort)

# demo arm (tuned|naive).
STACK ?= tuned
# run_bench.sh positional args for the smoke pass: repeats samples warmup.
SMOKE_ARGS ?= 1 2000 500

# ----------------------------------------------------------------------------
# venv guard: a real-file target — present means no-op, absent means loud
# instructions (the repo's canonical setup line, plan Task 0 Step 5).
$(PY):
	@echo "FATAL: no Python venv found (looked for $(CURDIR)/.venv then /opt/venv)." >&2
	@echo "  host:      python3.14 -m venv .venv && .venv/bin/pip install -e './python[dev,tuned]'" >&2
	@echo "  container: the image builds /opt/venv (docker/Dockerfile) — rebuild it" >&2
	@echo "  override:  make VENV=/path/to/venv <target>" >&2
	@exit 1

# demo.sh, run_bench.sh and the pytest engine fixture all honour MM_ENGINE.
# The override is legitimate (pointing the suite at a sanitizer binary) but
# it reopens the stale-binary hole for the binary it names — say so loudly.
.PHONY: warn-mm-engine
warn-mm-engine:
	@if [ -n "$${MM_ENGINE:-}" ]; then \
	  echo "WARNING: MM_ENGINE=$${MM_ENGINE} — this run only guarantees build/rel is fresh;" >&2; \
	  echo "         the freshness of what MM_ENGINE points at is not checked here." >&2; \
	fi

# ----------------------------------------------------------------------------
# Configure — the only real-file targets in this Makefile: one make-owned
# stamp per preset, touched only after a successful configure.
# CMakePresets.json is a prerequisite because ninja's regen rule does not
# watch it (verified); CMakeLists.txt deliberately is NOT one — ninja's regen
# graph already tracks it plus every included .cmake file transitively, which
# make cannot know. The stamp is make's own file rather than CMakeCache.txt
# because CMake 4.4 preserves the cache's mtime when a reconfigure changes
# nothing (measured: a presets touch then re-fired the rule forever), and
# touching CMake-owned files is worse — CMakeCache.txt is an input to ninja's
# regen rule, so bumping it re-arms a reconfigure on every build. A failed
# configure leaves no stamp (.DELETE_ON_ERROR), forcing a retry next run.
build/dev/.mk-configured: CMakePresets.json
	cmake --preset dev
	@touch $@
build/rel/.mk-configured: CMakePresets.json
	cmake --preset rel
	@touch $@
build/asan/.mk-configured: CMakePresets.json
	cmake --preset asan
	@touch $@
build/tsan/.mk-configured: CMakePresets.json
	cmake --preset tsan
	@touch $@
build/coverage/.mk-configured: CMakePresets.json
	cmake --preset coverage
	@touch $@

# ----------------------------------------------------------------------------
# Build. .PHONY on purpose: the recipe ALWAYS runs and ninja decides what is
# stale — ninja is the only system that knows the true input set. A no-op
# rebuild costs a fraction of a second; a skipped rebuild once cost three
# stale preset reports.
.PHONY: build-dev build-rel build-asan build-tsan build-coverage build-all
build-dev: build/dev/.mk-configured ## Configure (if needed) + build the dev preset
	cmake --build --preset dev
build-rel: build/rel/.mk-configured ## Configure (if needed) + build the rel preset
	cmake --build --preset rel
build-asan: build/asan/.mk-configured ## Configure (if needed) + build the asan preset
	# BOUNDED FAN-OUT. ninja defaults to every visible core; ASan-instrumented TUs pulling in
	# Boost.Beast headers are large, and Docker Desktop's VM here reports 24 CPUs against
	# 16.7 GB.
	#
	# WHAT IS MEASURED vs WHAT IS INFERRED, kept apart on purpose. Measured: the Docker daemon
	# wedged twice on this host, both times during an unbounded asan build and never during rel,
	# with `docker info`/`ps`/`images` all timing out — which reads exactly like a slow build
	# unless you are watching for it. Also measured: bounded to 4, the full asan suite builds and
	# runs in ~200 s in-container with the daemon responsive throughout.
	#
	# NOT established: that memory exhaustion is the mechanism. The window contained no OOM lines
	# and zero jetsam events, and the change that made it stop moved three variables at once, so
	# the earlier claim that an unbounded build "exhausts the VM and WEDGES THE DAEMON" asserted a
	# cause that was never demonstrated. The bound stays because the correlation is real and the
	# cost of the precaution is a slower asan build; the explanation does not stay.
	#
	# ASAN_BUILD_JOBS is overridable for a host with headroom, but the DEFAULT must be safe:
	# the value that has twice preceded a wedged Docker is not an acceptable default.
	cmake --build --preset asan -- -j $(ASAN_BUILD_JOBS)
build-tsan: build/tsan/.mk-configured ## Configure (if needed) + build the tsan preset
	cmake --build --preset tsan
build-coverage: build/coverage/.mk-configured ## Configure (if needed) + build the coverage preset
	cmake --build --preset coverage
build-all: build-dev build-rel build-asan build-tsan ## All four builds (parallel-safe: make -j4 build-all)

# ----------------------------------------------------------------------------
# Test — the coupling this file exists for. Each test-* target's only path to
# ctest runs through its build-* edge in the same make invocation; a build
# failure stops make before ctest starts. There is deliberately no cached
# "tests already passed" state (no stamp files): tests re-run every time.
# That is also what keeps changed FIXTURES honest — bench/scenarios/*.feed
# and tests/golden/* reach the binaries via MM_SCENARIO_DIR / MM_GOLDEN_DIR
# compile definitions, no build step depends on them, and only an
# unconditional re-run picks their changes up.
.PHONY: test-dev test-rel test-asan test-tsan test-coverage
test-dev: build-dev ## Build dev, then run its suite (perf label excluded by preset)
	ctest --preset dev
test-rel: build-rel ## Build rel, then run its suite
	ctest --preset rel
test-asan: build-asan ## Build asan, then run its suite under ASan+UBSan
	ctest --preset asan
test-tsan: build-tsan ## Build tsan, then run its suite under TSan
	ctest --preset tsan
test-coverage: build-coverage ## Build coverage, then run its suite
	ctest --preset coverage

.PHONY: test-all
test-all: ## Four-preset matrix: builds fan out under -j, test phases stay serial
	$(MAKE) build-all
	$(MAKE) test-dev
	$(MAKE) test-rel
	$(MAKE) test-asan
	$(MAKE) test-tsan

.PHONY: perf
perf: build-rel ## The Release-calibrated wall-clock case (ctest --preset perf; expect reds on a loaded host)
	ctest --preset perf

# ----------------------------------------------------------------------------
# Code coverage (C++ source-based via llvm-cov + Python branch coverage).
LLVM_PROFDATA ?= $(shell command -v llvm-profdata 2>/dev/null || which llvm-profdata-21 2>/dev/null || echo xcrun llvm-profdata)
LLVM_COV ?= $(shell command -v llvm-cov 2>/dev/null || which llvm-cov-21 2>/dev/null || echo xcrun llvm-cov)

.PHONY: coverage-cxx coverage-all
coverage-cxx: build-coverage ## Run C++ test suite and report source-based code coverage via llvm-cov
	@rm -rf $(CURDIR)/build/coverage/profiles
	@mkdir -p $(CURDIR)/build/coverage/profiles
	LLVM_PROFILE_FILE="$(CURDIR)/build/coverage/profiles/%m_%p.profraw" ctest --preset coverage
	@PROFILES=$$($(LLVM_PROFDATA) merge -sparse $$(find $(CURDIR)/build/coverage/profiles -name "*.profraw") -o $(CURDIR)/build/coverage/coverage.profdata); \
	echo ""; \
	echo "=== C++ Source-Based Code Coverage Summary (llvm-cov) ==="; \
	$(LLVM_COV) report $(CURDIR)/build/coverage/mm_tests \
	  -instr-profile=$(CURDIR)/build/coverage/coverage.profdata \
	  --sources cpp/src cpp/include/mm

coverage-all: coverage-cxx coverage ## Run both C++ (llvm-cov) and Python (coverage.py) test coverage suites

# ----------------------------------------------------------------------------
# Python. Always run from python/ (config residence — see header). pytest
# depends on build-rel because the integration suite execs
# build/rel/mm_engine (engine_fixture.require_engine): without the edge it
# either skips (binary missing) or tests a stale engine — the cross-language
# twin of the ctest incident.
.PHONY: pytest
pytest: $(PY) build-rel warn-mm-engine ## Python suite (integration tests exec a freshly built build/rel/mm_engine)
	cd python && $(PY_ABS) -m pytest tests -q

.PHONY: coverage
coverage: $(PY) build-rel warn-mm-engine ## Branch coverage with the fail_under ratchet (same commands as the container gate)
	cd python && $(PY_ABS) -m coverage run -m pytest tests -q
	cd python && $(PY_ABS) -m coverage report

# ----------------------------------------------------------------------------
# Python style/type gate — scope pinned to scripts/verify_linux.sh's.
.PHONY: lint
lint: $(PY) ## ruff check + ruff format --check over $(PY_SCOPE)
	@test -x "$(RUFF_ABS)" || { echo "FATAL: $(RUFF_ABS) missing — .venv/bin/pip install -e './python[dev,tuned]'" >&2; exit 1; }
	cd python && $(RUFF_ABS) check $(PY_SCOPE)
	cd python && $(RUFF_ABS) format --check $(PY_SCOPE)

.PHONY: mypy
mypy: $(PY) ## mypy --strict via python/pyproject.toml (same scope as lint)
	@test -x "$(MYPY_ABS)" || { echo "FATAL: $(MYPY_ABS) missing — .venv/bin/pip install -e './python[dev,tuned]'" >&2; exit 1; }
	cd python && $(MYPY_ABS) $(PY_SCOPE)

# ----------------------------------------------------------------------------
# C++ style. The plan's own command shapes (Task 12 Step 1b): clang-format
# over an explicit file list; run-clang-tidy over the first-party TU scope.
# SPLIT BY LANGUAGE, so a gate can say "Python format is verified and the C++ tool is absent"
# instead of collapsing both into one unavailable check. The authoritative image had no
# clang-format, so a single combined target made the whole format gate unrunnable there — and the
# only alternatives were to skip it silently or to fail the container on a host-tool gap.
.PHONY: format-cxx
format-cxx: ## APPLY C++ formatting (clang-format -i)
	@test -n "$(CLANG_FORMAT)" && test -x "$(CLANG_FORMAT)" || { echo "FATAL: no clang-format found (PATH, then $(LLVM_BIN)). Install: $(CLANG_HINT), or set CLANG_FORMAT=" >&2; exit 2; }
	$(CLANG_FORMAT) -i $(CXX_SOURCES)

.PHONY: format-py
format-py: $(PY) ## APPLY Python formatting (ruff format)
	cd python && $(RUFF_ABS) format $(PY_SCOPE)

.PHONY: format
format: format-cxx format-py ## APPLY formatting, both languages

.PHONY: format-check-cxx
format-check-cxx: ## Fail on any unformatted C++ file
	@test -n "$(CLANG_FORMAT)" && test -x "$(CLANG_FORMAT)" || { echo "FATAL: no clang-format found (PATH, then $(LLVM_BIN)). Install: $(CLANG_HINT), or set CLANG_FORMAT=" >&2; exit 2; }
	$(CLANG_FORMAT) --dry-run -Werror $(CXX_SOURCES)

.PHONY: format-check-py
format-check-py: $(PY) ## Fail on any unformatted Python file (safe without clang-format)
	cd python && $(RUFF_ABS) format --check $(PY_SCOPE)

.PHONY: format-check
format-check: format-check-cxx format-check-py ## Fail on any unformatted first-party file

.PHONY: tidy
tidy: build-dev ## run-clang-tidy over cpp/src cpp/tests against build/dev's compile DB (slow, minutes)
	@test -n "$(RUN_CLANG_TIDY)" && test -x "$(RUN_CLANG_TIDY)" || { echo "FATAL: no run-clang-tidy found (PATH, then $(LLVM_BIN)). Install: $(if $(filter Darwin,$(shell uname -s)),brew install llvm,apt install clang-tidy), or set RUN_CLANG_TIDY=" >&2; exit 2; }
	$(RUN_CLANG_TIDY) -quiet -p build/dev -clang-tidy-binary $(CLANG_TIDY) $(TIDY_EXTRA) cpp/src cpp/tests

# ----------------------------------------------------------------------------
# GATES ARE NAMED AGGREGATES. Never `make a b c d` as a gate: that is four independent goals,
# each printing its own success banner, and make's exit status is the first failure's. Measured —
# `make lint mypy build-rel test-rel coverage docs-check` printed four green banners and exited 2
# because docs-check died last. That is correct make behaviour and a misuse on the caller's part.
#
# The rule these two targets encode: the VERDICT is the final ALL GREEN line plus the exit code,
# and no intermediate "All checks passed!" means anything about the aggregate. Each member runs
# through a recursive $(MAKE) so the last line is unreachable unless every one succeeded.
.PHONY: container-gate
container-gate: ## Authoritative in-image gate (no git, no sanitizers, no tidy — see comments)
	@echo "--- lint";         $(MAKE) lint
	@echo "--- mypy";         $(MAKE) mypy
	@echo "--- format-check"; $(MAKE) format-check
	@echo "--- build-rel";    $(MAKE) build-rel
	@echo "--- test-rel";     $(MAKE) test-rel
	@echo "--- test-asan";    $(MAKE) test-asan
	@echo "--- coverage";     $(MAKE) coverage
	@echo "container-gate: ALL GREEN"

# THE EMULATED VARIANT, and why it is a separate NAMED aggregate rather than a flag on the one
# above. Gates are named targets whose final line is the verdict; a gate whose membership depends
# on an environment variable cannot be read off its own name, and "which checks actually ran" is
# the first question anyone asks of a green run.
#
# It omits ASAN ONLY. Under qemu (linux/amd64 on an arm64 host) every instruction is translated,
# and an ASan-instrumented build is the heaviest thing this repo compiles — the E-8 smoke exists
# to prove the code BUILDS AND ITS UNIT TESTS PASS on the other architecture, not to re-measure
# sanitizer cleanliness that the native run already established on the same sources. Sanitizer
# findings are properties of the code, and the native gate is where they are gated.
#
# Everything else is kept deliberately: ruff, mypy and coverage are arch-independent, so skipping
# them here would weaken the smoke for no saving worth having.
.PHONY: container-gate-emulated
container-gate-emulated: ## In-image gate for the qemu smoke: as container-gate, minus asan
	@echo "--- lint";         $(MAKE) lint
	@echo "--- mypy";         $(MAKE) mypy
	@echo "--- format-check"; $(MAKE) format-check
	@echo "--- build-rel";    $(MAKE) build-rel
	@echo "--- test-rel";     $(MAKE) test-rel
	@echo "--- coverage";     $(MAKE) coverage
	@echo "container-gate-emulated: ALL GREEN (asan omitted: see the target comment)"

# DELIBERATELY NOT in container-gate:
#   docs-check  — needs a git working tree; .dockerignore excludes .git, so it refuses with a
#                 diagnosis there (check_doc_refs.py require_git_repo). Host gate only.
#   tidy        — run-clang-tidy is absent in the image and costs minutes; host-advisory.
#   tsan        — host `check` owns it. Not excluded on principle: it was simply not needed to
#                 satisfy the plan's Step 2, and each sanitizer preset costs a full build here.
#
# ASAN IS INCLUDED, and the earlier note claiming the image lacked sanitizer runtimes was wrong:
# /usr/lib/gcc/aarch64-linux-gnu/15/libasan.so is present, and the full asan suite passes 179/179
# in-container in ~200 s at ASAN_BUILD_JOBS=4. It belongs here because Linux ASan enables
# LeakSanitizer, which Apple's ASan does not support at all — so this is the ONLY environment
# that can observe an entire class of defect. It found one immediately (see the __lsan_ignore_object
# note in cpp/tests/session_alloc_probe.cpp).

.PHONY: floor-check
floor-check: ## Substantiate requires-python ">=3.11" on a real 3.11 interpreter (needs docker)
	scripts/check_python_floor.sh

.PHONY: docs-check
docs-check: $(PY) ## Mechanical doc/comment cross-reference check — HOST ONLY (needs git)
	$(PY) scripts/check_doc_refs.py

# ----------------------------------------------------------------------------
# End-to-end and bench. demo.sh deliberately builds nothing — so the Makefile
# builds FOR it. bench-smoke is a plumbing proof, NOT numbers: the full
# §5.2 matrix shape at toy sample counts. Publishable runs stay
# scripts/run_bench.sh with its defaults, per the benchmark protocol.
.PHONY: demo
demo: $(PY) build-rel warn-mm-engine ## The §4 demo against a rel binary built just now (STACK=tuned|naive)
	scripts/demo.sh $(STACK)

.PHONY: bench-smoke
bench-smoke: $(PY) build-rel warn-mm-engine ## Tiny end-to-end run_bench.sh pass (SMOKE_ARGS='1 2000 500')
	scripts/run_bench.sh $(SMOKE_ARGS)

.PHONY: perf-track
perf-track: $(PY) build-rel warn-mm-engine ## Run tests, benchmark smoke and record snapshot in history
	scripts/perf_tracker.sh "$${DESCRIPTION:-Snapshot $$(date -u +%Y%m%dT%H%M%SZ)}"

.PHONY: perf-history
perf-history: $(PY) ## Compare recent performance snapshots and print history report
	PYTHONPATH="$(CURDIR)/bench" $(PY) -m harness.history compare || true
	PYTHONPATH="$(CURDIR)/bench" $(PY) -m harness.history report

# ----------------------------------------------------------------------------
# Aggregates. Stages are recipe LINES, not prerequisites, so they run in
# order and under -j only the build fan-out parallelises — test phases stay
# serial (sanitizer suites contend for cores and carry wall-clock TIMEOUTs;
# ports are already ephemeral everywhere, so serialisation is about timing,
# not ports). Each test-* line re-runs its own build edge: a no-op when
# nothing changed, a rebuild when something changed mid-gate — the invariant
# holds per line, not per gate.
.PHONY: fast
fast: ## DEFAULT — the inner loop: dev C++ suite + Python suite, both freshly built
	$(MAKE) build-dev build-rel
	$(MAKE) test-dev
	$(MAKE) pytest

# tidy is NOT in check: it costs minutes and the host LLVM (22.x) is not the
# pinned container toolchain, so its findings are advisory here. Run
# `make tidy` before a review round; the container gate owns enforcement.
# format-check-cxx, not format-check: lint's second line IS the Python format
# check (the container gate's ruff pair), so the full aggregate here would
# run `ruff format --check` twice per gate.
.PHONY: check
check: ## Full local gate: lint, types, format, 4-preset matrix, coverage, docs
	$(MAKE) lint
	$(MAKE) mypy
	$(MAKE) format-check-cxx
	$(MAKE) build-all
	$(MAKE) test-dev
	$(MAKE) test-rel
	$(MAKE) test-asan
	$(MAKE) test-tsan
	$(MAKE) coverage
	$(MAKE) docs-check
	$(MAKE) floor-check
	@echo "check: ALL GREEN (host gate; the authoritative gate is 'make verify-linux')"

.PHONY: verify-linux
verify-linux: ## Authoritative container gate (docker; toolchain series pins + in-image probes): scripts/verify_linux.sh [PLATFORM=linux/arm64|linux/amd64]
	scripts/verify_linux.sh $(if $(PLATFORM),--platform $(PLATFORM))

.PHONY: ci
ci: ## The one target CI calls: full host gate, then the authoritative container
	$(MAKE) check
	$(MAKE) verify-linux

# ----------------------------------------------------------------------------
.PHONY: clean
clean: ## Delete the four preset build trees (the fix for removed-cache-variable and compiler-upgrade staleness)
	rm -rf build/dev build/rel build/asan build/tsan

.PHONY: help
help: ## List targets
	@grep -hE '^[a-zA-Z][a-zA-Z0-9_-]*:.*## ' $(MAKEFILE_LIST) | sort | awk -F':.*## ' '{printf "  %-13s %s\n", $$1, $$2}'
