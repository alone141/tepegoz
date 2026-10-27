#!/usr/bin/env python3
"""
Process Manager Health Monitor – PyQt6 + ZMQ

DEALER wire format (exactly as specified by you):
    setsockopt(IDENTITY, b"GUI")
    send(b"BPM", SNDMORE)
    send(CommandMessage)          # packed: uint8 cmd + char[32] name + char[32] args
    reply: [b"BPM"][b"1" | b"0"]  # accepted / refused

SUB: receives array of DetailedHealthReport (raw or length-prefixed). The
manager samples GPU use itself and sends it in each report.

Layout: the sidebar lists the manager unit, its network manager and every
reported process; the pane on the right shows the selection — metric tiles, a
state band and the Graphs / cgroup PIDs / Journal tabs for a process,
systemctl status and the unit journal for the manager, or who is connected to
the network manager and what happened there (from its status publisher, see
network_status). The widgets live in process_views.py and network_view.py;
this module owns the workers and wires them to the views.
"""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import threading
import time
from collections import deque
from queue import Empty, SimpleQueue
from typing import Deque, Dict, List, Optional, Tuple

import zmq
from PyQt6.QtCore import QObject, QSize, QThread, QTimer, pyqtSignal, pyqtSlot, Qt
from PyQt6.QtGui import QColor, QFont, QPalette, QTextOption
from PyQt6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    QTextEdit,
    QToolBar,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from health_structs import (
    GUI_IDENTITY,
    CommandEnum,
    GpuState,
    make_command_message,
    parse_health_reports,
    report_to_dict,
)
from network_status import (
    NETWORK_ENDPOINT,
    NetworkEvent,
    NetworkStatus,
    forwarded_rate,
    message_rates,
    missing_services,
    parse_message,
)
from network_view import NetworkPage
from journal_view import (  # noqa: F401  re-exported: former home of these names
    JournalView,
    NO_PID_COLOR,
    PID_PALETTE,
    color_for_pid,
    new_journal_entries,
)
from process_views import (  # noqa: F401  re-exported
    ACCENT,
    ALIVE_STATES,
    BORDER,
    MANAGER_NAME,
    SIDEBAR_BG,
    STALE_COLOR,
    TEXT,
    TEXT_DIM,
    TEXT_MUTED,
    ElidedLabel,
    ProcessDetailPage,
    ProcessSidebar,
    ServiceDetailPage,
    mono_style,
    pill_style,
    service_active_text,
    uptime_text,
)
from remote_journal import (
    JOURNAL_ENDPOINT,
    JOURNAL_PORT,
    JournalReader,
    MembersReader,
    endpoint_host,
    endpoint_on,
    endpoint_port,
)
from systemd_logs import (
    REFRESH_MS,
    task_cgroup_name,
)
from ui_scale import (  # noqa: F401  re-exported
    DESIGN_UI_POINT_SIZE,
    EMPHASIS_FONT_SCALE,
    MONO_FONT_FAMILIES,
    fit_to_screen,
    ui_font,
    ui_point_size,
)
from usage_graphs import UsageGraphWindow, UsageHistory, UsageSample
from window_frame import FramelessMainWindow

_color_for_pid = color_for_pid  # former name

# A command the broker has not accepted by then is dropped, not sent later.
COMMAND_SEND_TIMEOUT_SEC = 3.0
# A sent command still waiting for the manager's reply after this is forgotten.
REPLY_TIMEOUT_SEC = 10.0
# Queued report messages read in one go before the newest is shown; a limit so
# a flood of them cannot hold up the commands.
MAX_REPORTS_PER_READ = 100
# The manager's ports: health reports, the broker (commands), the broker's status.
REPORT_PORT = 6667
COMMAND_PORT = 5557
NETWORK_PORT = endpoint_port(NETWORK_ENDPOINT) or 6669
# A worker round waits for the journal server in 100 ms steps and stops waiting
# when asked to stop (a local journalctl call takes at most 5 s), so waiting this
# long never has to fall back to QThread.terminate() in practice.
WORKER_STOP_TIMEOUT_MS = 8000
# Tooltip of the GPU indicator for each state without GPU data.
GPU_STATE_TEXT = {
    GpuState.NOT_SAMPLED: "The process manager has not sampled the GPUs yet",
    GpuState.NO_DRIVER: "The process manager could not load the NVIDIA driver library (libnvidia-ml.so.1)",
    GpuState.NO_DEVICE: "The process manager finds no NVIDIA GPU",
    GpuState.FAILED: "GPU sampling failed in the process manager; its journal has the NVML error",
    GpuState.STALLED: "The process manager's GPU sampling stopped: a driver call is not returning",
}


def typed_host(text: str) -> str:
    """The machine in what was typed in the HOST field: a name or an address,
    or the host of a pasted endpoint or host:port. An IPv6 address comes
    without brackets; endpoint_on adds them."""
    text = text.strip()
    if "://" in text:
        text = endpoint_host(text)
    elif text.count(":") == 1:  # host:port; an IPv6 address has more colons
        text = text.split(":", 1)[0]
    return text.strip("[]")


def stop_workers(
    pairs: List[Tuple[Optional[QObject], Optional[QThread]]],
    timeout_ms: int = WORKER_STOP_TIMEOUT_MS,
) -> List[QThread]:
    """Ask every worker to stop, then wait for all of them against one deadline.

    Stopping them one after another made closing take the sum of their
    rounds. Returns the threads that had to be terminated (a last resort).
    """
    threads = []
    for worker, thread in pairs:
        if worker is not None:
            worker.stop()
        if thread is not None:
            thread.quit()
            threads.append(thread)
    deadline = time.monotonic() + timeout_ms / 1000.0
    late = [
        thread
        for thread in threads
        if not thread.wait(max(0, int((deadline - time.monotonic()) * 1000)))
    ]
    for thread in late:
        thread.terminate()
        thread.wait(1000)
    return late


# ──────────────────────────────────────────────────────────────────────────────
# ZMQ Worker
# ──────────────────────────────────────────────────────────────────────────────

