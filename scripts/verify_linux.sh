#!/usr/bin/env bash
# Build the authoritative ubuntu:26.04 image and run the full test suite — plus the
# Python lint/type gate — inside it.
# Bare invocation targets the HOST-NATIVE platform (reviewer/CI path; on GitHub's x86-64
# runners this is real target-arch evidence for docx §9 acceptance criterion 1 — never qemu).
# On the macOS arm64 dev host, `--platform linux/arm64` is the authoritative-benchmark
# invocation (identical to bare there: the native Linux platform IS linux/arm64).
# `--platform linux/amd64` on an arm64 host is the E-8 qemu smoke (build + unit tests +
# the arch-independent Python lint/type/coverage gate, never benchmarks under emulation).
# Run with -h for usage.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/verify_linux.sh [--platform linux/arm64|linux/amd64] [-h|--help]

Builds the authoritative ubuntu:26.04 image and runs the full test suite plus the
Python lint/type/coverage gate (ruff, mypy --strict, coverage) inside it.
  (bare)                    host-native platform build — reviewer/CI path
  --platform linux/arm64    authoritative-benchmark invocation on the macOS arm64 dev host
  --platform linux/amd64    E-8 qemu smoke on an arm64 host (build + unit tests + the
                            Python gate; never benchmarks under emulation)
EOF
}

PLATFORM=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --platform)
      [[ $# -ge 2 ]] || { echo "--platform requires a value (linux/arm64|linux/amd64)" >&2; exit 2; }
      case "$2" in
        linux/arm64|linux/amd64) ;;
        *) echo "unsupported --platform: $2 (allowed: linux/arm64, linux/amd64)" >&2; exit 2 ;;
      esac
      PLATFORM="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# Only pass --platform when explicitly requested (gate finding): a hardcoded linux/arm64
# default would silently qemu-emulate the build on every x86-64 reviewer/CI host,
# defeating the E-8 and F-33 mitigations that protect acceptance criterion 1.
PLATFORM_ARGS=()
if [[ -n $PLATFORM ]]; then
  PLATFORM_ARGS=(--platform "$PLATFORM")
fi

cd "$(dirname "$0")/.."

# Arch-suffixed tag: the arm64 (authoritative) and amd64 (E-8 qemu smoke) runs must never
# overwrite each other's image under one shared name. For bare (native) runs the arch is
# resolved from the docker daemon so the collision guard still holds.
if [[ -n $PLATFORM ]]; then
  ARCH="${PLATFORM##*/}"
else
  ARCH="$(docker info --format '{{.Architecture}}')"
fi
# NORMALISED, because the two sources spell the same machine differently: `--platform` uses
# Docker's names (arm64 / amd64) while `docker info` reports the uname ones (aarch64 / x86_64).
# Unnormalised, `make verify-linux` tagged mm-engine-verify:aarch64 and
# `make verify-linux PLATFORM=linux/arm64` tagged mm-engine-verify:arm64 — two images for one
# machine — and scripts/profile.sh, which defaults to :aarch64, then either failed with "image
# not present" or silently profiled the older of the two. One spelling, so the collision guard
# still separates architectures without splitting a single one in half.
case "$ARCH" in
  arm64|aarch64) ARCH="aarch64" ;;
  amd64|x86_64)  ARCH="x86_64" ;;
esac
IMAGE="mm-engine-verify:${ARCH}"

# The TRACKED source tree, hashed BY CONTENT of the WORKING TREE.
#
# `git ls-files` bounds it to tracked paths, so untracked caches and build output cannot perturb
# it — a filesystem-walk comparison saw 897 host files against 102 in the image (`.mypy_cache`
# alone) and could never match. But the hash must come from the FILES, not from
# `git ls-files -s`'s blob shas: those describe the INDEX, and `docker build` copies the WORKING
# TREE. An unstaged edit would leave the index hash unchanged while the image content differed —
# the gate would pass on exactly the mismatch it exists to catch.
# WHICH GATE, decided by whether this run is EMULATED. `--platform linux/amd64` on an arm64 host
# translates every instruction, and an ASan build is the heaviest compile here — the E-8 smoke
# exists to prove the other architecture builds and its unit tests pass, which is what this
# script's own header has always claimed it does. Sanitizer cleanliness is a property of the
# sources and is gated by the NATIVE run on the same sources.
HOST_PLATFORM="linux/$(uname -m)"
case "$HOST_PLATFORM" in linux/arm64|linux/aarch64) HOST_PLATFORM="linux/arm64" ;; linux/x86_64) HOST_PLATFORM="linux/amd64" ;; esac
if [[ -n "$PLATFORM" && "$PLATFORM" != "$HOST_PLATFORM" ]]; then
  MM_GATE="container-gate-emulated"
  echo "--- EMULATED run ($PLATFORM on $HOST_PLATFORM): gate is $MM_GATE (asan omitted)"
