#!/usr/bin/env python3
"""Mechanically verify file-path and cross-reference mentions inside comments and docs.

Catches the "Class B" gate-finding pattern observed on Tasks 4 and 6: a comment or doc
names a file, or a PENDING_AMENDMENTS item, that no longer exists at the claimed location.
That class cost a full reviewer round each time because it was found incidentally during
open-ended review instead of exhaustively up front. This script does the exhaustive part
mechanically, in seconds, so it never needs to occupy a paid reviewer round at all.

Scope, deliberately narrow (v1 over-fired at 249 hits before this restriction — nearly all of
them the PLAN legitimately naming files for tasks not yet built, e.g. Task 7's server.cpp).
File-existence checking is restricted to CODE comments (cpp/, python/mmclient/, python/tests/,
bench/) referencing other CODE files — the shape both real Task-4 and Task-6 findings had. Docs
and the plan are forward-looking by design and are excluded from file-existence checking.
The PENDING_AMENDMENTS "(letter)number" item cross-reference is self-referential bookkeeping
with no forward-reference ambiguity, so it stays checked everywhere, including in docs/.

Exit 1 on any FAIL (a reference that does not resolve). WARN never fails the run — it flags
symbol mentions this script cannot verify with confidence, for a human/reviewer to judge.
"""
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

TEXT_EXTS = {".cpp", ".hpp", ".h", ".py", ".md", ".toml"}
# ORDER MATTERS in this alternation: Python's `re` takes the FIRST branch that matches, not the
# longest, so `json` ahead of `jsonl` truncates every `.jsonl` reference to `.json` — which then
# resolves to no tracked file and is reported as drift. Observed: comments discussing the
# telemetry artifact `tele.jsonl` were flagged as references to a non-existent `tele.json`.
# Longer extensions therefore come first.
PATH_EXTS = r"jsonl|cpp|hpp|h|py|md|toml|txt|yml|yaml|cmake|feed|json"

# A path-like token inside a comment/doc line, optionally followed by :LINE or :SYMBOL.
PATH_RE = re.compile(
    rf"(?<![\w/.])([\w][\w./-]*\.(?:{PATH_EXTS}))(?::(\d+(?:-\d+)?|[A-Za-z_]\w*))?"
)

# PENDING_AMENDMENTS' own item convention, e.g. "(p)11", "(r)6", cited elsewhere as a
# cross-reference. Definition side is NOT this token appearing literally: each letter is a
# "## (X) ..." section heading, and the numbers are plain markdown ordered-list items (1., 2.,
# ...) nested inside it — "(p)9" means "list item 9 within section (p)".
ITEM_REF_RE = re.compile(r"\((\w)\)(\d+)")
SECTION_RE = re.compile(r"^## \((\w)\)")
TOP_LIST_ITEM_RE = re.compile(r"^(\d+)\. ")

# Common source roots to try when a bare filename doesn't resolve as-is.
SEARCH_PREFIXES = [
    "", "cpp/", "cpp/include/mm/", "cpp/src/", "cpp/tests/", "python/mmclient/",
    "python/tests/", "docs/", "docs/decisions/", "docs/superpowers/plans/",
    "bench/probes/", "bench/scenarios/",
]

# Deliverables the plan's packaging bullet names for LATER tasks (13/14/15) — legitimately
# not yet tracked. Forward-referencing one from earlier code is normal, not drift.
FORWARD_DELIVERABLES = {"PROTOCOL.md", "BENCHMARK.md", "OPTIMIZATION.md"}

# Files the plan named as ONE translation unit and that shipped SPLIT under the repo's 500-line
# cap. A comment naming the predecessor is explaining the split, not referring to a file it
# expects to find — `server_test_support.hpp` opens by listing which of the seven TUs took which
# behavioural seam. Held as an explicit set rather than inferred: "the surrounding prose says it
# was split" is not something a mechanical check can read, and guessing would either miss real
# drift or excuse it.
SPLIT_PREDECESSORS = {"test_server_integration.cpp"}

# Names a TEST CREATES AT RUNTIME, quoted in comments that explain a behaviour about them. The
# case-variant pair below is the subject of a real finding — on a case-insensitive volume
# `weakly_canonical` does not case-fold while the trailing component is missing, so the engine's
# same-file guard was blind exactly where it was armed — and the only way to explain that is to
# write both spellings down. They are arguments a test passes, not paths anything resolves.
#
# Explicit, like the two sets above, because no mechanical rule separates "a filename this comment
# is ABOUT" from "a filename this comment expects to find". A regex over surrounding prose would
# either excuse real drift or keep flagging this.
RUNTIME_ARTIFACTS = {"TELE.jsonl", "tele.jsonl"}

# Third-party headers reach this build by TWO routes, and a rule covering only one of them
# reports the other's headers as drift:
#
#   * FETCHED — Catch2, glaze and nlohmann_json arrive through FetchContent and land under
#     build/*/_deps once configured. Never git-tracked.
#   * SYSTEM — Boost arrives through `find_package(Boost 1.83 REQUIRED CONFIG)`, so it lives in
#     an include root outside the tree entirely (/opt/homebrew/include on the macOS host,
#     /usr/include in the ubuntu:26.04 image).
#
# Only the fetched route was covered, so a comment citing a Boost INTERNAL header —
# `test_server_flow.cpp` explains a Beast timeout behaviour by naming `stream_impl.hpp` — was
# reported as a broken reference. It surfaced the first time this check ran inside a Makefile
# gate rather than by hand.
_SYSTEM_INCLUDE_ROOTS = (
    Path("/opt/homebrew/include"),  # macOS, Homebrew
    Path("/usr/include"),           # Debian/Ubuntu, incl. the authoritative image
    Path("/usr/local/include"),
)


