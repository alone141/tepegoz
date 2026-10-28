"""Journal reads through the manager's journal server, so a GUI on another machine shows the logs too. No Qt.

The manager answers on port 6670 with what journalctl and systemctl print, and its cgroups hold, on its own machine:
  ["version"]                                     -> ["ok", "2"]
  ["journal", kind, value, lines, after_cursor]   -> ["ok", <journalctl -o json lines>] or ["error", text]
        kind: manager | service (value: its ProcessName) | pid (value: the pid)
  ["unit-status"]                                 -> ["ok", <systemctl status text>] or ["error", text]
  ["members"] (version 2)                         -> ["ok", <the processes in each task_<name> cgroup>] or ["error", text]
        one line "cgroup\\t<name>" per cgroup, then "pid\\t<name>\\t<pid>\\t<rss bytes>\\t<comm>\\t<cmdline>" per process
A manager from before the journal server does not answer. When it runs on this machine, journalctl and systemctl
are run here instead, and the cgroups read here, as the GUI always did.
"""

from __future__ import annotations

import os
import shutil
import sys
import time
from typing import Callable, Dict, List, Optional, Tuple

import zmq

from systemd_logs import (
    CGROUP_ROOT,
    JOURNAL_LINES,
    SERVICE_UNIT,
    CgroupProc,
    JournalLine,
    fetch_journal,
    fetch_journal_cgroup,
    fetch_journal_pid,
    fetch_systemctl_status,
    find_task_cgroup,
    parse_journal_json,
    snapshot_task_cgroups,
    task_cgroup_name,
)

JOURNAL_PORT = 6670
JOURNAL_ENDPOINT = f"tcp://127.0.0.1:{JOURNAL_PORT}"
# The first protocol version whose server answers "members".
MEMBERS_VERSION = 2
REQUEST_TIMEOUT_MS = 7000  # the server gives journalctl 5 s
PROBE_TIMEOUT_MS = 1500
# After the server did not answer, ask it again only this much later; meanwhile reads fail at once (or run here).
RETRY_AFTER_SEC = 30.0
# Lines asked for after a cursor, per read: more than are written between two reads, which then lose nothing.
FOLLOW_LINES = 1000
# Lines a reader keeps; the journal view keeps more of what it was given.
KEEP_LINES = 500

LOCAL_HOSTS = {"127.0.0.1", "localhost", "::1", "[::1]"}


def endpoint_host(endpoint: str) -> str:
    """The host of a tcp://host:port endpoint."""
    rest = endpoint.split("://", 1)[-1]
    return rest.rsplit(":", 1)[0] if ":" in rest else rest


def endpoint_port(endpoint: str) -> Optional[int]:
    """The port of a tcp://host:port endpoint, or None without one."""
    rest = endpoint.split("://", 1)[-1]
    port = rest.rsplit(":", 1)[1] if ":" in rest else ""
    return int(port) if port.isdigit() else None


def endpoint_on(host: str, port: int) -> str:
    """tcp://host:port, with an IPv6 address in brackets."""
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    return f"tcp://{host}:{port}"


def is_local(endpoint: str) -> bool:
    return endpoint_host(endpoint) in LOCAL_HOSTS


def local_journal_available() -> bool:
    return sys.platform != "win32" and shutil.which("journalctl") is not None


def local_cgroups_available() -> bool:
    return sys.platform != "win32" and os.path.isdir(CGROUP_ROOT)


def parse_members(payload: str) -> Dict[str, List[CgroupProc]]:
    """A "members" reply: ProcessName -> the processes in its cgroup. A service without a cgroup has no key."""
    members: Dict[str, List[CgroupProc]] = {}
    for line in (payload or "").splitlines():
        fields = line.split("\t")
        if fields[0] == "cgroup" and len(fields) >= 2:
            members.setdefault(fields[1], [])
        elif fields[0] == "pid" and len(fields) >= 6:
            try:
                pid, rss = int(fields[2]), int(fields[3])
            except ValueError:
                continue
            members.setdefault(fields[1], []).append(
                CgroupProc(pid=pid, comm=fields[4], rss_bytes=rss, cmdline=fields[5])
            )
    return members