else
  MM_GATE="container-gate"
fi

MM_TREE_SHA="$(git ls-files -z | xargs -0 shasum | shasum | cut -d' ' -f1)"
echo "--- working tree: $MM_TREE_SHA"

docker build ${PLATFORM_ARGS[@]+"${PLATFORM_ARGS[@]}"} \
  --build-arg "MM_TREE_SHA=$MM_TREE_SHA" -t "$IMAGE" -f docker/Dockerfile .

# IMAGE-FRESHNESS GATE. The image is a build artifact exactly like `mm_tests`, and "did the
# artifact absorb my change?" is the question `make test-<preset>` answers for ctest and that
# nothing was asking here. Measured: a `docker build` reported rc=0, produced NO new image, and
# the run that followed tested a source tree 58 minutes stale — reporting a real, reproducible
# failure about code that no longer existed.
#
# `docker build` succeeding is NOT that evidence: it can succeed and leave the previous image in
# place. Asking the image which tree it carries is.
IMAGE_TREE_SHA="$(docker inspect --format '{{index .Config.Labels "mm.tree_sha"}}' "$IMAGE")"
if [[ "$IMAGE_TREE_SHA" != "$MM_TREE_SHA" ]]; then
  echo "FATAL: the image does not carry the working tree." >&2
  echo "  working tree: $MM_TREE_SHA" >&2
  echo "  image label : ${IMAGE_TREE_SHA:-<absent>}" >&2
  echo "  The build did not absorb your changes, so anything below would describe code you are" >&2
  echo "  not running. Rebuild and confirm a NEW image was produced." >&2
  exit 1
fi
echo "--- image freshness: image carries the working tree ($IMAGE_TREE_SHA)"

