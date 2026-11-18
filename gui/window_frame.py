"""A main window without the system title bar: its toolbar takes that place.

The window is frameless. Its toolbar and the window buttons share the top row,
the title bar: dragging the row's empty space moves the window, a double-click
maximizes or restores it, and the buttons at its right end minimize, maximize
and close it. A thin border around the window resizes it. Moving and resizing
are left to the window system (QWindow.startSystemMove / startSystemResize);
where it cannot move the window, the window follows the mouse instead.
"""

from __future__ import annotations

from typing import List, Optional

from PyQt6.QtCore import QEvent, QObject, QPoint, QPointF, QRect, QSize, Qt
from PyQt6.QtGui import QColor, QPainter, QPen, QPolygonF
from PyQt6.QtWidgets import (
    QAbstractButton,
    QApplication,
    QHBoxLayout,
    QMainWindow,
    QToolBar,
    QWidget,
)

from process_views import BORDER, TEXT

# The border that resizes the window, in logical pixels. A corner grip is
# twice as long, so corners are easier to hit.
RESIZE_BORDER = 4
TITLE_BUTTON_HOVER = "#3a3a3a"
CLOSE_BUTTON_HOVER = "#c42b1c"  # the close button's red on Windows

EDGE_CURSORS = {
    Qt.Edge.LeftEdge: Qt.CursorShape.SizeHorCursor,
    Qt.Edge.RightEdge: Qt.CursorShape.SizeHorCursor,
    Qt.Edge.TopEdge: Qt.CursorShape.SizeVerCursor,
    Qt.Edge.BottomEdge: Qt.CursorShape.SizeVerCursor,
    Qt.Edge.TopEdge | Qt.Edge.LeftEdge: Qt.CursorShape.SizeFDiagCursor,
    Qt.Edge.BottomEdge | Qt.Edge.RightEdge: Qt.CursorShape.SizeFDiagCursor,
    Qt.Edge.TopEdge | Qt.Edge.RightEdge: Qt.CursorShape.SizeBDiagCursor,
    Qt.Edge.BottomEdge | Qt.Edge.LeftEdge: Qt.CursorShape.SizeBDiagCursor,
}


