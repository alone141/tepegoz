"""The Record logs dialog: which services to record, and into which folder.

Pure widgets, like process_views: the main window starts and stops the
recording (log_recorder, in a worker thread) and feeds the dialog its states.
"""

from __future__ import annotations

import os
from typing import Dict, List, Optional, Sequence

from PyQt6.QtCore import Qt, QUrl, pyqtSignal
from PyQt6.QtGui import QColor, QDesktopServices
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QPushButton,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from health_structs import CommandEnum
from log_recorder import RecordingState
from process_views import (
    ACCENT,
    ACTION_STYLES,
    BG,
    BORDER,
    DANGER,
    SMALL_BUTTON_STYLE,
    TEXT,
    TEXT_DIM,
    TEXT_MUTED,
)
from ui_scale import MONO_FONT_FAMILIES

FOLDER_STYLE = f"""
    QLineEdit {{
        background: #1a1a1a; color: {TEXT}; border: 1px solid #3a3a3a;
        border-radius: 3px; padding: 3px 6px; font-family: {MONO_FONT_FAMILIES};
    }}
    QLineEdit:focus {{ border-color: {ACCENT}; }}
    QLineEdit:disabled {{ color: {TEXT_MUTED}; }}
"""


def tree_style(box: int) -> str:
    # The default check box is invisible on the dark palette; draw an outline, filled when ticked.
    return f"""
        QTreeWidget {{ background: {BG}; color: {TEXT}; border: 1px solid {BORDER}; }}
        QTreeWidget::indicator {{
            width: {box}px; height: {box}px; border-radius: 3px;
            border: 1px solid #777; background: #1a1a1a;
        }}
        QTreeWidget::indicator:checked {{ background: {ACCENT}; border-color: {ACCENT}; }}
        QHeaderView::section {{
            background: #2d2d2d; color: {TEXT_DIM}; padding: 4px 6px;
            border: none; font-weight: bold;
        }}
    """


