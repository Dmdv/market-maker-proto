#!/usr/bin/env python3
"""The mm_engine binary's own test fixture — the only thing in the repository that EXECUTES it.

Everything below cpp/src/main.cpp used to be unreachable from the suite: CMakeLists builds
mm_engine and nothing ran it, so the CLI surface (flag names, ranges, trailing-character
rejection), the ops contract (one stderr line + exit 2 on any startup failure), the
startup/shutdown line formats and the admission defaults were pinned by nothing at all.
Plan makes that flag surface a downstream contract ("confirm CLI flags used
here exist in main.cpp"), so it needs a fixture, not a reviewer's memory.

Run by ctest as `mm_engine_cli`; the engine's path is argv[1] (a CMake generator expression,
so the sanitizer trees test their own binaries). Every artifact — feeds, telemetry, bench
dumps — is written into a private temporary directory, never the repository or the CWD.

WHAT EACH ARM WOULD CATCH is stated at the arm, because an arm whose mutant nobody can name
is an arm that pins nothing. The arms that need a live listener drive RAW sockets: the HTTP
upgrade handshake is enough to open and hold a session, and a WebSocket library would be a
dependency this fixture does not otherwise need.
"""

from __future__ import annotations

import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

# The feed every long-running arm uses: one book, then a halt long enough that the engine is
# still up when the arm is finished with it. Every such engine is stopped by SIGTERM, which
# is also what makes the shutdown-line arm a test of the SIGNAL path rather than of feed
# exhaustion.
LONG_FEED = '{"set":[100,10,110,20]}\n{"halt_ms":600000}\n{"end":true}\n'
# ...and the feed for every arm that expects the engine NOT to start. It exhausts in a
# couple of hundred milliseconds, so an engine that wrongly accepts the input under test
# fails the arm on its exit code and its stray startup line instead of parking the fixture
# on `run`'s timeout — the timeout there is the belt, this is the braces.
SHORT_FEED = '{"set":[100,10,110,20]}\n{"end":true}\n'

STARTUP_RE = re.compile(
    r"^mm_engine \d+\.\d+\.\d+ listening port=(\d+) codec=(naive|tuned) "
    r"instrument=MOCKUSDT feed=(.+)$"
)
SHUTDOWN_RE = re.compile(
    r"^mm_engine shutdown orders=(\d+) fills=(\d+) conflated=(\d+) telemetry_ok=([01])$"
)

UPGRADE_REQUEST = (
    b"GET / HTTP/1.1\r\n"
    b"Host: 127.0.0.1\r\n"
    b"Upgrade: websocket\r\n"
    b"Connection: Upgrade\r\n"
    b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    b"Sec-WebSocket-Version: 13\r\n"
    b"Sec-WebSocket-Protocol: mm.v1\r\n"
    b"\r\n"
)

failures: list[str] = []
# Every engine is spawned HERE, and it is the private temporary directory main() creates.
# Config::telemetry_out defaults to a bare `telemetry.jsonl`, so an engine that starts when
# an arm expected it to refuse would otherwise drop that file into whatever directory ctest
# ran from — the repository root. A fixture whose FAILURE mode is littering the source tree
# is a fixture that has to be cleaned up after by hand; the arms below also pass
# --telemetry-out wherever they can, and this is what covers the ones that deliberately
# cannot (the arm that refuses an EMPTY --telemetry-out, and every arm whose engine is not
# supposed to reach an open at all).
WORKDIR = "."


def check(ok: bool, what: str) -> bool:
    if not ok:
        failures.append(what)
        print(f"FAILED: {what}", file=sys.stderr)
    return ok


