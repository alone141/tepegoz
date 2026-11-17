"""The service dialog: register a service with the manager, or change or copy one.

Pure widgets, like record_view: the dialog builds the request (service_registry) and asks for it with
``request_wanted``; the main window sends it through its registry worker and gives the answer back with
``show_reply``. Check sends the request as a dry run and shows the YAML the manager would write, or its problems.
"""

from __future__ import annotations

import os
from typing import Dict, List, Optional, Sequence, Tuple

from PyQt6.QtCore import QLocale, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QDoubleValidator, QIntValidator
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QDialog,
    QFileDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QTableWidget,
    QTableWidgetItem,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from health_structs import CommandEnum
from process_views import (
    ACCENT,
    ACTION_STYLES,
    BG,
    BORDER,
    DANGER,
    LOG_BG,
    SMALL_BUTTON_STYLE,
    TABLE_STYLE,
    TEXT,
    TEXT_DIM,
    TEXT_MUTED,
)
from service_registry import (
    ADVANCED_FIELDS,
    ARGS,
    CWD,
    DEPENDS,
    ENVIRONMENT,
    EXECUTABLE,
    OK,
    RegistryReply,
    ServiceForm,
    add_request,
    build_definition,
    check_name,
    other_keys,
    problem_fields,
    replace_request,
)
from ui_scale import MONO_FONT_FAMILIES, ui_font

ADD_MODE, EDIT_MODE, DUPLICATE_MODE = "add", "edit", "duplicate"
# The texts of an unset choice or flag: the manager's default applies.
DEFAULT_PREFIX = "Default: "
BOOL_CHOICES = (("true", "Yes"), ("false", "No"))

FIELD_STYLE = f"""
    QLineEdit, QComboBox {{
        background: #1a1a1a; color: {TEXT}; border: 1px solid #3a3a3a;
        border-radius: 3px; padding: 3px 6px;
    }}
    QLineEdit:focus, QComboBox:focus {{ border-color: {ACCENT}; }}
    QLineEdit:read-only {{ color: {TEXT_DIM}; background: #222; }}
    QLineEdit[problem="true"], QComboBox[problem="true"] {{ border-color: {DANGER}; }}
"""
PREVIEW_STYLE = (
    f"QPlainTextEdit {{ background: {LOG_BG}; color: {TEXT}; "
    f"font-family: {MONO_FONT_FAMILIES}; border: 1px solid {BORDER}; }}"
)
LIST_STYLE = f"""
    QListWidget, QTableWidget {{ background: {BG}; color: {TEXT}; border: 1px solid {BORDER}; }}
    QListWidget::indicator {{
        width: 12px; height: 12px; border-radius: 3px; border: 1px solid #777; background: #1a1a1a;
    }}
    QListWidget::indicator:checked {{ background: {ACCENT}; border-color: {ACCENT}; }}
"""
CHECK_STYLE = f"""
    QCheckBox {{ color: {TEXT}; }}
    QCheckBox::indicator {{
        width: 12px; height: 12px; border-radius: 3px; border: 1px solid #777; background: #1a1a1a;
    }}
    QCheckBox::indicator:checked {{ background: {ACCENT}; border-color: {ACCENT}; }}
"""
FOLD_STYLE = f"""
    QToolButton {{ color: {TEXT}; background: transparent; border: none; font-weight: bold; padding: 2px 0; }}
    QToolButton:hover {{ color: white; }}
"""
REGISTER_STYLE = ACTION_STYLES[CommandEnum.START]
SECTION_STYLE = f"color: {TEXT_DIM}; font-weight: bold; padding-top: 6px;"


