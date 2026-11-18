"""Tests of Göz's mark: goz.svg and the painted mark come from the same shapes, and the window shows it.

Run from the repository root (standard library unittest; PyQt6 and pyzmq installed):
    QT_QPA_PLATFORM=offscreen python3 -m unittest discover -s gui/tests -v
"""

from __future__ import annotations

import os
import socket
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
GUI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if GUI_DIR not in sys.path:
    sys.path.insert(0, GUI_DIR)

from PyQt6.QtGui import QColor  # noqa: E402
from PyQt6.QtWidgets import QApplication, QLabel  # noqa: E402

import brand  # noqa: E402

APP = None


def setUpModule():
    global APP
    APP = QApplication.instance() or QApplication([])


def closed_endpoint() -> str:
    """An endpoint nothing listens on: the system picks the port, and it is closed again at once."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return f"tcp://127.0.0.1:{probe.getsockname()[1]}"


class SvgFile(unittest.TestCase):
    def test_goz_svg_is_written_from_the_shapes(self):
        with open(brand.SVG_FILE, encoding="utf-8") as stored:
            self.assertEqual(stored.read(), brand.mark_svg(),
                             "goz.svg is out of date: write it again with python3 brand.py")

    def test_svg_names_the_theme_colours(self):
        svg = brand.mark_svg()
        for color in (brand.TILE_COLOR, brand.EYE_COLOR, brand.IRIS[3], brand.PUPIL[3], brand.GLINT[3]):
            self.assertIn(f'fill="{color}"', svg)


class PaintedMark(unittest.TestCase):
    def test_pixels_are_the_colours_of_the_shapes(self):
        image = brand.mark_pixmap(brand.GRID).toImage()

        def color_at(x: int, y: int) -> str:
            return image.pixelColor(x, y).name()

        self.assertEqual(image.pixelColor(0, 0).alpha(), 0, "the tile's corner is rounded off")
        self.assertEqual(color_at(32, 4), QColor(brand.TILE_COLOR).name())
        self.assertEqual(color_at(12, 32), QColor(brand.EYE_COLOR).name())
        self.assertEqual(color_at(32, 22), QColor(brand.IRIS[3]).name())
        self.assertEqual(color_at(30, 34), QColor(brand.PUPIL[3]).name())
        self.assertEqual(color_at(36, 28), QColor(brand.GLINT[3]).name())

    def test_pixmap_is_drawn_for_the_pixel_ratio(self):
        pixmap = brand.mark_pixmap(16, 2.0)
        self.assertEqual((pixmap.width(), pixmap.height()), (32, 32))
        self.assertEqual(pixmap.devicePixelRatio(), 2.0)

    def test_icon_has_every_size(self):
        icon = brand.mark_icon()
        sizes = {(size.width(), size.height()) for size in icon.availableSizes()}
        for size in brand.ICON_SIZES:
            self.assertIn((size, size), sizes)


class Window(unittest.TestCase):
    def test_window_has_the_icon_and_the_title_row_the_mark(self):
        from goz import GozWindow

        window = GozWindow(closed_endpoint(), closed_endpoint(), closed_endpoint(),
                           journal_endpoint=closed_endpoint(), registry_endpoint=closed_endpoint())
        self.addCleanup(window.close)
        self.assertFalse(window.windowIcon().isNull())
        labels = window.findChildren(QLabel)
        marks = [label for label in labels if label.pixmap() is not None and not label.pixmap().isNull()]
        self.assertTrue(marks, "no label in the window shows the mark")
        title = next(label for label in labels if label.text() == "Göz")
        self.assertIs(marks[0].parentWidget(), title.parentWidget(), "the mark sits next to the name")


if __name__ == "__main__":
    unittest.main()
