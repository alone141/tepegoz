"""
The network manager's status, as the process manager publishes it.

The network manager is the ZeroMQ broker inside the process manager: services
send their heartbeats through it, and the CLI and this GUI their commands. It
publishes on a PUB socket (NETWORK_ENDPOINT), in two-frame messages:

  [b"status"][JSON]  every second:
      {"version", "endpoint", "started", "now",
       "totals": {"forwarded", "notDelivered", "rejected"},
       "dealers": [{"name", "address", "since", "messages", "lastMessage"}],
       "unnamed": [{"address", "since"}]}
  [b"event"][JSON]   as things happen:
      {"version", "time", "level", "kind", "name", "text"}
      kind: joined, left, takeover, notDelivered, rejected, connected, closed

Times are milliseconds since the Unix epoch, 0 for "unknown". A dealer gets its
name from its first message, so one that only receives stays in "unnamed".

Pure functions: no Qt, no sockets.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple, Union

NETWORK_ENDPOINT = "tcp://127.0.0.1:6669"
STATUS_VERSION = 1
# A process that sends heartbeats and has said nothing for this long is shown as quiet.
QUIET_AFTER_MS = 10_000
# Names that are not services: the manager's own socket, the CLI and this GUI.
INFRASTRUCTURE_NAMES = ("BPM", "PMC", "GUI")
EVENT_KINDS = ("joined", "left", "takeover", "notDelivered", "rejected", "connected", "closed")


@dataclass
class Dealer:
    name: str
    address: str
    since_ms: int
    messages: int
    last_message_ms: int


@dataclass
class NetworkStatus:
    endpoint: str
    started_ms: int
    now_ms: int
    forwarded: int
    not_delivered: int
    rejected: int
    dealers: List[Dealer] = field(default_factory=list)
    unnamed: List[Tuple[str, int]] = field(default_factory=list)  # (address, since_ms)


@dataclass
class NetworkEvent:
    time_ms: int
    level: str
    kind: str
    name: str
    text: str


def parse_message(frames: Sequence[bytes]) -> Optional[Union[NetworkStatus, NetworkEvent]]:
    """One published message; None for a topic or version this GUI does not know.

    Raises ValueError when the message is not what the format above says.
    """
    if len(frames) != 2:
        raise ValueError(f"expected 2 frames (topic, JSON), got {len(frames)}")
    topic, payload = bytes(frames[0]), bytes(frames[1])
    if topic not in (b"status", b"event"):
        return None
    try:
        data = json.loads(payload.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as e:
        raise ValueError(f"{topic.decode()} is not JSON: {e}") from None
    if not isinstance(data, dict):
        raise ValueError(f"{topic.decode()} is not a JSON object")
    if data.get("version") != STATUS_VERSION:
        return None  # a newer process manager: skip rather than misread
    try:
        if topic == b"event":
            return NetworkEvent(
                int(data["time"]), str(data["level"]), str(data["kind"]),
                str(data["name"]), str(data["text"]),
            )
        totals = data["totals"]
        return NetworkStatus(
            endpoint=str(data["endpoint"]),
            started_ms=int(data["started"]),
            now_ms=int(data["now"]),
            forwarded=int(totals["forwarded"]),
            not_delivered=int(totals["notDelivered"]),
            rejected=int(totals["rejected"]),
            dealers=[
                Dealer(str(d["name"]), str(d["address"]), int(d["since"]),
                       int(d["messages"]), int(d["lastMessage"]))
                for d in data["dealers"]
            ],
            unnamed=[(str(u["address"]), int(u["since"])) for u in data["unnamed"]],
        )
    except (KeyError, TypeError, ValueError) as e:
        raise ValueError(f"{topic.decode()} misses or mistypes a field: {e!r}") from None


def message_rates(
    previous: Optional[NetworkStatus], current: NetworkStatus
) -> Dict[str, Optional[float]]:
    """Messages per second each dealer sent between two statuses; None without a usable earlier one."""
    if previous is None or current.now_ms <= previous.now_ms:
        return {d.name: None for d in current.dealers}
    seconds = (current.now_ms - previous.now_ms) / 1000.0
    before = {d.name: d for d in previous.dealers}
    rates: Dict[str, Optional[float]] = {}
    for dealer in current.dealers:
        old = before.get(dealer.name)
        # A dealer that reconnected starts counting again.
        if old is None or old.since_ms != dealer.since_ms or dealer.messages < old.messages:
            rates[dealer.name] = None
        else:
            rates[dealer.name] = (dealer.messages - old.messages) / seconds
    return rates


def forwarded_rate(previous: Optional[NetworkStatus], current: NetworkStatus) -> Optional[float]:
    """Messages per second the broker forwarded between two statuses."""
    if previous is None or current.now_ms <= previous.now_ms or current.started_ms != previous.started_ms:
        return None
    return (current.forwarded - previous.forwarded) / ((current.now_ms - previous.now_ms) / 1000.0)


def expects_connection(report: dict) -> bool:
    """A running process that has sent heartbeats should be connected to the broker."""
    from health_structs import RuntimeState  # local: keeps this module importable alone

    return bool(report.get("lastSeen")) and report.get("state") in (
        RuntimeState.RUNNING, RuntimeState.UNHEALTHY,
    )


def missing_services(reports: Dict[str, dict], status: NetworkStatus) -> List[str]:
    """Processes that should be connected (see expects_connection) but are not, sorted."""
    connected = {d.name for d in status.dealers}
    return sorted(
        name for name, report in reports.items()
        if expects_connection(report) and name not in connected
    )


def dealer_state(dealer: Dealer, now_ms: int, reports: Dict[str, dict]) -> str:
    """ "connected", or "quiet" for a process that sends heartbeats yet has been silent too long."""
    report = reports.get(dealer.name)
    if (
        report is not None
        and expects_connection(report)
        and dealer.last_message_ms
        and now_ms - dealer.last_message_ms > QUIET_AFTER_MS
    ):
        return "quiet"
    return "connected"


def format_age_ms(ms: int) -> str:
    """45s, 12m 03s, 5h 07m or 2d 03h."""
    seconds = max(0, ms) // 1000
    if seconds < 60:
        return f"{seconds}s"
    if seconds < 3600:
        return f"{seconds // 60}m {seconds % 60:02d}s"
    if seconds < 86400:
        return f"{seconds // 3600}h {seconds % 3600 // 60:02d}m"
    return f"{seconds // 86400}d {seconds % 86400 // 3600:02d}h"
