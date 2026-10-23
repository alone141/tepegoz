"""The network manager page: who is connected to the process manager's broker, and what happened.

Pure widgets, like process_views: the main window feeds them the statuses and
events its NetworkWorker receives (see network_status for the format).
"""

from __future__ import annotations

import time
from typing import Dict, List, Optional, Sequence, Tuple

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QFont
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from network_status import (
    NetworkEvent,
    NetworkStatus,
    dealer_state,
    format_age_ms,
    missing_services,
)
from process_views import (
    DANGER,
    NETWORK_NAME,
    SMALL_BUTTON_STYLE,
    STALE_COLOR,
    TABLE_STYLE,
    TEXT,
    TEXT_DIM,
    TEXT_MUTED,
    MetricTile,
    mono_style,
)
from ui_scale import ui_font

STATE_COLORS: Dict[str, str] = {
    "connected": "#4caf50",
    "quiet": "#ff9800",
    "no name yet": "#9e9e9e",
    "not connected": DANGER,
}
STATE_TIPS: Dict[str, str] = {
    "connected": "Connected to the network manager",
    "quiet": "Connected, but this process has sent nothing (no heartbeat) for a while",
    "no name yet": "Connected but has sent nothing yet, so its name is unknown "
                   "(a dealer that only receives stays like this)",
    "not connected": "This process sends heartbeats, but is not connected to the network manager",
}
EVENT_COLORS: Dict[str, str] = {
    "joined": "#81c784",
    "left": "#bdbdbd",
    "takeover": "#ffb74d",
    "notDelivered": "#e57373",
    "rejected": "#e57373",
    "connected": TEXT_MUTED,
    "closed": TEXT_MUTED,
}
EVENT_LABELS: Dict[str, str] = {
    "joined": "joined",
    "left": "left",
    "takeover": "taken over",
    "notDelivered": "not delivered",
    "rejected": "rejected",
    "connected": "connection",
    "closed": "closed",
}
# The event list keeps this many, newest first.
MAX_EVENTS = 500


def _item(text: str, color: Optional[str] = None, tip: str = "", mono: bool = False) -> QTableWidgetItem:
    item = QTableWidgetItem(text)
    item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
    if color:
        item.setForeground(QColor(color))
    if tip:
        item.setToolTip(tip)
    if mono:
        item.setFont(QFont("monospace"))
    return item


def _clock(ms: int) -> str:
    return time.strftime("%H:%M:%S", time.localtime(ms / 1000.0)) if ms > 0 else "—"