class ZmqWorker(QObject):
    reports_ready     = pyqtSignal()       # take_reports() has the newest list[dict]
    connection_status = pyqtSignal(str)
    log_message       = pyqtSignal(str)

    def __init__(
        self,
        sub_endpoint: str = "tcp://127.0.0.1:6667",
        dealer_endpoint: str = "tcp://127.0.0.1:5557",
        sub_topic: bytes = b"",
        parent: Optional[QObject] = None,
    ):
        super().__init__(parent)
        self.sub_endpoint    = sub_endpoint
        self.dealer_endpoint = dealer_endpoint
        self.sub_topic       = sub_topic
        # Cleared by stop() and never set again, so a stop that comes before the
        # thread runs start() is kept.
        self._running        = True
        self._ctx: Optional[zmq.Context] = None
        self._sub: Optional[zmq.Socket] = None
        self._dealer: Optional[zmq.Socket] = None
        self._command_queue: SimpleQueue = SimpleQueue()  # (label, payload)
        self._pending: Optional[Tuple[str, bytes]] = None  # command being sent
        self._pending_since = 0.0  # monotonic time the pending command was taken
        self._pending_logged = False
        self._awaiting: Deque[Tuple[float, str]] = deque()  # (sent at, label), oldest first
        self._finished = False  # the loop has ended; nothing drains the queue
        self._finish_lock = threading.Lock()
        self._newest: Optional[List[dict]] = None  # reports the window has not taken yet
        self._newest_lock = threading.Lock()

    @pyqtSlot()
    def start(self):
        self._ctx = zmq.Context.instance()
        try:
            # SUB
            self._sub = self._ctx.socket(zmq.SUB)
            self._sub.setsockopt(zmq.RCVHWM, 10)
            self._sub.setsockopt(zmq.SUBSCRIBE, self.sub_topic)
            self._sub.connect(self.sub_endpoint)
            self._sub.setsockopt(zmq.RCVTIMEO, 100)

            # DEALER – exact identity required by process manager
            self._dealer = self._ctx.socket(zmq.DEALER)
            self._dealer.setsockopt(zmq.IDENTITY, GUI_IDENTITY)
            self._dealer.setsockopt(zmq.SNDHWM, 100)
            self._dealer.setsockopt(zmq.SNDTIMEO, 500)
            self._dealer.setsockopt(zmq.LINGER, 0)
            # Refuse sends while the broker is unreachable instead of queueing
            # them to fire whenever it comes back.
            self._dealer.setsockopt(zmq.IMMEDIATE, 1)
            self._dealer.connect(self.dealer_endpoint)

            self.connection_status.emit("connected")
        except zmq.ZMQError as e:
            self._running = False
            self._cleanup()
            self._finish("connection failed")
            self.connection_status.emit(f"error: {e}")
            self.log_message.emit(f"ZMQ connect failed: {e}")
            return

        poller = zmq.Poller()
        poller.register(self._sub, zmq.POLLIN)
        poller.register(self._dealer, zmq.POLLIN)  # the manager's replies

        while self._running:
            # ── drain command queue in order (thread-safe SimpleQueue) ──
            while True:
                if self._pending is None:
                    try:
                        self._pending = self._command_queue.get_nowait()
                    except Empty:
                        break
                    self._pending_since = time.monotonic()
                    self._pending_logged = False
                label, payload = self._pending
                try:
                    # Original wire format (DEALER → ROUTER):
                    #   send(b"BPM", SNDMORE)
                    #   send(CommandMessage)
                    self._dealer.send(b"BPM", flags=zmq.SNDMORE | zmq.NOBLOCK)
                    self._dealer.send(payload, flags=zmq.NOBLOCK)
                    self._awaiting.append((time.monotonic(), label))
                    self.log_message.emit(f"Sent {label}")
                except zmq.Again:
                    # No broker connection: retry briefly, then give up rather
                    # than fire the command whenever the broker comes back.
                    if time.monotonic() - self._pending_since > COMMAND_SEND_TIMEOUT_SEC:
                        self.log_message.emit(
                            f"Not sent {label}: process manager not reachable at "
                            f"{self.dealer_endpoint}"
                        )
                        self._pending = None
                        continue
                    if not self._pending_logged:
                        self.log_message.emit(
                            f"Waiting to send {label} (connecting to {self.dealer_endpoint})"
                        )
                        self._pending_logged = True
                    break
                except zmq.ZMQError as e:
                    self.log_message.emit(f"Could not send {label}: {e}")
                self._pending = None

            # ── poll health reports ──────────────────────────────────────
            try:
                events = dict(poller.poll(50))
            except zmq.ZMQError:
                break

            if self._dealer in events:
                self._read_replies()

            if self._sub in events:
                try:
                    data = self._read_newest()
                    if data is not None:
                        # An empty payload is a manager with no processes left.
                        reports = parse_health_reports(data)
                        self._offer([report_to_dict(r) for r in reports])
                except ValueError as e:
                    self.log_message.emit(f"Parse error: {e}")
                except Exception as e:
                    self.log_message.emit(f"Recv error: {e}")

        self._running = False  # also when the poller failed
        self._cleanup()
        self._finish("connection closed")
        self.connection_status.emit("disconnected")

    def stop(self):
        self._running = False

    def take_reports(self) -> Optional[List[dict]]:
        """Thread-safe: the newest reports not taken yet, or None."""
        with self._newest_lock:
            reports, self._newest = self._newest, None
        return reports

    def _read_newest(self) -> Optional[bytes]:
        """Take the queued report messages and keep the payload of the last one:
        each report has every process, so the older ones are out of date."""
        data = None
        for _ in range(MAX_REPORTS_PER_READ):
            try:
                frames = self._sub.recv_multipart(flags=zmq.NOBLOCK)
            except zmq.Again:
                break
            data = frames[-1]  # last frame = payload
        return data

    def _offer(self, reports: List[dict]) -> None:
        """Leave the newest reports for the window. It is only signalled once it
        has taken the previous ones, so a busy window skips reports instead of
        working through a backlog of old ones."""
        with self._newest_lock:
            signalled = self._newest is not None
            self._newest = reports
        if not signalled:
            self.reports_ready.emit()

    def send_command(self, command: CommandEnum, service_name: str, args: str = "") -> bool:
        """Thread-safe: SimpleQueue from any thread; worker drains it.

        Returns False when the worker loop has ended, so nothing would send it.
        Raises ValueError when the name or args do not fit the command message.
        """
        payload = make_command_message(command, service_name, args)
        with self._finish_lock:
            if self._finished:
                return False
            self._command_queue.put((f"{command.name} for {service_name}", payload))
        return True

    def _read_replies(self) -> None:
        """The manager answers each command from the GUI identity with b"1" or b"0"."""
        while True:
            try:
                frames = self._dealer.recv_multipart(flags=zmq.NOBLOCK)
            except zmq.Again:
                return
            except zmq.ZMQError as e:
                self.log_message.emit(f"Reply error: {e}")
                return
            reply = frames[-1] if frames else b""
            if reply not in (b"1", b"0"):
                continue  # not a command reply (version info, for one)
            now = time.monotonic()
            while self._awaiting and now - self._awaiting[0][0] > REPLY_TIMEOUT_SEC:
                self._awaiting.popleft()
            label = self._awaiting.popleft()[1] if self._awaiting else "Command"
            if reply == b"1":
                self.log_message.emit(f"{label}: accepted by the process manager")
            else:
                self.log_message.emit(
                    f"{label}: refused by the process manager (unknown process, or not possible now)"
                )

    def _finish(self, reason: str) -> None:
        """End of the loop: report every command that will never be sent."""
        with self._finish_lock:
            self._finished = True
        dropped = [self._pending] if self._pending is not None else []
        self._pending = None
        while True:
            try:
                dropped.append(self._command_queue.get_nowait())
            except Empty:
                break
        for label, _payload in dropped:
            self.log_message.emit(f"Not sent {label}: {reason}")

    def _cleanup(self):
        if self._sub:
            self._sub.close(linger=0)
            self._sub = None
        if self._dealer:
            self._dealer.close(linger=0)
            self._dealer = None


