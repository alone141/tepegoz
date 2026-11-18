#!/usr/bin/env python3
"""
Mock process-manager for testing the GUI.

PUB  (bind)  – broadcasts array of DetailedHealthReport, GPU use included
               (--no-gpu: as a manager on a machine without the NVIDIA driver)
ROUTER (bind) – accepts DEALER commands with:
      identity = b"GUI" (or the CLI's b"PMC")
      frame0   = b"BPM"
      frame1   = packed CommandMessage (65 B)
    and answers the GUI identity with [b"BPM"][b"1" | b"0"], as the manager does.
PUB  (bind, --network) – the network manager's status and events (network_status):
      BPM is connected; running processes connect and send heartbeats, unhealthy
      ones go quiet, stopped ones leave; the GUI joins with its first command.
      --missing NAME keeps a heartbeating process off the network manager.
ROUTER (bind, --journal) – the manager's journal server (remote_journal): the
      manager and every running process write journal lines now and then, and
      the GUI reads them as from a manager on another machine. Each running
      process has a cgroup holding its pid, and a worker child for the GPU users.
ROUTER (bind, --registry) – the manager's service registry (service_registry):
      version, list, show, add, replace, update and remove, from services kept in
      memory. The six processes are the main file's; map_server is registered,
      and path_planner depends on it. A registered service joins the reports and
      starts; a changed running one is marked "restart to apply" until it
      restarts; a removed one leaves. The checks are a stand-in for the manager's:
      every absolute path exists except one whose file name starts with "missing".
"""

from __future__ import annotations

import argparse
import copy
import ctypes
import json
import random
import re
import signal
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

import zmq

from network_status import STATUS_VERSION, format_age_ms

from health_structs import (
    COMMAND_SIZE,
    GUI_IDENTITY,
    CommandEnum,
    CommandMessage,
    DetailedHealthReport,
    GpuState,
    REPORT_SIZE,
    RuntimeState,
    make_command_message,   # only for size check
)


PROCESSES = [
    "sensor_fusion",
    "path_planner",
    "vision_pipeline",
    "control_loop",
    "telemetry_bridge",
    "logger_daemon",
]
# The processes that use a GPU: (VRAM, utilization range in percent).
GPU_USERS = {
    "vision_pipeline": (2_100 * 1024 * 1024, (35, 90)),
    "sensor_fusion":   (600 * 1024 * 1024, (5, 30)),
}


def make_report(
    name: str,
    pid: int,
    state: RuntimeState,
    mem: int,
    cpu_usec: int,
    start_ns: int,
    last_seen_ns: int,
    missed: int,
    restarts: int,
    snap_ns: int,
    gpu_state: GpuState = GpuState.SAMPLED,
    gpu_util: int = 0,
    vram: int = 0,
    restart_to_apply: bool = False,
) -> DetailedHealthReport:
    r = DetailedHealthReport()
    name_bytes = name.encode("utf-8")[:63]
    r.processName = name_bytes + b"\x00" * (64 - len(name_bytes))
    r.pid = pid
    r.memoryUsageInBytes = mem
    r.cpuUsageInUsec = cpu_usec
    r.runtimeHealth.state = int(state)
    r.runtimeHealth.restartToApply = 1 if restart_to_apply else 0
    r.runtimeHealth.start_time = start_ns
    r.runtimeHealth.lastSeen = last_seen_ns
    r.runtimeHealth.missedBeats = missed
    r.runtimeHealth.restartCount = restarts
    r.snapshotTime = snap_ns
    r.gpuUsage.state = int(gpu_state)
    r.gpuUsage.utilizationPercent = gpu_util
    r.gpuUsage.memoryUsageInBytes = vram
    return r


def reports_to_bytes(reports: List[DetailedHealthReport]) -> bytes:
    size = len(reports) * REPORT_SIZE
    buf = (ctypes.c_char * size)()
    for i, rep in enumerate(reports):
        ctypes.memmove(
            ctypes.addressof(buf) + i * REPORT_SIZE,
            ctypes.addressof(rep),
            REPORT_SIZE,
        )
    return bytes(buf)


class MockNetwork:
    """The network manager's side: who is connected, and the status and events it publishes."""

    def __init__(self, sock: zmq.Socket, endpoint: str) -> None:
        self.sock = sock
        self.endpoint = endpoint
        self.started_ms = int(time.time() * 1000)
        self.dealers: Dict[str, dict] = {}
        self.next_port = 40000
        self.forwarded = 0
        self.not_delivered = 0
        self.last_status_ms = 0
        self.join("BPM")

    def join(self, name: str) -> None:
        now = int(time.time() * 1000)
        address = f"127.0.0.1:{self.next_port}"
        self.next_port += 1
        self.dealers[name] = {"address": address, "since": now, "messages": 1, "lastMessage": now}
        self.event("info", "joined", name, f"joined from {address}")

    def leave(self, name: str) -> None:
        dealer = self.dealers.pop(name)
        age = format_age_ms(int(time.time() * 1000) - dealer["since"]).replace(" ", "")
        self.event("info", "left", name,
                   f"left {dealer['address']} after {age}, {dealer['messages']} messages")

    def sent(self, name: str) -> None:
        dealer = self.dealers[name]
        dealer["messages"] += 1
        dealer["lastMessage"] = int(time.time() * 1000)
        self.forwarded += 1

    def event(self, level: str, kind: str, name: str, text: str) -> None:
        self.publish(b"event", {"version": STATUS_VERSION, "time": int(time.time() * 1000),
                                "level": level, "kind": kind, "name": name, "text": text})

    def publish_status_if_due(self) -> None:
        now = int(time.time() * 1000)
        if now - self.last_status_ms < 1000:
            return
        self.last_status_ms = now
        self.publish(b"status", {
            "version": STATUS_VERSION, "endpoint": self.endpoint, "started": self.started_ms, "now": now,
            "totals": {"forwarded": self.forwarded, "notDelivered": self.not_delivered, "rejected": 0},
            "dealers": [{"name": name, **d} for name, d in sorted(self.dealers.items())],
            "unnamed": [],
        })

    def publish(self, topic: bytes, payload: dict) -> None:
        self.sock.send_multipart([topic, json.dumps(payload).encode()])


