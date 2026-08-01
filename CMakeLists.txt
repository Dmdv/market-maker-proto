cmake_minimum_required(VERSION 3.28)
project(mm_engine VERSION 0.1.0 LANGUAGES CXX)

# C++20 is the project default. The ONE documented deviation is the glaze codec TU
# (cpp/src/codec_glaze.cpp): glaze 7.9.x requires C++23, so that single
# source file is compiled with CXX_STANDARD 23 below. Everything else stays C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
# Headers suffice for Beast/Asio (header-only); no compiled Boost components needed.
# CONFIG mode: BoostConfig.cmake ships with Boost >= 1.70 everywhere we build (brew 1.90,
# ubuntu:26.04 libboost-dev 1.90) and the legacy FindBoost module is removed in CMake 4.
find_package(Boost 1.83 REQUIRED CONFIG)

include(FetchContent)
# GIT_SHALLOW (glaze, Catch2): both pins are release tags (not commit hashes), so a depth-1
# clone is valid and sufficient. CMake emits `--depth 1 --no-single-branch`, so every branch
# tip still lands at depth 1 — measured residual .git: catch2 9.5 MB + glaze 6.6 MB ≈ 16 MB
# per build tree (review finding).
# nlohmann_json is fetched as the release ARCHIVE instead (review finding): its shallow
# clone still dragged 30 branch tips / 177 MB of .git through --no-single-branch, while the
# release tarball is ~112 KB and the URL_HASH pin is strictly stronger provenance than a
# movable tag.
# SYSTEM (CMake >= 3.25; we require 3.28): third-party headers reach our
# -Wall -Wextra -Wpedantic TUs via -isystem instead of plain -I, keeping warnings scoped
# to first-party code (review finding).
FetchContent_Declare(nlohmann_json
  URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  SYSTEM)
FetchContent_Declare(glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG        v7.9.1
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  SYSTEM)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.15.2
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  SYSTEM)
FetchContent_MakeAvailable(nlohmann_json glaze Catch2)

