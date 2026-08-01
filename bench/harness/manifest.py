"""What was true about the machine when a run happened.

A latency number without its environment is not a measurement — it cannot be reproduced,
compared, or defended. Every field here is something that CHANGES THE ANSWER, so an omission
makes a run unreproducible rather than merely under-documented.

The awkward fields are the honest ones. CPU frequency scaling moves p99 more than most
optimizations do, and inside a container VM the governor is usually not readable at all — so
`cpu_governor` says *"not controllable inside the container VM — uncontrolled"* in words rather
than being absent. A missing key reads as "controlled"; it almost never was.

`qualifies_as_primary` is the gate between "a run happened" and "a run can appear in a primary
table". It is deliberately conjunctive and deliberately strict: rejects mean the message mix
tripped an engine limit, so the pattern measured is not the pattern intended; saturation
means the recorder's streams are truncated; a gap means the process stopped.
"""

import hashlib
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any

__all__ = ["capture", "cpu_seconds", "qualifies_as_primary"]

# Stated in words when the value cannot be read, rather than omitted.
_GOVERNOR_UNKNOWN = "not readable on this host — uncontrolled (see BENCHMARK.md methodology)"
_LINUX_GOVERNOR = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")


def _run(argv: list[str]) -> str:
    """A short command's stdout, or "" if it is not available. Never raises: a manifest that
    fails to capture is worse than one with an honest blank, because it takes the run with it."""
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=10, check=False)
    except OSError, subprocess.SubprocessError:
        return ""
    return out.stdout.strip()


def _cpu_model() -> str:
    if sys.platform == "darwin":
        return _run(["sysctl", "-n", "machdep.cpu.brand_string"]) or platform.processor()
    model = ""
    arm: dict[str, str] = {}
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith(("model name", "Model")):
                model = line.split(":", 1)[1].strip()
                break
            # AARCH64 HAS NO `model name`. Its /proc/cpuinfo identifies the core by numeric
            # implementer/part instead, so the x86 lookup above falls through and the field read
            # "unknown" in all 22 manifests of the first matrix — on the arm64 host that produced
            # the only authoritative numbers. A CPU identity is not a nice-to-have in a latency
            # report: it is what makes two runs comparable at all.
            if line.startswith("CPU "):
                key, _, value = line.partition(":")
                arm[key.strip()] = value.strip()
    if not model and arm.get("CPU part"):
        # Reported as the raw pairs rather than decoded through a part-number table: a wrong
        # marketing name is worse than an exact identifier a reader can look up.
        model = " ".join(
            f"{key}={arm[key]}"
            for key in ("CPU implementer", "CPU architecture", "CPU variant", "CPU part")
            if key in arm
        )
    return model or platform.processor() or "unknown"


def _cpu_governor() -> str:
    """The governor, or an explicit statement that it could not be read.

    macOS exposes no equivalent and a container VM generally does not surface the host's, so the
    common answer here is the sentence rather than a value — which is exactly the point: the
    reader needs to know the frequency was uncontrolled, not be left to assume it was not.
    """
    if _LINUX_GOVERNOR.is_file():
        try:
            return _LINUX_GOVERNOR.read_text().strip() or _GOVERNOR_UNKNOWN
        except OSError:
            return _GOVERNOR_UNKNOWN
    return _GOVERNOR_UNKNOWN


def _affinity() -> str:
    """Which CPUs the process may run on. macOS has no API for pinning a process, so the honest
    answer there is "none available", not an empty string that reads like "not set"."""
    getter = getattr(os, "sched_getaffinity", None)
    if getter is None:
        return "no affinity API on this platform (macOS)"
    try:
        return ",".join(str(c) for c in sorted(getter(0)))
    except OSError:  # pragma: no cover - defensive
        return "unavailable"


def _packages() -> list[str]:
    """The installed set for THIS arm. The naive and tuned arms differ in their dependencies, so
    which versions were present is part of what produced the number."""
    frozen = _run([sys.executable, "-m", "pip", "freeze", "--disable-pip-version-check"])
    return frozen.splitlines() if frozen else []


def _engine_version(engine: Path | None) -> str:
    """The engine's own `--version`, which CMake stamps with the compiler and flags. Read from
    the binary rather than assumed, so a stale build cannot be reported as a fresh one."""
    if engine is None or not engine.is_file():
        return ""
    return _run([str(engine), "--version"])


def cpu_seconds(pid: int) -> float | None:
    """A process's total CPU time (user + system) in seconds, or None where it cannot be read.

    E-5 asks the saturation probes to answer "do queues build", and its stated evidence is CPU%,
    send-lag and queue depth. The last two were already recorded — send lag in the `.lag.i64`
    series and depth in the telemetry snapshots' `outbox_depth_hw` — but CPU was not captured at
    all, which left the saturation question only partly answerable from the artifacts: a client
    that is 40% busy and one that is pinned at 100% produce the same lag curve until the moment
    the second one falls off a cliff.

    Reads `/proc/<pid>/stat` fields 14 and 15 (utime, stime) in clock ticks. Returns None on hosts
    without procfs — the authoritative runs happen in the Linux container, and a Darwin dev run
    should say "not measured" rather than substitute something else.
    """
    stat = Path(f"/proc/{pid}/stat")
    try:
        # The comm field can itself contain spaces and parentheses, so split after the LAST ')'.
        fields = stat.read_text().rpartition(")")[2].split()
        ticks = float(fields[11]) + float(fields[12])  # utime, stime (14, 15 minus the first 2)
    except OSError, IndexError, ValueError:
        return None
    hz = os.sysconf("SC_CLK_TCK")
    return ticks / hz if hz > 0 else None