class NetworkPage(QWidget):
    """Header, tiles, the table of connected dealers and the event list."""

    COLUMNS = ("Name", "State", "Address", "Connected for", "Messages", "Rate", "Last message")
    EVENT_COLUMNS = ("Time", "Event", "Name", "Detail")
    TILES = (
        ("connected", "Connected"),
        ("rate", "Forwarded / s"),
        ("not_delivered", "Not delivered"),
        ("rejected", "Rejected"),
    )

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.status: Optional[NetworkStatus] = None
        self._stale = False

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 12, 14, 10)
        root.setSpacing(8)

        header = QHBoxLayout()
        header.setSpacing(10)
        title = QLabel(NETWORK_NAME)
        title.setFont(ui_font(1.45, QFont.Weight.Bold))
        header.addWidget(title)
        self.lbl_endpoint = QLabel("")
        self.lbl_endpoint.setStyleSheet(mono_style(TEXT_DIM))
        header.addWidget(self.lbl_endpoint)
        header.addStretch()
        root.addLayout(header)

        self.lbl_meta = QLabel("Waiting for the network manager's status…")
        self.lbl_meta.setStyleSheet(mono_style(TEXT_DIM))
        self.lbl_meta.setWordWrap(True)
        root.addWidget(self.lbl_meta)

        tiles = QGridLayout()
        tiles.setSpacing(8)
        self.tiles: Dict[str, MetricTile] = {}
        for i, (key, label) in enumerate(self.TILES):
            tile = MetricTile(label)
            tiles.addWidget(tile, 0, i)
            self.tiles[key] = tile
        root.addLayout(tiles)

        # Processes that send heartbeats but are not connected get a line of their own.
        self.lbl_missing = QLabel()
        self.lbl_missing.setStyleSheet(f"color: {DANGER}; font-weight: bold;")
        self.lbl_missing.setWordWrap(True)
        self.lbl_missing.setVisible(False)
        root.addWidget(self.lbl_missing)

        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.setChildrenCollapsible(False)
        splitter.setHandleWidth(6)

        self.table = QTableWidget(0, len(self.COLUMNS))
        self.table.setHorizontalHeaderLabels(self.COLUMNS)
        self._setup_table(self.table)
        splitter.addWidget(self.table)

        events_box = QWidget()
        events_layout = QVBoxLayout(events_box)
        events_layout.setContentsMargins(0, 0, 0, 0)
        events_layout.setSpacing(4)
        events_header = QHBoxLayout()
        lbl_events = QLabel(f"Events · newest first, the last {MAX_EVENTS}")
        lbl_events.setStyleSheet(f"color: {TEXT_DIM};")
        events_header.addWidget(lbl_events)
        events_header.addStretch()
        self.btn_clear = QPushButton("Clear")
        self.btn_clear.setStyleSheet(SMALL_BUTTON_STYLE)
        self.btn_clear.setToolTip("Empty the event list (new events keep coming)")
        self.btn_clear.clicked.connect(lambda: self.events.setRowCount(0))
        events_header.addWidget(self.btn_clear)
        events_layout.addLayout(events_header)
        self.events = QTableWidget(0, len(self.EVENT_COLUMNS))
        self.events.setHorizontalHeaderLabels(self.EVENT_COLUMNS)
        self._setup_table(self.events)
        self.events.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        events_layout.addWidget(self.events, stretch=1)
        splitter.addWidget(events_box)

        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 2)
        root.addWidget(splitter, stretch=1)

    @staticmethod
    def _setup_table(table: QTableWidget) -> None:
        table.setStyleSheet(TABLE_STYLE)
        table.verticalHeader().setVisible(False)
        table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        table.setShowGrid(False)
        table.setWordWrap(False)
        header = table.horizontalHeader()
        header.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
        header.setStretchLastSection(True)

    # ── data in ───────────────────────────────────────────────────────────

    def set_status(
        self,
        status: NetworkStatus,
        reports: Dict[str, dict],
        rates: Dict[str, Optional[float]],
        forwarded_per_s: Optional[float],
    ) -> None:
        self.status = status
        now = status.now_ms
        self.lbl_endpoint.setText(status.endpoint)
        self.lbl_meta.setText(
            f"up {format_age_ms(now - status.started_ms)} · since {_clock(status.started_ms)}"
            f" · status {_clock(now)} · {status.forwarded} forwarded in all"
        )
        missing = missing_services(reports, status)
        values = {
            "connected": str(len(status.dealers)) + (f" + {len(status.unnamed)}" if status.unnamed else ""),
            "rate": "—" if forwarded_per_s is None else f"{forwarded_per_s:.1f}",
            "not_delivered": str(status.not_delivered),
            "rejected": str(status.rejected),
        }
        for key, text in values.items():
            self.tiles[key].set_value(text)
        self.lbl_missing.setText(
            "Not connected to the network manager, though they send heartbeats: " + ", ".join(missing)
        )
        self.lbl_missing.setVisible(bool(missing))

        rows: List[Tuple[str, str, str, str, str, str, str]] = []
        for dealer in status.dealers:
            rate = rates.get(dealer.name)
            rows.append((
                dealer.name,
                dealer_state(dealer, now, reports),
                dealer.address,
                format_age_ms(now - dealer.since_ms) if dealer.since_ms else "—",
                str(dealer.messages),
                "—" if rate is None else f"{rate:.1f}/s",
                f"{format_age_ms(now - dealer.last_message_ms)} ago" if dealer.last_message_ms else "—",
            ))
        for address, since in status.unnamed:
            rows.append(("—", "no name yet", address, format_age_ms(now - since) if since else "—", "0", "—", "—"))
        for name in missing:
            rows.append((name, "not connected", "—", "—", "—", "—", "—"))
        self._fill(rows)

    def _fill(self, rows: Sequence[Tuple[str, ...]]) -> None:
        self.table.setRowCount(len(rows))
        for r, row in enumerate(rows):
            state = row[1]
            color = STALE_COLOR.name() if self._stale else STATE_COLORS.get(state, TEXT)
            text_color = STALE_COLOR.name() if self._stale else (DANGER if state == "not connected" else TEXT)
            for c, text in enumerate(row):
                if c == 1:
                    item = _item(f"● {text}", color, STATE_TIPS.get(state, ""))
                else:
                    item = _item(text, text_color, mono=c == 2)
                if c in (4, 5):
                    item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                self.table.setItem(r, c, item)

    def add_events(self, events: Sequence[NetworkEvent]) -> None:
        """Newest first; the list keeps MAX_EVENTS."""
        for event in events:
            self.events.insertRow(0)
            color = EVENT_COLORS.get(event.kind, TEXT)
            label = EVENT_LABELS.get(event.kind, event.kind)
            for c, text in enumerate((_clock(event.time_ms), label, event.name, event.text)):
                self.events.setItem(0, c, _item(text, color if c in (1, 2) else TEXT_DIM, mono=c == 0))
        while self.events.rowCount() > MAX_EVENTS:
            self.events.removeRow(self.events.rowCount() - 1)

    def set_stale(self, stale: bool) -> None:
        self._stale = stale
        for tile in self.tiles.values():
            tile.set_stale(stale)
        self.lbl_meta.setStyleSheet(mono_style(STALE_COLOR.name() if stale else TEXT_DIM))
        if self.status is not None:
            # Repaint the rows in grey or in their colours.
            rows = [
                tuple(self.table.item(r, c).text() for c in range(self.table.columnCount()))
                for r in range(self.table.rowCount())
            ]
            self._fill([(row[0], row[1][2:]) + row[2:] for row in rows])

    # ── for checks ────────────────────────────────────────────────────────

    def row_states(self) -> Dict[str, str]:
        """Name (or address of an unnamed connection) → its State column, without the dot."""
        states = {}
        for r in range(self.table.rowCount()):
            name = self.table.item(r, 0).text()
            key = self.table.item(r, 2).text() if name == "—" else name
            states[key] = self.table.item(r, 1).text()[2:]
        return states

    def event_rows(self) -> List[Tuple[str, str, str]]:
        """(event, name, detail) per row, newest first."""
        return [
            tuple(self.events.item(r, c).text() for c in (1, 2, 3))
            for r in range(self.events.rowCount())
        ]

    def tile_values(self) -> Dict[str, str]:
        return {key: tile.val.text() for key, tile in self.tiles.items()}