class Engine:
    """A spawned mm_engine, killed by the context manager whatever the arm does."""

    def __init__(self, cmd: list[str]) -> None:
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=WORKDIR
        )
        self.startup = self.proc.stdout.readline().rstrip("\n")
        match = STARTUP_RE.match(self.startup)
        self.port = int(match.group(1)) if match else None
        self.codec = match.group(2) if match else None

    def __enter__(self) -> "Engine":
        return self

    def __exit__(self, *_exc: object) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.communicate()

    def terminate(self) -> tuple[int, str, str]:
        """SIGTERM, then the remaining stdout/stderr and the exit code."""
        self.proc.send_signal(signal.SIGTERM)
        out, err = self.proc.communicate(timeout=60)
        return self.proc.returncode, out, err


def upgrade(port: int, hold: list[socket.socket] | None = None) -> str:
    """One HTTP upgrade; returns the status line. `hold` keeps the connection open."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.sendall(UPGRADE_REQUEST)
    status = sock.recv(256).split(b"\r\n")[0].decode("ascii", "replace")
    if hold is None:
        sock.close()
    else:
        hold.append(sock)
    return status


def run(engine: str, *args: str, timeout: float = 30) -> subprocess.CompletedProcess | None:
    """The engine, run to completion. None — and a recorded failure — when it does not
    exit, because "it kept running" is a VERDICT here, not an infrastructure problem: every
    caller below passes input the engine must refuse, so an engine that starts on it has
    already failed the arm. Reporting that as a hang (and then as a TimeoutExpired
    traceback out of the fixture) hid which arm was wrong and cost the whole timeout;
    measured against a mutant that drops parse_int's trailing-character check, this is the
    difference between a named failure in 30 ms and an unnamed one in 60 s."""
    try:
        return subprocess.run(
            [engine, *args], capture_output=True, text=True, timeout=timeout, cwd=WORKDIR
        )
    except subprocess.TimeoutExpired:
        check(
            False,
            f"`{' '.join(args) or '(no arguments)'}`: the engine was still running after "
            f"{timeout}s — it STARTED on input it must refuse",
        )
        return None


def host_address() -> str | None:
    """A non-loopback IPv4 this host owns, or None. A UDP connect to a TEST-NET address
    routes nothing and sends nothing; it only asks the kernel which local address it would
    use. Returns None where there is no such address (an isolated container), which turns
    the default-bind arm into a printed skip rather than a false failure."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("192.0.2.1", 9))
        address = sock.getsockname()[0]
    except OSError:
        return None
    finally:
        sock.close()
    return None if address.startswith("127.") else address


# --------------------------------------------------------------------------- arms


def arm_startup_failures(engine: str, tmp: str) -> None:
    """every startup failure is ONE stderr line plus exit 2 — and nothing on stdout,
    because a half-written startup line is what an operator's log scraper would trip over.
    Kills: a parse helper that clamps instead of refusing (--hwm 0 would start at 1), a
    stoll that ignores trailing characters (--interval-ms 12x would run at 12), an unknown
    flag silently skipped, and any of these degraded to exit 1 or exit 0."""
    good = os.path.join(tmp, "ok.feed")
    with open(good, "w", encoding="utf-8") as handle:
        handle.write(SHORT_FEED)

    port = ["--port", "0"]  # ephemeral: parallel ctest processes must never contend
    cases = [
        ([*port, "--feed", os.path.join(tmp, "no-such.feed")], "cannot open feed file"),
        ([*port, "--feed", good, "--hwm", "0"], "out of range"),
        ([*port, "--feed", good, "--interval-ms", "12x"], "not an integer"),
        (["--feed", good, "--port", "99999"], "out of range"),
        ([*port, "--feed", good, "--codec", "bogus"], "expected naive|tuned"),
        ([*port, "--feed", good, "--max-sessions", "0"], "out of range"),
        ([*port, "--feed", good, "--max-session-entries", "0"], "out of range"),
        ([*port, "--feed", good, "--upgrade-timeout-ms", "0"], "out of range"),
        ([*port, "--feed", good, "--bind", "not.an.ip"], "bind_address is not an IP address"),
        # TEST-NET-1 (RFC 5737): syntactically valid and assigned to no interface, so the
        # BIND fails where the parse does not. Without it --bind could be parsed, validated
        # and then dropped on the floor and every other arm here would still pass.
        ([*port, "--feed", good, "--bind", "192.0.2.1"], "bind"),
        ([*port, "--feed", good, "--telemetry-out", ""], "telemetry_out must name a path"),
        ([], "--feed is required"),
        # LAST, and with no value after it: the value lambda's own bound.
        ([*port, "--feed"], "missing value"),
        ([*port, "--feed", good, "--nonsense"], "unknown argument"),
    ]
    for args, expected in cases:
        result = run(engine, *args)
        label = " ".join(args) or "(no arguments)"
        if result is None:
            continue # `run` already recorded the verdict
        check(result.returncode == 2, f"startup failure `{label}`: exit {result.returncode} != 2")
        check(result.stdout == "", f"startup failure `{label}`: wrote stdout {result.stdout!r}")
        check(
            expected in result.stderr,
            f"startup failure `{label}`: stderr {result.stderr!r} lacks {expected!r}",
        )