# Shared usage requirements (review finding).
#   mm_warnings — the warning set every first-party target compiles with.
#   mm_common   — the public include dir + header-only deps every first-party target uses.
# Neither carries a compile-feature requirement, so the C++23 isolation of mm_codec_glaze
# below (and its deliberate non-linking of glaze::glaze into mm_core) is unaffected.
add_library(mm_warnings INTERFACE)
target_compile_options(mm_warnings INTERFACE -Wall -Wextra -Wpedantic)
add_library(mm_common INTERFACE)
target_include_directories(mm_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
target_link_libraries(mm_common INTERFACE Boost::headers Threads::Threads)

# Core library: pure logic + codecs; later tasks append sources.
# frame_preflight.cpp is the shared frame authority BOTH codec arms call (review finding).
# server.cpp / server_accept.cpp / session.cpp: the Beast server split under the
# 500-line file cap along its ownership seams — server.cpp owns process lifecycle, the
# feed driver and the telemetry pump; server_accept.cpp owns the listen loop and the
# HTTP/WebSocket upgrade (split off at gate R1, when pending-upgrade tracking, admission
# control and accept re-arm carried the one file past the cap); session.cpp owns one
# connection's read/command/write state machine, with session_close.cpp taking its close
# half (latching, drain-then-close, epoch retirement, teardown) when the Phase-4 gate work
# carried THAT file past the cap in turn; the shared internal declarations live in
# cpp/src/server_impl.hpp (deliberately not under cpp/include/mm/: Beast stays out of the
# public header).
add_library(mm_core STATIC
  cpp/src/frame_preflight.cpp
  cpp/src/codec_nlohmann.cpp
  cpp/src/engine.cpp
  cpp/src/feed.cpp
  cpp/src/server.cpp
  cpp/src/server_accept.cpp
  cpp/src/session.cpp
  cpp/src/session_close.cpp)
target_link_libraries(mm_core PUBLIC mm_common PRIVATE mm_warnings
  nlohmann_json::nlohmann_json)

# Documented deviation (design decision: glaze codec TU is C++23; the rest of the tree is C++20).
# CXX_STANDARD is a TARGET property — setting it per-source via set_source_files_properties is
# a silent no-op (review finding). The C++23 TU therefore lives in its own OBJECT
# library carrying the property for real; its objects fold into mm_core. This is
# verified TWO-SIDEDLY in compile_commands.json: "-std=c++23" appears on the codec_glaze.cpp
# TU AND every other cpp/src/*.cpp entry still carries "-std=c++20" (one-sided checking is
# unfalsifiable against feature-propagation leaks — gate finding).
# (CMAKE_CXX_EXTENSIONS is OFF, so CMake emits -std=c++23, never the gnu variant.)
add_library(mm_codec_glaze OBJECT cpp/src/codec_glaze.cpp)
set_target_properties(mm_codec_glaze PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
# NOT linked against mm_core (that would be a dependency cycle: mm_core consumes these
# objects below); mm_common is the shared decoupling for includes + header-only deps.
target_link_libraries(mm_codec_glaze PRIVATE mm_common mm_warnings glaze::glaze)
target_sources(mm_core PRIVATE $<TARGET_OBJECTS:mm_codec_glaze>)
# Deliberately NO `target_link_libraries(mm_core ... glaze::glaze)` here: glaze's INTERFACE
# carries a cxx_std_23 compile feature that would silently raise EVERY mm_core TU to C++23
# (empirically reproduced — gate finding, Phase1-R4). glaze is header-only; the compiled
# objects above are all mm_core needs. Only mm_codec_glaze may link glaze::glaze.

add_executable(mm_engine cpp/src/main.cpp)
# Single source of truth for the C++ version string is project(VERSION) above. The compiler
# and flag stamps below are what `--version` prints and what the benchmark benchmark manifest
# records: a latency table is not reproducible without knowing which compiler, at which
# optimization level, built the binary that produced it. Read from the BINARY rather than
# re-derived by the harness, so a stale build cannot be reported as a fresh one.
#
# The per-configuration flags are resolved through a generator expression because
# CMAKE_CXX_FLAGS_<CONFIG> is not interpolated at configure time for multi-config generators —
# writing `${CMAKE_CXX_FLAGS_RELEASE}` here would silently stamp an empty string under Ninja
# Multi-Config and a correct one under a single-config generator, which is exactly the kind of
# difference a manifest exists to make impossible.
target_compile_definitions(mm_engine PRIVATE
  MM_ENGINE_VERSION="${PROJECT_VERSION}"
  MM_ENGINE_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  MM_ENGINE_STD="C++${CMAKE_CXX_STANDARD}"
  MM_ENGINE_BUILD_TYPE="$<CONFIG>"
  MM_ENGINE_CXX_FLAGS="${CMAKE_CXX_FLAGS} $<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}>$<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}>$<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO}>"
)
target_link_libraries(mm_engine PRIVATE mm_core mm_warnings)

enable_testing()
# test_codec_equivalence.cpp (+ the shared codec_test_support.hpp) splits the codec suite
# under the 500-line file cap (addition to the design's file map, same rationale as the
# frame_preflight.cpp source split above). The engine suite is split the same way, along
# behavioral seams: lifecycle / session lifetime / fill rule (+ the shared
# engine_test_support.hpp). The allocation cases (the engine inventory + injection cases in
# test_engine_alloc.cpp, the Outbox cases in test_outbox_alloc.cpp) all arm ONE global
# operator new replacement — REPLACING it (to count and, armed, to FAIL allocations) is a
# whole-binary property that belongs in exactly one TU, so it is DEFINED once in
# alloc_probe.cpp (probe state + replacement, shared via alloc_probe_support.hpp) rather than
# behind a test-only allocator seam on the shipped mm::Outbox — one harness, no production
# API widened for a test. All four additions to the design's file map are queued in
# the limitations backlog; the alloc-probe split's three files in item (r).
add_executable(mm_tests
  cpp/tests/alloc_probe.cpp
  cpp/tests/test_types.cpp
  cpp/tests/test_codec.cpp
  cpp/tests/test_codec_equivalence.cpp
  cpp/tests/test_engine.cpp
  cpp/tests/test_engine_session.cpp
  cpp/tests/test_engine_fills.cpp
  cpp/tests/test_engine_alloc.cpp
  cpp/tests/test_feed.cpp
  cpp/tests/test_outbox.cpp
  cpp/tests/test_outbox_alloc.cpp
  cpp/tests/test_telemetry.cpp
  cpp/tests/test_telemetry_writer.cpp
  cpp/tests/test_telemetry_errors.cpp
  cpp/tests/test_telemetry_wire.cpp
  cpp/tests/test_server_ws.cpp
  cpp/tests/test_server_frames.cpp
  cpp/tests/test_server_session.cpp
  cpp/tests/test_server_epoch.cpp
  cpp/tests/test_server_flow.cpp
  cpp/tests/test_server_shutdown.cpp
  cpp/tests/test_server_artifacts.cpp
  cpp/tests/test_bench_recorder.cpp)
# tests/golden/ (repo root) is the cross-language contract — Python reads the same
# files; the absolute path keeps ctest working from any build dir. bench/scenarios/ is the
# same idea for the feed: the SHIPPED scenario files are pinned event-for-event, so
# the tests must read the very files the demo and the benchmarks run, not copies.
target_compile_definitions(mm_tests PRIVATE
  MM_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
  MM_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/bench/scenarios")
# nlohmann linked into the tests directly: the semantic encode checks parse both codecs'
# output with an independent reader (mm_core's nlohmann link is PRIVATE by design).
target_link_libraries(mm_tests PRIVATE mm_core Catch2::Catch2WithMain mm_warnings
  nlohmann_json::nlohmann_json)
# cpp/src on the TESTS' include path, and only theirs: the acceptor's apply_socket_options
# is declared in cpp/src/server_impl.hpp, and the white-box case that pins TCP_NODELAY
# compiles against that same declaration (the rationale for declaring it there, rather than
# in the test, is on the declaration). Deliberately NOT a widening of mm_core's PUBLIC
# includes — cpp/include is the shipped interface and server_impl.hpp lives outside it
# precisely to keep Beast/Asio out of it.
target_include_directories(mm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/src)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
# TIMEOUT 60, blanket over every test this call discovers (the PROPERTIES option,
# documented in the vendored extras/Catch.cmake): the bounded-destruction telemetry case's
# failure mode of interest is a HANG — an unbounded shutdown drain never returns — which
# without a TIMEOUT would eat ctest's 1500 s default instead of failing fast. Blanket
# rather than name-scoped deliberately: the previous mechanism (set_tests_properties on
# the one case's NAME, via a TEST_INCLUDE_FILES script) was measured to do the OPPOSITE of
# failing loudly on a rename — ctest 4.4 ignores set_tests_properties on an unknown test
# name, so renaming the case silently dropped the timeout (json-v1 TIMEOUT gone, rc=0, no
# stderr). The bound's headroom is a HOST-SCOPED figure, not a property of the suite: on
# the arm64-native dev host the slowest discovered test measures 0.83 s under tsan
# (2026-07-29; re-check by timing `ctest --preset tsan` and confirming the property with
# `ctest --show-only=json-v1`) — 72x inside the bound — while under amd64 qemu emulation
# (scripts/verify_linux.sh --platform linux/amd64) sanitizer cases run an order of
# magnitude slower and the margin is single digits: a case added near that ceiling should
# raise the bound, not trust the native margin. The mechanized every-discovered-test-
# carries-TIMEOUT check is queued (the limitations backlog).
catch_discover_tests(mm_tests PROPERTIES TIMEOUT 60)

# The session write path's / TOB fan-out's allocation discipline, measured against a live
# engine. A SECOND EXECUTABLE rather than another mm_tests TU, for the reason the one-TU
# note above already implies: it needs its OWN global `operator new` replacement, and the
# replacement is a whole-binary property. Its probe state is thread_local (mm_tests' is a
# plain global, correctly — every case that arms it is single-threaded) because counting a
# running engine means counting ONE thread of a multi-threaded process; the file's header
# comment carries the rest of the rationale. No Catch2: the per-assertion machinery would
# allocate on the very thread being counted.
add_executable(mm_session_alloc_probe cpp/tests/session_alloc_probe.cpp)
target_link_libraries(mm_session_alloc_probe PRIVATE mm_core mm_warnings)
# TIMEOUT well past the discovered suite's blanket 60 s, deliberately, and by the rule that
# comment states: this test is SIX complete engine runs, each paying a fixed 250 ms
# session-establishment halt before its ticks — ~2.4 s measured natively and ~6 s under
# tsan, where the blanket bound's host-scoped headroom (72x native, single digits under
# amd64 qemu emulation) would not survive the qemu factor. A case added near the ceiling
# raises the bound rather than trusting the native margin.
add_test(NAME server_tick_path_allocations COMMAND mm_session_alloc_probe)
set_tests_properties(server_tick_path_allocations PROPERTIES TIMEOUT 300)

# The mm_engine BINARY's own fixture — until it was added, nothing in the repository ran
# mm_engine at all, so cpp/src/main.cpp (the CLI surface, the one-line-plus-exit-2 ops
# contract, the startup/shutdown line formats, the admission defaults) was pinned by
# nothing. Plan requires the flag surface to hold, which makes it a contract
# and not a convenience.
# REQUIRED, deliberately: guarding the registration on an optional interpreter would let
# main.cpp's ONLY automated coverage disappear from a tree that merely lacks python3 —
# silently, and with a green ctest. The project already ships a Python client arm # and the authoritative container installs python3 in the same layer as the compiler, so
# this adds no dependency the repository did not already have. Interpreter only: the fixture
# imports nothing outside the standard library.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# $<TARGET_FILE:mm_engine> rather than a path: each preset's fixture then exercises ITS OWN
# binary, so the sanitizer trees run the engine under their sanitizer.
add_test(NAME mm_engine_cli
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mm_engine_cli_test.py
                 $<TARGET_FILE:mm_engine>)
# TIMEOUT past the discovered suite's blanket 60 s for the same reason as the probe above:
# the fixture spawns ~20 engines, and the two that wait on a bound (the mute-peer reap, the
# pre-stop settle) pay ~1 s of that between them — 1.3 s measured natively, against an
# amd64-qemu factor the native margin cannot speak for.
set_tests_properties(mm_engine_cli PROPERTIES TIMEOUT 300)
# The engine history-scaling case is the suite's ONLY wall-clock-bounded test: its
# Catch2 tag starts with `.` (hidden), so catch_discover_tests leaves it out of the
# correctness set — a scheduler preemption on a loaded host must never red a
# correctness gate. It stays ENFORCED by an automated run through the explicit
# registration below: label `perf`, excluded by the default test presets and run via
# `ctest --preset perf` (running it routinely is queued in the backlog. Its only other tag is `[perf]` — deliberately NOT the
# `[engine]` tag the correctness engine cases carry, because `[.]` hides a case from the
# UNFILTERED run only: Catch2 still selects hidden cases that MATCH an explicit spec, so
# a targeted `./mm_tests "[engine]"` in any build tree would otherwise reach it. The case
# is reachable ONLY via the `perf` label (this registration) or the `[perf]` tag.
#
# Registered ONLY in the flavor it is calibrated for. Its bounds are measured against -O3
# -DNDEBUG; in a Debug or sanitizer tree they mean nothing, and a bare `ctest` there (no
# preset, hence no label filter — `ctest --test-dir build/dev`, an IDE runner, a future CI
# step) would run a Release-calibrated timing case at -O0 and red on load. The preset-level
# label exclusion is the outer guard; this is the inner one, and nothing is lost: the `perf`
# test preset pins configurePreset `rel`, whose CMAKE_BUILD_TYPE is Release.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_test(NAME engine_history_scaling COMMAND mm_tests "[perf]")
  set_tests_properties(engine_history_scaling PROPERTIES LABELS perf)
endif()
