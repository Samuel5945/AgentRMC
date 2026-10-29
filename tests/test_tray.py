"""托盘常驻行为的无头回归：关窗只隐藏、监听不停；真退出才停。

    .venv\\Scripts\\python.exe tests\\test_tray.py
"""
from __future__ import annotations

import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from PySide6.QtCore import Qt, QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication, QSystemTrayIcon  # noqa: E402

import agentrc as core  # noqa: E402
import agentrc_gui as gui  # noqa: E402

APP = QApplication.instance() or QApplication(sys.argv)
APP.setStyleSheet(gui.QSS)


def _spin(ms: int = 60) -> None:
    t = QTimer()
    t.setSingleShot(True)
    t.start(ms)
    while t.isActive():
        APP.processEvents()


def _window() -> gui.MainWindow:
    tmp = Path(tempfile.gettempdir()) / "agentrmc_tray_profiles.json"
    shutil.copy2(ROOT / "profiles.json", tmp)
    core.PROFILES_PATH = tmp
    win = gui.MainWindow()
    win.setAttribute(Qt.WA_DontShowOnScreen)
    win.profiles_path = tmp
    win.show()
    _spin()
    return win


class _Counter:
    def __init__(self) -> None:
        self.n = 0

    def __call__(self) -> None:
        self.n += 1


def test_close_hides_and_keeps_listening() -> None:
    if not QSystemTrayIcon.isSystemTrayAvailable():
        print("SKIP 这台机器没有可用托盘")
        return
    win = _window()
    try:
        assert win.tray_ready(), "托盘没建起来"
        assert win.close_to_tray, "close_to_tray 默认应为 True"
        stopped = _Counter()
        win.ctl.stop = stopped  # type: ignore[method-assign]
        win.ctl.running = True  # 只给状态判断用，不真起监听线程

        win.close()
        _spin()
        assert not win.isVisible(), "关窗后窗口应隐藏"
        assert stopped.n == 0, "关窗不该停监听"
        assert win.ctl.running, "关窗后应仍在监听状态"

        win._tray_reveal()
        _spin()
        assert win.isVisible(), "托盘唤回后窗口应可见"
        assert stopped.n == 0
        print("PASS 关窗=收进托盘继续监听，托盘可唤回")
    finally:
        win.ctl.running = False
        win.deleteLater()
        _spin()


def test_quit_from_tray_stops() -> None:
    if not QSystemTrayIcon.isSystemTrayAvailable():
        print("SKIP 这台机器没有可用托盘")
        return
    win = _window()
    try:
        stopped = _Counter()
        win.ctl.stop = stopped  # type: ignore[method-assign]
        win._quit_app()
        _spin()
        assert stopped.n == 1, f"真退出必须停监听（{stopped.n}）"
        assert not win.isVisible()
        print("PASS 托盘「退出 AgentRMC」会停监听并关窗")
    finally:
        win.deleteLater()
        _spin()


def test_no_tray_falls_back_to_quit() -> None:
    win = _window()
    try:
        stopped = _Counter()
        win.ctl.stop = stopped  # type: ignore[method-assign]
        win.close_to_tray = False
        win.close()
        _spin()
        assert stopped.n == 1, "关掉 close_to_tray 后应回到老行为：关窗即停"
        print("PASS close_to_tray=false 时关窗即退出")
    finally:
        win.deleteLater()
        _spin()


def test_hidden_start_then_reveal() -> None:
    """启动即藏托盘：窗口从没显示过，唤回后映射页必须照常画出来。"""
    if not QSystemTrayIcon.isSystemTrayAvailable():
        print("SKIP 这台机器没有可用托盘")
        return
    tmp = Path(tempfile.gettempdir()) / "agentrmc_tray_hidden.json"
    shutil.copy2(ROOT / "profiles.json", tmp)
    core.PROFILES_PATH = tmp
    win = gui.MainWindow()
    win.setAttribute(Qt.WA_DontShowOnScreen)
    try:
        win.tabs.setCurrentIndex(1)  # 藏起来的时候也允许切页
        _spin()
        win._tray_reveal()
        _spin()
        rows = [w for w in win.map_rows.values()]
        assert rows and all(r.isVisible() for r in rows), "唤回后映射行没显示"
        assert all(r.width() > 300 for r in rows), [r.width() for r in rows]
        print("PASS 从未显示直接唤回：映射行照常可见")
    finally:
        win.deleteLater()
        _spin()


if __name__ == "__main__":
    test_close_hides_and_keeps_listening()
    test_quit_from_tray_stops()
    test_no_tray_falls_back_to_quit()
    test_hidden_start_then_reveal()
    print("\n全部通过")