def arm_case_variant_collision(engine: str, tmp: str) -> None:
    """The bench dump and the telemetry JSONL must never name one file: finalize() closes
    the JSONL and then truncates it with the binary dump, destroying the run's narration
    while telemetry_ok() still reads true. The lexical guard at construction cannot see a
    CASE-variant re-spelling, so on a case-insensitive volume the verdict comes from run()
    — AFTER the startup line, which is the half this arm pins. Kills: dropping the
    filesystem::equivalent check in run() (the run would then start and shred its own
    telemetry), and moving the verdict back to a lexical-only compare."""
    probe = os.path.join(tmp, "CaseProbe.tmp")
    with open(probe, "w", encoding="utf-8") as handle:
        handle.write("x")
    variant = os.path.join(tmp, "caseprobe.tmp")
    try:
        case_insensitive = os.path.samefile(probe, variant)
    except OSError:
        case_insensitive = False
    if not case_insensitive:
        print("skipped: the case-variant collision arm needs a case-insensitive volume")
        return

    feed = os.path.join(tmp, "collide.feed")
    with open(feed, "w", encoding="utf-8") as handle:
        handle.write(SHORT_FEED)
    result = run(
        engine,
        "--feed", feed, "--port", "0",
        "--bench-out", os.path.join(tmp, "Collide.Bin"),
        "--telemetry-out", os.path.join(tmp, "collide.bin"),
    )
    if result is None:
        return
    check(result.returncode == 2, f"case-variant collision: exit {result.returncode} != 2")
    check(
        "bench_out must not name telemetry_out" in result.stderr,
        f"case-variant collision: stderr {result.stderr!r} lacks the verdict",
    )
    check(
        STARTUP_RE.match(result.stdout.strip()) is not None,
        "case-variant collision: the verdict must come from run(), so the startup line "
        f"must already be on stdout; got {result.stdout!r}",
    )


def arm_lifecycle_lines(engine: str, tmp: str) -> None:
    """The two lines ops reads, and NOTHING per message between them . The
    shutdown line's telemetry_ok digit is the run's health verdict (T-16) and must read 1
    on a clean run. Kills: a shutdown line that loses a counter or prints telemetry_ok as
    true/false, a startup line that stops reporting the BOUND port (breaking every
    --port 0 harness), and --codec being parsed but not applied."""
    feed = os.path.join(tmp, "lifecycle.feed")
    with open(feed, "w", encoding="utf-8") as handle:
        handle.write(LONG_FEED)
    for requested, expected in (([], "tuned"), (["--codec", "naive"], "naive")):
        with Engine(
            [engine, "--feed", feed, "--port", "0",
             "--telemetry-out", os.path.join(tmp, f"life_{expected}.jsonl"), *requested]
        ) as eng:
            check(
                eng.port is not None,
                f"startup line does not match the ops contract: {eng.startup!r}",
            )
            check(eng.codec == expected, f"--codec {expected}: startup says {eng.codec!r}")
            if eng.port is None:
                return
            check(
                upgrade(eng.port).startswith("HTTP/1.1 101"),
                "the reported port does not accept an mm.v1 upgrade",
            )
            code, out, _err = eng.terminate()
            check(code == 0, f"SIGTERM shutdown: exit {code} != 0")
            lines = [line for line in out.splitlines() if line]
            check(
                len(lines) == 1 and SHUTDOWN_RE.match(lines[0]) is not None,
                f"shutdown output must be exactly the one shutdown line; got {lines!r}",
            )
            if lines and SHUTDOWN_RE.match(lines[0]):
                check(
                    SHUTDOWN_RE.match(lines[0]).group(4) == "1",
                    f"a clean run must report telemetry_ok=1; got {lines[0]!r}",
                )


