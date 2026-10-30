"""最小化残影修复的接线回归（2026-10-30 现场：137x51 小窗连闪 177 次）。

修复不是「调一次 DwmSetWindowAttribute」就完事：原生 HWND 在 show / 跨屏
DPI 变化时可能被重建，旧句柄上的属性不会跟过去。所以这里钉住四个补设点
（showEvent / 跨屏 / 点「─」前 / 250ms 定时）以及无托盘启动路径。

    .venv\\Scripts\\python.exe tests\\test_minimize_ghost.py
"""
from __future__ import annotations

import inspect
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import agentrc_gui as gui  # noqa: E402


def _src(fn) -> str:
    return inspect.getsource(fn)


def test_minimize_button_goes_through_helper() -> None:
    src = (ROOT / "agentrc_gui.py").read_text(encoding="utf-8")
    assert "btn_min.clicked.connect(self._minimize_window)" in src
    assert "btn_min.clicked.connect(self.showMinimized)" not in src
    body = _src(gui.MainWindow._minimize_window)
    assert body.index("_stop_minimize_ghost") < body.index("showMinimized")
    print("PASS 「─」先关 DWM 过渡再最小化")


def test_attribute_is_reapplied_after_show_and_screen_change() -> None:
    show = _src(gui.MainWindow.showEvent)
    assert "_stop_minimize_ghost" in show
    assert "_ghost_timer" in show and "timer.start()" in show
    screen = _src(gui.MainWindow._on_screen_changed)
    assert "_stop_minimize_ghost" in screen
    init = _src(gui.MainWindow.__init__)
    assert "setInterval(250)" in init
    assert "self._ghost_timer.timeout.connect(self._stop_minimize_ghost)" in init
    print("PASS show / 跨屏 / 250ms 定时都补设")


def test_start_minimized_without_tray_disables_transition() -> None:
    body = _src(gui.main)
    assert body.index("_stop_minimize_ghost") < body.index("win.showMinimized()")
    print("PASS 无托盘启动先关过渡再 showMinimized")


def test_helper_reports_dwm_result() -> None:
    body = _src(gui.MainWindow._stop_minimize_ghost)
    assert "DwmSetWindowAttribute" in body
    assert "return int(res) == 0" in body
    print("PASS DwmSetWindowAttribute 的返回值被读回")


if __name__ == "__main__":
    test_minimize_button_goes_through_helper()
    test_attribute_is_reapplied_after_show_and_screen_change()
    test_start_minimized_without_tray_disables_transition()
    test_helper_reports_dwm_result()
    print("\n全部通过")