def is_vendored(basename):
    """Whether `basename` is a third-party header rather than one of ours.

    Searched rather than allowlisted: a hardcoded list of third-party filenames rots silently the
    moment a dependency is upgraded, and the failure mode is this same false positive again.
    """
    for hit in (REPO / "build").glob(f"*/_deps/*/**/{basename}"):
        if hit.is_file():
            return True
    for root in _SYSTEM_INCLUDE_ROOTS:
        # Bounded to the libraries this build actually declares, so the check cannot be satisfied
        # by an unrelated header that happens to share a name with one of ours.
        for library in ("boost",):
            if not (root / library).is_dir():
                continue
            for hit in (root / library).rglob(basename):
                if hit.is_file():
                    return True
    return False


CODE_DIRS = ("cpp/", "python/mmclient/", "python/tests/", "bench/")


def require_git_repo():
    """Refuse, with a diagnosis, when there is no git working tree.

    This check's CONTRACT is the tracked set — `git ls-files` — because an earlier filesystem-walk
    version over-fired on forward-looking plan paths that are referenced before they exist. That
    makes the check host-only, and the honest response to running it without a repository is to
    say so.

    Neither of the two tempting alternatives is acceptable. A silent skip reports a check that did
    not run as a check that passed, which is the failure mode this whole file exists to prevent. A
    raw traceback is what it used to do: inside the authoritative container `git ls-files` exits
    128 because `.dockerignore` excludes `.git`, and the resulting CalledProcessError was invisible
    behind four other targets' green output. `git` the binary IS installed there — what is missing
    is a repository, and the message has to say which.

    Exit 2, not 1: 2 is "the environment cannot support this check", 1 is reserved for a check that
    ran and FOUND broken references. A caller that conflates them cannot tell a misconfiguration
    from a real defect.
    """
    probe = subprocess.run(
        ["git", "-C", str(REPO), "rev-parse", "--is-inside-work-tree"],
        capture_output=True,
        text=True,
        check=False,
    )
    if probe.returncode != 0 or probe.stdout.strip() != "true":
        print(
            f"FATAL: docs-check requires a git working tree ({REPO} is not one).\n"
            "  host:      run from the clone root — make docs-check\n"
            "  container: .git is excluded by .dockerignore, so this target is HOST-ONLY and is\n"
            "             deliberately not a member of container-gate\n"
            "  This is refused rather than skipped: a check that did not run must never report as\n"
            "  a check that passed.",
            file=sys.stderr,
        )
        sys.exit(2)


def tracked_files():
    require_git_repo()
    out = subprocess.run(["git", "-C", str(REPO), "ls-files"], capture_output=True, text=True, check=True)
    for rel in out.stdout.splitlines():
        p = REPO / rel
        if p.suffix in TEXT_EXTS and p.is_file():
            yield rel, p


def is_code_file(rel):
    return rel.startswith(CODE_DIRS)


def is_comment_or_prose(line, suffix):
    s = line.strip()
    if suffix == ".md":
        return True
    if suffix == ".py":
        return s.startswith("#") or '"""' in line or "'''" in line
    return s.startswith("//") or s.startswith("*") or s.startswith("/*")


def resolve(token):
    base = token.rsplit("/", 1)[-1]
    if base in FORWARD_DELIVERABLES or base in SPLIT_PREDECESSORS or base in RUNTIME_ARTIFACTS:
        return True
    for prefix in SEARCH_PREFIXES:
        if (REPO / (prefix + token)).is_file():
            return True
    # also allow a match anywhere in the tree by basename, for a moved-not-renamed file
    hits = subprocess.run(
        ["git", "-C", str(REPO), "ls-files", f"*{base}"], capture_output=True, text=True
    ).stdout.split()
    if hits:
        return True
    return is_vendored(base)


def pending_amendment_items():
    """(letter, number) pairs that actually exist: number N is the Nth top-level ordered-list
    item found inside section '## (letter) ...', up to the next '## (' section boundary."""
    f = REPO / "docs/PENDING_AMENDMENTS.md"
    if not f.is_file():
        return set()
    defined = set()
    current_letter = None
    for line in f.read_text().splitlines():
        sec = SECTION_RE.match(line)
        if sec:
            current_letter = sec.group(1)
            continue
        item = TOP_LIST_ITEM_RE.match(line)
        if item and current_letter is not None:
            defined.add((current_letter, item.group(1)))
    return defined


def main():
    fails, warns = [], []
    defined_items = pending_amendment_items()

    for rel, path in tracked_files():
        suffix = path.suffix
        try:
            lines = path.read_text(errors="replace").splitlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, 1):
            if not is_comment_or_prose(line, suffix):
                continue
            if is_code_file(rel):
                for m in PATH_RE.finditer(line):
                    token = m.group(1)
                    if token == Path(rel).name:
                        continue  # a file naming itself is not a cross-reference
                    if not resolve(token):
                        fails.append(f"{rel}:{lineno}: references '{token}' — no file by that path or basename is tracked")
            if "PENDING_AMENDMENTS" not in rel:
                for m in ITEM_REF_RE.finditer(line):
                    key = (m.group(1), m.group(2))
                    if key not in defined_items:
                        fails.append(f"{rel}:{lineno}: cross-reference '({key[0]}){key[1]}' has no matching item in docs/PENDING_AMENDMENTS.md")

    if fails:
        print(f"FAIL — {len(fails)} broken reference(s):")
        for f in fails:
            print(f"  {f}")
    if warns:
        print(f"WARN — {len(warns)} unverified mention(s) (informational, does not block):")
        for w in warns:
            print(f"  {w}")
    if not fails and not warns:
        print("OK — no broken file-path or cross-reference mentions found.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
