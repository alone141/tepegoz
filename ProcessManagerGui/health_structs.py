"""
Binary layouts matching the C++ side.

Packing: _pack_ = 8  (8-byte alignment)

=== Health reports (ZMQ SUB) ===
Message body = ARRAY of DetailedHealthReport (128 bytes each with pack=8)
  - raw concatenation  OR
  - [uint32_t count LE] + array

=== Commands (ZMQ DEALER → ROUTER) ===
Identity set to b"PMC"
Multipart:
  frame 0 : b"BPM"
  frame 1 : packed CommandMessage (65 bytes)

struct CommandMessage {
    uint8_t command;          // CommandEnum
    char    ServiceName[32];
    char    Args[32];         // may be empty
};
"""

from __future__ import annotations

import ctypes
from enum import IntEnum
from typing import List


# ──────────────────────────────────────────────────────────────────────────────
# Runtime / Health
# ──────────────────────────────────────────────────────────────────────────────

class RuntimeState(IntEnum):
    """Wire values must match the C++ RuntimeState enum."""
    UNKNOWN   = 0
    STARTING  = 1
    RUNNING   = 2
    STOPPED   = 3
    UNHEALTHY = 4

    @classmethod
    def from_byte(cls, value: int) -> "RuntimeState":
        try:
            return cls(value)
        except ValueError:
            return cls.UNKNOWN


class RuntimeHealth(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("state",        ctypes.c_uint8),
        ("start_time",   ctypes.c_int64),   # ns
        ("lastSeen",     ctypes.c_int64),   # ns
        ("missedBeats",  ctypes.c_int32),
        ("restartCount", ctypes.c_int32),
    ]


class DetailedHealthReport(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("processName",        ctypes.c_char * 64),
        ("pid",                ctypes.c_int32),
        ("memoryUsageInBytes", ctypes.c_uint64),
        ("cpuUsageInUsec",     ctypes.c_uint64),
        ("runtimeHealth",      RuntimeHealth),
        ("snapshotTime",       ctypes.c_int64),  # ns
    ]


REPORT_SIZE = ctypes.sizeof(DetailedHealthReport)
assert REPORT_SIZE == 128, f"Unexpected REPORT_SIZE={REPORT_SIZE}"


def parse_health_reports(data: bytes) -> List[DetailedHealthReport]:
    """
    Parse ZMQ message that contains an ARRAY of DetailedHealthReport.
    Supports raw concat and length-prefixed (u32 count) formats.
    """
    if not data:
        return []

    # length-prefixed?
    if len(data) >= 4:
        count = int.from_bytes(data[:4], "little", signed=False)
        body = data[4:]
        if 0 < count < 10_000 and len(body) == count * REPORT_SIZE:
            return _unpack_array(body, count)

    # raw concatenation
    if len(data) % REPORT_SIZE == 0:
        n = len(data) // REPORT_SIZE
        if n > 0:
            return _unpack_array(data, n)

    raise ValueError(
        f"Cannot parse health-report array.\n"
        f"  received length = {len(data)} bytes\n"
        f"  REPORT_SIZE     = {REPORT_SIZE}\n"
        f"  remainder       = {len(data) % REPORT_SIZE}\n"
        f"  first 32 bytes  = {data[:32].hex(' ')}"
    )


def _unpack_array(data: bytes, n: int) -> List[DetailedHealthReport]:
    reports = []
    for i in range(n):
        reports.append(
            DetailedHealthReport.from_buffer_copy(data, i * REPORT_SIZE)
        )
    return reports


def report_to_dict(report: DetailedHealthReport) -> dict:
    name = report.processName.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    rh = report.runtimeHealth
    return {
        "processName":        name,
        "pid":                int(report.pid),
        "memoryUsageInBytes": int(report.memoryUsageInBytes),
        "cpuUsageInUsec":     int(report.cpuUsageInUsec),
        "state":              RuntimeState.from_byte(rh.state),
        "start_time":         int(rh.start_time),
        "lastSeen":           int(rh.lastSeen),
        "missedBeats":        int(rh.missedBeats),
        "restartCount":       int(rh.restartCount),
        "snapshotTime":       int(report.snapshotTime),
    }


def format_duration_ns(ns: int) -> str:
    if ns < 0:
        return "—"
    total_sec = ns / 1_000_000_000
    if total_sec < 60:
        return f"{total_sec:.1f}s"
    minutes, sec = divmod(int(total_sec), 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        return f"{hours:02d}:{minutes:02d}:{sec:02d}"
    return f"{minutes:02d}:{sec:02d}"


def format_bytes(n: int) -> str:
    if n < 1024:
        return f"{n} B"
    for unit, div in (("KB", 1024), ("MB", 1024**2), ("GB", 1024**3), ("TB", 1024**4)):
        if n < div * 1024:
            return f"{n / div:.1f} {unit}"
    return f"{n / 1024**4:.1f} TB"


# ──────────────────────────────────────────────────────────────────────────────
# Commands (DEALER)
# ──────────────────────────────────────────────────────────────────────────────

class CommandEnum(IntEnum):
    """Must match the C++ CommandEnum wire values."""
    START   = 78
    STOP    = 79
    RESTART = 81


class CommandMessage(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("command",     ctypes.c_uint8),      # CommandEnum
        ("ServiceName", ctypes.c_char * 32),
        ("Args",        ctypes.c_char * 32),
    ]


COMMAND_SIZE = ctypes.sizeof(CommandMessage)
assert COMMAND_SIZE == 65, f"Unexpected COMMAND_SIZE={COMMAND_SIZE}"


def _utf8_fit(text: str, max_bytes: int) -> bytes:
    """Encode UTF-8 without splitting a multi-byte character."""
    raw = text.encode("utf-8")
    if len(raw) <= max_bytes:
        return raw
    raw = raw[:max_bytes]
    while raw and (raw[-1] & 0xC0) == 0x80:
        raw = raw[:-1]
    if raw and (raw[-1] & 0xC0) == 0xC0:
        raw = raw[:-1]
    return raw


def make_command_message(
    command: CommandEnum,
    service_name: str,
    args: str = "",
) -> bytes:
    """Build the binary payload that is sent as the second multipart frame."""
    msg = CommandMessage()
    msg.command = int(command)

    # c_char arrays are C strings: assign bytes without forcing a 32-byte
    # pad (ctypes copies and NUL-terminates). Cap at 31 so the last byte
    # stays NUL — same as the original packer.
    name_b = _utf8_fit(service_name, 31)
    args_b = _utf8_fit(args, 31)
    msg.ServiceName = name_b
    msg.Args = args_b

    return bytes(msg)
