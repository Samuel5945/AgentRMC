"""跨屏几何回夹的无头测试：只算 QRect，不创建窗口。

    .venv\\Scripts\\python.exe tests\\test_gui_geometry.py
"""
from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QRect

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import agentrc_gui as gui  # noqa: E402

fit = gui.MainWindow.fit_rect


def test_fit_keeps_fitting_window() -> None:
    frame = QRect(100, 100, 1020, 700)
    avail = QRect(0, 0, 1920, 1080)
    assert fit(frame, avail) == frame
    print("PASS 放得下：位置不动")


def test_fit_offscreen_left() -> None:
    r = fit(QRect(-500, 500, 1020, 700), QRect(0, 0, 1920, 1080))
    assert r == QRect(0, 380, 1020, 700), r.getRect()
    print("PASS 左边出屏：拉回可用区")


def test_fit_offscreen_right() -> None:
    r = fit(QRect(3000, 500, 1020, 700), QRect(0, 0, 1920, 1080))
    assert r == QRect(900, 380, 1020, 700), r.getRect()
    print("PASS 右边出屏：拉回可用区")


def test_fit_small_screen_shrinks() -> None:
    r = fit(QRect(0, 0, 1020, 700), QRect(0, 0, 1138, 640))
    assert r == QRect(0, 0, 1020, 640), r.getRect()
    print("PASS 小屏（1138x640）：高度收到 640")


def test_fit_small_screen_top_aligns() -> None:
    r = fit(QRect(100, 600, 1020, 700), QRect(0, 0, 1138, 640))
    assert r == QRect(100, 0, 1020, 640), r.getRect()
    print("PASS 小屏底部出界：顶对齐")


if __name__ == "__main__":
    test_fit_keeps_fitting_window()
    test_fit_offscreen_left()
    test_fit_offscreen_right()
    test_fit_small_screen_shrinks()
    test_fit_small_screen_top_aligns()
    print("\n全部通过")