class ServiceDialog(QDialog):
    """Add, Edit or Duplicate a service: the fields up front, the rest folded in Advanced, Check and Register.

    ``request_wanted(operation, params, dry_run)`` asks the window to send a request; the window answers with
    ``show_reply``. An accepted change that is not a dry run closes the dialog with ``registered(reply)``.
    """

    request_wanted = pyqtSignal(str, dict, bool)  # operation, parameters, dry run
    registered = pyqtSignal(object)  # RegistryReply of the change

    def __init__(
        self,
        mode: str,
        form: ServiceForm,
        services: Sequence[str],
        base: Optional[dict] = None,
        browse: bool = False,
        source: str = "",
        notice: str = "",
        parent: Optional[QWidget] = None,
    ):
        super().__init__(parent)
        self.mode = mode
        self.base = base  # the definition the form was filled from: its other keys are kept
        self._busy = False
        self._pending_dry_run = False
        self._inputs: Dict[str, QWidget] = {}
        self._problem_labels: Dict[str, QLabel] = {}
        self._advanced_keys = {field.key for field in ADVANCED_FIELDS} | {DEPENDS, ENVIRONMENT}

        titles = {
            ADD_MODE: "Add service",
            EDIT_MODE: f"Edit {form.name}",
            DUPLICATE_MODE: f"Duplicate {source}",
        }
        self.setWindowTitle(titles.get(mode, "Service"))
        self.setStyleSheet(FIELD_STYLE + CHECK_STYLE)

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 12, 14, 12)
        root.setSpacing(8)

        intro = QLabel(self._intro_text(mode, source))
        intro.setWordWrap(True)
        intro.setStyleSheet(f"color: {TEXT_DIM};")
        root.addWidget(intro)
        kept = other_keys(base)
        if kept:
            self.lbl_kept = QLabel("Also in its definition, and kept as they are: " + ", ".join(kept))
            self.lbl_kept.setWordWrap(True)
            self.lbl_kept.setStyleSheet(f"color: {TEXT_MUTED};")
            root.addWidget(self.lbl_kept)
        if notice:
            self.lbl_notice = QLabel(notice)
            self.lbl_notice.setWordWrap(True)
            self.lbl_notice.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            self.lbl_notice.setStyleSheet(f"color: {DANGER};")
            root.addWidget(self.lbl_notice)

        # The fields scroll when the folded part opens on a small screen; the answer stays in view below them.
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        content = QWidget()
        scroll.setWidget(content)
        fields = QVBoxLayout(content)
        fields.setContentsMargins(0, 0, 4, 0)
        fields.setSpacing(6)

        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(2)
        grid.setColumnStretch(1, 1)
        self.edit_name = QLineEdit(form.name)
        self.edit_name.setPlaceholderText("camera")
        self.edit_name.setToolTip("Its ProcessName and its file name: 1 to 31 letters, digits, _, - and .")
        self.edit_name.setReadOnly(mode == EDIT_MODE)
        self._add_row(grid, "Name", "name", self.edit_name)

        self.edit_executable = QLineEdit(form.values.get(EXECUTABLE, ""))
        self.edit_executable.setPlaceholderText("/opt/app/bin/server")
        self.edit_executable.setToolTip("Absolute path of the program on the manager's machine; the manager checks it")
        self.btn_browse_executable: Optional[QPushButton] = None
        extra = None
        if browse:
            self.btn_browse_executable = self._small_button("Browse…", self._browse_executable)
            extra = self.btn_browse_executable
        self._add_row(grid, "Executable", EXECUTABLE, self.edit_executable, extra)

        self.edit_args = QLineEdit(form.values.get(ARGS, ""))
        self.edit_args.setPlaceholderText("--port 8080 --name 'two words'")
        self.edit_args.setToolTip("The arguments on one line, split like a shell splits them: quote an argument with spaces")
        self._add_row(grid, "Arguments", ARGS, self.edit_args)

        self.edit_cwd = QLineEdit(form.values.get(CWD, ""))
        self.edit_cwd.setPlaceholderText("the manager's own")
        self.edit_cwd.setToolTip("The working directory on the manager's machine; empty keeps the manager's")
        self.btn_browse_cwd: Optional[QPushButton] = None
        extra = None
        if browse:
            self.btn_browse_cwd = self._small_button("Browse…", self._browse_cwd)
            extra = self.btn_browse_cwd
        self._add_row(grid, "Working folder", CWD, self.edit_cwd, extra)
        fields.addLayout(grid)

        if mode == EDIT_MODE:
            self.check_now = QCheckBox("Restart now to apply")
            self.check_now.setChecked(False)
            self.check_now.setToolTip(
                "Off: a running service keeps running and is marked \"restart to apply\" until its next start. "
                "On: it restarts at once; a stopped one takes the new definition and stays stopped"
            )
        else:
            self.check_now = QCheckBox("Start it now")
            self.check_now.setChecked(True)
            self.check_now.setToolTip(
                "On: the service starts by its work mode. Off: it is registered and held until you start it"
            )
        fields.addWidget(self.check_now)

        self.btn_advanced = QToolButton()
        self.btn_advanced.setCheckable(True)
        self.btn_advanced.setStyleSheet(FOLD_STYLE)
        self.btn_advanced.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
        self.btn_advanced.toggled.connect(self._show_advanced)
        fields.addWidget(self.btn_advanced)
        self.advanced = self._build_advanced(form, services)
        fields.addWidget(self.advanced)
        fields.addStretch(1)
        root.addWidget(scroll, 1)

        # The manager's answer: what it would write, or why it refused.
        self.lbl_result = QLabel()
        self.lbl_result.setWordWrap(True)
        self.lbl_result.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.lbl_result.setVisible(False)
        root.addWidget(self.lbl_result)
        self.txt_result = QPlainTextEdit()
        self.txt_result.setReadOnly(True)
        self.txt_result.setStyleSheet(PREVIEW_STYLE)
        self.txt_result.setVisible(False)
        self.txt_result.setMaximumHeight(self.fontMetrics().height() * 12)
        root.addWidget(self.txt_result)

        buttons = QHBoxLayout()
        buttons.setSpacing(8)
        self.btn_check = self._small_button("Check", lambda: self._submit(True))
        self.btn_check.setToolTip("Ask the manager whether it would take this, and show the file it would write")
        buttons.addWidget(self.btn_check)
        buttons.addStretch()
        self.btn_register = QPushButton("Save" if mode == EDIT_MODE else "Register")
        self.btn_register.setStyleSheet(REGISTER_STYLE)
        self.btn_register.clicked.connect(lambda: self._submit(False))
        buttons.addWidget(self.btn_register)
        self.btn_cancel = self._small_button("Cancel", self.reject)
        buttons.addWidget(self.btn_cancel)
        root.addLayout(buttons)
        # Enter checks; only a click on Register changes anything.
        for button in (self.btn_register, self.btn_cancel):
            button.setAutoDefault(False)
        self.btn_check.setDefault(True)

        self._fill_advanced(form)
        self.btn_advanced.setChecked(False)
        self._show_advanced(False)
        width = self.fontMetrics().averageCharWidth() * 110
        self.resize(width, width * 3 // 4)
        (self.edit_executable if mode == EDIT_MODE else self.edit_name).setFocus()

    # ── layout helpers ────────────────────────────────────────────────────

    @staticmethod
    def _intro_text(mode: str, source: str) -> str:
        if mode == EDIT_MODE:
            return ("Change the service's definition. The manager checks it, rewrites the service's file in "
                    "services.d and reloads. Keys this form does not show are kept as they are.")
        if mode == DUPLICATE_MODE:
            return (f"A new service, filled from the definition of {source}. Give it a name of its own; the manager "
                    "checks it, writes its file in services.d and reloads.")
        return ("Register a new service with the manager. It checks the service on its own machine, writes its "
                "file in services.d and reloads. Only the fields you fill in are written.")

    def _small_button(self, text: str, slot) -> QPushButton:
        button = QPushButton(text)
        button.setStyleSheet(SMALL_BUTTON_STYLE + "QPushButton:disabled { background: #333; color: #777; }")
        button.clicked.connect(slot)
        return button

    def _problem_label(self, key: str) -> QLabel:
        label = QLabel()
        label.setWordWrap(True)
        label.setStyleSheet(f"color: {DANGER};")
        label.setFont(ui_font(0.9))
        label.setVisible(False)
        label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self._problem_labels[key] = label
        return label

    def _add_row(self, grid: QGridLayout, text: str, key: str, widget: QWidget,
                 extra: Optional[QWidget] = None) -> None:
        """A label, the field (and a button), and a problem line under the field that shows when it has one."""
        row = grid.rowCount()
        label = QLabel(text)
        label.setStyleSheet(f"color: {TEXT};")
        grid.addWidget(label, row, 0)
        grid.addWidget(widget, row, 1)
        if extra is not None:
            grid.addWidget(extra, row, 2)
        grid.addWidget(self._problem_label(key), row + 1, 1, 1, 2)
        if widget.toolTip():
            label.setToolTip(widget.toolTip())
        self._inputs[key] = widget

    def _section(self, layout: QVBoxLayout, text: str) -> None:
        label = QLabel(text)
        label.setStyleSheet(SECTION_STYLE)
        layout.addWidget(label)

    def _build_advanced(self, form: ServiceForm, services: Sequence[str]) -> QWidget:
        box = QWidget()
        layout = QVBoxLayout(box)
        layout.setContentsMargins(16, 0, 0, 0)
        layout.setSpacing(4)
        self._choices: Dict[str, QComboBox] = {}
        self._numbers: Dict[str, QLineEdit] = {}
        sections = (
            ("Running", ("WorkMode",)),
            ("Restarts", ("RestartMode", "MaxRestarts", "RestartDelayMs", "BackoffMultiplier", "RestartResetMs")),
            ("Health check", ("CheckHealth", "GracePeriodMs", "HeartbeatPeriodMs", "MissedHeartbeatThreshold")),
            ("Output", ("StdioMode", "StdioFilePath", "StdioAppend")),
        )
        by_key = {field.key: field for field in ADVANCED_FIELDS}
        # One grid for every section, so the fields line up.
        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(2)
        grid.setColumnStretch(1, 1)
        layout.addLayout(grid)
        for title, keys in sections:
            section = QLabel(title)
            section.setStyleSheet(SECTION_STYLE)
            grid.addWidget(section, grid.rowCount(), 0, 1, 2)
            for key in keys:
                field = by_key[key]
                if field.kind in ("choice", "bool"):
                    widget: QWidget = self._choice_box(field)
                else:
                    edit = QLineEdit()
                    edit.setPlaceholderText(f"{field.default} (default)")
                    if field.kind == "int":
                        edit.setValidator(QIntValidator(0, 2_000_000_000, edit))
                    elif field.kind == "float":
                        validator = QDoubleValidator(0.0, 1000.0, 3, edit)
                        validator.setNotation(QDoubleValidator.Notation.StandardNotation)
                        validator.setLocale(QLocale.c())  # a decimal point, as YAML and the manager read it
                        edit.setValidator(validator)
                    self._numbers[key] = edit
                    widget = edit
                widget.setToolTip(field.tip)
                self._add_row(grid, field.label, key, widget)

        self._section(layout, "Dependencies")
        hint = QLabel("Tick the services that start before this one.")
        hint.setStyleSheet(f"color: {TEXT_MUTED};")
        layout.addWidget(hint)
        self.list_depends = QListWidget()
        self.list_depends.setStyleSheet(LIST_STYLE)
        self.list_depends.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        self.list_depends.setMaximumHeight(self.fontMetrics().height() * 8)
        own = form.name
        for name in sorted(set(services) | set(form.depends)):
            if name == own and self.mode == EDIT_MODE:
                continue
            item = QListWidgetItem(name)
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Unchecked)
            if name not in services:
                item.setToolTip("The manager has no service of this name")
                item.setForeground(QColor(DANGER))
            self.list_depends.addItem(item)
        if self.list_depends.count() == 0:
            self.list_depends.addItem(QListWidgetItem("(the manager has no other service)"))
        layout.addWidget(self.list_depends)
        layout.addWidget(self._problem_label(DEPENDS))
        self._inputs[DEPENDS] = self.list_depends

        self._section(layout, "Environment variables")
        self.table_env = QTableWidget(0, 2)
        self.table_env.setHorizontalHeaderLabels(["Name", "Value"])
        self.table_env.verticalHeader().setVisible(False)
        self.table_env.setStyleSheet(TABLE_STYLE + LIST_STYLE)
        self.table_env.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Interactive)
        self.table_env.horizontalHeader().setStretchLastSection(True)
        self.table_env.setColumnWidth(0, self.fontMetrics().averageCharWidth() * 24)
        self.table_env.setMaximumHeight(self.fontMetrics().height() * 8)
        self.table_env.setToolTip("Variables set for the service, after the manager's own environment")
        layout.addWidget(self.table_env)
        env_buttons = QHBoxLayout()
        env_buttons.addWidget(self._small_button("Add variable", lambda: self._add_env_row("", "")))
        env_buttons.addWidget(self._small_button("Remove variable", self._remove_env_row))
        env_buttons.addStretch()
        layout.addLayout(env_buttons)
        layout.addWidget(self._problem_label(ENVIRONMENT))
        self._inputs[ENVIRONMENT] = self.table_env
        return box

    def _choice_box(self, field) -> QComboBox:
        box = QComboBox()
        box.addItem(f"{DEFAULT_PREFIX}{field.default}", "")
        if field.kind == "bool":
            for value, text in BOOL_CHOICES:
                box.addItem(text, value)
        else:
            for choice in field.choices:
                box.addItem(choice, choice)
        self._choices[field.key] = box
        return box

    def _fill_advanced(self, form: ServiceForm) -> None:
        for key, box in self._choices.items():
            value = form.values.get(key, "")
            index = box.findData(value)
            if index < 0:
                box.addItem(f"{value} (as written)", value)  # kept as it is unless another is chosen
                index = box.count() - 1
            box.setCurrentIndex(index)
        for key, edit in self._numbers.items():
            # The validator would refuse a value written by hand that it does not take; show it anyway.
            validator = edit.validator()
            edit.setValidator(None)
            edit.setText(form.values.get(key, ""))
            edit.setValidator(validator)
        for i in range(self.list_depends.count()):
            item = self.list_depends.item(i)
            if item.flags() & Qt.ItemFlag.ItemIsUserCheckable:
                checked = item.text() in form.depends
                item.setCheckState(Qt.CheckState.Checked if checked else Qt.CheckState.Unchecked)
        for key, value in form.environment:
            self._add_env_row(key, value)

    def _add_env_row(self, key: str, value: str) -> None:
        row = self.table_env.rowCount()
        self.table_env.insertRow(row)
        self.table_env.setItem(row, 0, QTableWidgetItem(key))
        self.table_env.setItem(row, 1, QTableWidgetItem(value))
        if not key:
            self.table_env.setCurrentCell(row, 0)
            self.table_env.editItem(self.table_env.item(row, 0))

    def _remove_env_row(self) -> None:
        row = self.table_env.currentRow()
        if row < 0:
            row = self.table_env.rowCount() - 1
        if row >= 0:
            self.table_env.removeRow(row)

    def _show_advanced(self, shown: bool) -> None:
        self.advanced.setVisible(shown)
        self.btn_advanced.setArrowType(Qt.ArrowType.DownArrow if shown else Qt.ArrowType.RightArrow)
        count = self.form().advanced_set() if hasattr(self, "table_env") else 0
        self.btn_advanced.setText(f"Advanced · {count} set" if count else "Advanced")

    # ── the form ──────────────────────────────────────────────────────────

    def form(self) -> ServiceForm:
        """What the fields say now."""
        form = ServiceForm()
        form.name = self.edit_name.text().strip()
        form.values[EXECUTABLE] = self.edit_executable.text()
        form.values[ARGS] = self.edit_args.text()
        form.values[CWD] = self.edit_cwd.text()
        for key, box in self._choices.items():
            form.values[key] = box.currentData() or ""
        for key, edit in self._numbers.items():
            form.values[key] = edit.text()
        form.depends = [
            self.list_depends.item(i).text()
            for i in range(self.list_depends.count())
            if self.list_depends.item(i).flags() & Qt.ItemFlag.ItemIsUserCheckable
            and self.list_depends.item(i).checkState() == Qt.CheckState.Checked
        ]
        form.environment = []
        for row in range(self.table_env.rowCount()):
            key_item, value_item = self.table_env.item(row, 0), self.table_env.item(row, 1)
            form.environment.append((key_item.text() if key_item else "", value_item.text() if value_item else ""))
        return form

    def request(self, dry_run: bool) -> Tuple[Optional[Tuple[str, dict]], Dict[str, str]]:
        """The request the fields make, or None and the problems found before asking the manager."""
        form = self.form()
        definition, problems = build_definition(form, self.base)
        if self.mode != EDIT_MODE:
            why = check_name(form.name)
            if why:
                problems["name"] = why
        if problems:
            return None, problems
        if self.mode == EDIT_MODE:
            return replace_request(form.name, definition, self.check_now.isChecked(), dry_run), {}
        return add_request(form.name, definition, self.check_now.isChecked(), dry_run), {}

    # ── asking and answering ──────────────────────────────────────────────

    def _submit(self, dry_run: bool) -> None:
        if self._busy:
            return
        request, problems = self.request(dry_run)
        self._clear_problems()
        if request is None:
            self._show_problems({key: [text] for key, text in problems.items()})
            self._show_result("Fix the fields marked in red first.", DANGER)
            return
        operation, params = request
        self._busy = True
        self._pending_dry_run = dry_run
        self._set_buttons()
        self._show_result("Checking with the manager…" if dry_run else "Sending to the manager…", TEXT_DIM)
        self.request_wanted.emit(operation, params, dry_run)

    def show_reply(self, reply: RegistryReply) -> None:
        """The manager's answer to the last request."""
        dry_run = self._pending_dry_run
        self._busy = False
        self._set_buttons()
        self._clear_problems()
        if reply.status == OK:
            if dry_run:
                where = reply.get("file") or "its file"
                self._show_result(f"The manager would take it, and write {where}:", TEXT)
                self.txt_result.setPlainText(str(reply.get("yaml") or ""))
                self.txt_result.setVisible(True)
                return
            self.registered.emit(reply)
            self.accept()
            return
        fields = problem_fields(reply.message, reply.problems, self.edit_name.text().strip())
        self._show_problems({key: texts for key, texts in fields.items() if key})
        lines = [reply.message[:1].upper() + reply.message[1:]] + [f"• {problem}" for problem in reply.problems]
        self._show_result("\n".join(lines), DANGER)

    def _set_buttons(self) -> None:
        for button in (self.btn_check, self.btn_register):
            button.setEnabled(not self._busy)

    def _show_result(self, text: str, color: str) -> None:
        self.lbl_result.setText(text)
        self.lbl_result.setStyleSheet(f"color: {color};")
        self.lbl_result.setVisible(bool(text))
        self.txt_result.setVisible(False)

    def _clear_problems(self) -> None:
        for key, label in self._problem_labels.items():
            label.setVisible(False)
            label.setText("")
            self._mark(key, False)

    def _show_problems(self, problems: Dict[str, List[str]]) -> None:
        for key, texts in problems.items():
            label = self._problem_labels.get(key)
            if label is None:
                continue
            label.setText("\n".join(texts))
            label.setVisible(True)
            self._mark(key, True)
            if key in self._advanced_keys and not self.btn_advanced.isChecked():
                self.btn_advanced.setChecked(True)

    def _mark(self, key: str, problem: bool) -> None:
        widget = self._inputs.get(key)
        if widget is None:
            return
        widget.setProperty("problem", "true" if problem else "false")
        widget.style().unpolish(widget)
        widget.style().polish(widget)

    def problem_text(self, key: str) -> str:
        label = self._problem_labels.get(key)
        return label.text() if label is not None and not label.isHidden() else ""

    def result_text(self) -> str:
        return self.lbl_result.text() if not self.lbl_result.isHidden() else ""

    def preview_text(self) -> str:
        return self.txt_result.toPlainText() if not self.txt_result.isHidden() else ""

    def busy(self) -> bool:
        return self._busy

    # ── browsing (the manager on this machine only) ──────────────────────

    def _browse_executable(self) -> None:
        start = os.path.dirname(self.edit_executable.text()) or "/"
        chosen, _filter = QFileDialog.getOpenFileName(self, "The service's program", start)
        if chosen:
            self.edit_executable.setText(chosen)

    def _browse_cwd(self) -> None:
        start = self.edit_cwd.text() or os.path.expanduser("~")
        chosen = QFileDialog.getExistingDirectory(self, "The service's working folder", start)
        if chosen:
            self.edit_cwd.setText(chosen)