class NetworkWorker(QObject):
    """Subscribes to the network manager's status and events (see network_status)."""

    status_received   = pyqtSignal(object)  # NetworkStatus, the newest of a round
    events_received   = pyqtSignal(list)    # list[NetworkEvent], oldest first
    connection_status = pyqtSignal(str)
    log_message       = pyqtSignal(str)

    def __init__(self, endpoint: str = NETWORK_ENDPOINT, parent: Optional[QObject] = None):
        super().__init__(parent)
        self.endpoint = endpoint
        self._running = True  # cleared by stop() only, which may come before start()
        self._sub: Optional[zmq.Socket] = None
        self._last_error = ""

    @pyqtSlot()
    def start(self):
        try:
            self._sub = zmq.Context.instance().socket(zmq.SUB)
            self._sub.setsockopt(zmq.RCVHWM, 1000)
            self._sub.setsockopt(zmq.LINGER, 0)
            self._sub.setsockopt(zmq.SUBSCRIBE, b"")
            self._sub.connect(self.endpoint)
        except zmq.ZMQError as e:
            self._running = False
            self._close()
            self.connection_status.emit(f"error: {e}")
            self.log_message.emit(f"Network manager status: connect to {self.endpoint} failed: {e}")
            return
        self.connection_status.emit("connected")

        poller = zmq.Poller()
        poller.register(self._sub, zmq.POLLIN)
        while self._running:
            try:
                if not dict(poller.poll(100)):
                    continue
            except zmq.ZMQError:
                break
            status: Optional[NetworkStatus] = None
            events: List[NetworkEvent] = []
            while True:  # everything queued, so a burst of events is one update
                try:
                    frames = self._sub.recv_multipart(flags=zmq.NOBLOCK)
                except zmq.Again:
                    break
                except zmq.ZMQError:
                    self._running = False
                    break
                try:
                    message = parse_message(frames)
                except ValueError as e:
                    self._report_error(str(e))
                    continue
                if isinstance(message, NetworkStatus):
                    status = message
                elif isinstance(message, NetworkEvent):
                    events.append(message)
            if events:
                self.events_received.emit(events)
            if status is not None:
                self.status_received.emit(status)

        self._running = False
        self._close()
        self.connection_status.emit("disconnected")

    def stop(self):
        self._running = False

    def _report_error(self, error: str) -> None:
        # Once per distinct error: a bad message every second must not flood the status bar.
        if error != self._last_error:
            self._last_error = error
            self.log_message.emit(f"Network manager status: {error}")

    def _close(self):
        if self._sub:
            self._sub.close(linger=0)
            self._sub = None


# ──────────────────────────────────────────────────────────────────────────────
# Systemd / cgroup workers
# ──────────────────────────────────────────────────────────────────────────────

def journal_pairs(lines: list) -> list:
    """(pid, text) pairs for the journal views."""
    return [(ln.pid if ln.pid is not None else 0, ln.text) for ln in lines]


class JournalWorkerBase(QObject):
    """A worker that reads journals through the manager's journal server (remote_journal).

    The endpoint can change while it runs; the reader is made again in the
    worker's thread, which owns its ZMQ socket.
    """

    finished = pyqtSignal()

    def __init__(self, endpoint: str, parent: Optional[QObject] = None):
        super().__init__(parent)
        self._running = True  # cleared by stop() only, which may come before start()
        self._endpoint = endpoint
        self._changed = False

    def set_endpoint(self, endpoint: str) -> None:
        """Thread-safe: single attribute writes, read by the loop each round."""
        self._endpoint = endpoint
        self._changed = True

    def stop(self):
        self._running = False

    def _reader(self, kind: str, value: str = "") -> JournalReader:
        # A wait for the journal server ends as soon as the worker is asked to stop.
        return JournalReader(self._endpoint, kind, value, cancelled=lambda: not self._running)

    def _sleep(self) -> None:
        slept = 0
        while self._running and slept < REFRESH_MS and not self._changed:
            QThread.msleep(100)
            slept += 100


class SystemdLogWorker(JournalWorkerBase):
    """Poll the manager unit's systemctl status and journal off the UI thread."""

    snapshot_ready = pyqtSignal(str, list, str)  # status, [(pid, text), ...], error

    def __init__(self, endpoint: str = JOURNAL_ENDPOINT, parent: Optional[QObject] = None):
        super().__init__(endpoint, parent)

    @pyqtSlot()
    def start(self):
        reader: Optional[JournalReader] = None
        while self._running:
            self._changed = False
            if reader is None or reader.endpoint != self._endpoint:
                if reader is not None:
                    reader.close()
                reader = self._reader("manager")
            try:
                lines, _where, journal_err = reader.read()
                status, status_err = reader.unit_status()
                pairs = journal_pairs(lines)
                if not pairs and journal_err:
                    pairs = [(0, journal_err)]
                errors = [e for e in dict.fromkeys((status_err, journal_err)) if e]
                self.snapshot_ready.emit(status or status_err or "", pairs, "; ".join(errors))
            except Exception as e:
                self.snapshot_ready.emit("", [], str(e))
            self._sleep()
        if reader is not None:
            reader.close()
        self.finished.emit()


class CgroupLogWorker(JournalWorkerBase):
    """Poll the journal of a service (task_<name> cgroup) or of a single PID (log windows)."""

    lines_ready = pyqtSignal(list, str, str)  # pairs, where_or_empty, error

    def __init__(
        self,
        process_name: str = "",
        pid: Optional[int] = None,
        endpoint: str = JOURNAL_ENDPOINT,
        parent: Optional[QObject] = None,
    ):
        super().__init__(endpoint, parent)
        self.process_name = process_name
        self.pid = pid

    @pyqtSlot()
    def start(self):
        reader: Optional[JournalReader] = None
        while self._running:
            self._changed = False
            if reader is None or reader.endpoint != self._endpoint:
                if reader is not None:
                    reader.close()
                if self.pid:
                    reader = self._reader("pid", str(int(self.pid)))
                else:
                    reader = self._reader("service", self.process_name)
            try:
                lines, where, err = reader.read()
                self.lines_ready.emit(journal_pairs(lines), where, err or "")
            except Exception as e:
                self.lines_ready.emit([], "", str(e))
            self._sleep()
        if reader is not None:
            reader.close()
        self.finished.emit()


class DetailJournalWorker(JournalWorkerBase):
    """Poll the journal of whichever process is selected.

    The target follows the sidebar selection; changing it wakes the loop,
    so the new journal appears without waiting out the refresh interval.
    """

    # process, pairs, where_or_empty, error
    lines_ready = pyqtSignal(str, list, str, str)

    def __init__(self, endpoint: str = JOURNAL_ENDPOINT, parent: Optional[QObject] = None):
        super().__init__(endpoint, parent)
        self._name = ""

    def set_process(self, name: str) -> None:
        """Thread-safe: single attribute writes, read by the loop each round."""
        self._name = name or ""
        self._changed = True

    @pyqtSlot()
    def start(self):
        reader: Optional[JournalReader] = None
        while self._running:
            self._changed = False
            name = self._name
            if reader is not None and (reader.value != name or reader.endpoint != self._endpoint):
                reader.close()
                reader = None
            if name:
                if reader is None:
                    reader = self._reader("service", name)
                try:
                    lines, where, err = reader.read()
                    self.lines_ready.emit(name, journal_pairs(lines), where, err or "")
                except Exception as e:
                    self.lines_ready.emit(name, [], "", str(e))
            self._sleep()
        if reader is not None:
            reader.close()
        self.finished.emit()


class CgroupMembersWorker(JournalWorkerBase):
    """Poll the processes in every service's cgroup through the journal server, off the UI thread."""

    members_ready = pyqtSignal(object, str)  # {name: [CgroupProc]} or None, error

    def __init__(self, endpoint: str = JOURNAL_ENDPOINT, parent: Optional[QObject] = None):
        super().__init__(endpoint, parent)

    @pyqtSlot()
    def start(self):
        reader: Optional[MembersReader] = None
        while self._running:
            self._changed = False
            if reader is None or reader.endpoint != self._endpoint:
                if reader is not None:
                    reader.close()
                reader = MembersReader(self._endpoint, cancelled=lambda: not self._running)
            try:
                members, error = reader.read()
                self.members_ready.emit(members, error or "")
            except Exception as e:
                self.members_ready.emit(None, str(e))
            self._sleep()
        if reader is not None:
            reader.close()
        self.finished.emit()