class TitleBarButton(QAbstractButton):
    """Minimize, maximize, restore or close, drawn as a thin line icon."""

    def __init__(self, kind: str, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.kind = kind  # "minimize", "maximize", "restore" or "close"
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)

    def set_kind(self, kind: str) -> None:
        self.kind = kind
        self.update()

    def sizeHint(self) -> QSize:
        height = self.fontMetrics().height()
        return QSize(round(height * 3), round(height * 2.1))

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        lit = self.isDown() or self.underMouse()
        if lit:
            hover = CLOSE_BUTTON_HOVER if self.kind == "close" else TITLE_BUTTON_HOVER
            painter.fillRect(self.rect(), QColor(hover))
        color = "white" if lit and self.kind == "close" else TEXT
        painter.setPen(QPen(QColor(color), 1.0))

        size = max(8, round(self.fontMetrics().height() * 0.62))
        x = (self.width() - size) // 2
        y = (self.height() - size) // 2
        if self.kind == "minimize":
            painter.drawLine(x, y + size // 2, x + size, y + size // 2)
        elif self.kind == "maximize":
            painter.drawRect(x, y, size, size)
        elif self.kind == "restore":
            step = max(2, size // 4)
            painter.drawRect(x, y + step, size - step, size - step)
            painter.drawPolyline(QPolygonF([
                QPointF(x + step, y + step),
                QPointF(x + step, y),
                QPointF(x + size, y),
                QPointF(x + size, y + size - step),
                QPointF(x + size - step, y + size - step),
            ]))
        else:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            painter.drawLine(QPointF(x, y), QPointF(x + size, y + size))
            painter.drawLine(QPointF(x + size, y), QPointF(x, y + size))


class TitleBarButtons(QWidget):
    """Minimize, maximize/restore and close, for the right end of the title bar."""

    def __init__(self, window: FramelessMainWindow, parent: Optional[QWidget] = None):
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        self.btn_minimize = TitleBarButton("minimize")
        self.btn_minimize.setToolTip("Minimize")
        self.btn_minimize.clicked.connect(window.showMinimized)
        self.btn_maximize = TitleBarButton("maximize")
        self.btn_maximize.clicked.connect(window.toggle_maximized)
        self.btn_close = TitleBarButton("close")
        self.btn_close.setToolTip("Close")
        self.btn_close.clicked.connect(window.close)
        for button in (self.btn_minimize, self.btn_maximize, self.btn_close):
            layout.addWidget(button)
        self.show_maximized(False)

    def show_maximized(self, maximized: bool) -> None:
        self.btn_maximize.set_kind("restore" if maximized else "maximize")
        self.btn_maximize.setToolTip("Restore" if maximized else "Maximize")


class ResizeGrip(QWidget):
    """An invisible strip on the window's border that resizes the window."""

    def __init__(self, edges: Qt.Edge, parent: QWidget):
        super().__init__(parent)
        self.edges = edges
        self.setCursor(EDGE_CURSORS[edges])

    def mousePressEvent(self, event) -> None:
        handle = self.window().windowHandle()
        if (
            event.button() == Qt.MouseButton.LeftButton
            and handle is not None
            and handle.startSystemResize(self.edges)
        ):
            event.accept()
            return
        super().mousePressEvent(event)


class FramelessMainWindow(QMainWindow):
    """A QMainWindow without the system frame. set_title_bar() puts a toolbar
    in its title bar."""

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        # The system menu and min/max hints keep minimizing from the taskbar.
        self.setWindowFlags(
            Qt.WindowType.Window
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowSystemMenuHint
            | Qt.WindowType.WindowMinMaxButtonsHint
        )
        self._grips: List[ResizeGrip] = [
            ResizeGrip(edges, self) for edges in EDGE_CURSORS
        ]
        self._title_bar: Optional[QWidget] = None
        self._title_buttons: Optional[TitleBarButtons] = None
        # Where a press on the title bar started, until it moves or ends.
        self._press_pos: Optional[QPoint] = None
        # Cursor minus window position, when the window system cannot move it.
        self._drag_offset: Optional[QPoint] = None
        self._apply_window_state()

    def set_title_bar(self, toolbar: QToolBar) -> QWidget:
        """Put toolbar and the window buttons in the title bar, the row above
        everything else, and return that row (object name titleBar) for its
        style sheet. The buttons sit outside the toolbar so that they reach
        the row's edges, whatever the toolbar's padding."""
        bar = QWidget()
        bar.setObjectName("titleBar")
        bar.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        layout = QHBoxLayout(bar)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(toolbar, 1)
        self._title_buttons = TitleBarButtons(self)
        self._title_buttons.show_maximized(self.isMaximized())
        layout.addWidget(self._title_buttons)
        self.setMenuWidget(bar)
        # Presses on the toolbar's empty space and labels reach the row.
        bar.installEventFilter(self)
        self._title_bar = bar
        return bar

    def toggle_maximized(self) -> None:
        if self.isMaximized():
            self.showNormal()
        else:
            self.showMaximized()

    # ── Moving ────────────────────────────────────────────────────────────

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        if watched is not self._title_bar:
            return super().eventFilter(watched, event)
        kind = event.type()
        if kind == QEvent.Type.MouseButtonDblClick:
            if event.button() == Qt.MouseButton.LeftButton:
                self._press_pos = None
                self.toggle_maximized()
                return True
        elif kind == QEvent.Type.MouseButtonPress:
            if event.button() == Qt.MouseButton.LeftButton:
                self._press_pos = event.globalPosition().toPoint()
                return True
        elif kind == QEvent.Type.MouseMove:
            cursor = event.globalPosition().toPoint()
            if self._drag_offset is not None:
                self.move(cursor - self._drag_offset)
                return True
            if (
                self._press_pos is not None
                and event.buttons() & Qt.MouseButton.LeftButton
                and (cursor - self._press_pos).manhattanLength()
                >= QApplication.startDragDistance()
            ):
                self._start_move(self._press_pos)
                return True
        elif kind == QEvent.Type.MouseButtonRelease:
            self._press_pos = None
            self._drag_offset = None
        return super().eventFilter(watched, event)

    def _start_move(self, cursor: QPoint) -> None:
        self._press_pos = None
        if self.isMaximized():
            # Like a system title bar: restore, keeping the cursor at the same
            # place along the title bar.
            ratio = (cursor.x() - self.x()) / max(1, self.width())
            depth = cursor.y() - self.y()
            normal = self.normalGeometry()
            self.showNormal()
            self.move(
                cursor.x() - round(normal.width() * ratio),
                cursor.y() - depth - RESIZE_BORDER,
            )
        handle = self.windowHandle()
        if handle is not None and handle.startSystemMove():
            return
        # The window system cannot move it: follow the mouse here.
        self._drag_offset = cursor - self.pos()

    # ── Window state and border ───────────────────────────────────────────

    def changeEvent(self, event: QEvent) -> None:
        if event.type() == QEvent.Type.WindowStateChange:
            self._apply_window_state()
        super().changeEvent(event)

    def _apply_window_state(self) -> None:
        maximized = self.isMaximized() or self.isFullScreen()
        border = 0 if maximized else RESIZE_BORDER
        self.setContentsMargins(border, border, border, border)
        for grip in self._grips:
            grip.setVisible(not maximized)
        if self._title_buttons is not None:
            self._title_buttons.show_maximized(maximized)
        self.update()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        width, height = self.width(), self.height()
        side, corner = RESIZE_BORDER, RESIZE_BORDER * 2
        places = {
            Qt.Edge.LeftEdge: QRect(0, corner, side, height - 2 * corner),
            Qt.Edge.RightEdge: QRect(width - side, corner, side, height - 2 * corner),
            Qt.Edge.TopEdge: QRect(corner, 0, width - 2 * corner, side),
            Qt.Edge.BottomEdge: QRect(corner, height - side, width - 2 * corner, side),
            Qt.Edge.TopEdge | Qt.Edge.LeftEdge: QRect(0, 0, corner, corner),
            Qt.Edge.TopEdge | Qt.Edge.RightEdge: QRect(width - corner, 0, corner, corner),
            Qt.Edge.BottomEdge | Qt.Edge.LeftEdge: QRect(0, height - corner, corner, corner),
            Qt.Edge.BottomEdge | Qt.Edge.RightEdge: QRect(
                width - corner, height - corner, corner, corner
            ),
        }
        for grip in self._grips:
            grip.setGeometry(places[grip.edges])
            grip.raise_()

    def paintEvent(self, event) -> None:
        super().paintEvent(event)
        if self.contentsMargins().left() > 0:
            # Without the system frame, a dark window would melt into a dark desktop.
            painter = QPainter(self)
            painter.setPen(QColor(BORDER))
            painter.drawRect(self.rect().adjusted(0, 0, -1, -1))
