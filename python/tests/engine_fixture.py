"""Owns a real `mm_engine` subprocess for the integration tests.

Everything here exists to make an integration suite that spawns a real binary safe to run
repeatedly, in parallel, and after an abort — the three ways a subprocess test becomes worse
than no test at all.

PORT: `--port 0` and read the bound port back off the engine's startup line. The design's
bind-to-0 probe would work, but it has a window — the probe closes the socket before the
engine opens it, so two runs starting together can pick the same port and one dies with
EADDRINUSE. Letting the engine bind and report closes it: no probe, no window.

LIFECYCLE: `terminate()` -> bounded wait -> `kill()`, in a `finally`. An orphaned engine
holds its port and its telemetry file, so the NEXT run fails for a reason that has nothing to
do with the code under test — which is the failure mode that makes people stop trusting a
suite.
"""

import os
import re
import shutil
import signal
import subprocess
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
DEFAULT_ENGINE = REPO / "build" / "rel" / "mm_engine"
DEMO_FEED = REPO / "bench" / "scenarios" / "demo.feed"

# The engine's startup line is a contract main.cpp prints and this fixture parses; the
# integration suite is its only automated reader, so a format change breaks here first.
_LISTENING = re.compile(r"^mm_engine \S+ listening port=(\d+) codec=(\S+)")

STARTUP_TIMEOUT_S = 10.0
SHUTDOWN_GRACE_S = 5.0


@dataclass
class Engine:
    """A running engine, its URL, and the artifacts it will leave behind."""

    proc: subprocess.Popen[str]
    port: int
    telemetry: Path

    @property
    def url(self) -> str:
        return f"ws://127.0.0.1:{self.port}"

    def wait_for_exit(self, timeout: float = SHUTDOWN_GRACE_S) -> int:
        return self.proc.wait(timeout=timeout)


def engine_path() -> Path:
    """`MM_ENGINE` overrides, so the suite can be pointed at a debug or sanitizer build."""
    return Path(os.environ.get("MM_ENGINE", str(DEFAULT_ENGINE)))


def require_engine() -> Path:
    path = engine_path()
    if not path.is_file():
        pytest.skip(f"engine binary not built at {path} (set MM_ENGINE to override)")
    return path


def start_engine(tmp_path: Path, *, feed: Path = DEMO_FEED, **flags: object) -> Engine:
    """Spawn an engine on an ephemeral port and block until it says it is listening.

    Raises rather than skipping if it fails to start: a binary that exists but will not run
    is a defect, and skipping would hide it behind a green suite.
    """
    binary = require_engine()
    telemetry = tmp_path / "engine.jsonl"
    argv = [
        str(binary),
        "--feed",
        str(feed),
        "--port",
        "0",
        "--telemetry-out",
        str(telemetry),
    ]
    for key, value in flags.items():
        flag = f"--{key.replace('_', '-')}"
        # BOOLEANS ARE BARE FLAGS. `main.cpp` takes `--loop` and `--telemetry-verbose` with no
        # value, so appending one produced `--loop True` and the engine died with
        # `unknown argument: True`. Every call site today happens to pass only value-taking flags,
        # so the suite is green and the disagreement is latent — which is exactly the kind of
        # helper-vs-binary drift that bites the first time someone writes `loop=True` in a test
        # and reads the failure as a bug in the engine.
        if isinstance(value, bool):
            if value:
                argv.append(flag)
            continue
        argv += [flag, str(value)]

    proc = subprocess.Popen(
        argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1
    )
    deadline = time.monotonic() + STARTUP_TIMEOUT_S
    assert proc.stdout is not None
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        match = _LISTENING.match(line)
        if match:
            return Engine(proc=proc, port=int(match.group(1)), telemetry=telemetry)
    _hard_stop(proc)
    stderr = proc.stderr.read() if proc.stderr else ""
    msg = f"engine did not report a listening port within {STARTUP_TIMEOUT_S}s: {stderr[:400]}"
    raise RuntimeError(msg)


def _hard_stop(proc: subprocess.Popen[str]) -> None:
    """SIGTERM, then a bounded wait, then SIGKILL. Never leaves the process running."""
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=SHUTDOWN_GRACE_S)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=SHUTDOWN_GRACE_S)


@pytest.fixture
def live_engine(tmp_path: Path) -> Iterator[object]:
    """Hands back a starter; every engine it starts is stopped on the way out.

    `live_engine`, not `engine`: test_ws_hardening.py already owns an `engine` fixture that
    serves a SCRIPTED FAKE. A module-level fixture shadows a conftest one, so the two would
    coexist quietly today — and the day someone deleted the local one, a suite of unit tests
    would silently start spawning real subprocesses instead of failing.
    """
    started: list[Engine] = []

    def start(**flags: object) -> Engine:
        eng = start_engine(tmp_path, feed=DEMO_FEED, **flags)
        started.append(eng)
        return eng

    try:
        yield start
    finally:
        for eng in started:
            _hard_stop(eng.proc)


def drain(eng: Engine) -> tuple[str, str]:
    """Whatever the engine said. stderr is asserted EMPTY by the integration tests — a
    sanitizer build writes its findings there, so silence is the evidence."""
    out = eng.proc.stdout.read() if eng.proc.stdout else ""
    err = eng.proc.stderr.read() if eng.proc.stderr else ""
    return out, err


def stop_gracefully(eng: Engine) -> int:
    """SIGINT, the path an operator takes and the one the engine's signal handling is for."""
    eng.proc.send_signal(signal.SIGINT)
    return eng.wait_for_exit()


def have_engine() -> bool:
    return engine_path().is_file() and DEMO_FEED.is_file() and shutil.which("cmake") is not None
