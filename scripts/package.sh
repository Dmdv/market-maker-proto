#!/usr/bin/env bash
# Builds the submission archive, and PROVES it is self-sufficient by building it from scratch.
#
# WHY THE VERIFICATION IS THE POINT, not the tarball. `git archive` will happily produce an archive
# that is missing something the build needs — a file that was never tracked, a path excluded one
# character too broadly — and the failure surfaces on the reviewer's machine, which is the worst
# possible place for it. So this script extracts what it just built, into a directory with no
# relationship to this checkout, and builds THAT in the pinned image. An archive that has not been
# built from is a guess.
#
# WHAT IS EXCLUDED, and why each one:
#   docs/decisions/    the ratification record — internal deliberation, not a deliverable
#   docs/superpowers/  the implementation plan and specs — same
#   .claude/           agent configuration, nothing to do with the project
#   bench/results/     ~96 MB of raw samples. See MM_INCLUDE_RESULTS below: the numbers belong to
#                      the submission, but shipping them by default makes the archive unwieldy for
#                      a reviewer who only wants to read and build.
#
# Usage: scripts/package.sh [output.tar.gz]
#   MM_INCLUDE_RESULTS=1   include bench/results/ (the raw §5.2 artifacts)
#   MM_SKIP_BUILD=1        write the archive without the standalone build check (NOT recommended;
#                          the check is the reason this script exists)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/mm-engine-submission.tar.gz}"
INCLUDE_RESULTS="${MM_INCLUDE_RESULTS:-0}"

cd "$REPO"

# Refuse to package a dirty tree. `git archive` reads committed state, so an uncommitted fix would
# be silently absent from the archive while present in the tree the author just tested.
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "FATAL: the working tree has uncommitted tracked changes." >&2
  echo "  git archive packages COMMITTED state, so those changes would be missing from the" >&2
  echo "  archive while still present in the tree you tested. Commit or stash first." >&2
  git status --short --untracked-files=no >&2
  exit 1
fi

EXCLUDES=(
  ':!docs/decisions'
  ':!docs/superpowers'
  ':!docs/PENDING_AMENDMENTS.md'
  ':!docs/followup_email*'
  ':!docs/interview_transcrip.md'
  ':!docs/transcript.txt'
  ':!.claude'
)
if [[ "$INCLUDE_RESULTS" != "1" ]]; then
  EXCLUDES+=(':!bench/results')
fi

echo "--- packaging $(git rev-parse --short HEAD) -> $OUT"
printf '    excluding: %s\n' "${EXCLUDES[*]}"
git archive --format=tar.gz --prefix=mm-engine/ -o "$OUT" HEAD -- . "${EXCLUDES[@]}"

SIZE="$(du -h "$OUT" | cut -f1)"
COUNT="$(tar -tzf "$OUT" | wc -l | tr -d ' ')"
echo "    $COUNT entries, $SIZE"

# The exclusions must actually have excluded. A rename or a path typo silently ships internal
# deliberation to a reviewer, which is the one leak this script must not have.
LEAKED="$(tar -tzf "$OUT" | grep -E 'docs/(decisions|superpowers|PENDING_AMENDMENTS|followup_email|interview_transcrip|transcript\.txt)|\.claude/' || true)"
if [[ -n "$LEAKED" ]]; then
  echo "FATAL: excluded paths are present in the archive:" >&2
  printf '%s\n' "$LEAKED" | head -10 >&2
  exit 1
fi
echo "    exclusions verified: no decisions/, superpowers/, PENDING_AMENDMENTS or .claude/ entries"

# The deliverables must actually BE there — the mirror of the check above, because an over-broad
# exclusion fails in exactly the opposite direction and is just as invisible in a tarball listing.
for required in mm-engine/README.md mm-engine/docs/PROTOCOL.md mm-engine/docs/BENCHMARK.md \
                mm-engine/docs/OPTIMIZATION.md mm-engine/CMakeLists.txt mm-engine/Makefile \
                mm-engine/docker/Dockerfile mm-engine/scripts/demo.sh; do
  tar -tzf "$OUT" | grep -qx "$required" || {
    echo "FATAL: the archive is missing $required" >&2
    exit 1
  }
done
echo "    deliverables verified: README, PROTOCOL, BENCHMARK, OPTIMIZATION, build files, demo"

if [[ "${MM_SKIP_BUILD:-0}" == "1" ]]; then
  echo "package: archive written (STANDALONE BUILD SKIPPED — this is not a verified archive)"
  exit 0
fi

command -v docker > /dev/null 2>&1 || {
  echo "FATAL: docker is required to prove the archive builds standalone." >&2
  echo "  Set MM_SKIP_BUILD=1 to write an UNVERIFIED archive." >&2
  exit 2
}

# Extracted somewhere with no relationship to this checkout: no .git, no build dir, no .venv, no
# stale CMake cache pointing at absolute paths from this machine.
STAGE="$(mktemp -d -t mm_pkg.XXXXXX)"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT
tar -xzf "$OUT" -C "$STAGE"
echo "--- building the extracted tree standalone in $STAGE/mm-engine"

docker build --platform linux/arm64 -f "$STAGE/mm-engine/docker/Dockerfile" \
  -t mm-engine-package-check "$STAGE/mm-engine" > "$STAGE/build.log" 2>&1 || {
  echo "FATAL: the extracted archive does NOT build. Last 30 lines:" >&2
  tail -30 "$STAGE/build.log" >&2
  exit 1
}

# Building is necessary but not sufficient: the image must also run the engine it just built.
docker run --rm mm-engine-package-check ./build/rel/mm_engine --version > "$STAGE/version.log" 2>&1 || {
  echo "FATAL: the archive built but its engine does not run:" >&2
  cat "$STAGE/version.log" >&2
  exit 1
}
sed 's/^/    /' "$STAGE/version.log"

echo "package: ALL GREEN — $OUT builds and runs standalone from a fresh extraction"