def arm_verbose_forced_off(engine: str, tmp: str) -> None:
    """--telemetry-verbose under --bench-out is refused OUT LOUD: the
    per-message events would distort the very run being measured. Kills: silently unsetting
    the flag the operator typed — the run would then measure correctly and the operator
    would never learn why the events are missing — and, in the other direction, honouring
    it under --bench-out. The Server-side belt is separately covered; this is the CLI arm."""
    feed = os.path.join(tmp, "verbose.feed")
    with open(feed, "w", encoding="utf-8") as handle:
        handle.write(SHORT_FEED)
    telemetry = os.path.join(tmp, "verbose.jsonl")
    result = run(
        engine, "--feed", feed, "--port", "0", "--interval-ms", "1",
        "--telemetry-out", telemetry, "--bench-out", os.path.join(tmp, "verbose.bin"),
        "--telemetry-verbose",
    )
    if result is None:
        return
    check(result.returncode == 0, f"--telemetry-verbose --bench-out: exit {result.returncode}")
    check(
        "--telemetry-verbose is forced off under --bench-out" in result.stderr,
        f"the forced-off notice is missing from stderr: {result.stderr!r}",
    )
    # ...and the notice is not the only evidence: no per-message event may reach the file.
    with open(telemetry, encoding="utf-8") as handle:
        body = handle.read()
    check(
        '"event":"tob_out"' not in body and '"event":"cmd_in"' not in body,
        "per-message events reached the telemetry file of a --bench-out run",
    )


def arm_admission_bounds(engine: str, tmp: str) -> None:
    """--max-sessions, both its DEFAULT and the flag. The default is 4 and that figure is a
    memory budget, not a capacity plan (mm/server.hpp does the max_sessions x
    max_session_entries arithmetic), so it is worth pinning as a number. Kills: the default
    drifting back to a larger figure — the worst case is ~205 MB per session, so 64 would
    put ~13 GB within reach of one unauthenticated peer — and --max-sessions being parsed
    but never reaching the admission check."""
    feed = os.path.join(tmp, "admission.feed")
    with open(feed, "w", encoding="utf-8") as handle:
        handle.write(LONG_FEED)
    for flag, limit in (([], 4), (["--max-sessions", "2"], 2)):
        held: list[socket.socket] = []
        with Engine(
            [engine, "--feed", feed, "--port", "0",
             "--telemetry-out", os.path.join(tmp, f"adm_{limit}.jsonl"), *flag]
        ) as eng:
            if not check(eng.port is not None, f"admission arm {limit}: no startup line"):
                return
            try:
                accepted = [upgrade(eng.port, held) for _ in range(limit)]
                refused = upgrade(eng.port, held)
            finally:
                for sock in held:
                    sock.close()
            check(
                all(status.startswith("HTTP/1.1 101") for status in accepted),
                f"max_sessions={limit}: the first {limit} upgrades were {accepted}",
            )
            check(
                refused.startswith("HTTP/1.1 503"),
                f"max_sessions={limit}: upgrade {limit + 1} was {refused!r}, expected 503",
            )
            eng.terminate()