def _engine_sha256(engine: Path | None) -> str:
    """The measured binary's content hash — the one provenance field a reader can CHECK.

    `engine_version` narrows the build to a compiler and a flag set, but every rebuild from the
    same source prints the same string, so it cannot distinguish the binary that produced these
    numbers from a different one built the same way. A content hash can: `shasum -a 256` on the
    artifact either matches or it does not.

    This exists because a whole 22-run matrix previously taken inside an image built one commit
    behind the tree, and nothing in the resulting manifests recorded which binary ran — the
    mismatch had to be reconstructed afterwards from image labels and commit archaeology. The
    sibling `image_digest` field is for the container the run happened in; this one is for the
    executable, and it is the narrower, more useful of the two.
    """
    if engine is None or not engine.is_file():
        return ""
    digest = hashlib.sha256()
    with engine.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture(
    *,
    stack: str,
    mode: str,
    rate_hz: float,
    warmup: int,
    samples: int,
    interval_ms: int,
    url: str,
    repeat_tag: str,
    rejects: int,
    gaps: bool,
    engine: Path | None = None,
    engine_codec: str = "",
    image_digest: str = "",
    client_cpu_pct: float | None = None,
    engine_cpu_pct: float | None = None,
    wall_s: float | None = None,
) -> dict[str, Any]:
    """Everything a reader needs to reproduce this run, as a JSON-serialisable dict.

    `rejects` and `gaps` are passed IN rather than discovered here: only the client that ran the
    mix knows how many of its commands the engine refused and whether its own send stream
    stalled. Both are inputs to `qualifies_as_primary`, and both are recorded here so the verdict
    can be recomputed later from the artifact alone.

    The THIRD input — the engine recorder's `saturated` count — is deliberately absent: it lives
    in the `--bench-out` dump, which the engine writes during shutdown, i.e. after this client has
    already exited. That is why no single tool can stamp the final verdict, and why `summarize`
    (the one place holding manifest + dump + series together) is what applies the gate.
    """
    return {
        # --- machine -----------------------------------------------------------------
        "cpu_model": _cpu_model(),
        "cores": os.cpu_count() or 0,
        "os": f"{platform.system()} {platform.release()}",
        "kernel": platform.version(),
        "machine": platform.machine(),
        "cpu_governor": _cpu_governor(),
        "affinity": _affinity(),
        "image_digest": image_digest,
        # --- toolchain ---------------------------------------------------------------
        "python_version": platform.python_version(),
        "python_build": " ".join(platform.python_build()),
        "python_implementation": platform.python_implementation(),
        "engine_version": _engine_version(engine),
        "engine_sha256": _engine_sha256(engine),
        "packages": _packages(),
        # --- what was run ------------------------------------------------------------
        # BOTH LEGS of the arm are named. `stack` alone described only the Python side, so a
        # matrix in which the engine's codec never moved off `tuned` produced 22 manifests that
        # were all individually truthful and collectively concealed a wrong baseline. The A/B swap
        # spans four components across two languages; the artifact has to say which arm each leg
        # was on, or the comparison is unfalsifiable from its own evidence.
        "stack": stack,
        "engine_codec": engine_codec,
        "mode": mode,
        "rate_hz": rate_hz,
        "warmup": warmup,
        "samples": samples,
        "interval_ms": interval_ms,
        "url": url,
        "repeat_tag": repeat_tag,
        # --- transport configuration that moves the number ---------------------------
        # Stated rather than measured: both arms disable Nagle and refuse compression by
        # construction, and a manifest that guessed would be worth less than none.
        "tcp_nodelay": True,
        "compression": "disabled (both arms pass compression=None / negotiate no extensions)",
        "per_message_logging": False,
        # --- run quality -------------------------------------------------------------
        # Two of the three `qualifies_as_primary` inputs. The third (`saturated`) is only in the
        # engine dump; `summarize` joins them and states the verdict.
        "rejects": rejects,
        "gaps": gaps,
        # E-5's saturation evidence. `None` means "not measured on this host" (no procfs) rather
        # than "idle" — the distinction the cpu_governor field exists to make for power settings.
        "client_cpu_pct": client_cpu_pct,
        "engine_cpu_pct": engine_cpu_pct,
        "wall_s": wall_s,
    }


def qualifies_as_primary(*, rejects: int, saturated: int, gaps: bool) -> bool:
    """Whether this run may appear in a PRIMARY benchmark table.

    All three disqualifiers describe a run that measured something other than what it set out to:
      * `rejects` — the mix tripped an engine limit, so the pattern is not the intended one;
      * `saturated` — the recorder hit capacity, so its streams are truncated;
      * `gaps` — the process stopped mid-run, so the tail is a stall, not a latency.

    A disqualified run is still publishable as a labelled secondary probe. It is not deleted;
    it is just not allowed to answer a percentile question.
    """
    return rejects == 0 and saturated == 0 and not gaps