# `-e MM_GATE` — WITHOUT THIS THE GATE SILENTLY WEAKENS ITSELF. MM_GATE is computed on the HOST
# (container-gate vs container-gate-emulated), and the script inside the container runs
# `make "$MM_GATE"`. That variable did not cross the boundary, so it expanded to empty and the
# container ran `make ""` — which is `make` with no target, i.e. the Makefile's .DEFAULT_GOAL,
# `fast`. The run then printed "ALL GREEN" having executed build + test-dev + pytest and NONE of
# lint, mypy, format-check, test-asan or coverage. The banner read `--- make ` with nothing after
# it on every single run, which is exactly how a gate lies quietly for a long time.
docker run --rm ${PLATFORM_ARGS[@]+"${PLATFORM_ARGS[@]}"} \
  -e "MM_GATE=$MM_GATE" \
  "$IMAGE" bash -eo pipefail -c '
  # ONE GATE, NOT A SECOND COPY OF IT. This block used to inline ctest, pytest, ruff, mypy and
  # coverage with hardcoded /opt/venv paths — a parallel definition of the same gate the Makefile
  # already expresses, which drifts the moment either side is edited alone. `make container-gate`
  # is now the single definition; what stays below is what genuinely belongs to the IMAGE rather
  # than to the project: the toolchain-series assertions and the glaze/Beast/perf probes.
  #
  # The gate also gains what the inline block could not have: every test target BUILDS before it
  # tests (the incident this pipeline exists for), and the venv is DISCOVERED, so the same target
  # runs against a repo-local .venv and against this image /opt/venv, one code path only.
  #
  # Members, and the three deliberate exclusions (docs-check needs a git tree that
  # .dockerignore removes; run-clang-tidy is absent here and costs minutes; the sanitizer
  # matrix belongs to the host `check`) are documented on the target itself.
  # FAIL CLOSED. An unset MM_GATE must not fall through to the Makefile default: `make ""` runs
  # .DEFAULT_GOAL and reports success for a target nobody asked for. The expected values are named
  # so a typo is caught here rather than as a confusing "no rule to make target" later.
  case "${MM_GATE:-}" in
    container-gate|container-gate-emulated) ;;
    "") echo "FATAL: MM_GATE is unset inside the container — the host did not pass it." >&2
        echo "  Refusing to run make with no target: that silently runs .DEFAULT_GOAL (fast)" >&2
        echo "  and would report a green gate for a weaker set of checks." >&2
        exit 2 ;;
    *)  echo "FATAL: MM_GATE=$MM_GATE is not a gate target." >&2; exit 2 ;;
  esac
  echo "--- make $MM_GATE"
  make "$MM_GATE"

  # Toolchain versions: ASSERTED, not merely printed — an apt-archive refresh under the
  # same ubuntu:26.04 tag must fail this run loudly, not silently drift the measurement
  # environment away from what docker/Dockerfile line 1 asserts (g++ 15.2, boost 1.90,
  # python 3.14). The asserted CONTRACT is the minor series (g++ 15.x, Python 3.14.x, Boost 1_90),
  # not an exact patch: ubuntu:26.04 ships patch updates under the same tag and pinning the
  # patch would fail on an ordinary archive refresh. Task 11/13 publish a benchmark manifest
  # against these (P1-R3 finding; series-vs-patch contract stated per codex hard-gate S3).
  # pip is recorded (unpinned-by-design: the venv bundled pip), not asserted.
  echo "--- toolchain versions (asserted: g++ 15.*, BOOST_LIB_VERSION 1_90, Python 3.14.*)"
  GXX_VER=$(g++ --version | head -1)
  PY_VER=$(python3.14 --version)
  BOOST_VER=$(grep "#define BOOST_LIB_VERSION" /usr/include/boost/version.hpp)
  PIP_VER=$(/opt/venv/bin/pip --version)
  printf "%s\n" "$GXX_VER" "$PY_VER" "$BOOST_VER" "$PIP_VER"
  case "$GXX_VER" in *" 15."*) ;; *) echo "FATAL toolchain drift: want g++ 15.*, got: $GXX_VER" >&2; exit 1 ;; esac
  case "$PY_VER" in "Python 3.14."*) ;; *) echo "FATAL toolchain drift: want Python 3.14.*, got: $PY_VER" >&2; exit 1 ;; esac
  case "$BOOST_VER" in *\"1_90\"*) ;; *) echo "FATAL toolchain drift: want BOOST_LIB_VERSION \"1_90\", got: $BOOST_VER" >&2; exit 1 ;; esac

  # Toolchain probes in the pinned image (F-25/F-35: the design-time probes ran on Debian
  # trixie; these re-prove the three risky assumptions inside ubuntu:26.04 itself).
  echo "--- probe: glaze TU compiles under -std=c++23"
  printf "%s\n" \
    "#include <glaze/glaze.hpp>" \
    "struct P { int x{}; };" \
    "int main() {" \
    "  std::string out = glz::write_json(P{42}).value_or(\"ERR\");" \
    "  return out.empty(); }" > /tmp/probe_glaze.cpp
  g++ -std=c++23 -I build/rel/_deps/glaze-src/include -c /tmp/probe_glaze.cpp -o /tmp/probe_glaze.o
  echo "glaze probe: ok"

  echo "--- probe: Boost.Beast include TU compiles under -std=c++20"
  printf "%s\n" \
    "#include <boost/beast.hpp>" \
    "int main() { return 0; }" > /tmp/probe_beast.cpp
  g++ -std=c++20 -c /tmp/probe_beast.cpp -o /tmp/probe_beast.o
  echo "beast probe: ok"

  echo "--- probe: perf runs in the pinned image (26.04 ships it as linux-perf ->"
  echo "    /usr/bin/perf, no kernel-version wrapper; profile.sh uses this path)"
  /usr/bin/perf --version
'
echo "verify_linux.sh: ALL GREEN (platform ${PLATFORM:-native}, arch ${ARCH}, image ${IMAGE})"