class RecordLogsDialog(QDialog):
    """Tick services and press Record: each one's new log lines go to a file of its own until Stop."""

    record_requested = pyqtSignal(list, str)  # names, folder
    stop_requested = pyqtSignal()

    COLUMNS = ("Service", "Lines", "File")

    def __init__(self, folder: str, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setWindowTitle("Record logs")
        self._recording = False
        self._states: Dict[str, RecordingState] = {}
        self._items: Dict[str, QTreeWidgetItem] = {}

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 12, 14, 12)
        root.setSpacing(8)
        intro = QLabel(
            "Tick the services to record and press Record. From then on, each one's new log lines go to a "
            "file of its own on this machine, until you press Stop."
        )
        intro.setWordWrap(True)
        intro.setStyleSheet(f"color: {TEXT_DIM};")
        root.addWidget(intro)

        folder_row = QHBoxLayout()
        folder_row.setSpacing(8)
        folder_row.addWidget(QLabel("Folder"))
        self.edit_folder = QLineEdit(folder)
        self.edit_folder.setStyleSheet(FOLDER_STYLE)
        self.edit_folder.setToolTip("The recordings' folder on this machine; it is made if missing")
        folder_row.addWidget(self.edit_folder, 1)
        self.btn_browse = QPushButton("Browse…")
        self.btn_browse.setStyleSheet(SMALL_BUTTON_STYLE)
        self.btn_browse.clicked.connect(self._browse)
        folder_row.addWidget(self.btn_browse)
        root.addLayout(folder_row)

        self.tree = QTreeWidget()
        self.tree.setColumnCount(len(self.COLUMNS))
        self.tree.setHeaderLabels(list(self.COLUMNS))
        self.tree.setRootIsDecorated(False)
        self.tree.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        self.tree.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.tree.setStyleSheet(tree_style(max(10, self.fontMetrics().height() - 4)))
        header = self.tree.header()
        header.setStretchLastSection(True)
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self.tree.itemChanged.connect(lambda _item, _column: self._show_buttons())
        root.addWidget(self.tree, 1)

        self.lbl_summary = QLabel()
        self.lbl_summary.setWordWrap(True)
        self.lbl_summary.setStyleSheet(f"color: {TEXT_MUTED};")
        root.addWidget(self.lbl_summary)

        buttons = QHBoxLayout()
        buttons.setSpacing(8)
        self.btn_open = QPushButton("Open folder")
        self.btn_open.setStyleSheet(SMALL_BUTTON_STYLE)
        self.btn_open.clicked.connect(self._open_folder)
        buttons.addWidget(self.btn_open)
        buttons.addStretch()
        self.btn_record = QPushButton("Record")
        self.btn_record.clicked.connect(self._on_record_clicked)
        buttons.addWidget(self.btn_record)
        self.btn_close = QPushButton("Close")
        self.btn_close.setStyleSheet(SMALL_BUTTON_STYLE)
        self.btn_close.clicked.connect(self.close)
        buttons.addWidget(self.btn_close)
        root.addLayout(buttons)

        width = self.fontMetrics().averageCharWidth() * 90
        self.resize(width, width * 2 // 3)
        self._show_buttons()

    # ── data in ───────────────────────────────────────────────────────────

    def folder(self) -> str:
        text = self.edit_folder.text().strip()
        return os.path.normpath(os.path.expanduser(text)) if text else ""

    def checked(self) -> List[str]:
        return [name for name, item in self._items.items()
                if item.checkState(0) == Qt.CheckState.Checked]

    def set_services(self, names: Sequence[str], ticked: Sequence[str] = ()) -> None:
        """The services to offer, with ``ticked`` ticked; a service still recorded or shown stays listed."""
        keep = set(names) | set(self._states)
        self.tree.blockSignals(True)
        for name in [n for n in self._items if n not in keep and n not in self.checked()]:
            self.tree.takeTopLevelItem(self.tree.indexOfTopLevelItem(self._items.pop(name)))
        for name in sorted(keep):
            if name not in self._items:
                item = QTreeWidgetItem([name, "", ""])
                item.setTextAlignment(1, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                item.setCheckState(0, Qt.CheckState.Unchecked)
                self._items[name] = item
                self.tree.addTopLevelItem(item)
                self._show_state(name)
            if name in ticked:
                self._items[name].setCheckState(0, Qt.CheckState.Checked)
        self.tree.sortItems(0, Qt.SortOrder.AscendingOrder)
        self.tree.blockSignals(False)
        self._set_checkable(not self._recording)
        self._show_buttons()

    def set_recording(self, recording: bool) -> None:
        """While recording, the ticks and the folder stay as they were when it started."""
        self._recording = recording
        self.edit_folder.setEnabled(not recording)
        self.btn_browse.setEnabled(not recording)
        self._set_checkable(not recording)
        self._show_buttons()

    def set_states(self, states: Sequence[RecordingState]) -> None:
        self._states = {state.name: state for state in states}
        self.set_services(list(self._items))
        for name in self._items:
            self._show_state(name)
        self._show_summary()

    # ── display ───────────────────────────────────────────────────────────

    def _set_checkable(self, checkable: bool) -> None:
        self.tree.blockSignals(True)
        for item in self._items.values():
            flags = item.flags()
            if checkable:
                flags |= Qt.ItemFlag.ItemIsUserCheckable
            else:
                flags &= ~Qt.ItemFlag.ItemIsUserCheckable
            item.setFlags(flags)
        self.tree.blockSignals(False)

    def _show_state(self, name: str) -> None:
        item = self._items[name]
        state = self._states.get(name)
        if state is None:
            item.setText(1, "")
            item.setText(2, "")
            item.setToolTip(2, "")
            return
        item.setText(1, str(state.lines))
        if state.error:
            item.setText(2, state.error)
            item.setForeground(2, QColor(DANGER))
            item.setToolTip(2, f"{state.path}\n{state.error}")
        else:
            item.setText(2, os.path.basename(state.path))
            item.setForeground(2, QColor(TEXT if state.active else TEXT_MUTED))
            item.setToolTip(2, state.path)
        item.setForeground(1, QColor(TEXT if state.active else TEXT_MUTED))

    def _show_summary(self) -> None:
        active = [s for s in self._states.values() if s.active]
        stopped = [s for s in self._states.values() if not s.active]
        if active:
            plural = "service" if len(active) == 1 else "services"
            self.lbl_summary.setText(f"Recording {len(active)} {plural} into {self.folder()}")
        elif stopped:
            lines = sum(s.lines for s in stopped)
            files = "file" if len(stopped) == 1 else "files"
            self.lbl_summary.setText(
                f"Stopped: {lines} lines in {len(stopped)} {files} in {os.path.dirname(stopped[0].path)}"
            )
        else:
            self.lbl_summary.setText("")

    def _show_buttons(self) -> None:
        command = CommandEnum.STOP if self._recording else CommandEnum.START
        self.btn_record.setText("Stop" if self._recording else "Record")
        self.btn_record.setStyleSheet(ACTION_STYLES[command])
        self.btn_record.setEnabled(self._recording or bool(self.checked()))

    # ── events ────────────────────────────────────────────────────────────

    def _on_record_clicked(self) -> None:
        if self._recording:
            self.stop_requested.emit()
            return
        names = self.checked()
        if names and self.folder():
            self.record_requested.emit(names, self.folder())

    def _browse(self) -> None:
        start = self.folder() if os.path.isdir(self.folder()) else os.path.expanduser("~")
        chosen = QFileDialog.getExistingDirectory(self, "Folder for the recordings", start)
        if chosen:
            self.edit_folder.setText(os.path.normpath(chosen))

    def _open_folder(self) -> None:
        folder = self.folder()
        if os.path.isdir(folder):
            QDesktopServices.openUrl(QUrl.fromLocalFile(folder))
        else:
            self.lbl_summary.setText(f"{folder} does not exist yet: it is made when recording starts.")
