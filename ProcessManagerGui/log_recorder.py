"""Recording services' logs into files on this machine. No Qt.

A recording follows one service's journal through the manager's journal server from the moment it starts, by
the manager's clock (remote_journal.JournalFollower), and adds every new line to the service's own file, as
journalctl -o short-iso prints it. Lines written while the server does not answer are fetched once it answers
again, as long as the journal still holds them.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence

from remote_journal import JournalFollower

# Where recordings go until another folder is picked.
DEFAULT_FOLDER = str(Path.home() / "ProcessMonitorLogs")
# Reads in a row for a service that is behind, per round: a busy one catches up without holding up the rest.
MAX_READS_PER_ROUND = 10


def recording_path(folder: str, name: str, started: datetime) -> str:
    """<folder>/<name>_<YYYYmmdd>_<HHMMSS>.log"""
    return os.path.join(folder, f"{name}_{started:%Y%m%d_%H%M%S}.log")


@dataclass
class RecordingState:
    name: str
    path: str
    lines: int  # written to the file so far
    error: str  # why lines are not coming, or the file could not be written; empty when all is well
    active: bool  # still recording; False once stopped


class ServiceRecording:
    """One service's journal, followed from now on into its own file."""

    def __init__(self, name: str, path: str, endpoint: str, since_us: Optional[int] = None,
                 cancelled: Optional[Callable[[], bool]] = None):
        self.name = name
        self.path = path
        self.lines = 0
        self.error = ""
        self._cancelled = cancelled
        self._follower = JournalFollower(endpoint, "service", name, since_us, cancelled=cancelled)
        self._file = None
        try:
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            self._file = open(path, "a", encoding="utf-8", newline="\n")
        except OSError as e:
            self.error = f"cannot write {path}: {e.strerror or e}"

    @property
    def endpoint(self) -> str:
        return self._follower.endpoint

    def set_endpoint(self, endpoint: str) -> None:
        """Follow the journal at endpoint from its end: a cursor of another journal means nothing there."""
        self._follower.close()
        self._follower = JournalFollower(endpoint, "service", self.name, cancelled=self._cancelled)

    def poll(self) -> None:
        if self._file is None:
            return
        for _ in range(MAX_READS_PER_ROUND):
            new, error = self._follower.read_new()
            self.error = error or ""
            if new:
                try:
                    self._file.write("".join(line.text + "\n" for line in new))
                    self._file.flush()
                except OSError as e:
                    self.error = f"cannot write {self.path}: {e.strerror or e}"
                    return
                self.lines += len(new)
            if not self._follower.behind:
                return

    def close(self) -> None:
        self._follower.close()
        if self._file is not None:
            try:
                self._file.close()
            except OSError:
                pass
            self._file = None

    def state(self, active: bool) -> RecordingState:
        return RecordingState(self.name, self.path, self.lines, self.error, active)


class LogRecorder:
    """The services being recorded, each into its own file. Use it from one thread: it owns their sockets."""

    def __init__(self, cancelled: Optional[Callable[[], bool]] = None,
                 now: Callable[[], datetime] = datetime.now):
        self._cancelled = cancelled
        self._now = now
        self._recordings: Dict[str, ServiceRecording] = {}
        # Recordings stopped since the last start from nothing: their files and final counts stay on show.
        self._stopped: Dict[str, RecordingState] = {}

    def update(self, names: Sequence[str], folder: str, endpoint: str, since_us: Optional[int] = None) -> None:
        """Record exactly ``names``: new ones start a file in ``folder`` with the lines written from ``since_us``
        on (by the journal's clock; None: from their first read), and dropped ones read their journal once more
        and close theirs. A changed endpoint follows each journal there from its end."""
        wanted = list(dict.fromkeys(names))
        for name in [n for n in self._recordings if n not in wanted]:
            recording = self._recordings.pop(name)
            recording.poll()  # the lines written up to the stop
            recording.close()
            self._stopped[name] = recording.state(active=False)
        if wanted and not self._recordings:
            self._stopped = {}
        started = self._now()
        for name in wanted:
            recording = self._recordings.get(name)
            if recording is None:
                path = recording_path(folder, name, started)
                self._recordings[name] = ServiceRecording(name, path, endpoint, since_us, self._cancelled)
                self._stopped.pop(name, None)
            elif recording.endpoint != endpoint:
                recording.set_endpoint(endpoint)

    def poll(self) -> None:
        for recording in self._recordings.values():
            recording.poll()

    def states(self) -> List[RecordingState]:
        """The recordings in progress, then the ones stopped since recording last started, by name."""
        active = [r.state(active=True) for r in self._recordings.values()]
        return sorted(active, key=lambda s: s.name) + sorted(self._stopped.values(), key=lambda s: s.name)

    def close(self) -> None:
        for recording in self._recordings.values():
            recording.close()
        self._recordings = {}