def arm_upgrade_timeout(engine: str, tmp: str) -> None:
    """--upgrade-timeout-ms bounds a peer that connects and then says NOTHING — the only
    thing that ever recycles an accept-tier admission slot such a peer is holding. Kills:
    the flag being parsed and not threaded into the pre-upgrade read (the default is 10 s,
    so a mute peer would still be there well past the bound asserted here)."""
    feed = os.path.join(tmp, "mute.feed")
    with open(feed, "w", encoding="utf-8") as handle:
        handle.write(LONG_FEED)
    with Engine(
        [engine, "--feed", feed, "--port", "0", "--upgrade-timeout-ms", "300",
         "--telemetry-out", os.path.join(tmp, "mute.jsonl")]
    ) as eng:
        if not check(eng.port is not None, "upgrade-timeout arm: no startup line"):
            return
        sock = socket.create_connection(("127.0.0.1", eng.port), timeout=10)
        started = time.monotonic()
        try:
            # Deliberately sends nothing. The bound is checked from ABOVE only: 300 ms is
            # the engine's, and the 3 s ceiling here is what separates it from the 10 s
            # default without making a loaded or emulated host's scheduling a failure.
            sock.settimeout(5)
            leftover = sock.recv(4096)
            elapsed = time.monotonic() - started
            check(
                leftover == b"" and elapsed < 3.0,
                f"a mute peer survived {elapsed:.3f}s and read {leftover!r}; "
                "--upgrade-timeout-ms 300 did not reach the pre-upgrade read",
            )
        except socket.timeout:
            check(False, "a mute peer was never reaped under --upgrade-timeout-ms 300")
        finally:
            sock.close()
        eng.terminate()


def arm_bind_reach(engine: str, tmp: str) -> None:
    """The listener's DEFAULT reach. This engine authenticates nothing, so loopback-only is
    a security decision and the default is the whole of it. Two-sided within ONE engine:
    the same port answers on 127.0.0.1 and refuses on this host's own non-loopback address.
    Kills: the default reverting to 0.0.0.0 — the non-loopback connect would then succeed.
    Deliberately does NOT start a 0.0.0.0 listener to prove the other direction: an arm
    that opens a port on every interface is a poor thing to run on a developer's machine,
    and --bind's reach into the bind is already pinned by the TEST-NET-1 arm above."""
    address = host_address()
    if address is None:
        print("skipped: the default-bind arm needs a non-loopback IPv4 on this host")
        return
    feed = os.path.join(tmp, "bind.feed")
    with open(feed, "w", encoding="utf-8") as handle:
        handle.write(LONG_FEED)
    with Engine(
        [engine, "--feed", feed, "--port", "0",
         "--telemetry-out", os.path.join(tmp, "bind.jsonl")]
    ) as eng:
        if not check(eng.port is not None, "bind arm: no startup line"):
            return
        check(
            upgrade(eng.port).startswith("HTTP/1.1 101"),
            "the default listener does not answer on 127.0.0.1",
        )
        try:
            socket.create_connection((address, eng.port), timeout=5).close()
            reachable = True
        except OSError:
            reachable = False
        check(
            not reachable,
            f"the default listener answered on {address}:{eng.port} — the default bind is "
            "127.0.0.1, and an engine that authenticates nothing must not be reachable off "
            "the host without an explicit --bind",
        )
        eng.terminate()


