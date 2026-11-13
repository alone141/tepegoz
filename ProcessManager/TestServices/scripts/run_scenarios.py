"""Runs the TestServices scenarios under the real manager and checks what it does.

Each scenario starts the manager with one of the scenario configs, follows its health reports through pmt_watch,
sends commands through the manager's own command line, and checks the states, restart counts and output that
follow. It needs Python 3.7 or newer and nothing else, and the manager's ports (5557, 6667, 6669) free.

  PASS / FAIL  behaviour the scenario checks
  NOTE         behaviour worth knowing, reported without a verdict
  SKIP         a check that needs cgroups where the manager has none (WSL1, most containers)

Usage:
  python3 run_scenarios.py --build-dir BUILD [SCENARIO ...]   programs and configs of a build tree
  python3 run_scenarios.py --prefix /usr [SCENARIO ...]       installed programs and configs
  python3 run_scenarios.py --list
The manager's log of each scenario is kept in the run folder it prints.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Tuple

MANAGER_PORTS = (5557, 6667, 6669)
INSTALLED_MANAGER = "iltaren-ProcessManager"
STOP_LIMIT_S = 30.0
FIRST_REPORT_LIMIT_S = 10.0
MIB = 1024 * 1024


@dataclass
class Report:
    pid: int
    state: str
    restarts: int
    missed: int
    cpu_us: int
    mem: int
    started_ms: int
    at: float  # time.monotonic() when it arrived


@dataclass
class Paths:
    manager: str
    bin_dir: str
    configs: str


REPORT_LINE = re.compile(
    r"service pid=(-?\d+) state=(\w+) restarts=(-?\d+) missed=(-?\d+) cpu_us=(\d+) mem=(\d+) "
    r"started_ms=(-?\d+) name=(.*)$"
)


class Watch:
    """Follows the manager's health reports through pmt_watch."""

    def __init__(self, exe: str):
        self.lock = threading.Lock()
        self.latest: Dict[str, Report] = {}
        self.history: Dict[str, List[Report]] = {}
        self.reports = 0
        self.process = subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        universal_newlines=True)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self) -> None:
        for line in self.process.stdout:
            line = line.rstrip("\n")
            if line.startswith("report "):
                with self.lock:
                    self.reports += 1
                continue
            match = REPORT_LINE.match(line)
            if not match:
                continue
            report = Report(int(match[1]), match[2], int(match[3]), int(match[4]), int(match[5]), int(match[6]),
                            int(match[7]), time.monotonic())
            with self.lock:
                self.latest[match[8]] = report
                seen = self.history.setdefault(match[8], [])
                if not seen or (seen[-1].pid, seen[-1].state, seen[-1].restarts) != \
                        (report.pid, report.state, report.restarts):
                    seen.append(report)

    def get(self, name: str) -> Optional[Report]:
        with self.lock:
            return self.latest.get(name)

    def changes(self, name: str) -> List[Report]:
        with self.lock:
            return list(self.history.get(name, []))

    def wait(self, name: str, predicate: Callable[[Report], bool], timeout: float) -> Optional[Report]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            report = self.get(name)
            if report is not None and predicate(report):
                return report
            time.sleep(0.05)
        return None

    def wait_reports(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                if self.reports > 0:
                    return True
            time.sleep(0.05)
        return False

    def pids(self, name: str) -> List[int]:
        """Every pid the service ran with, in order."""
        pids: List[int] = []
        for report in self.changes(name):
            if report.pid > 0 and (not pids or pids[-1] != report.pid):
                pids.append(report.pid)
        return pids

    def stop(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(5)
        except subprocess.TimeoutExpired:
            self.process.kill()


class Manager:
    """The manager under test, with its output captured in manager.log."""

    def __init__(self, paths: Paths, config: str, workdir: str):
        self.paths = paths
        self.log_path = os.path.join(workdir, "manager.log")
        self.log_file = open(self.log_path, "w")
        self.process = subprocess.Popen([paths.manager, "--config", config], cwd=workdir, stdout=self.log_file,
                                        stderr=subprocess.STDOUT, start_new_session=True)

    def command(self, flag: str, name: str) -> int:
        # --no-wait: the scenarios time the effect themselves, from before the command.
        result = subprocess.run([self.paths.manager, "--" + flag, name, "--no-wait"], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=10)
        return result.returncode

    def log(self) -> str:
        with open(self.log_path, errors="replace") as log:
            return log.read()

    def service_lines(self, name: str) -> List[str]:
        """What the service printed through a pipe: the text after "[name] " in the manager's log."""
        marker = "[" + name + "] "
        return [line.split(marker, 1)[1] for line in self.log().splitlines() if marker in line]

    def said(self, name: str) -> List[str]:
        """The test program's own messages: the text after its "pmt_x[pid]: " prefix."""
        return [line.split("]: ", 1)[1] for line in self.service_lines(name) if "]: " in line]

    def chatter(self, name: str) -> List[str]:
        """pmt_chatty's numbered lines among the service's lines, leaving out the manager's own events."""
        return [line for line in self.service_lines(name) if re.search(r"(stdout|stderr) line \d+|^x+$", line)]

    def start_times(self, name: str) -> List[float]:
        """When each run of the service printed "started", in seconds of the day, from its own time stamps."""
        times = []
        for line in self.service_lines(name):
            match = re.match(r"^(\d\d):(\d\d):(\d\d\.\d{3}) pmt_\w+\[\d+\]: started", line)
            if match:
                times.append(int(match[1]) * 3600 + int(match[2]) * 60 + float(match[3]))
        return times

    def stop(self) -> Optional[int]:
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(STOP_LIMIT_S)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                self.log_file.close()
                return None
        self.log_file.close()
        return self.process.returncode


class Results:
    def __init__(self, scenario: str):
        self.scenario = scenario
        self.items: List[Tuple[str, str]] = []

    def _add(self, kind: str, text: str) -> None:
        self.items.append((kind, text))
        print("  %-4s %s" % (kind, text), flush=True)

    def check(self, ok: bool, text: str) -> bool:
        self._add("PASS" if ok else "FAIL", text)
        return ok

    def note(self, text: str) -> None:
        self._add("NOTE", text)

    def skip(self, text: str) -> None:
        self._add("SKIP", text)

    def count(self, kind: str) -> int:
        return sum(1 for item_kind, _ in self.items if item_kind == kind)


@dataclass
class Context:
    manager: Manager
    watch: Watch
    results: Results
    paths: Paths


def is_running(report: Report) -> bool:
    return report.state == "Running" and report.pid > 0


def is_stopped(report: Report) -> bool:
    return report.state == "Stopped" and report.pid <= 0


def has_pid(report: Report) -> bool:
    return report.pid > 0


def pid_alive(pid: int) -> bool:
    try:
        with open("/proc/%d/stat" % pid) as stat:
            return stat.read().rsplit(")", 1)[1].split()[0] != "Z"
    except (OSError, IndexError):
        return False


def numbers(lines: List[str], pattern: str) -> List[int]:
    found = []
    for line in lines:
        match = re.search(pattern, line)
        if match:
            found.append(int(match[1]))
    return found


def wait_all(c: Context, names: List[str], predicate: Callable[[Report], bool], timeout: float) -> Dict[str, Report]:
    deadline = time.monotonic() + timeout
    found = {}
    for name in names:
        report = c.watch.wait(name, predicate, max(deadline - time.monotonic(), 0.1))
        if report is not None:
            found[name] = report
    return found


def cgroups_in_use(c: Context, names: List[str]) -> bool:
    """The manager reads memory from a service's cgroup, so a running service at 0 bytes has none."""
    return any((c.watch.get(name) or Report(0, "", 0, 0, 0, 0, 0, 0)).mem > 0 for name in names)


# ── Scenarios ────────────────────────────────────────────────────────────────────────────────────────────────────


def scenario_basic(c: Context) -> None:
    chain = ["first", "second", "third"]
    running = wait_all(c, chain, is_running, 15)
    if c.results.check(len(running) == 3, "first, second and third run (Mode: Auto)"):
        starts = [running[name].started_ms for name in chain]
        c.results.check(starts[0] < starts[1] < starts[2],
                        "they start in DependsOn order, %d and %d ms apart" % (starts[1] - starts[0],
                                                                              starts[2] - starts[1]))
    time.sleep(1)
    manual = c.watch.get("manual")
    c.results.check(manual is not None and manual.pid <= 0, "manual is not started (WorkMode: Manual)")

    c.manager.command("start", "manual")
    c.results.check(c.watch.wait("manual", is_running, 5) is not None, "--start manual starts it")

    c.manager.command("kill", "second")
    c.results.check(c.watch.wait("second", is_stopped, 8) is not None, "--kill second stops it")
    time.sleep(2.5)
    second = c.watch.get("second")
    c.results.check(second is not None and is_stopped(second), "second stays stopped after --kill")
    third = c.watch.get("third")
    c.results.note("third, which depends on second, is %s after second was stopped" % (third.state if third else "?"))

    first_pid = c.watch.get("first").pid
    c.manager.command("restart", "first")
    c.results.check(c.watch.wait("first", lambda r: is_running(r) and r.pid != first_pid, 8) is not None,
                    "--restart first runs it again with a new pid")
    c.manager.command("start", "second")
    c.results.check(c.watch.wait("second", is_running, 5) is not None, "--start second starts it again")


def scenario_restarts(c: Context) -> None:
    c.results.check(c.watch.wait("fail_backoff", lambda r: r.restarts >= 3, 15) is not None,
                    "fail_backoff (exit 1) is restarted 3 times")
    time.sleep(3)
    report = c.watch.get("fail_backoff")
    c.results.check(report.restarts == 3 and is_stopped(report),
                    "then left stopped at MaxRestarts 3 (restarts %d, %s)" % (report.restarts, report.state))
    # Each run lasts 200 ms, so the gaps are 200 ms longer than the delays of 500, 1000 and 2000 ms.
    starts = c.manager.start_times("fail_backoff")
    gaps = [later - earlier for earlier, later in zip(starts, starts[1:])]
    c.results.check(len(gaps) == 3 and gaps[0] < gaps[1] < gaps[2] and gaps[2] > gaps[0] + 1.0,
                    "the delay grows (BackoffMultiplier 2): runs started %s s apart" %
                    ", ".join("%.1f" % gap for gap in gaps))

    for name, restarts, why in (("clean_exit", 0, "exit code 0 is not a failure under OnFailure"),
                                ("always", 2, "RestartMode Always restarts after exit 0 too, up to MaxRestarts 2"),
                                ("segv", 1, "SIGSEGV is a failure: restarted up to MaxRestarts 1"),
                                ("term", 0, "SIGTERM is not a failure under OnFailure")):
        report = c.watch.get(name)
        c.results.check(report is not None and report.restarts == restarts and is_stopped(report),
                        "%s: %s (restarts %s, %s)" % (name, why, report.restarts if report else "?",
                                                      report.state if report else "?"))
    c.results.check(re.search(r"\[segv\] pid \d+ terminated by signal 11", c.manager.log()) is not None,
                    "the manager logs segv's end as signal 11")

    report = c.watch.wait("long_runs", lambda r: len(c.watch.pids("long_runs")) >= 3, 10)
    c.results.check(report is not None,
                    "long_runs keeps being restarted past MaxRestarts 1: each run outlasts RestartResetMs "
                    "(%d runs)" % len(c.watch.pids("long_runs")))


def scenario_stopping(c: Context) -> None:
    running = wait_all(c, ["slow", "stubborn"], is_running, 10)
    c.results.check(len(running) == 2, "slow and stubborn run")
    time.sleep(1)

    started = time.monotonic()
    c.manager.command("kill", "slow")
    report = c.watch.wait("slow", is_stopped, 10)
    took = (report.at - started) if report else -1
    c.results.check(report is not None and 1.8 <= took < 5.0,
                    "--kill slow waits for its 2 s shutdown, within ForceKillTimeoutMs 5 s (%.1f s)" % took)
    c.results.check(re.search(r"\[slow\] pid \d+ exited with code 0", c.manager.log()) is not None,
                    "slow exits with code 0 by itself")

    children = numbers(c.manager.said("stubborn"), r"^child pid (\d+)")
    started = time.monotonic()
    c.manager.command("kill", "stubborn")
    report = c.watch.wait("stubborn", is_stopped, 10)
    took = (report.at - started) if report else -1
    c.results.check(report is not None and 1.4 <= took < 4.5,
                    "--kill stubborn ends it after ForceKillTimeoutMs 1.5 s (%.1f s)" % took)
    said = c.manager.said("stubborn")
    c.results.check(any("ignored SIGTERM #1" in line for line in said), "stubborn got SIGTERM first, and ignored it")
    log = c.manager.log()
    c.results.check("[stubborn] Graceful shutdown failed" in log, "the manager logs the failed graceful shutdown")
    c.results.check(re.search(r"\[stubborn\] pid \d+ terminated by signal 9", log) is not None,
                    "stubborn is ended by SIGKILL (signal 9)")
    time.sleep(0.5)
    c.results.check(len(children) == 1 and not pid_alive(children[0]),
                    "its child, which ignored SIGTERM too, is gone: the SIGKILL went to the process group")


def scenario_health(c: Context) -> None:
    c.results.check(c.watch.wait("hb_ok", is_running, 8) is not None, "hb_ok is Running once it beats")

    first = c.watch.wait("hb_silent", has_pid, 10)
    c.results.check(c.watch.wait("hb_silent", lambda r: r.state == "Unhealthy", 15) is not None,
                    "hb_silent turns Unhealthy after it stops beating")
    first_pid = first.pid if first else -1
    c.results.check(c.watch.wait("hb_silent", lambda r: has_pid(r) and r.pid != first_pid, 10) is not None,
                    "hb_silent is stopped and started again (RestartMode OnFailure)")
    c.results.check("[hb_silent] Unresponsive" in c.manager.log(), "the manager logs hb_silent as Unresponsive")

    never = c.watch.wait("hb_silent_never", lambda r: r.state == "Unhealthy", 15)
    time.sleep(3)
    report = c.watch.get("hb_silent_never")
    c.results.check(never is not None and report.state == "Unhealthy" and report.pid == never.pid,
                    "hb_silent_never stays Unhealthy and running with RestartMode Never")

    report = c.watch.get("hb_ok")
    c.results.check(is_running(report) and report.restarts == 0 and report.missed <= 1,
                    "hb_ok stays Running throughout (missed %d, restarts %d)" % (report.missed, report.restarts))

    time.sleep(6)
    states = [r.state for r in c.watch.changes("hb_late")]
    said = c.manager.said("hb_late")
    c.results.note("hb_late (first beat 4 s after its start, grace period 2 s): %d runs, %d of them sent a "
                   "heartbeat; states seen %s" % (len(c.watch.pids("hb_late")),
                                                  sum(1 for line in said if line.startswith("heartbeat 1")),
                                                  " > ".join(dict.fromkeys(states))))


def scenario_process_tree(c: Context) -> None:
    names = ["tree", "escapee", "escapee_cgroup", "orphaner"]
    wait_all(c, names, has_pid, 10)
    time.sleep(1)
    cgroups = cgroups_in_use(c, ["tree", "escapee_cgroup"])

    children = numbers(c.manager.said("tree"), r"^child \d+ pid (\d+)")
    c.results.check(len(children) == 2 and all(pid_alive(pid) for pid in children), "tree's 2 children run")
    c.manager.command("kill", "tree")
    c.results.check(c.watch.wait("tree", is_stopped, 8) is not None, "--kill tree stops it")
    time.sleep(0.5)
    c.results.check(children and not any(pid_alive(pid) for pid in children),
                    "its children, in its process group, are gone too")

    escapee = numbers(c.manager.said("escapee"), r"^escapee pid (\d+)")
    c.manager.command("kill", "escapee")
    c.watch.wait("escapee", is_stopped, 8)
    time.sleep(1)
    c.results.note("escapee (CreateProcessGroup: false): its setsid() child %s --kill escapee" %
                   ("survives" if escapee and pid_alive(escapee[0]) else "does not survive"))

    escapee = numbers(c.manager.said("escapee_cgroup"), r"^escapee pid (\d+)")
    c.manager.command("kill", "escapee_cgroup")
    c.watch.wait("escapee_cgroup", is_stopped, 8)
    time.sleep(1)
    survived = bool(escapee) and pid_alive(escapee[0])
    if cgroups:
        c.results.check(not survived, "escapee_cgroup: the cgroup takes the setsid() child down with the service")
    else:
        c.results.skip("escapee_cgroup: no cgroups here, so its setsid() child %s the stop" %
                       ("survives" if survived else "does not survive"))

    c.results.check(c.watch.wait("orphaner", is_stopped, 10) is not None, "orphaner's parent crashes and stays stopped")
    orphans = numbers(c.manager.said("orphaner"), r"^child \d+ pid (\d+)")
    c.results.note("orphaner: %d of its %d children still run after the parent crashed (RestartMode Never)" %
                   (sum(1 for pid in orphans if pid_alive(pid)), len(orphans)))


def scenario_stdio(c: Context) -> None:
    names = ["piped", "long_lines", "partial", "to_file", "to_file_append", "discarded", "inherited"]
    c.results.check(len(wait_all(c, names, is_running, 10)) == len(names), "all 7 services run")
    time.sleep(3)

    piped = c.manager.chatter("piped")
    c.results.check(any("stdout line" in line for line in piped), "piped: stdout lines are in the manager's log")
    c.results.check(any("stderr line" in line for line in piped), "piped: stderr lines are in the manager's log")

    long_lines = c.manager.chatter("long_lines")
    c.results.check(any(len(line) == 2500 for line in long_lines),
                    "long_lines: a complete 2500-byte line is logged whole (longest %d bytes)" %
                    max((len(line) for line in long_lines), default=0))

    # Output without a newline is logged once 1024 bytes of it have gathered.
    partial = c.manager.chatter("partial")
    c.results.note("partial: 3 s of output without newlines gave %d log lines, the longest %d bytes" %
                   (len(partial), max((len(line) for line in partial), default=0)))

    with open("/tmp/pmt_to_file.txt", errors="replace") as output:
        c.results.check("stdout line" in output.read(), "to_file: output goes to /tmp/pmt_to_file.txt")
    c.results.check(not c.manager.chatter("to_file"), "to_file: none of it is in the manager's log")

    old = {name: c.watch.get(name).pid for name in ("to_file", "to_file_append")}
    for name in old:
        c.manager.command("restart", name)
    for name in old:
        c.watch.wait(name, lambda r, pid=old[name]: is_running(r) and r.pid != pid, 8)
    time.sleep(1.5)
    for name, path, runs, why in (("to_file", "/tmp/pmt_to_file.txt", 1, "a restart starts the file again"),
                                  ("to_file_append", "/tmp/pmt_to_file_append.txt", 2,
                                   "a restart adds to the file (StdioAppend: true)")):
        with open(path, errors="replace") as output:
            found = len(re.findall(r"pmt_chatty\[\d+\]: started", output.read()))
        c.results.check(found == runs, "%s: %s (%d runs in the file)" % (name, why, found))

    c.results.check(not c.manager.chatter("discarded"), "discarded: nothing of it is in the manager's log")
    raw = [line for line in c.manager.log().splitlines() if re.match(r"^stdout line \d+ x+$", line)]
    c.results.check(bool(raw), "inherited: its lines are in the manager's own output, without a [name] prefix")


def scenario_environment(c: Context) -> None:
    names = ["args_list", "args_text", "env_vars", "env_cleared", "display", "cwd_tmp", "cwd_missing"]
    wait_all(c, names, has_pid, 10)
    time.sleep(1)

    def argv(name: str) -> List[str]:
        return [m[1] for m in (re.match(r"^argv\[\d+\]='(.*)'$", line) for line in c.manager.said(name)) if m]

    def said(name: str, text: str) -> bool:
        return text in c.manager.said(name)

    c.results.check(argv("args_list") == ["--show", "HOME", "two words", "plain"],
                    "args_list: a list keeps 'two words' as one argument %s" % argv("args_list"))
    c.results.check(argv("args_text") == ["--show", "HOME", "two", "words"],
                    "args_text: a text is split at spaces %s" % argv("args_text"))
    c.results.check(said("env_vars", "env PMT_COLOR='blue'"), "env_vars: PMT_COLOR=blue is set")
    c.results.note("env_vars: PMT_EMPTY: \"\" gives %s" %
                   next((line for line in c.manager.said("env_vars") if line.startswith("env PMT_EMPTY")), "nothing"))
    count = numbers(c.manager.said("env_cleared"), r"^environment has (\d+) variables")
    c.results.check(said("env_cleared", "env PMT_ONLY='this'") and said("env_cleared", "env PATH unset"),
                    "env_cleared: starts with PMT_ONLY and without PATH (%s variables in all)" %
                    (count[0] if count else "?"))
    c.results.check(said("display", "env DISPLAY=':7'") and said("display", "env XAUTHORITY='/tmp/pmt.Xauthority'"),
                    "display: DISPLAY and XAUTHORITY are set from Display and XAuthority")
    c.results.check(said("cwd_tmp", "cwd /tmp"), "cwd_tmp: runs in /tmp")
    cwd = next((line for line in c.manager.said("cwd_missing") if line.startswith("cwd ")), None)
    c.results.note("cwd_missing (Cwd /nonexistent/pmt): %s" %
                   ("it still runs, in %s" % cwd[4:] if cwd else "it did not start"))


def scenario_resources(c: Context) -> None:
    c.results.check(len(wait_all(c, ["hog", "idle"], is_running, 10)) == 2, "hog and idle run")
    time.sleep(2)
    before = c.watch.get("hog")
    time.sleep(3)
    after = c.watch.get("hog")
    if after.mem == 0:
        c.results.skip("no cgroups here: the manager reads CPU and memory from a service's cgroup, so both read 0")
        return
    percent = (after.cpu_us - before.cpu_us) / ((after.at - before.at) * 1e6) * 100
    c.results.check(30 <= percent <= 75, "hog's CPU reads %.0f%% (it is busy 50%% of the time)" % percent)
    c.results.check(after.mem >= 64 * MIB, "hog's memory reads %.0f MB (it holds 64 MB)" % (after.mem / MIB))
    idle = c.watch.get("idle")
    c.results.check(idle.mem < after.mem, "idle uses less memory than hog (%.1f MB)" % (idle.mem / MIB))


def scenario_everything(c: Context) -> None:
    auto = ["steady", "heartbeat", "flapping", "flaky", "crasher", "stubborn", "forker", "chatty", "journal_writer",
            "env", "hog"]
    started = wait_all(c, auto, has_pid, 25)
    c.results.check(len(started) == len(auto), "every Auto service starts (%d of %d)" % (len(started), len(auto)))
    time.sleep(3)
    summary = []
    for name in auto + ["manual"]:
        report = c.watch.get(name)
        summary.append("%s %s/%d" % (name, report.state if report else "?", report.restarts if report else 0))
    c.results.note("after the start: " + ", ".join(summary))


def prepare_stdio(paths: Paths) -> None:
    for path in ("/tmp/pmt_to_file.txt", "/tmp/pmt_to_file_append.txt"):
        if os.path.exists(path):
            os.remove(path)


SCENARIOS: Dict[str, Callable[[Context], None]] = {
    "basic": scenario_basic,
    "restarts": scenario_restarts,
    "stopping": scenario_stopping,
    "health": scenario_health,
    "process_tree": scenario_process_tree,
    "stdio": scenario_stdio,
    "environment": scenario_environment,
    "resources": scenario_resources,
    "everything": scenario_everything,
}
PREPARE: Dict[str, Callable[[Paths], None]] = {"stdio": prepare_stdio}
# Errors 'validate' reports on purpose.
EXPECTED_VALIDATE_ERRORS: Dict[str, Tuple[str, ...]] = {"environment": ("cwd_missing Cwd",)}


# ── Running ──────────────────────────────────────────────────────────────────────────────────────────────────────


def validate(paths: Paths, scenario: str, config: str, results: Results) -> None:
    output = subprocess.run([paths.manager, "validate", "--config", config], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True, timeout=30).stdout
    errors = []
    inside = False
    for line in output.splitlines():
        if line.startswith("====="):
            inside = not inside
        elif inside and not line.startswith("PLEASE FIX"):
            errors.append(line)
    expected = EXPECTED_VALIDATE_ERRORS.get(scenario, ())
    unexpected = [error for error in errors if not error.startswith(expected)] if expected else errors
    results.check(not unexpected and ("NO ERRORS" in output or bool(errors)),
                  "validate reports %s" % ("only the planned errors (%d)" % len(errors) if errors else "no errors")
                  if not unexpected else "validate reports: " + "; ".join(unexpected))


def leftovers(bin_dir: str) -> List[Tuple[int, str]]:
    """Test programs still running from bin_dir, other than pmt_watch."""
    found = []
    real_bin = os.path.realpath(bin_dir)
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            exe = os.readlink("/proc/%s/exe" % entry)
        except OSError:
            continue
        name = os.path.basename(exe)
        if os.path.dirname(exe) == real_bin and name.startswith("pmt_") and name != "pmt_watch":
            if pid_alive(int(entry)):
                found.append((int(entry), name))
    return found


def run_scenario(name: str, paths: Paths, run_dir: str) -> Results:
    print("== %s" % name, flush=True)
    results = Results(name)
    config = os.path.join(paths.configs, name + ".yaml")
    workdir = os.path.join(run_dir, name)
    os.makedirs(workdir)
    validate(paths, name, config, results)
    if name in PREPARE:
        PREPARE[name](paths)

    manager = Manager(paths, config, workdir)
    watch = Watch(os.path.join(paths.bin_dir, "pmt_watch"))
    try:
        if results.check(watch.wait_reports(FIRST_REPORT_LIMIT_S), "the manager publishes health reports"):
            SCENARIOS[name](Context(manager, watch, results, paths))
    except Exception as error:  # a scenario that breaks is a failure, and the others still run
        results.check(False, "the scenario raised %s: %s" % (type(error).__name__, error))
    finally:
        code = manager.stop()
        watch.stop()
        results.check(code == 0, "the manager stops on SIGTERM with exit code 0 (got %s)" % code)
        left = leftovers(paths.bin_dir)
        if left:
            results.note("left running after the manager stopped, now killed: %s" %
                         ", ".join("%s %d" % (program, pid) for pid, program in left))
            for pid, _ in left:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
    return results


def ports_in_use() -> List[int]:
    busy = []
    for port in MANAGER_PORTS:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.5)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                busy.append(port)
    return busy


def resolve_paths(args: argparse.Namespace) -> Paths:
    if args.build_dir:
        build = os.path.abspath(args.build_dir)
        paths = Paths(os.path.join(build, "ProcessManagerApp", "src", "ProcessManagerApp"),
                      os.path.join(build, "TestServices", "apps"), os.path.join(build, "TestServices", "configs"))
    elif args.prefix:
        prefix = os.path.abspath(args.prefix)
        paths = Paths(os.path.join(prefix, "bin", INSTALLED_MANAGER), os.path.join(prefix, "bin"),
                      os.path.join(prefix, "share", "processmanager", "test-setup"))
    else:
        paths = Paths("", "", "")
    paths.manager = args.manager or paths.manager
    paths.bin_dir = args.bin_dir or paths.bin_dir
    paths.configs = args.configs or paths.configs
    return paths


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("scenarios", nargs="*", help="scenarios to run; all of them when none is named")
    parser.add_argument("--build-dir", help="a build tree with ENABLE_TEST on")
    parser.add_argument("--prefix", help="the install prefix of the manager and the test programs")
    parser.add_argument("--manager", help="the manager program")
    parser.add_argument("--bin-dir", help="the folder of the pmt_* programs")
    parser.add_argument("--configs", help="the folder of the scenario configs")
    parser.add_argument("--list", action="store_true", help="list the scenarios and exit")
    args = parser.parse_args()

    if args.list:
        print("\n".join(SCENARIOS))
        return 0
    paths = resolve_paths(args)
    unknown = [name for name in args.scenarios if name not in SCENARIOS]
    if unknown:
        print("Unknown scenario: %s. Use --list." % ", ".join(unknown), file=sys.stderr)
        return 2
    for what, path in (("manager", paths.manager), ("pmt_watch", os.path.join(paths.bin_dir, "pmt_watch")),
                       ("configs", paths.configs)):
        if not path or not os.path.exists(path):
            print("No %s at '%s': give --build-dir or --prefix (see --help)." % (what, path), file=sys.stderr)
            return 2
    busy = ports_in_use()
    if busy:
        print("Ports %s are in use: stop the running manager (or GUI mock) first." %
              ", ".join(map(str, busy)), file=sys.stderr)
        return 2

    run_dir = tempfile.mkdtemp(prefix="pmt_run_")
    print("Manager logs go to %s" % run_dir, flush=True)
    all_results = [run_scenario(name, paths, run_dir) for name in (args.scenarios or list(SCENARIOS))]

    print("\n%-14s %5s %5s %5s %5s" % ("scenario", "PASS", "FAIL", "NOTE", "SKIP"))
    for results in all_results:
        print("%-14s %5d %5d %5d %5d" % (results.scenario, results.count("PASS"), results.count("FAIL"),
                                         results.count("NOTE"), results.count("SKIP")))
    failed = sum(results.count("FAIL") for results in all_results)
    print("\n%s: %d checks failed" % ("FAILED" if failed else "PASSED", failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
