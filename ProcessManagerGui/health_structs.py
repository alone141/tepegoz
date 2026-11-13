"""
Binary layouts matching the C++ side.

Packing: _pack_ = 8  (8-byte alignment)

=== Health reports (ZMQ SUB) ===
Message body = ARRAY of DetailedHealthReport (144 bytes each with pack=8)
  - raw concatenation  OR
  - [uint32_t count LE] + array
Each report ends with the GPU use the manager sampled through NVML (GpuUsage).
Managers from before GPU reporting sent 128-byte reports. A payload that fits both
sizes (9 x 128 = 8 x 144 bytes) is told apart by the GPU fields: memory is 0 unless SAMPLED.

=== Commands (ZMQ DEALER → ROUTER) ===
Identity set to b"GUI" (GUI_IDENTITY); the C++ CLI uses b"PMC"
Multipart:
  frame 0 : b"BPM"
  frame 1 : packed CommandMessage (65 bytes)
The manager answers a command from b"GUI" with [b"BPM"][b"1" accepted | b"0" refused].

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


class GpuState(IntEnum):
    """Wire values must match the C++ GpuUsageState."""
    NOT_SAMPLED = 0  # the manager has not sampled the GPUs yet
    SAMPLED     = 1
    NO_DRIVER   = 2  # libnvidia-ml.so.1 could not be loaded
    NO_DEVICE   = 3  # NVML finds no GPU
    FAILED      = 4  # NVML failed; the manager's journal has its error code
    STALLED     = 5  # no new sample for a while: a driver call is not returning

    @classmethod
    def from_byte(cls, value: int) -> "GpuState":
        try:
            return cls(value)
        except ValueError:
            return cls.NOT_SAMPLED  # a state this GUI does not know: no data


class RuntimeHealth(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("state",        ctypes.c_uint8),
        ("start_time",   ctypes.c_int64),   # ns
        ("lastSeen",     ctypes.c_int64),   # ns
        ("missedBeats",  ctypes.c_int32),
        ("restartCount", ctypes.c_int32),
    ]


class GpuUsage(ctypes.Structure):
    """GPU use of every process of a service, on every GPU."""
    _pack_ = 8
    _fields_ = [
        ("memoryUsageInBytes", ctypes.c_uint64),  # 0 unless state is SAMPLED
        ("utilizationPercent", ctypes.c_int32),  # summed over GPUs (200: two busy GPUs); -1 unknown
        ("state",              ctypes.c_uint8),  # GpuState; the other fields count only when SAMPLED
        ("reserved",           ctypes.c_uint8 * 3),
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
        ("gpuUsage",           GpuUsage),
    ]


REPORT_SIZE = ctypes.sizeof(DetailedHealthReport)
assert REPORT_SIZE == 144, f"Unexpected REPORT_SIZE={REPORT_SIZE}"
LEGACY_REPORT_SIZE = 128  # managers from before GPU reporting


def parse_health_reports(data: bytes) -> List[DetailedHealthReport]:
    """
    Parse ZMQ message that contains an ARRAY of DetailedHealthReport.
    Supports raw concat and length-prefixed (u32 count) formats.
    """
    if not data:
        return []

    layouts = []
    # length-prefixed?
    if len(data) >= 4:
        count = int.from_bytes(data[:4], "little", signed=False)
        body = data[4:]
        if 0 < count < 10_000 and len(body) == count * REPORT_SIZE:
            layouts.append((body, count))

    # raw concatenation
    if len(data) % REPORT_SIZE == 0:
        layouts.append((data, len(data) // REPORT_SIZE))

    for body, n in layouts:
        reports = _unpack_array(body, n)
        if all(_gpu_fields_consistent(r) for r in reports):
            return reports

    mismatch = ""
    if layouts:
        mismatch = f"\n  read as {REPORT_SIZE}-byte reports, a report has GPU memory without a sampled GPU"
    legacy = ""
    if len(data) % LEGACY_REPORT_SIZE == 0:
        legacy = (
            f"\n  {len(data) // LEGACY_REPORT_SIZE} reports of {LEGACY_REPORT_SIZE} bytes fit: "
            "the process manager predates GPU reporting and needs updating"
        )
    raise ValueError(
        f"Cannot parse health-report array.\n"
        f"  received length = {len(data)} bytes\n"
        f"  REPORT_SIZE     = {REPORT_SIZE}\n"
        f"  remainder       = {len(data) % REPORT_SIZE}\n"
        f"  first 32 bytes  = {data[:32].hex(' ')}"
        f"{mismatch}{legacy}"
    )


def _gpu_fields_consistent(report: DetailedHealthReport) -> bool:
    # The manager sends GPU memory only in a SAMPLED report. Bytes of 128-byte reports read as 144-byte ones
    # put the second report's name in the first one's GPU fields, which fails this.
    gpu = report.gpuUsage
    return gpu.state == GpuState.SAMPLED or gpu.memoryUsageInBytes == 0


def _unpack_array(data: bytes, n: int) -> List[DetailedHealthReport]:
    reports = []
    for i in range(n):
        reports.append(
            DetailedHealthReport.from_buffer_copy(data, i * REPORT_SIZE)
        )
    return reports


def report_to_dict(report: DetailedHealthReport) -> dict:
    """The report's fields; gpuUtilPct and vramBytes are None without GPU data."""
    name = report.processName.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    rh = report.runtimeHealth
    gpu = report.gpuUsage
    gpu_state = GpuState.from_byte(gpu.state)
    sampled = gpu_state == GpuState.SAMPLED
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
        "gpuState":           gpu_state,
        "gpuUtilPct":         float(gpu.utilizationPercent)
                              if sampled and gpu.utilizationPercent >= 0 else None,
        "vramBytes":          int(gpu.memoryUsageInBytes) if sampled else None,
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

# The manager's GUI_ID: commands from it are answered, and it does not clash
# with the C++ CLI, which connects as b"PMC".
GUI_IDENTITY = b"GUI"


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


COMMAND_TEXT_MAX_BYTES = 31  # char[32] fields, the last byte stays NUL


def _encode_field(text: str, what: str) -> bytes:
    """UTF-8 bytes for a command field, refusing text that does not fit.

    Truncating would address a different (or no) process: the manager
    matches the whole name.
    """
    raw = text.encode("utf-8")
    if len(raw) > COMMAND_TEXT_MAX_BYTES:
        raise ValueError(
            f"{what} is {len(raw)} bytes; commands carry at most "
            f"{COMMAND_TEXT_MAX_BYTES}"
        )
    return raw


def make_command_message(
    command: CommandEnum,
    service_name: str,
    args: str = "",
) -> bytes:
    """Build the binary payload that is sent as the second multipart frame.

    Raises ValueError when the service name or args do not fit.
    """
    msg = CommandMessage()
    msg.command = int(command)

    # c_char arrays are C strings: assign bytes without forcing a 32-byte
    # pad (ctypes copies and NUL-terminates).
    msg.ServiceName = _encode_field(service_name, "process name")
    msg.Args = _encode_field(args, "args")

    return bytes(msg)