def arm_bench_dump_peak_sessions(engine: str, tmp: str) -> None:
    """--bench-out, and the sixth header word END TO END. The recorder's own unit case can
    only prove that word round-trips whatever the caller passed; the engine half — that the
    caller passes a WATERMARK and not a gauge — is invisible to it. Here three sessions are
    opened and then ALL CLOSED before the engine is stopped, so a gauge would dump 0 and
    only a watermark dumps 3. Kills: sourcing the word from sessions_.size() (or from
    Counters::sessions) at stop, which is exactly the reading that makes a 79x-understated
    m0->m0' distribution indistinguishable from a correct single-session one."""
    feed = os.path.join(tmp, "bench.feed")
    with open(feed, "w", encoding="utf-8") as handle:
        handle.write(LONG_FEED)
    dump = os.path.join(tmp, "peak.bin")
    held: list[socket.socket] = []
    with Engine(
        [engine, "--feed", feed, "--port", "0", "--bench-out", dump,
         "--telemetry-out", os.path.join(tmp, "bench.jsonl")]
    ) as eng:
        if not check(eng.port is not None, "bench arm: no startup line"):
            return
        statuses = [upgrade(eng.port, held) for _ in range(3)]
        check(
            all(status.startswith("HTTP/1.1 101") for status in statuses),
            f"bench arm: the three sessions were {statuses}",
        )
        for sock in held:
            sock.close()
        # The sessions are gone before the stop: the gauge reads 0 here, the watermark 3.
        time.sleep(0.5)
        code, _out, _err = eng.terminate()
        check(code == 0, f"bench arm: exit {code} != 0")

    if not check(os.path.exists(dump), "--bench-out wrote no dump"):
        return
    with open(dump, "rb") as handle:
        raw = handle.read(48)
    check(len(raw) == 48, f"--bench-out dump is {len(raw)} bytes, shorter than its header")
    if len(raw) == 48:
        header = struct.unpack("<6Q", raw)
        check(
            header[5] == 3,
            f"the dump's peak-session word is {header[5]}, expected 3 — the sessions were "
            "all closed before the stop, so anything but 3 means a gauge, not a watermark",
        )


def arm_version_stamp(engine: str, tmp: str) -> None:
    """`--version` prints the toolchain the benchmark benchmark manifest records.

    Pinned because the manifest READS these lines from the binary rather than re-deriving them —
    a harness that inferred the compiler from its own environment would happily describe a stale
    build as a fresh one. If the format drifts, the manifest silently records a blank field and
    every table it accompanies loses its provenance, which is a failure nobody notices until
    someone tries to reproduce a number.

    It also has to work with NO other argument: `--feed` is otherwise mandatory, and a reviewer
    running `mm_engine --version` on a fresh clone must get a version, not a usage error.
    """
    done = run(engine, "--version")
    if not check(done is not None, "--version returned"):
        return
    assert done is not None
    check(done.returncode == 0, f"--version exits 0 (got {done.returncode})")
    lines = done.stdout.splitlines()
    check(bool(lines) and lines[0].startswith("mm_engine "), f"first line names the binary: {lines[:1]}")
    for key in ("compiler:", "standard:", "build-type:", "cxx-flags:"):
        present = any(line.startswith(key) for line in lines)
        check(present, f"--version reports {key}")
    # The compiler line must carry an actual identifier, not an empty CMake variable: an
    # unset stamp would print `compiler: ` and read as "captured" in the manifest.
    compiler = next((line for line in lines if line.startswith("compiler:")), "compiler:")
    check(len(compiler.split(":", 1)[1].strip()) > 0, "the compiler stamp is not empty")

    # An unknown argument is still a usage error, and still exit 2 — adding --version must not
    # have turned the parser permissive.
    bad = run(engine, "--not-a-flag")
    check(bad is not None and bad.returncode == 2, "an unknown argument still exits 2")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: mm_engine_cli_test.py <path-to-mm_engine>", file=sys.stderr)
        return 2
    engine = os.path.abspath(sys.argv[1]) # the arms run from the scratch dir, not from here
    global WORKDIR
    with tempfile.TemporaryDirectory(prefix="mm_engine_cli_") as tmp:
        WORKDIR = tmp
        for arm in (
            arm_startup_failures,
            arm_case_variant_collision,
            arm_lifecycle_lines,
            arm_verbose_forced_off,
            arm_admission_bounds,
            arm_upgrade_timeout,
            arm_bind_reach,
            arm_bench_dump_peak_sessions,
            arm_version_stamp,
        ):
            arm(engine, tmp)
    if failures:
        print(f"{len(failures)} mm_engine CLI assertion(s) failed", file=sys.stderr)
        return 1
    print(f"mm_engine CLI fixture: {9} arms passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