class ProcessLogWindow(QMainWindow):
    """A journal window for one PID (or, kept for scripts, one cgroup)."""

    def __init__(
        self,
        process_name: str = "",
        parent: Optional[QWidget] = None,
        pid: Optional[int] = None,
        comm: str = "",
        journal_endpoint: str = JOURNAL_ENDPOINT,
    ):
        super().__init__(parent)
        self.process_name = process_name
        self.pid = pid
        if pid:
            title = f"Logs — {comm or 'pid'} ({pid})"
            meta = f"journalctl _PID={pid}"
        else:
            title = f"Logs — {process_name}"
            meta = f"cgroup {task_cgroup_name(process_name)}"
        self.setWindowTitle(title)
        fit_to_screen(self, QSize(860, 480))
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(8, 8, 8, 8)
        self.lbl_meta = QLabel(meta)
        self.lbl_meta.setStyleSheet(mono_style(TEXT_DIM))
        self.lbl_meta.setWordWrap(True)
        layout.addWidget(self.lbl_meta)
        self.txt = QTextEdit()
        self.txt.setReadOnly(True)
        self.txt.setLineWrapMode(QTextEdit.LineWrapMode.WidgetWidth)
        self.txt.setWordWrapMode(QTextOption.WrapMode.WrapAnywhere)
        self.txt.setStyleSheet(
            "QTextEdit { background: #1a1a1a; color: #c8e6c9; "
            f"font-family: {MONO_FONT_FAMILIES}; "
            "border: 1px solid #333; }"
        )
        self._journal = JournalView(self.txt)
        layout.addWidget(self.txt, stretch=1)
        self.setCentralWidget(central)

        self._thread = QThread(self)
        self._worker = CgroupLogWorker(process_name=process_name, pid=pid, endpoint=journal_endpoint)
        self._worker.moveToThread(self._thread)
        self._thread.started.connect(self._worker.start)
        self._worker.lines_ready.connect(self._on_lines)
        self._thread.start()

    def set_journal_endpoint(self, endpoint: str) -> None:
        worker = getattr(self, "_worker", None)
        if worker is not None:
            worker.set_endpoint(endpoint)

    @pyqtSlot(list, str, str)
    def _on_lines(self, pairs: list, where: str, error: str):
        target = f"_PID={self.pid}" if self.pid else task_cgroup_name(self.process_name)
        if where:
            self.lbl_meta.setText(f"{target}  →  {where}")
            self.lbl_meta.setStyleSheet(mono_style(TEXT_DIM))
        else:
            self.lbl_meta.setText(error or f"{target} not found")
            self.lbl_meta.setStyleSheet(mono_style("#f44336"))
        self._journal.show([(0, error)] if error and not pairs else pairs)

    def detach_worker(self) -> Tuple[Optional[QObject], Optional[QThread]]:
        """Hand the worker and its thread to the caller, who stops them."""
        worker = getattr(self, "_worker", None)
        thread = getattr(self, "_thread", None)
        if worker is not None:
            try:
                worker.lines_ready.disconnect()
            except TypeError:
                pass
        self._worker = None
        self._thread = None
        return worker, thread

    def closeEvent(self, event):
        stop_workers([self.detach_worker()])
        super().closeEvent(event)


# ──────────────────────────────────────────────────────────────────────────────
# Report metrics and feed freshness
# ──────────────────────────────────────────────────────────────────────────────

# A process can't use more than every core; clamps sampling glitches.
CPU_PCT_MAX = 100.0 * (os.cpu_count() or 1)
# Health data is stale after this long without a report (or three report
# intervals, for a publisher slower than that).
STALE_AFTER_SEC = 5.0
FEED_COLORS = {"live": "#4caf50", "waiting": "orange", "stale": "#f44336"}


def cpu_percent(
    prev: Optional[Tuple[int, int]], cpu_usec: int, snap_ns: int
) -> Optional[float]:
    """CPU use since the previous (cpu_usec, snapshot_ns) sample, in percent
    of one core (400 = four busy cores); None without a usable sample."""
    if prev is None:
        return None
    delta_cpu = cpu_usec - prev[0]
    delta_ns = snap_ns - prev[1]
    if delta_ns <= 0 or delta_cpu < 0:  # clock went back / counter reset
        return None
    return min(delta_cpu / (delta_ns / 1000.0) * 100.0, CPU_PCT_MAX)


class FeedMonitor:
    """Tells live health data from stale, from when reports arrive.

    Times are time.monotonic() seconds. A ZMQ connect succeeds even when
    nothing is listening, so only arriving reports prove the feed is live.
    """

    def __init__(self) -> None:
        self.connected_at: Optional[float] = None
        self.last_report_at: Optional[float] = None
        self._gaps: Deque[float] = deque(maxlen=8)

    def on_connected(self, now: float) -> None:
        self.connected_at = now
        self._gaps.clear()

    def on_disconnected(self) -> None:
        self.connected_at = None

    def on_report(self, now: float) -> None:
        if self._received_since_connect():
            self._gaps.append(now - self.last_report_at)
        self.last_report_at = now

    def _received_since_connect(self) -> bool:
        return (
            self.connected_at is not None
            and self.last_report_at is not None
            and self.last_report_at >= self.connected_at
        )

    def stale_after(self) -> float:
        if not self._gaps:
            return STALE_AFTER_SEC
        return max(STALE_AFTER_SEC, 3 * statistics.median(self._gaps))

    def data_stale(self, now: float) -> bool:
        """The last report received is too old to show as current."""
        return (
            self.last_report_at is not None
            and now - self.last_report_at > self.stale_after()
        )

    def status(self, now: float) -> Optional[Tuple[str, str]]:
        """(text, level) for the connection indicator, level being "live",
        "waiting" or "stale"; None while not connected."""
        if self.connected_at is None:
            return None
        if not self._received_since_connect():
            waited = now - self.connected_at
            if waited <= self.stale_after():
                return "Waiting for data…", "waiting"
            return f"No data for {waited:.0f}s", "stale"
        age = now - self.last_report_at
        if age > self.stale_after():
            return f"No data for {age:.0f}s", "stale"
        return "Live", "live"


# ──────────────────────────────────────────────────────────────────────────────
# Main window
# ──────────────────────────────────────────────────────────────────────────────

TOOLBAR_STYLE = f"""
    QWidget#titleBar {{ background: #252525; border-bottom: 1px solid {BORDER}; }}
    QToolBar {{ spacing: 10px; padding: 4px 10px; background: transparent; border: none; }}
    QToolBar QLabel {{ color: {TEXT_DIM}; }}
    QToolButton {{
        color: {TEXT}; background: transparent; border: 1px solid #444;
        border-radius: 3px; padding: 3px 10px;
    }}
    QToolButton:hover {{ background: #2f2f2f; }}
    QToolButton:checked {{ background: #333; border-color: {ACCENT}; }}
"""
CONNECTION_STYLE = f"""
    QWidget#connection {{ background: #232323; border-bottom: 1px solid {BORDER}; }}
    QWidget#connection QLabel {{ color: {TEXT_DIM}; }}
    QLineEdit {{
        background: #1a1a1a; color: {TEXT};
        border: 1px solid #444; border-radius: 3px;
        padding: 3px 6px; min-width: 260px;
        font-family: {MONO_FONT_FAMILIES};
    }}
    QLineEdit:focus {{ border-color: {ACCENT}; }}
"""
RECONNECT_STYLE = """
    QPushButton {
        background: #1565c0; color: white; border: none;
        border-radius: 3px; padding: 4px 14px; font-weight: bold;
    }
    QPushButton:hover { background: #1976d2; }
    QPushButton:pressed { background: #0d47a1; }
"""
FILTER_STYLE = f"""
    QLineEdit {{
        background: #1a1a1a; color: {TEXT}; border: 1px solid #3a3a3a;
        border-radius: 3px; padding: 3px 6px;
    }}
    QLineEdit:focus {{ border-color: {ACCENT}; }}
"""
STATUSBAR_STYLE = f"""
    QStatusBar {{ background: #252525; border-top: 1px solid {BORDER}; }}
    QStatusBar::item {{ border: none; }}
    QStatusBar QLabel {{ color: {TEXT_MUTED}; padding: 0 6px; }}
"""


