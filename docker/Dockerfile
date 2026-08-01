# Authoritative environment (design decision): ubuntu:26.04 — g++ 15.2, boost 1.90, python 3.14.
FROM ubuntu:26.04

# THE TREE THIS IMAGE WAS BUILT FROM, recorded so a reader can ask the image what it contains
# instead of inferring it. scripts/verify_linux.sh passes the hash of the TRACKED source tree and
# refuses to test an image whose label does not match the working tree.
#
# Why a build-arg label rather than hashing the tree inside the container: .dockerignore
# deliberately excludes .git, caches and build/, so the two sides can never see the same file
# set — a first attempt compared 897 host files against 102 in the image and "failed" every
# time. A label is the same question asked once, at the only moment both sides agree.
ARG MM_TREE_SHA=unset
LABEL mm.tree_sha=$MM_TREE_SHA

# ARG, not ENV: build-scoped only — must not persist into the runtime environment or
# derived images (e.g. the asan stage) per Docker best practices (review finding).
ARG DEBIAN_FRONTEND=noninteractive
# perf note: ubuntu:26.04 ships perf in `linux-perf` (Debian-style,
# /usr/bin/perf, no kernel-version wrapper) — NOT in linux-tools-generic, whose 7.0.0-28
# payload is only cpupower/rtla/usbip.
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake make ninja-build libboost-dev python3.14-venv python3-pip git \
      clang-format \
      ca-certificates linux-perf \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# Coarse-to-fine layered copies (review finding): the expensive C++ dependency fetch +
# release build re-runs only when C++ inputs change; Python-only edits reuse that layer.
COPY CMakeLists.txt CMakePresets.json ./
COPY cpp cpp

# C++ release build (FetchContent: pinned glaze/Catch2 tags + hash-pinned nlohmann release archive).
RUN cmake --preset rel && cmake --build --preset rel

# Python 3.14 venv with both client arms' dependencies. Deliberately NO
# `pip install --upgrade pip` (review finding): an unpinned pip would be the only floating
# dependency in an otherwise fully pinned image — the venv's bundled pip installs the
# ==-pinned wheels fine, and scripts/verify_linux.sh records the effective pip version
# alongside the other toolchain versions.
COPY python python
RUN python3.14 -m venv /opt/venv \
    && /opt/venv/bin/pip install -e ./python[dev,tuned]

# Remainder of the repo (scripts, bench, docs, dotfiles). build/ is .dockerignore'd from
# the context, so this cannot clobber the in-image build/rel tree that the
# scripts/verify_linux.sh glaze probe reads (build/rel/_deps/glaze-src/include).
COPY . .

# venv on PATH so console scripts (mm-client from the transport adapters, pytest) resolve unqualified
# inside the container (review finding).
ENV PATH="/opt/venv/bin:${PATH}"