class JournalClient:
    """A REQ socket to the journal server. A request left without an answer closes it, so the next starts clean."""

    def __init__(self, endpoint: str, context: Optional[zmq.Context] = None):
        self.endpoint = endpoint
        self._context = context or zmq.Context.instance()
        self._socket: Optional[zmq.Socket] = None

    def request(
        self, frames: List[str], timeout_ms: int = REQUEST_TIMEOUT_MS,
        cancelled: Optional[Callable[[], bool]] = None,
    ) -> Tuple[Optional[str], Optional[str], bool]:
        """Ask the server. Returns (payload, error, answered): payload on "ok", error otherwise.

        While waiting it calls ``cancelled`` every 100 ms and gives up when that returns True.
        """
        if self._socket is None:
            self._socket = self._context.socket(zmq.REQ)
            self._socket.setsockopt(zmq.LINGER, 0)
            self._socket.connect(self.endpoint)
        try:
            self._socket.send_multipart([frame.encode("utf-8") for frame in frames])
            deadline = time.monotonic() + timeout_ms / 1000.0
            while not self._socket.poll(100, zmq.POLLIN):
                if time.monotonic() >= deadline or (cancelled is not None and cancelled()):
                    self.close()
                    return None, f"no answer from the journal server at {self.endpoint}", False
            reply = self._socket.recv_multipart()
        except zmq.ZMQError as e:
            self.close()
            return None, f"journal server at {self.endpoint}: {e}", False
        payload = reply[1].decode("utf-8", errors="replace") if len(reply) > 1 else ""
        if reply and reply[0] == b"ok":
            return payload, None, True
        return None, payload or "the journal server refused the request", True

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close(linger=0)
            self._socket = None


class ServerReader:
    """What the readers share: a client of the journal server, and whether, and in which version, it answers.

    Use it from one thread: its socket belongs to the thread that first asks.
    """

    def __init__(self, endpoint: str, client: Optional[JournalClient] = None,
                 cancelled: Optional[Callable[[], bool]] = None):
        self.endpoint = endpoint
        self.version = 0  # the server's protocol version, once it has answered
        self._client = client or JournalClient(endpoint)
        self._cancelled = cancelled  # a worker's "stop asked": ends a wait for the server early
        self._server_up = False
        self._retry_at = 0.0

    def where(self) -> str:
        return f"journal server {self.endpoint}"

    def close(self) -> None:
        self._client.close()

    def _server_answers(self) -> bool:
        if self._server_up:
            return True
        if time.monotonic() < self._retry_at:
            return False
        payload, _error, answered = self._client.request(["version"], PROBE_TIMEOUT_MS, self._cancelled)
        if answered:
            self._server_up = True
            self.version = int(payload) if (payload or "").isdigit() else 1
            return True
        self._server_lost()
        return False

    def _server_lost(self) -> None:
        self._server_up = False
        self._retry_at = time.monotonic() + RETRY_AFTER_SEC

    def _no_answer(self) -> str:
        return (f"no answer from the journal server at {self.endpoint} "
                f"(a manager from before the journal server has none); asking again every {RETRY_AFTER_SEC:.0f} s")