class ProcessMonitorWindow(FramelessMainWindow):
    def __init__(
        self,
        sub_endpoint: str,
        dealer_endpoint: str,
        network_endpoint: str = NETWORK_ENDPOINT,
        parent: Optional[QWidget] = None,
        journal_endpoint: str = JOURNAL_ENDPOINT,
    ):
        super().__init__(parent)
        self.sub_endpoint    = sub_endpoint
        self.dealer_endpoint = dealer_endpoint
        self.network_endpoint = network_endpoint
        self.journal_endpoint = journal_endpoint

        self.setWindowTitle("Process Monitor")
        fit_to_screen(self, QSize(1180, 760), minimum=QSize(900, 600))

        self._prev: Dict[str, Tuple[int, int]] = {}   # name → (cpu_usec, snap_ns)
        self._current: Dict[str, dict] = {}
        self._log_windows: Dict[str, ProcessLogWindow] = {}
        # ProcessName -> its processes, from the manager's machine; None until read, or when it cannot be.
        self._cgroup_members: Optional[dict] = None
        self._cgroup_error = ""
        self._feed = FeedMonitor()
        self._stale = False  # the pages show data that stopped updating
        self._link_status: Optional[Tuple[str, str, str]] = None
        self._gpu_state: Optional[GpuState] = None  # from the last reports; None before any
        self._usage = UsageHistory()  # recorded from the start, for the graphs
        self._usage_window: Optional[UsageGraphWindow] = None
        self._auto_select = True  # show the first process once reports arrive
        self._network_feed = FeedMonitor()  # the network manager's status, like the reports
        self._network_status: Optional[NetworkStatus] = None
        self._network_rates: Dict[str, Optional[float]] = {}
        self._network_forwarded_rate: Optional[float] = None
        self._network_stale = False

        self._build_ui()
        self._feed_timer = QTimer(self)
        self._feed_timer.setInterval(1000)
        self._feed_timer.timeout.connect(self._refresh_feed_status)
        self._feed_timer.start()
        self._start_zmq_worker()
        self._start_network_worker()
        self._start_systemd_worker()
        self._start_cgroup_members_worker()
        self._start_detail_journal_worker()
        self._show_connecting()

    # ── UI ────────────────────────────────────────────────────────────────

    def _build_ui(self):
        toolbar = QToolBar("Main")
        toolbar.setMovable(False)
        # The window has no system title bar: the toolbar's row is its title bar.
        self.set_title_bar(toolbar).setStyleSheet(TOOLBAR_STYLE)

        title = QLabel("Process Monitor")
        title.setFont(ui_font(1.1, QFont.Weight.Bold))
        title.setStyleSheet(f"color: {TEXT}; padding: 0 4px;")
        toolbar.addWidget(title)

        self.lbl_status = QLabel()
        toolbar.addWidget(self.lbl_status)
        self._set_link_status("Connecting…", "orange")

        # Takes the spare toolbar width and elides, so the pills and the
        # Connection button never fall into the toolbar's overflow menu.
        self.lbl_machine = ElidedLabel()
        self.lbl_machine.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
        )
        self.lbl_machine.setStyleSheet(mono_style(TEXT_MUTED))
        toolbar.addWidget(self.lbl_machine)

        self.lbl_network = QLabel("Network …")
        toolbar.addWidget(self.lbl_network)

        self.lbl_gpu = QLabel("GPU …")
        toolbar.addWidget(self.lbl_gpu)
        self._show_gpu_state(None)

        self.btn_connection = QToolButton()
        self.btn_connection.setText("Connection")
        self.btn_connection.setCheckable(True)
        self.btn_connection.setToolTip("Show the manager's machine and reconnect")
        toolbar.addWidget(self.btn_connection)

        # Machine strip: shown on demand, and when connecting fails. The four
        # endpoints are not shown; HOST moves them all, keeping their ports.
        self.connection_strip = QWidget()
        self.connection_strip.setObjectName("connection")
        self.connection_strip.setStyleSheet(CONNECTION_STYLE)
        strip = QHBoxLayout(self.connection_strip)
        strip.setContentsMargins(12, 6, 12, 6)
        strip.setSpacing(8)
        strip.addWidget(QLabel("HOST"))
        self.edit_host = QLineEdit()
        self.edit_host.setPlaceholderText("machine")
        self.edit_host.setToolTip("The manager's machine, a name or an address. Enter reconnects to it")
        self.edit_host.setClearButtonEnabled(True)
        self.edit_host.returnPressed.connect(self._apply_host)
        strip.addWidget(self.edit_host, 1)
        self.btn_reconnect = QPushButton("Reconnect")
        self.btn_reconnect.setStyleSheet(RECONNECT_STYLE)
        self.btn_reconnect.setToolTip("Reconnect to the machine in HOST")
        self.btn_reconnect.clicked.connect(self._apply_host)
        strip.addWidget(self.btn_reconnect)
        strip.addStretch(2)
        self.connection_strip.setVisible(False)
        self.btn_connection.toggled.connect(self.connection_strip.setVisible)

        # Sidebar: filter + manager / process list
        side = QWidget()
        side.setStyleSheet(f"background: {SIDEBAR_BG};")
        side_layout = QVBoxLayout(side)
        side_layout.setContentsMargins(8, 8, 8, 8)
        side_layout.setSpacing(8)
        self.sidebar_filter = QLineEdit()
        self.sidebar_filter.setPlaceholderText("Filter processes")
        self.sidebar_filter.setClearButtonEnabled(True)
        self.sidebar_filter.setStyleSheet(FILTER_STYLE)
        side_layout.addWidget(self.sidebar_filter)
        self.sidebar = ProcessSidebar()
        self.sidebar_filter.textChanged.connect(self.sidebar.set_filter)
        self.sidebar.manager_selected.connect(self._on_manager_selected)
        self.sidebar.network_selected.connect(self._on_network_selected)
        self.sidebar.process_selected.connect(self._on_process_selected)
        side_layout.addWidget(self.sidebar, stretch=1)

        # Detail: one page per kind of selection
        self.detail = ProcessDetailPage(self._usage)
        self.detail.command_requested.connect(self._send_cmd)
        self.detail.pid_activated.connect(self._on_pid_activated)
        self.detail.pop_out_requested.connect(self._open_usage_graphs)
        self.service_page = ServiceDetailPage()
        self.network_page = NetworkPage()
        self.stack = QStackedWidget()
        self.stack.addWidget(self.service_page)
        self.stack.addWidget(self.network_page)
        self.stack.addWidget(self.detail)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.setChildrenCollapsible(False)
        splitter.setHandleWidth(1)
        splitter.setStyleSheet(f"QSplitter::handle {{ background: {BORDER}; }}")
        splitter.addWidget(side)
        splitter.addWidget(self.stack)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        side_w = self.fontMetrics().averageCharWidth() * 34
        splitter.setSizes([side_w, side_w * 4])

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.connection_strip)
        layout.addWidget(splitter, stretch=1)
        self.setCentralWidget(central)

        # Status bar: last message at the left, counts at the right
        bar = self.statusBar()
        bar.setStyleSheet(STATUSBAR_STYLE)
        self.lbl_log = QLabel("Ready")
        # Ignored: a long message must not raise the window's minimum width.
        self.lbl_log.setSizePolicy(
            QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred
        )
        bar.addPermanentWidget(self.lbl_log, 1)
        self.lbl_process_count = QLabel("Processes: 0")
        self.lbl_last_update = QLabel("Last update: —")
        bar.addPermanentWidget(self.lbl_process_count)
        bar.addPermanentWidget(self.lbl_last_update)

        self._show_endpoints()
        self._show_network_state()
        # Start on the manager page; the first report selects a process.
        self.sidebar.blockSignals(True)
        self.sidebar.select_manager()
        self.sidebar.blockSignals(False)
        self.stack.setCurrentWidget(self.service_page)
        self.sidebar.setFocus()

    def _machine(self) -> Optional[str]:
        """The machine all four endpoints are on, or None when they are on several."""
        endpoints = (self.sub_endpoint, self.dealer_endpoint, self.network_endpoint, self.journal_endpoint)
        hosts = {endpoint_host(endpoint).strip("[]") for endpoint in endpoints}
        return next(iter(hosts)) if len(hosts) == 1 else None

    def _show_endpoints(self) -> None:
        machine = self._machine()
        self.lbl_machine.setText("several machines" if machine is None else machine)
        self.lbl_machine.setToolTip(
            f"SUB {self.sub_endpoint}\nDEALER {self.dealer_endpoint}"
            f"\nNET {self.network_endpoint}\nJOURNAL {self.journal_endpoint}"
        )
        # HOST shows the machine when all four are on one; it stays empty otherwise.
        self.edit_host.setText(machine or "")
        self.edit_host.setPlaceholderText("several machines" if machine is None else "machine")

    def _apply_host(self) -> None:
        """Put the machine typed in HOST in all four endpoints, keeping their ports, and reconnect.

        An empty HOST reconnects to the endpoints as they are, which may be on several machines.
        """
        typed = self.edit_host.text().strip()
        if typed:
            host = typed_host(typed)
            if not host or any(c.isspace() or c == "/" for c in host):
                self._show_status(f"Not a machine name or address: {typed!r}")
                return
            self.sub_endpoint = endpoint_on(host, endpoint_port(self.sub_endpoint) or REPORT_PORT)
            self.dealer_endpoint = endpoint_on(host, endpoint_port(self.dealer_endpoint) or COMMAND_PORT)
            self.network_endpoint = endpoint_on(host, endpoint_port(self.network_endpoint) or NETWORK_PORT)
            self.journal_endpoint = endpoint_on(host, endpoint_port(self.journal_endpoint) or JOURNAL_PORT)
        self._reconnect()

    def _show_status(self, message: str) -> None:
        # One line: multi-line parse errors would make the status bar grow.
        self.lbl_log.setText(" ".join(message.split()))
        self.lbl_log.setToolTip(message)

    # ── selection ─────────────────────────────────────────────────────────

    def _on_process_selected(self, name: str) -> None:
        self._auto_select = False
        self.stack.setCurrentWidget(self.detail)
        report = self._current.get(name)
        self.detail.set_process(name, report, *self._members_of(name))
        self.detail.set_stale(self._stale)
        worker = getattr(self, "detail_journal_worker", None)
        if worker is not None:
            worker.set_process(name)

    def _on_manager_selected(self) -> None:
        self._auto_select = False
        self.stack.setCurrentWidget(self.service_page)
        worker = getattr(self, "detail_journal_worker", None)
        if worker is not None:
            worker.set_process("")  # nothing to tail while the manager is shown

    def _on_network_selected(self) -> None:
        self._auto_select = False
        self.stack.setCurrentWidget(self.network_page)
        worker = getattr(self, "detail_journal_worker", None)
        if worker is not None:
            worker.set_process("")

    def _refresh_detail(self) -> None:
        name = self.detail.name
        if not name or self.stack.currentWidget() is not self.detail:
            return
        report = self._current.get(name)
        if report is not None:
            self.detail.update_report(report)

    def _apply_stale(self) -> None:
        self.sidebar.set_stale(self._stale)
        self.detail.set_stale(self._stale)

    # ── ZMQ worker lifecycle ──────────────────────────────────────────────

    def _start_zmq_worker(self):
        self.worker_thread = QThread()
        self.worker = ZmqWorker(self.sub_endpoint, self.dealer_endpoint)
        self.worker.moveToThread(self.worker_thread)

        self.worker_thread.started.connect(self.worker.start)
        self.worker.reports_ready.connect(self._on_reports_ready)
        self.worker.connection_status.connect(self._on_connection_status)
        self.worker.log_message.connect(self._on_log)

        self.worker_thread.start()

    def _stop_zmq_worker(self, timeout_ms: int = WORKER_STOP_TIMEOUT_MS) -> None:
        stop_workers([self._detach_zmq_worker()], timeout_ms)

    def _detach_zmq_worker(self) -> Tuple[Optional[QObject], Optional[QThread]]:
        worker = getattr(self, "worker", None)
        thread = getattr(self, "worker_thread", None)
        if worker is not None:
            for sig in (
                worker.reports_ready,
                worker.connection_status,
                worker.log_message,
            ):
                try:
                    sig.disconnect()
                except TypeError:
                    pass
        self.worker = None
        self.worker_thread = None
        return worker, thread

    def _detach_background_worker(
        self, worker_attr: str, thread_attr: str
    ) -> Tuple[Optional[QObject], Optional[QThread]]:
        worker = getattr(self, worker_attr, None)
        thread = getattr(self, thread_attr, None)
        if worker is not None:
            for name in ("snapshot_ready", "members_ready", "lines_ready"):
                signal = getattr(worker, name, None)
                if signal is None:
                    continue
                try:
                    signal.disconnect()
                except TypeError:
                    pass
        setattr(self, worker_attr, None)
        setattr(self, thread_attr, None)
        return worker, thread

    def _start_cgroup_members_worker(self):
        self.cgroup_members_thread = QThread()
        self.cgroup_members_worker = CgroupMembersWorker(self.journal_endpoint)
        self.cgroup_members_worker.moveToThread(self.cgroup_members_thread)
        self.cgroup_members_thread.started.connect(self.cgroup_members_worker.start)
        self.cgroup_members_worker.members_ready.connect(self._on_cgroup_members)
        self.cgroup_members_thread.start()

    def _start_systemd_worker(self):
        self.systemd_thread = QThread()
        self.systemd_worker = SystemdLogWorker(self.journal_endpoint)
        self.systemd_worker.moveToThread(self.systemd_thread)
        self.systemd_thread.started.connect(self.systemd_worker.start)
        self.systemd_worker.snapshot_ready.connect(self._on_systemd_snapshot)
        self.systemd_thread.start()

    def _start_detail_journal_worker(self):
        self.detail_journal_thread = QThread()
        self.detail_journal_worker = DetailJournalWorker(self.journal_endpoint)
        self.detail_journal_worker.moveToThread(self.detail_journal_thread)
        self.detail_journal_thread.started.connect(self.detail_journal_worker.start)
        self.detail_journal_worker.lines_ready.connect(self._on_detail_journal)
        self.detail_journal_thread.start()
        if self.stack.currentWidget() is self.detail and self.detail.name:
            self.detail_journal_worker.set_process(self.detail.name)

    def _start_network_worker(self):
        self.network_thread = QThread()
        self.network_worker = NetworkWorker(self.network_endpoint)
        self.network_worker.moveToThread(self.network_thread)
        self.network_thread.started.connect(self.network_worker.start)
        self.network_worker.status_received.connect(self._on_network_status)
        self.network_worker.events_received.connect(self._on_network_events)
        self.network_worker.connection_status.connect(self._on_network_connection)
        self.network_worker.log_message.connect(self._on_log)
        self.network_thread.start()

    def _detach_network_worker(self) -> Tuple[Optional[QObject], Optional[QThread]]:
        worker = getattr(self, "network_worker", None)
        thread = getattr(self, "network_thread", None)
        if worker is not None:
            for sig in (
                worker.status_received,
                worker.events_received,
                worker.connection_status,
                worker.log_message,
            ):
                try:
                    sig.disconnect()
                except TypeError:
                    pass
        self.network_worker = None
        self.network_thread = None
        return worker, thread

    # ── slots: data in ────────────────────────────────────────────────────

    @pyqtSlot(str, list, str)
    def _on_systemd_snapshot(self, status: str, pairs: list, error: str):
        self.service_page.show_snapshot(status, pairs, error)
        self.sidebar.set_manager_hint(service_active_text(status))

    @pyqtSlot(str, list, str, str)
    def _on_detail_journal(self, name: str, pairs: list, cgroup_path: str, error: str):
        if name and name == self.detail.name:
            self.detail.set_journal(pairs, cgroup_path, error)

    @pyqtSlot()
    def _on_reports_ready(self) -> None:
        reports = self.worker.take_reports() if self.worker is not None else None
        if reports is not None:
            self._on_reports(reports)

    @pyqtSlot(list)
    def _on_reports(self, reports: List[dict]):
        new_current: Dict[str, dict] = {}
        for r in reports:
            name = r["processName"]
            r["_cpu_pct"] = cpu_percent(
                self._prev.get(name), r["cpuUsageInUsec"], r["snapshotTime"]
            )
            self._prev[name] = (r["cpuUsageInUsec"], r["snapshotTime"])
            new_current[name] = r

        for gone in set(self._current) - set(new_current):
            self._prev.pop(gone, None)
        self._current = new_current
        if reports:  # one GPU sample for all of them; an empty array says nothing
            self._on_gpu_state(reports[0]["gpuState"])

        now = time.monotonic()
        self._feed.on_report(now)
        self._record_usage(now)
        if self._stale:
            self._stale = False
            self._apply_stale()
        shown = self.detail.name
        if self.stack.currentWidget() is self.detail and shown and shown not in self._current:
            self.sidebar.select_manager()  # the shown process left the reports
        self.sidebar.set_processes(self._current)
        self._refresh_detail()
        if self._auto_select and self._current:
            self._auto_select = False
            self.sidebar.select_process(sorted(self._current)[0])
        self.lbl_last_update.setText(
            f"Last update: {time.strftime('%H:%M:%S')}"
        )
        self.lbl_process_count.setText(f"Processes: {len(self._current)}")
        self._refresh_network()  # which heartbeat senders are missing follows the reports
        self._refresh_feed_status()

    def _record_usage(self, now: float) -> None:
        """Sample every process for the usage graphs and the state band."""
        for name, r in self._current.items():
            state = r.get("state")
            self._usage.add(
                name,
                UsageSample(
                    now,
                    r["_cpu_pct"],
                    r["memoryUsageInBytes"],
                    r.get("gpuUtilPct"),  # None: the manager sent no GPU data
                    r.get("vramBytes"),
                    None if state is None else int(state),
                ),
            )
        self._usage.prune(now)

    @pyqtSlot(str)
    def _on_connection_status(self, status: str):
        if status == "connected":
            # Sockets are up; the feed only counts as live once reports arrive.
            self._feed.on_connected(time.monotonic())
            self._refresh_feed_status()
            return
        self._feed.on_disconnected()
        if status.startswith("error"):
            self._set_link_status(status, "#f44336")
            self.btn_connection.setChecked(True)  # show HOST to change the machine
        else:
            self._set_link_status("Disconnected", "#f44336")

    @pyqtSlot(object)
    def _on_network_status(self, status: NetworkStatus) -> None:
        previous = self._network_status
        self._network_rates = message_rates(previous, status)
        self._network_forwarded_rate = forwarded_rate(previous, status)
        self._network_status = status
        self._network_feed.on_report(time.monotonic())
        if self._network_stale:
            self._network_stale = False
            self.network_page.set_stale(False)
        self._refresh_network()

    @pyqtSlot(list)
    def _on_network_events(self, events: List[NetworkEvent]) -> None:
        self.network_page.add_events(events)

    @pyqtSlot(str)
    def _on_network_connection(self, status: str) -> None:
        if status == "connected":
            self._network_feed.on_connected(time.monotonic())
        else:
            self._network_feed.on_disconnected()
        self._show_network_state()

    def _refresh_network(self) -> None:
        if self._network_status is not None:
            self.network_page.set_status(
                self._network_status, self._current, self._network_rates, self._network_forwarded_rate
            )
        self._show_network_state()

    def _show_network_state(self) -> None:
        """The toolbar pill and the sidebar row of the network manager."""
        status = self._network_status
        if status is None:
            feed = self._network_feed.status(time.monotonic())
            if feed is not None and feed[1] == "stale":
                text, color, hint, kind = "Network · no data", "#f44336", "no data", "badge"
                tip = (f"Connected to {self.network_endpoint}, but no status has come "
                       "(is the process manager running, and publishing on this port?)")
            else:
                text, color, hint, kind = "Network …", "orange", "waiting", "muted"
                tip = f"Waiting for the network manager's status from {self.network_endpoint}"
        elif self._network_stale:
            text, color, hint, kind = "Network stale", "#f44336", "stale", "badge"
            tip = (f"No status from the network manager at {self.network_endpoint} for a while; "
                   "its page shows the last one received (greyed out)")
        else:
            missing = missing_services(self._current, status)
            if missing:
                text, color = f"Network · {len(missing)} missing", "#f44336"
                hint, kind = f"{len(missing)} missing", "badge"
                tip = ("Not connected to the network manager, though they send heartbeats: "
                       + ", ".join(missing))
            else:
                text, color = f"Network · {len(status.dealers)}", "#4caf50"
                hint, kind = str(len(status.dealers)), "muted"  # short: the row's name stays whole
                names = ", ".join(d.name for d in status.dealers) or "none"
                tip = f"{len(status.dealers)} connected to the network manager: {names}"
        self.lbl_network.setText(text)
        self.lbl_network.setStyleSheet(pill_style(color))
        self.lbl_network.setToolTip(tip)
        self.sidebar.set_network_hint(hint, kind, color)

    def _refresh_feed_status(self) -> None:
        now = time.monotonic()
        network_stale = self._network_feed.data_stale(now)
        if network_stale != self._network_stale:
            self._network_stale = network_stale
            self.network_page.set_stale(network_stale)
        self._show_network_state()
        stale = self._feed.data_stale(now)
        if stale != self._stale:
            self._stale = stale
            self._apply_stale()  # grey out / restore the pages
        if self.stack.currentWidget() is self.detail:
            self.detail.tick(now, time.time())
        status = self._feed.status(now)
        if status is None:  # not connected: keep the error / reconnect text
            return
        text, level = status
        tips = {
            "live": f"Receiving health reports from {self.sub_endpoint}",
            "waiting": f"Connected to {self.sub_endpoint}; no health report yet",
            "stale": f"No health reports from {self.sub_endpoint}"
                     + ("; the pages show the last values received (greyed out)"
                        if self._current else ""),
        }
        self._set_link_status(text, FEED_COLORS[level], tips[level])

    def _set_link_status(self, text: str, color: str, tooltip: str = "") -> None:
        if self._link_status == (text, color, tooltip):
            return
        self._link_status = (text, color, tooltip)
        self.lbl_status.setText(text)
        self.lbl_status.setStyleSheet(pill_style(color))
        self.lbl_status.setToolTip(tooltip)

    @pyqtSlot(str)
    def _on_log(self, msg: str):
        self._show_status(msg)

    def _show_gpu_state(self, state: Optional[GpuState]) -> None:
        """The GPU indicator for the manager's GPU state (None: no report yet)."""
        if state == GpuState.SAMPLED:
            text, color = "GPU ok", "#4caf50"
            tip = "GPU use sampled by the process manager through NVML"
        elif state is None or state == GpuState.NOT_SAMPLED:
            text, color = "GPU …", "orange"
            tip = (
                "Waiting for the process manager's first report"
                if state is None else GPU_STATE_TEXT[state]
            )
        else:
            text, color = "GPU unavailable", "#f44336"
            tip = GPU_STATE_TEXT.get(state, "The process manager sends no GPU data")
        self.lbl_gpu.setText(text)
        self.lbl_gpu.setStyleSheet(pill_style(color))
        self.lbl_gpu.setToolTip(tip)

    def _on_gpu_state(self, state: GpuState) -> None:
        if state == self._gpu_state:
            return
        self._gpu_state = state
        self._show_gpu_state(state)
        # Reports come twice a second; post a lost GPU feed once, not each time.
        if state not in (GpuState.SAMPLED, GpuState.NOT_SAMPLED):
            self._show_status(f"No GPU data: {GPU_STATE_TEXT.get(state, state.name)}")

    @pyqtSlot(object, str)
    def _on_cgroup_members(self, mapping, error: str) -> None:
        self._cgroup_members = mapping if isinstance(mapping, dict) else None
        self._cgroup_error = error
        name = self.detail.name
        if name:
            self.detail.set_members(*self._members_of(name))

    def _members_of(self, name: str) -> Tuple[list, Optional[bool], str]:
        """(its processes, whether it has a cgroup or None when unknown, why they could not be read)."""
        if self._cgroup_members is None:
            return [], None, self._cgroup_error
        if name not in self._cgroup_members:
            return [], False, ""
        return self._cgroup_members[name], True, ""

    # ── slots: user actions ───────────────────────────────────────────────

    def _on_pid_activated(self, pid: int, comm: str) -> None:
        self._open_log_window(f"pid:{pid}", pid=pid, comm=comm)

    def _open_log_window(
        self,
        key: str,
        process_name: str = "",
        pid: Optional[int] = None,
        comm: str = "",
    ) -> None:
        existing = self._log_windows.get(key)
        if existing is not None:
            try:
                existing.raise_()
                existing.activateWindow()
                return
            except RuntimeError:
                self._log_windows.pop(key, None)
        win = ProcessLogWindow(
            process_name=process_name, pid=pid, comm=comm, journal_endpoint=self.journal_endpoint
        )
        win.destroyed.connect(lambda *_n, k=key: self._log_windows.pop(k, None))
        self._log_windows[key] = win
        win.show()

    def _open_usage_graphs(self) -> None:
        if self._usage_window is not None:
            try:
                self._usage_window.raise_()
                self._usage_window.activateWindow()
                return
            except RuntimeError:  # already deleted
                self._usage_window = None
        win = UsageGraphWindow(self._usage)
        win.destroyed.connect(lambda *_: setattr(self, "_usage_window", None))
        fit_to_screen(win, QSize(1000, 640))
        self._usage_window = win
        win.show()

    def _reconnect(self):
        self.btn_reconnect.setEnabled(False)
        self._feed.on_disconnected()
        self._set_link_status("Reconnecting…", "orange")

        self._network_feed.on_disconnected()
        stop_workers([self._detach_zmq_worker(), self._detach_network_worker()], 4000)
        self._start_zmq_worker()
        self._start_network_worker()
        # The journal workers keep running and switch endpoints on their next round.
        self._show_endpoints()
        for worker in (
            getattr(self, "systemd_worker", None),
            getattr(self, "detail_journal_worker", None),
            getattr(self, "cgroup_members_worker", None),
        ):
            if worker is not None:
                worker.set_endpoint(self.journal_endpoint)
        for window in list(self._log_windows.values()):
            try:
                window.set_journal_endpoint(self.journal_endpoint)
            except RuntimeError:
                pass
        self.btn_reconnect.setEnabled(True)
        self._show_connecting()

    def _show_connecting(self) -> None:
        machine = self._machine()
        self._show_status(f"Connecting to {'several machines' if machine is None else machine}")

    def _send_cmd(self, cmd: CommandEnum, name: str):
        reply = QMessageBox.question(
            self,
            f"Confirm {cmd.name.title()}",
            f"Are you sure you want to <b>{cmd.name.lower()}</b> process "
            f"<b>{name}</b>?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if reply != QMessageBox.StandardButton.Yes:
            return
        worker = getattr(self, "worker", None)
        try:
            queued = worker is not None and worker.send_command(cmd, name, args="")
        except ValueError as e:
            self._show_status(f"{cmd.name} for {name} not sent: {e}")
            return
        if not queued:
            self._show_status("Not connected — command not sent")
            return
        # The worker reports "Sent …" once the command is actually handed off.
        self._show_status(f"Sending {cmd.name} for {name}…")

    def closeEvent(self, event):
        # Every worker, the log windows' included, is told to stop before any is
        # waited for, so they wind down together.
        pairs = []
        for w in list(self._log_windows.values()):
            try:
                pairs.append(w.detach_worker())
            except RuntimeError:
                pass
        for worker_attr, thread_attr in (
            ("cgroup_members_worker", "cgroup_members_thread"),
            ("detail_journal_worker", "detail_journal_thread"),
            ("systemd_worker", "systemd_thread"),
        ):
            pairs.append(self._detach_background_worker(worker_attr, thread_attr))
        pairs.append(self._detach_zmq_worker())
        pairs.append(self._detach_network_worker())
        stop_workers(pairs)

        for w in list(self._log_windows.values()):
            try:
                w.close()
            except RuntimeError:
                pass
        self._log_windows.clear()
        if self._usage_window is not None:
            try:
                self._usage_window.close()
            except RuntimeError:
                pass
        super().closeEvent(event)


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Process Manager Health Monitor (PyQt6 + ZMQ)"
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="machine the manager runs on: the default host of every endpoint below (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--sub",
        help="ZMQ SUB endpoint (default: tcp://HOST:6667)",
    )
    parser.add_argument(
        "--dealer",
        help="ZMQ DEALER endpoint (default: tcp://HOST:5557)",
    )
    parser.add_argument(
        "--network",
        help="ZMQ SUB endpoint of the network manager's status (default: tcp://HOST:6669)",
    )
    parser.add_argument(
        "--journal",
        help=f"ZMQ endpoint of the manager's journal server (default: tcp://HOST:{JOURNAL_PORT})",
    )
    args = parser.parse_args()

    app = QApplication(sys.argv)
    apply_dark_theme(app)

    win = ProcessMonitorWindow(
        args.sub or endpoint_on(args.host, REPORT_PORT),
        args.dealer or endpoint_on(args.host, COMMAND_PORT),
        args.network or endpoint_on(args.host, NETWORK_PORT),
        journal_endpoint=args.journal or endpoint_on(args.host, JOURNAL_PORT),
    )
    win.show()
    sys.exit(app.exec())


def apply_dark_theme(app: QApplication) -> None:
    """Fusion style with the dark palette every window is designed for."""
    app.setStyle("Fusion")
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window,          QColor(30, 30, 30))
    palette.setColor(QPalette.ColorRole.WindowText,      QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Base,            QColor(25, 25, 25))
    palette.setColor(QPalette.ColorRole.AlternateBase,   QColor(40, 40, 40))
    palette.setColor(QPalette.ColorRole.ToolTipBase,     QColor(40, 40, 40))
    palette.setColor(QPalette.ColorRole.ToolTipText,     QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Text,            QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Button,          QColor(50, 50, 50))
    palette.setColor(QPalette.ColorRole.ButtonText,      QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.BrightText,      QColor(255, 0, 0))
    palette.setColor(QPalette.ColorRole.Link,            QColor(42, 130, 218))
    palette.setColor(QPalette.ColorRole.Highlight,       QColor(42, 130, 218))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor(0, 0, 0))
    app.setPalette(palette)


if __name__ == "__main__":
    main()
