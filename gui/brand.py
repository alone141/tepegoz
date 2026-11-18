"""Göz's mark, an eye: drawn here for the GUI, and written as goz.svg for everything else.

Göz means "eye", and Tepegöz, the manager it watches, is the one-eyed giant. The mark is one eye on a dark tile,
in the colours of the GUI's theme. QPainter draws it, so the GUI needs nothing beyond PyQt6 (no SVG module). The
same shapes make goz.svg, the icon that cmake --install puts in the desktop's icon theme and the README shows.
After changing a shape or a colour, write goz.svg again with: python3 brand.py (tests/test_brand.py checks it).
"""

from __future__ import annotations

import os
import sys

from PyQt6.QtCore import QPointF, QRectF, Qt
from PyQt6.QtGui import QColor, QIcon, QPainter, QPainterPath, QPixmap

from process_views import ACCENT, BG, SIDEBAR_BG, TEXT

# The shapes, on a 64 × 64 grid.
GRID = 64
TILE_CORNER = 14
TILE_COLOR = SIDEBAR_BG
# The white of the eye: from its left corner over the upper lid to the right corner, and back under the lower
# lid. Each lid is one quadratic curve, given as (control point, end point).
EYE_START = (7.0, 32.0)
UPPER_LID = ((32.0, 9.0), (57.0, 32.0))
LOWER_LID = ((32.0, 55.0), (7.0, 32.0))
EYE_COLOR = TEXT
# Circles drawn over it, in order: (centre x, centre y, radius, colour).
IRIS = (32.0, 32.0, 12.0, ACCENT)
PUPIL = (32.0, 32.0, 5.5, BG)
GLINT = (36.0, 28.0, 2.6, "#ffffff")
CIRCLES = (IRIS, PUPIL, GLINT)

# The sizes a taskbar, Alt+Tab and an icon theme ask for; each is drawn at its own size, not scaled from another.
ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SVG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "goz.svg")
# Windows gives a process's windows the taskbar button of its app ID; without one they share python.exe's.
WINDOWS_APP_ID = "Tepegoz.Goz"


def paint_mark(painter: QPainter, side: float) -> None:
    """Draw the mark in a side × side square at the painter's origin."""
    painter.save()
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.scale(side / GRID, side / GRID)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(QColor(TILE_COLOR))
    painter.drawRoundedRect(QRectF(0, 0, GRID, GRID), TILE_CORNER, TILE_CORNER)
    eye = QPainterPath(QPointF(*EYE_START))
    for control, end in (UPPER_LID, LOWER_LID):
        eye.quadTo(QPointF(*control), QPointF(*end))
    painter.setBrush(QColor(EYE_COLOR))
    painter.drawPath(eye)
    for x, y, radius, color in CIRCLES:
        painter.setBrush(QColor(color))
        painter.drawEllipse(QPointF(x, y), radius, radius)
    painter.restore()


def mark_pixmap(size: int, device_pixel_ratio: float = 1.0) -> QPixmap:
    """The mark at size × size logical pixels, drawn sharp for a display with this pixel ratio."""
    pixels = max(1, round(size * device_pixel_ratio))
    pixmap = QPixmap(pixels, pixels)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    paint_mark(painter, pixels)
    painter.end()
    pixmap.setDevicePixelRatio(device_pixel_ratio)
    return pixmap


def mark_icon() -> QIcon:
    """The mark as a window icon, with a pixmap for each size in ICON_SIZES."""
    icon = QIcon()
    for size in ICON_SIZES:
        icon.addPixmap(mark_pixmap(size))
    return icon


def _number(value: float) -> str:
    return f"{value:g}"


def mark_svg() -> str:
    """The mark as SVG: the content of goz.svg."""
    lids = " ".join(
        f"Q{_number(cx)} {_number(cy)} {_number(ex)} {_number(ey)}" for (cx, cy), (ex, ey) in (UPPER_LID, LOWER_LID)
    )
    circles = "".join(
        f'  <circle cx="{_number(x)}" cy="{_number(y)}" r="{_number(radius)}" fill="{color}"/>\n'
        for x, y, radius, color in CIRCLES
    )
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{GRID}" height="{GRID}" viewBox="0 0 {GRID} {GRID}">\n'
        "  <title>Göz</title>\n"
        f'  <rect width="{GRID}" height="{GRID}" rx="{TILE_CORNER}" fill="{TILE_COLOR}"/>\n'
        f'  <path d="M{_number(EYE_START[0])} {_number(EYE_START[1])} {lids} Z" fill="{EYE_COLOR}"/>\n'
        f"{circles}"
        "</svg>\n"
    )


def write_svg(path: str = SVG_FILE) -> None:
    """Write mark_svg() to goz.svg, with LF line endings on every system."""
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(mark_svg())


def use_own_taskbar_button() -> None:
    """On Windows, give Göz a taskbar button of its own, which shows its icon, instead of python.exe's.

    Called before the first window is shown; elsewhere it does nothing.
    """
    if sys.platform != "win32":
        return
    try:
        import ctypes

        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(WINDOWS_APP_ID)
    except (AttributeError, OSError):
        pass  # an old Windows without app IDs: the button stays python.exe's


if __name__ == "__main__":
    write_svg()
    print(f"wrote {SVG_FILE}")