class JournalReader(ServerReader):
    """Follows one journal: the manager's, a service's (by ProcessName) or one process's (by pid).

    The first read gets the newest lines. Later reads ask only for the lines after the last one seen, so lines
    written between two reads are not lost, however many there are. The last KEEP_LINES are kept.
    """

    def __init__(self, endpoint: str, kind: str, value: str = "", lines: int = JOURNAL_LINES,
                 client: Optional[JournalClient] = None, cancelled: Optional[Callable[[], bool]] = None):
        super().__init__(endpoint, client, cancelled)
        self.kind = kind
        self.value = value
        self.lines = lines
        self._kept: List[JournalLine] = []
        self._cursor = ""

    def read(self) -> Tuple[List[JournalLine], str, Optional[str]]:
        """Returns (the kept lines, where they come from, error). "Where" is empty when nothing could be read."""
        if self._server_answers():
            lines = str(FOLLOW_LINES if self._cursor else self.lines)
            payload, error, answered = self._client.request(
                ["journal", self.kind, self.value, lines, self._cursor], cancelled=self._cancelled
            )
            if answered:
                if error is None:
                    self._add(parse_journal_json(payload or ""))
                return list(self._kept), self.where(), error
            self._server_lost()
        if is_local(self.endpoint) and local_journal_available():
            return self._read_here()
        return list(self._kept), "", self._no_answer()

    def unit_status(self) -> Tuple[str, Optional[str]]:
        """systemctl status of the manager's unit: (text, error)."""
        if self._server_answers():
            payload, error, answered = self._client.request(["unit-status"], cancelled=self._cancelled)
            if answered:
                return payload or "", error
            self._server_lost()
        if is_local(self.endpoint) and local_journal_available():
            return fetch_systemctl_status(SERVICE_UNIT)
        return "", self._no_answer()

    def _add(self, new: List[JournalLine]) -> None:
        if new and new[-1].cursor:
            self._cursor = new[-1].cursor
        self._kept = (self._kept + new)[-KEEP_LINES:]

    def _read_here(self) -> Tuple[List[JournalLine], str, Optional[str]]:
        """journalctl on this machine, newest lines only, as before the journal server."""
        if self.kind == "manager":
            lines, error = fetch_journal(SERVICE_UNIT, self.lines)
            return lines, f"journalctl -u {SERVICE_UNIT} (here)", error
        if self.kind == "pid":
            lines, error = fetch_journal_pid(int(self.value), self.lines)
            return lines, f"_PID={self.value} (here)", error
        cgroup = find_task_cgroup(self.value)
        if not cgroup:
            return [], "", f"cgroup {task_cgroup_name(self.value)} not found under /sys/fs/cgroup"
        lines, error = fetch_journal_cgroup(cgroup, self.lines)
        return lines, f"{cgroup} (here)", error


class JournalFollower(ServerReader):
    """The lines a journal gets from a moment on, for recording it.

    The first read that reaches the server takes the newest FOLLOW_LINES lines and keeps those written at
    ``since_us`` or later, by the journal's clock; without ``since_us`` it keeps none, and only finds where the
    journal ends. So a server that answers late loses nothing written since. Each later read returns the lines
    written after the previous one, at most FOLLOW_LINES; ``behind`` then says more are waiting. Nothing is read
    on this machine instead: without the server there is no cursor to go on from.
    """

    def __init__(self, endpoint: str, kind: str, value: str = "", since_us: Optional[int] = None,
                 client: Optional[JournalClient] = None, cancelled: Optional[Callable[[], bool]] = None):
        super().__init__(endpoint, client, cancelled)
        self.kind = kind
        self.value = value
        self.since_us = since_us
        self.started = False  # the first read is done: what comes after it is new
        self.behind = False
        self._cursor = ""

    def read_new(self) -> Tuple[List[JournalLine], Optional[str]]:
        """Returns (the lines written since the previous read, error)."""
        self.behind = False
        if not self._server_answers():
            return [], self._no_answer()
        lines = str(FOLLOW_LINES if self.started or self.since_us is not None else 1)
        payload, error, answered = self._client.request(
            ["journal", self.kind, self.value, lines, self._cursor], cancelled=self._cancelled
        )
        if not answered:
            self._server_lost()
            return [], error
        if error is not None:
            return [], error
        new = parse_journal_json(payload or "")
        if new and new[-1].cursor:
            self._cursor = new[-1].cursor
        if not self.started:
            # Before the first line, an empty journal leaves no cursor: all it gets later is new.
            self.started = True
            if self.since_us is None:
                return [], None
            return [line for line in new if line.realtime_us >= self.since_us], None
        self.behind = len(new) >= FOLLOW_LINES
        return new, None


class MembersReader(ServerReader):
    """The processes in every service's cgroup (task_<name>) on the manager's machine."""

    def read(self) -> Tuple[Optional[Dict[str, List[CgroupProc]]], Optional[str]]:
        """Returns (ProcessName -> its processes, error). A service without a cgroup has no key.

        The mapping is None when nothing could be read; the error then says why.
        """
        if self._server_answers():
            if self.version >= MEMBERS_VERSION:
                payload, error, answered = self._client.request(["members"], cancelled=self._cancelled)
                if answered:
                    return (parse_members(payload or ""), None) if error is None else (None, error)
                self._server_lost()
            elif not (is_local(self.endpoint) and local_cgroups_available()):
                return None, (f"the manager at {self.endpoint} cannot list its cgroups: its journal server is "
                              f"version {self.version}, and {MEMBERS_VERSION} is needed; update the manager")
        if is_local(self.endpoint) and local_cgroups_available():
            return snapshot_task_cgroups(), None
        return None, self._no_answer()