class MockJournal:
    """The manager's journal server: answers version, journal, unit-status and members like JournalServer."""

    MANAGER_IDENT = "tepegoz"
    KEEP = 5000

    def __init__(self, sock: zmq.Socket, manager_pid: int):
        self.sock = sock
        self.manager_pid = manager_pid
        self.seq = 0
        self.entries: List[dict] = []  # {"seq", "ident", "pid", "record"}, oldest first
        # name -> [(pid, rss bytes, comm, cmdline)]: the cgroups of the running processes, set by the main loop
        self.cgroups: Dict[str, List[tuple]] = {}

    def write(self, ident: str, pid: int, message: str) -> None:
        self.seq += 1
        record = {
            "__CURSOR": f"s=mock;i={self.seq:x}",
            "__REALTIME_TIMESTAMP": str(time.time_ns() // 1000),
            "_HOSTNAME": "mock-target",
            "SYSLOG_IDENTIFIER": ident,
            "_PID": str(pid),
            "MESSAGE": message,
        }
        self.entries.append({"seq": self.seq, "ident": ident, "pid": str(pid), "record": record})
        del self.entries[:-self.KEEP]

    def answer(self) -> None:
        frames = self.sock.recv_multipart(zmq.NOBLOCK)
        # [identity][empty][request…] from a REQ client, [identity][request…] from a DEALER.
        envelope = frames[:2] if len(frames) > 1 and frames[1] == b"" else frames[:1]
        request = [f.decode("utf-8", errors="replace") for f in frames[len(envelope):]]
        status, payload = self._handle(request)
        self.sock.send_multipart(envelope + [status, payload.encode("utf-8")])

    def _handle(self, request: List[str]):
        name = request[0] if request else ""
        if name == "version":
            return b"ok", "2"
        if name == "members":
            lines = []
            for cgroup in sorted(self.cgroups):
                lines.append(f"cgroup\t{cgroup}")
                lines += [f"pid\t{cgroup}\t{pid}\t{rss}\t{comm}\t{cmdline}"
                          for pid, rss, comm, cmdline in self.cgroups[cgroup]]
            return b"ok", "".join(line + "\n" for line in lines)
        if name == "unit-status":
            return b"ok", (
                "● tepegoz.service - Tepegoz (mock)\n"
                "     Loaded: loaded (/etc/systemd/system/tepegoz.service; enabled)\n"
                "     Active: active (running)\n"
                f"   Main PID: {self.manager_pid} (tepegoz)\n"
            )
        if name != "journal" or len(request) < 2:
            return b"error", "unknown request; the journal server answers version, journal, unit-status and members"
        kind = request[1]
        value = request[2] if len(request) > 2 else ""
        lines = int(request[3]) if len(request) > 3 and request[3].isdigit() else 50
        after = request[4] if len(request) > 4 else ""
        if kind == "manager":
            chosen = [e for e in self.entries if e["ident"] == self.MANAGER_IDENT]
        elif kind == "service":
            chosen = [e for e in self.entries if e["ident"] == value]
        elif kind == "pid":
            chosen = [e for e in self.entries if e["pid"] == value]
        else:
            return b"error", "unknown journal target; use manager, service or pid"
        if after:
            try:
                after_seq = int(after.rsplit("i=", 1)[1], 16)
            except (IndexError, ValueError):
                return b"error", "bad journal cursor"
            chosen = [e for e in chosen if e["seq"] > after_seq][:max(lines, 1)]
        else:
            chosen = chosen[-max(lines, 1):]
        return b"ok", "".join(json.dumps(e["record"]) + "\n" for e in chosen)


class MockServices:
    """The simulated services, as the health reports show them. Services come and go through the registry."""

    def __init__(self, names: List[str]):
        self.names: List[str] = []  # in report order
        self.states: Dict[str, RuntimeState] = {}
        self.pids: Dict[str, int] = {}
        self.start_times: Dict[str, int] = {}
        self.cpu_base: Dict[str, int] = {}
        self.mem_base: Dict[str, int] = {}
        self.restarts: Dict[str, int] = {}
        self.missed: Dict[str, int] = {}
        self.manual: Dict[str, bool] = {}
        self.restart_to_apply: Dict[str, bool] = {}
        for i, name in enumerate(names):
            self.add(name, RuntimeState.RUNNING, pid=10000 + i)
            self.start_times[name] = time.time_ns() - random.randint(60, 3600) * 1_000_000_000
            self.restarts[name] = random.randint(0, 3)

    def add(self, name: str, state: RuntimeState, pid: int = 0, manual: bool = False) -> None:
        self.names.append(name)
        self.states[name] = state
        self.pids[name] = pid
        self.start_times[name] = time.time_ns() if pid else 0
        self.cpu_base[name] = random.randint(1_000_000, 50_000_000)
        self.mem_base[name] = random.randint(20, 400) * 1024 * 1024
        self.restarts[name] = 0
        self.missed[name] = 0
        self.manual[name] = manual
        self.restart_to_apply[name] = False

    def remove(self, name: str) -> None:
        self.names.remove(name)
        for table in (self.states, self.pids, self.start_times, self.cpu_base, self.mem_base, self.restarts,
                      self.missed, self.manual, self.restart_to_apply):
            table.pop(name, None)

    def running(self, name: str) -> bool:
        return name in self.states and self.states[name] in (
            RuntimeState.STARTING, RuntimeState.RUNNING, RuntimeState.UNHEALTHY)

    def restart(self, name: str) -> None:
        """A start or restart: the new definition, if one waits, takes effect."""
        self.states[name] = RuntimeState.STARTING
        self.manual[name] = False
        self.restart_to_apply[name] = False


# The registry's view of the files: the main file's services, and one per file in services.d.
MAIN_FILE = "/etc/tepegoz/tepegoz.yaml"
REGISTRY_FOLDER = "/etc/tepegoz/services.d"
RESERVED_NAMES = ("pm", "all", "BPM", "PMC", "GUI")
TEXT_KEYS = ("ProcessName", "ExecutablePath", "Cwd", "StdioFilePath", "Display", "XAuthority")
INT_KEYS = {"MaxRestarts": (0, 1000), "RestartDelayMs": (0, None), "RestartResetMs": (0, None),
            "ForceKillTimeoutMs": (1000, None), "GracePeriodMs": (0, None), "HeartbeatPeriodMs": (1, None),
            "MissedHeartbeatThreshold": (1, 1000)}
FLOAT_KEYS = {"BackoffMultiplier": (1.0, 10.0)}
BOOL_KEYS = ("Enabled", "CheckHealth", "StdioAppend", "InheritParentEnvironment", "UseDisplay", "CreateProcessGroup",
             "InheritParentEnviroment")
CHOICE_KEYS = {"WorkMode": ("Auto", "Manual"), "RestartMode": ("Never", "OnFailure", "Always", "UnlessStopped"),
               "StdioMode": ("Inherit", "Pipe", "File", "NullDevice")}
LIST_KEYS = ("Args", "DependsOn", "EnvironmentVariables", "EnviromentVariables")
KNOWN_KEYS = set(TEXT_KEYS) | set(INT_KEYS) | set(FLOAT_KEYS) | set(BOOL_KEYS) | set(CHOICE_KEYS) | set(LIST_KEYS)
ALLOWED_PARAMETERS = {
    "version": (), "list": (), "show": ("name",),
    "add": ("name", "definition", "start", "dryRun"),
    "replace": ("name", "definition", "restart", "dryRun"),
    "update": ("name", "set", "unset", "restart", "dryRun"),
    "remove": ("name", "force", "dryRun"),
}
STATE_WORDS = {RuntimeState.UNKNOWN: "Unknown", RuntimeState.STARTING: "Starting", RuntimeState.RUNNING: "Running",
               RuntimeState.STOPPED: "Stopped", RuntimeState.UNHEALTHY: "Unhealthy"}


def mock_definitions(names: List[str]) -> Tuple[Dict[str, dict], Dict[str, dict]]:
    """(the main file's services, the registered ones) the mock registry starts with."""
    main_file = {
        name: {"ProcessName": name, "ExecutablePath": f"/opt/app/bin/{name}",
               "Args": ["--config", f"/etc/app/{name}.yaml"], "WorkMode": "Auto"}
        for name in names
    }
    registered = {
        "map_server": {"ProcessName": "map_server", "ExecutablePath": "/opt/maps/bin/map_server",
                       "Args": ["--port", "8090", "--tiles", "/var/lib/maps/tiles"], "Cwd": "/var/lib/maps",
                       "RestartMode": "Always", "EnvironmentVariables": {"MAP_CACHE": "/var/cache/maps"},
                       "UseDisplay": False},
    }
    if "path_planner" in main_file:
        main_file["path_planner"]["DependsOn"] = ["map_server"]
    return main_file, registered


def yaml_scalar(value: Any) -> str:
    if value is True or value is False:
        return "true" if value else "false"
    if value is None:
        return "null"
    if isinstance(value, (int, float)):
        return repr(value)
    text = str(value)
    plain = re.fullmatch(r"[A-Za-z_/][A-Za-z0-9_./=-]*( [A-Za-z0-9_./=-]+)*", text) is not None
    if plain and text.lower() not in ("true", "false", "null", "yes", "no", "on", "off"):
        return text
    return json.dumps(text)


def emit_yaml(definition: dict) -> str:
    """The file the registry would write: ProcessName first, then the keys in name order."""
    keys = [k for k in ("ProcessName",) if k in definition] + sorted(k for k in definition if k != "ProcessName")
    lines = []
    for key in keys:
        value = definition[key]
        if isinstance(value, dict):
            lines.append(f"{key}:")
            lines += [f"  {k}: {yaml_scalar(v)}" for k, v in value.items()]
        elif isinstance(value, list):
            lines.append(f"{key}: [{', '.join(yaml_scalar(v) for v in value)}]")
        else:
            lines.append(f"{key}: {yaml_scalar(value)}")
    return "".join(line + "\n" for line in lines)


def quoted(name: str) -> str:
    return f"'{name}'"


class MockRegistry:
    """The manager's service registry: answers [operation, parameters] with [status, JSON object] like the manager."""

    def __init__(self, sock: Optional[zmq.Socket], sim: MockServices, main_file: Dict[str, dict],
                 registered: Dict[str, dict], journal: Optional["MockJournal"] = None,
                 network: Optional["MockNetwork"] = None):
        self.sock = sock
        self.sim = sim
        self.journal = journal
        self.network = network
        # name -> (origin, definition): the main file's first, then services.d in file name order.
        self.services: Dict[str, Tuple[str, dict]] = {name: ("main", d) for name, d in main_file.items()}
        for name in sorted(registered):
            self.services[name] = ("registered", registered[name])

    # ── the socket ────────────────────────────────────────────────────────

    def answer(self) -> None:
        frames = self.sock.recv_multipart(zmq.NOBLOCK)
        envelope = frames[:2] if len(frames) > 1 and frames[1] == b"" else frames[:1]
        status, body = self.handle(frames[len(envelope):])
        self.sock.send_multipart(envelope + [status.encode(), json.dumps(body).encode()])

    def handle(self, frames: List[bytes]) -> Tuple[str, dict]:
        """The answer to one request's frames: (status, object)."""
        if not frames or len(frames) > 2:
            return self._error("a request is an operation and at most one frame of parameters")
        operation = frames[0].decode("utf-8", errors="replace")
        if operation not in ALLOWED_PARAMETERS:
            return self._error(f"unknown operation {operation!r}; use version, list, show, add, replace, update "
                               "or remove")
        params: Dict[str, Any] = {}
        if len(frames) == 2 and frames[1].strip():
            try:
                params = json.loads(frames[1].decode("utf-8", errors="replace"))
            except ValueError as e:
                return self._error(f"the parameters are not JSON: {e}")
            if not isinstance(params, dict):
                return self._error("the parameters must be a mapping")
        for key, value in params.items():
            if key not in ALLOWED_PARAMETERS[operation]:
                return self._error(f"{operation} takes no parameter {key!r}")
            if key in ("start", "restart", "force", "dryRun") and not isinstance(value, bool):
                return self._error(f"{key} must be true or false")
        if operation not in ("version", "list") and not isinstance(params.get("name"), str):
            return self._error(f"{operation} needs a name")
        if operation == "version":
            return self._ok(operation, "service registry, protocol version 1", folder=REGISTRY_FOLDER)
        if operation == "list":
            items = [self._item(name) for name in self.services]
            return self._ok(operation, f"{len(items)} services", folder=REGISTRY_FOLDER, services=items)
        if operation == "show":
            return self._show(params["name"])
        return self._change(operation, params)

    # ── answers ───────────────────────────────────────────────────────────

    @staticmethod
    def _error(message: str) -> Tuple[str, dict]:
        return "error", {"version": 1, "message": message}

    @staticmethod
    def _ok(operation: str, message: str, **fields: Any) -> Tuple[str, dict]:
        return "ok", {"version": 1, "operation": operation, "message": message, **fields}

    @staticmethod
    def _refuse(operation: str, name: str, message: str, problems: Optional[List[str]] = None) -> Tuple[str, dict]:
        print(f"   registry: {operation} {name} refused: {message} {problems or ''}")
        return "refused", {"version": 1, "operation": operation, "name": name, "message": message,
                           "problems": problems or []}

    def _file(self, name: str) -> str:
        origin = self.services[name][0] if name in self.services else "registered"
        return MAIN_FILE if origin == "main" else f"{REGISTRY_FOLDER}/{name}.yaml"

    def _run_state(self, name: str) -> dict:
        sim = self.sim
        loaded = name in sim.states
        return {
            "loaded": loaded,
            "state": STATE_WORDS.get(sim.states[name], "Unknown") if loaded else "NotLoaded",
            "pid": sim.pids[name] if loaded and sim.pids[name] else -1,
            "workMode": ("Manual" if sim.manual[name] else "Auto") if loaded else "",
            "restartToApply": bool(loaded and sim.restart_to_apply[name]),
        }

    def _item(self, name: str) -> dict:
        origin, definition = self.services[name]
        return {"name": name, "origin": origin, "file": self._file(name),
                "enabled": definition.get("Enabled", True) is not False, **self._run_state(name)}

    def _show(self, name: str) -> Tuple[str, dict]:
        if name not in self.services:
            return self._refuse("show", name, f"there is no service named {quoted(name)}")
        origin, definition = self.services[name]
        where = "defined in the main file" if origin == "main" else "registered"
        return self._ok("show", f"{quoted(name)} is {where}", name=name, origin=origin, file=self._file(name),
                        enabled=definition.get("Enabled", True) is not False, definition=definition,
                        yaml=emit_yaml(definition), **self._run_state(name))

    # ── changes ───────────────────────────────────────────────────────────

    def _change(self, operation: str, params: dict) -> Tuple[str, dict]:
        name = params["name"]
        file = f"{REGISTRY_FOLDER}/{name}.yaml"
        why = self._name_problem(name)
        if why:
            return self._refuse(operation, name, why)
        origin = self.services[name][0] if name in self.services else ""
        if operation == "add" and origin == "main":
            return self._refuse(operation, name, f"the main file {MAIN_FILE} defines {quoted(name)}; edit it there")
        if operation == "add" and origin == "registered":
            return self._refuse(operation, name, f"{quoted(name)} is registered already ({file}); replace or update "
                                                 "it, or remove it first")
        if operation != "add" and origin != "registered":
            return self._refuse(operation, name, f"{quoted(name)} is defined in the main file {MAIN_FILE}, which only "
                                                 "an edit of that file changes" if origin == "main"
                                else f"no service named {quoted(name)} is registered")

        if operation == "remove":
            dependents = self._dependents(name)
            if dependents and not params.get("force"):
                return self._refuse(operation, name, f"{quoted(name)} is needed by other services; change them "
                                                     "first, or remove it with force",
                                    [f"{d} depends on {quoted(name)}" for d in dependents])
            if params.get("dryRun"):
                return self._ok(operation, f"{quoted(name)} can be removed; nothing was written", name=name,
                                outcome="valid", file=file)
            return self._remove(name, file)

        if operation == "update":
            definition = copy.deepcopy(self.services[name][1])
            unset = params.get("unset") or []
            unset = [unset] if isinstance(unset, str) else unset
            changes = params.get("set") or {}
            if "ProcessName" in changes or "ProcessName" in unset:
                return self._refuse(operation, name, "ProcessName cannot be changed: register the service under its "
                                                     "new name and remove this one")
            for key in unset:
                definition.pop(key, None)
            definition.update(changes)
        else:
            definition = params.get("definition")
            if not isinstance(definition, dict):
                return self._error("definition must be a mapping of the service's keys to their values")
            definition = copy.deepcopy(definition)
        if definition.get("ProcessName", name) != name:
            return self._refuse(operation, name, "the definition names another service in ProcessName; a registered "
                                                 f"service's ProcessName is its name, {quoted(name)}")
        definition = {"ProcessName": name, **{k: v for k, v in definition.items() if k != "ProcessName"}}

        problems = self._problems(name, definition)
        if problems:
            plural = "problem" if len(problems) == 1 else "problems"
            return self._refuse(operation, name, f"the change would bring {len(problems)} {plural}; nothing was "
                                                 "written", problems)
        yaml = emit_yaml(definition)
        if params.get("dryRun"):
            return self._ok(operation, "the change is valid; nothing was written", name=name, outcome="valid",
                            file=file, yaml=yaml)
        return self._write(operation, name, file, definition, yaml, params)

    def _name_problem(self, name: str) -> str:
        if not name:
            return "the service needs a name"
        if len(name.encode()) > 31:
            return f"{quoted(name)} has {len(name.encode())} bytes; a registered service's name has at most 31"
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            return f"{quoted(name)} is not a name for a registered service: use letters, digits, '_', '-' and '.' only"
        if name.startswith("."):
            return f"{quoted(name)} is not a name for a registered service: it must not start with a dot"
        if name in RESERVED_NAMES:
            return f"{quoted(name)} is reserved: pm, all, BPM, PMC and GUI cannot name a service"
        return ""

    def _dependents(self, name: str) -> List[str]:
        dependents = []
        for other, (_origin, definition) in self.services.items():
            if other == name or definition.get("Enabled", True) is False:
                continue
            depends = definition.get("DependsOn") or []
            depends = depends.split() if isinstance(depends, str) else depends
            if name in depends:
                dependents.append(other)
        return dependents

    @staticmethod
    def _path_missing(path: Any) -> bool:
        text = str(path)
        return not text.startswith("/") or text.rstrip("/").rsplit("/", 1)[-1].startswith("missing")

    def _problems(self, name: str, definition: dict) -> List[str]:
        """What the mock's validate finds in the service: its keys and values, its paths, its dependencies."""
        label = f"service {quoted(name)}"
        problems = []
        for key, value in definition.items():
            if key not in KNOWN_KEYS:
                problems.append(f"{label}: {key}: unknown key")
            elif key in TEXT_KEYS and not isinstance(value, str):
                problems.append(f"{label}: {key}: must be a text")
            elif key in INT_KEYS:
                low, high = INT_KEYS[key]
                if isinstance(value, bool) or not isinstance(value, int):
                    problems.append(f"{label}: {key}: must be a whole number")
                elif value < low or (high is not None and value > high):
                    limit = low if value < low else high
                    problems.append(f"{label}: {key}: {value} is out of range; clamped to {limit}")
            elif key in FLOAT_KEYS:
                low, high = FLOAT_KEYS[key]
                if isinstance(value, bool) or not isinstance(value, (int, float)):
                    problems.append(f"{label}: {key}: must be a number")
                elif not low <= value <= high:
                    problems.append(f"{label}: {key}: {value} is out of range; clamped to {low if value < low else high}")
            elif key in BOOL_KEYS and not isinstance(value, bool):
                problems.append(f"{label}: {key}: must be true or false")
            elif key in CHOICE_KEYS and value not in CHOICE_KEYS[key]:
                problems.append(f"{label}: {key}: must be one of {', '.join(CHOICE_KEYS[key])}")
        if "ExecutablePath" not in definition:
            problems.append(f"{label}: ProcessName and ExecutablePath are required; service ignored")
        elif self._path_missing(definition["ExecutablePath"]):
            problems.append(f"{name} ExecutablePath does not exists")
        if "Cwd" in definition and self._path_missing(definition["Cwd"]):
            problems.append(f"{name} Cwd does not exists")
        depends = definition.get("DependsOn") or []
        depends = depends.split() if isinstance(depends, str) else depends
        for dependency in depends:
            if dependency not in self.services or dependency == name:
                problems.append(f"missing dependency: {name} depends on {quoted(str(dependency))}")
            elif self._needs(str(dependency), name, set()):
                problems.append(f"dependency cycle: {name} and {dependency} need each other")
        return problems

    def _needs(self, start: str, target: str, seen: set) -> bool:
        """Whether ``start`` depends on ``target``, directly or through others."""
        if start == target:
            return True
        if start in seen or start not in self.services:
            return False
        seen.add(start)
        depends = self.services[start][1].get("DependsOn") or []
        depends = depends.split() if isinstance(depends, str) else depends
        return any(self._needs(str(d), target, seen) for d in depends)

    def _log(self, text: str) -> None:
        print(f"   registry: {text}")
        if self.journal is not None:
            self.journal.write(MockJournal.MANAGER_IDENT, self.journal.manager_pid, f"Registry: {text}")

    def _write(self, operation: str, name: str, file: str, definition: dict, yaml: str,
               params: dict) -> Tuple[str, dict]:
        sim = self.sim
        before = self.services.get(name, ("", None))[1]
        self.services[name] = ("registered", definition)
        enabled = definition.get("Enabled", True) is not False
        counts = {"added": 0, "removed": 0, "changed": 0, "kept": 0, "skipped": 0, "unchanged": len(self.services)}
        if not enabled:
            outcome = "disabled"
            message = f"{quoted(name)} is registered and switched off (Enabled: false)"
            if name in sim.states:
                sim.remove(name)
                counts["removed"] = 1
        elif name not in sim.states:
            counts["added"] = 1
            manual = definition.get("WorkMode") == "Manual"
            if operation == "add" and params.get("start") is False:
                sim.add(name, RuntimeState.UNKNOWN, manual=True)
                outcome, message = "held", f"{quoted(name)} is registered and held: start it with --start {name}"
            elif manual:
                sim.add(name, RuntimeState.UNKNOWN, manual=True)
                outcome = "manual"
                message = f"{quoted(name)} is registered; its WorkMode is Manual, so start it with --start {name}"
            else:
                sim.add(name, RuntimeState.STARTING)
                outcome, message = "starting", f"{quoted(name)} is registered and starts"
        elif before == definition:
            outcome, message = "unchanged", f"{quoted(name)} has that definition already"
        else:
            counts["changed"] = 1
            running = sim.running(name)
            if not params.get("restart"):
                sim.restart_to_apply[name] = True
                outcome = "waiting"
                message = (f"{quoted(name)} keeps running; its new definition takes effect when it restarts" if running
                           else f"{quoted(name)} takes its new definition at its next start")
            elif running:
                sim.restart(name)
                sim.restarts[name] += 1
                outcome, message = "restarting", f"{quoted(name)} restarts with its new definition"
            else:
                sim.restart_to_apply[name] = False
                outcome = "applied"
                message = f"{quoted(name)} is not running; its new definition is in place for its next start"
        self._log(f"{operation} {name}: {message}")
        return self._ok(operation, message, name=name, outcome=outcome, file=file, yaml=yaml, reload=counts)

    def _remove(self, name: str, file: str) -> Tuple[str, dict]:
        sim = self.sim
        running = sim.running(name)
        del self.services[name]
        if name in sim.states:
            sim.remove(name)
        if self.network is not None and name in self.network.dealers:
            self.network.leave(name)
        message = f"{quoted(name)} is removed" + (" and stops" if running else "")
        self._log(f"remove {name}: {message}")
        counts = {"added": 0, "removed": 1, "changed": 0, "kept": 0, "skipped": 0, "unchanged": len(self.services)}
        return self._ok("remove", message, name=name, outcome="removed", file=file, reload=counts)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pub", default="tcp://*:6667")
    parser.add_argument("--router", default="tcp://*:5557")
    parser.add_argument("--network", default="tcp://*:6669",
                        help="PUB endpoint of the network manager's status ('' for none)")
    parser.add_argument("--journal", default="tcp://*:6670",
                        help="ROUTER endpoint of the manager's journal server ('' for none)")
    parser.add_argument("--registry", default="tcp://*:6671",
                        help="ROUTER endpoint of the manager's service registry ('' for none, and no map_server)")
    parser.add_argument("--missing", action="append", default=[],
                        help="a process that sends heartbeats but never connects (repeatable)")
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument(
        "--no-gpu", action="store_true",
        help="report that the manager could not load the NVIDIA driver library",
    )
    args = parser.parse_args()

    ctx = zmq.Context()

    pub = ctx.socket(zmq.PUB)
    pub.bind(args.pub)
    print(f"[PUB]    bound to {args.pub}")

    network = None
    if args.network:
        net = ctx.socket(zmq.PUB)
        net.bind(args.network)
        network = MockNetwork(net, args.router.replace("*", "0.0.0.0"))
        print(f"[NET]    bound to {args.network}")

    journal = None
    if args.journal:
        journal_sock = ctx.socket(zmq.ROUTER)
        journal_sock.bind(args.journal)
        journal = MockJournal(journal_sock, manager_pid=4321)
        journal.write(MockJournal.MANAGER_IDENT, journal.manager_pid, "Mock manager started")
        print(f"[JOURNAL] bound to {args.journal}")

    router = ctx.socket(zmq.ROUTER)
    # Like the manager's broker: a dealer that connects under an identity in use, such as a second GUI, takes it
    # over, and a reply to an identity that is not connected fails instead of vanishing.
    router.setsockopt(zmq.ROUTER_HANDOVER, 1)
    router.setsockopt(zmq.ROUTER_MANDATORY, 1)
    router.bind(args.router)
    print(f"[ROUTER] bound to {args.router}")
    print("Expecting DEALER identity = 'GUI' (or the CLI's 'PMC'), frames: 'BPM' + CommandMessage(65 B)\n")

    # simulated state: the dicts are the services' own, so services the registry adds or removes show here too
    main_file, registered = mock_definitions(PROCESSES)
    sim = MockServices(PROCESSES + (sorted(registered) if args.registry else []))
    states, pids, start_times = sim.states, sim.pids, sim.start_times
    cpu_base, mem_base, restarts, missed = sim.cpu_base, sim.mem_base, sim.restarts, sim.missed
    logged_states: Dict[str, RuntimeState] = {}  # what the mock journal last said about each

    registry = None
    if args.registry:
        registry_sock = ctx.socket(zmq.ROUTER)
        registry_sock.bind(args.registry)
        registry = MockRegistry(registry_sock, sim, main_file, registered, journal, network)
        print(f"[REGISTRY] bound to {args.registry}")

    last_snap = time.time_ns()
    running = True

    def handle_sig(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    poller = zmq.Poller()
    poller.register(router, zmq.POLLIN)
    if journal is not None:
        poller.register(journal.sock, zmq.POLLIN)
    if registry is not None:
        poller.register(registry.sock, zmq.POLLIN)

    print("Publishing health reports… (Ctrl+C to stop)\n")

    while running:
        # ── handle commands ──────────────────────────────────────────────
        try:
            events = dict(poller.poll(10))
        except zmq.ZMQError:
            break

        if journal is not None and journal.sock in events:
            try:
                journal.answer()
            except zmq.Again:
                pass

        if registry is not None and registry.sock in events:
            try:
                registry.answer()
            except zmq.Again:
                pass
            except Exception as e:
                print(f"   registry handling error: {e}")

        if router in events:
            try:
                # ROUTER frames: [identity][empty?][…]  we take last two meaningful
                frames = router.recv_multipart(flags=zmq.NOBLOCK)
                identity = frames[0]
                # remaining frames after identity
                payload_frames = frames[1:]
                if not payload_frames:
                    continue

                # Expect: identity | "BPM" | CommandMessage
                # (some stacks put an empty delimiter, so we are flexible)
                topic = None
                payload = None
                for f in payload_frames:
                    if f == b"BPM":
                        topic = f
                    elif len(f) == COMMAND_SIZE:
                        payload = f

                print(f"← from identity={identity!r}  topic={topic!r}  "
                      f"payload={len(payload) if payload else 0} B")

                if identity not in (GUI_IDENTITY, b"PMC"):
                    print(f"   WARNING: expected identity 'GUI' or 'PMC', got {identity!r}")
                if topic != b"BPM":
                    print(f"   WARNING: expected topic 'BPM', got {topic!r}")

                if payload and len(payload) == COMMAND_SIZE:
                    cmd_msg = CommandMessage.from_buffer_copy(payload)
                    try:
                        cmd = CommandEnum(cmd_msg.command)
                    except ValueError:
                        cmd = cmd_msg.command
                    svc  = cmd_msg.ServiceName.split(b"\x00", 1)[0].decode(errors="replace")
                    cmd_args = cmd_msg.Args.split(b"\x00", 1)[0].decode(errors="replace")
                    print(f"   CMD={cmd}  ServiceName={svc!r}  Args={cmd_args!r}")

                    accepted = svc in states and cmd in (
                        CommandEnum.START, CommandEnum.STOP, CommandEnum.RESTART
                    )
                    if svc in states:
                        if cmd == CommandEnum.START:
                            sim.restart(svc)  # a waiting definition takes effect
                            print(f"   → starting {svc}")
                        elif cmd == CommandEnum.STOP:
                            states[svc] = RuntimeState.STOPPED
                            pids[svc] = 0
                            sim.manual[svc] = True  # a stop command keeps it stopped
                            print(f"   → stopped {svc}")
                        elif cmd == CommandEnum.RESTART:
                            sim.restart(svc)
                            restarts[svc] += 1
                            print(f"   → restarting {svc}")
                    if journal is not None:
                        journal.write(MockJournal.MANAGER_IDENT, journal.manager_pid,
                                      f"{cmd.name if isinstance(cmd, CommandEnum) else cmd} {svc} from "
                                      f"{identity.decode(errors='replace')}: "
                                      f"{'accepted' if accepted else 'refused'}")
                    if identity == GUI_IDENTITY:
                        # Like the manager: [BPM][b"1" accepted | b"0" refused], GUI only.
                        try:
                            router.send_multipart([identity, b"BPM", b"1" if accepted else b"0"])
                        except zmq.ZMQError as e:
                            print(f"   reply not delivered: {e}")
                    if network is not None:
                        sender = identity.decode(errors="replace")
                        if sender not in network.dealers:
                            network.join(sender)
                        network.sent(sender)
                else:
                    print(f"   payload size mismatch "
                          f"(got {len(payload) if payload else 0}, expected {COMMAND_SIZE})")
                    if payload:
                        print(f"   hex: {payload[:32].hex(' ')}…")
            except zmq.Again:
                pass
            except Exception as e:
                print(f"   command handling error: {e}")

        # ── simulate state changes ───────────────────────────────────────
        for name in sim.names:
            if states[name] == RuntimeState.STARTING and random.random() < 0.3:
                states[name] = RuntimeState.RUNNING
                pids[name] = random.randint(10000, 20000)
                if start_times[name]:  # a service's first start is no restart
                    restarts[name] += 1
                start_times[name] = time.time_ns()
            elif states[name] == RuntimeState.RUNNING and random.random() < 0.01:
                states[name] = RuntimeState.UNHEALTHY
                missed[name] += 1
            elif states[name] == RuntimeState.UNHEALTHY and random.random() < 0.05:
                states[name] = RuntimeState.STOPPED
                pids[name] = 0

        # ── publish health array ─────────────────────────────────────────
        now = time.time_ns()
        reports = []
        for name in sim.names:
            elapsed_us = (now - last_snap) / 1000.0
            load = random.uniform(0.05, 0.6) if states[name] == RuntimeState.RUNNING else 0.0
            cpu_base[name] += int(elapsed_us * load)

            mem = mem_base[name] + random.randint(-2_000_000, 2_000_000)
            mem = max(10 * 1024 * 1024, mem)

            last_seen = now - random.randint(0, 200_000_000)
            if states[name] in (
                RuntimeState.UNKNOWN,
                RuntimeState.STOPPED,
                RuntimeState.UNHEALTHY,
            ):
                last_seen = start_times[name]

            gpu_state = GpuState.NO_DRIVER if args.no_gpu else GpuState.SAMPLED
            gpu_util = vram = 0
            if gpu_state != GpuState.SAMPLED:
                gpu_util = -1  # what the manager sends without GPU data (memory stays 0)
            elif name in GPU_USERS and states[name] == RuntimeState.RUNNING:
                vram_base, (low, high) = GPU_USERS[name]
                vram = vram_base + random.randint(-50, 50) * 1024 * 1024
                gpu_util = random.randint(low, high)

            rep = make_report(
                name=name,
                pid=pids[name],
                state=states[name],
                mem=mem,
                cpu_usec=cpu_base[name],
                start_ns=start_times[name],
                last_seen_ns=last_seen,
                missed=missed[name],
                restarts=restarts[name],
                snap_ns=now,
                gpu_state=gpu_state,
                gpu_util=gpu_util,
                vram=vram,
                restart_to_apply=sim.restart_to_apply[name],
            )
            reports.append(rep)

        last_snap = now
        pub.send(reports_to_bytes(reports))

        # ── journal: running processes write now and then, the manager logs state changes
        if journal is not None:
            # A cgroup per running process; the GPU users have a worker child next to the main process.
            journal.cgroups = {}
            for name in sim.names:
                if pids[name]:
                    cmdline = f"/opt/app/bin/{name} --config /etc/app/{name}.yaml"
                    procs = [(pids[name], mem_base[name], name[:15], cmdline)]
                    if name in GPU_USERS:
                        procs.append((pids[name] + 1, mem_base[name] // 4, f"{name[:11]}-wkr", f"{name}-worker --gpu 0"))
                    journal.cgroups[name] = procs
            for name in sim.names:
                if states[name] == RuntimeState.RUNNING and random.random() < 0.15:
                    journal.write(name, pids[name], f"{name}: cycle {random.randint(1, 99999)} done")
                if states[name] != logged_states.get(name):
                    journal.write(MockJournal.MANAGER_IDENT, journal.manager_pid,
                                  f"[{name}] is {states[name].name.title()}")
                    logged_states[name] = states[name]

        # ── network manager: heartbeats of the running, joins and leaves ─
        if network is not None:
            for name in sim.names:
                connected = states[name] in (RuntimeState.RUNNING, RuntimeState.UNHEALTHY)
                connected = connected and name not in args.missing
                if connected and name not in network.dealers:
                    network.join(name)
                elif not connected and name in network.dealers:
                    network.leave(name)
                if name in network.dealers and states[name] == RuntimeState.RUNNING:
                    network.sent(name)  # unhealthy ones send no heartbeat: they go quiet
            if random.random() < 0.02:
                network.not_delivered += 1
                network.event("warning", "notDelivered", "GUI -> PMC", "not delivered: PMC is not connected")
            network.publish_status_if_due()

        time.sleep(args.interval)

    print("\nShutting down…")
    pub.close(0)
    router.close(0)
    if network is not None:
        network.sock.close(0)
    if journal is not None:
        journal.sock.close(0)
    if registry is not None:
        registry.sock.close(0)
    ctx.term()


if __name__ == "__main__":
    main()
