"""按键映射页的无头 UI 回归：真建窗口，但不往屏幕上画。

踩过的坑：映射行是「先无父控件创建、再进布局」的，窗口显示之后再重建时 Qt 不会
自动把它们置为可见 —— 表头在、表体空白。这里把可见性和列宽钉住。

    .venv\\Scripts\\python.exe tests\\test_keymap_ui.py
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from PySide6.QtCore import Qt, QTimer  # noqa: E402
from PySide6.QtTest import QTest  # noqa: E402
from PySide6.QtWidgets import QApplication, QDialog  # noqa: E402

import agentrc as core  # noqa: E402
import agentrc_gui as gui  # noqa: E402

APP = QApplication.instance() or QApplication(sys.argv)
APP.setStyleSheet(gui.QSS)


def _spin(ms: int = 60) -> None:
    """跑掉 singleShot(0) 和布局的两轮排队事件。"""
    end = QTimer()
    end.setSingleShot(True)
    end.start(ms)
    while end.isActive():
        APP.processEvents()


def _make_window(tmp: Path) -> gui.MainWindow:
    core.PROFILES_PATH = tmp
    win = gui.MainWindow(with_tray=False)  # 页面测试不需要托盘图标
    win.setAttribute(Qt.WA_DontShowOnScreen)  # 不在屏幕上出现，但走完整显示流程
    win.profiles_path = tmp
    win.show()
    _spin()
    return win


def _rows(win: gui.MainWindow):
    return [(i, win.map_grid.itemAt(i).widget())
            for i in range(win.map_grid.count())
            if win.map_grid.itemAt(i).widget() is not None]


def test_rows_visible_after_tab_switch() -> None:
    tmp = Path(tempfile.gettempdir()) / "agentrmc_ui_profiles.json"
    shutil.copy2(ROOT / "profiles.json", tmp)
    win = _make_window(tmp)
    try:
        win.tabs.setCurrentIndex(1)
        _spin()
        rows = [w for _, w in _rows(win) if isinstance(w, gui.MapKeyRow)]
        assert len(rows) == len(win.map_rows), (len(rows), len(win.map_rows))
        for w in rows:
            assert w.isVisible(), f"{w.vk} 行没被显示（表体会空白）"
            assert w.width() > 300 and w.height() == 35, w.geometry().getRect()
            for cell in w.cells.values():
                assert cell.isVisible(), f"vk={w.vk} 手势格隐藏"
        # 分组标题同样要可见
        heads = [w for _, w in _rows(win) if not isinstance(w, gui.MapKeyRow)]
        assert heads and all(h.isVisible() for h in heads)
        # 场景名不能被同行规则挤到截断
        name = win.map_scene_name
        assert name.width() >= name.fontMetrics().horizontalAdvance(name.text()), \
            (name.text(), name.width())
        print("PASS 切到映射页后 10 行 + 分组标题全部可见，列宽正常")
    finally:
        win.deleteLater()
        _spin()
        tmp.unlink(missing_ok=True)


def test_preset_edit_roundtrip() -> None:
    tmp = Path(tempfile.gettempdir()) / "agentrmc_ui_edit.json"
    shutil.copy2(ROOT / "profiles.json", tmp)
    win = _make_window(tmp)
    try:
        idx = win.map_combo.findData("qoder")
        assert idx >= 0
        win.map_combo.setCurrentIndex(idx)
        win.tabs.setCurrentIndex(1)
        _spin()

        win._apply_mapping("qoder", 38, "single", "pass")
        win._apply_mapping("qoder", 13, "double", "mode_media")
        win._apply_mapping("qoder", 38, "hold", "repeat_accel")
        win._apply_mapping("qoder", 27, "double", "none")
        _spin()

        raw = json.loads(tmp.read_text(encoding="utf-8"))
        prof = next(p for p in raw["profiles"] if p["name"] == "qoder")
        assert prof["keys"]["38"] == "k38_pass", prof["keys"].get("38")
        assert prof["double"]["13"] == "k13_d_mode_media"
        assert prof["actions"]["k13_d_mode_media"]["mode"] == "media"
        assert prof["hold"]["38"] == "repeat_accel"
        assert prof["double"]["27"] == "none"
        assert (tmp.parent / (tmp.name + ".bak")).exists(), "没有写 .bak 备份"

        # 重建后的单元格文案要跟上，且仍然可见
        assert win.map_rows[38].cells["single"].title_text == "原生直通"
        assert win.map_rows[13].cells["double"].title_text == "音量/亮度模式"
        assert win.map_rows[27].cells["double"].effect_text == "已关闭"
        for w in win.map_rows.values():
            assert w.isVisible()
        print("PASS 预设改键写入 profiles.json + 界面回显一致")
    finally:
        win.deleteLater()
        _spin()
        tmp.unlink(missing_ok=True)


def test_cell_click_opens_menu() -> None:
    tmp = Path(tempfile.gettempdir()) / "agentrmc_ui_click.json"
    shutil.copy2(ROOT / "profiles.json", tmp)
    win = _make_window(tmp)
    try:
        win.tabs.setCurrentIndex(1)
        _spin()
        seen: list[tuple[int, str]] = []
        win._open_preset_menu = lambda vk, g: seen.append((vk, g))  # type: ignore[method-assign]
        cell = win.map_rows[13].cells["double"]
        QTest.mouseClick(cell, Qt.LeftButton)
        APP.processEvents()
        assert seen == [(13, "double")], seen
        menu = win._build_preset_menu(13, "double")
        labels = [a.text() for a in menu.actions()]
        assert "当前：" in "".join(labels) or any(t.startswith("当前") for t in labels)
        assert "关闭双击" in labels and "模式" in labels
        hold = win._build_preset_menu(38, "hold")
        hold_labels = [a.text() for a in hold.actions()]
        assert {"关闭长按", "连续触发（按住重复）", "连发加速（按住越来越快）"} <= set(hold_labels) \
            or "连续加速（按住越来越快）" in hold_labels, hold_labels
        print("PASS 点手势格弹出对应预设菜单")
    finally:
        win.deleteLater()
        _spin()
        tmp.unlink(missing_ok=True)


def test_custom_combo_capture() -> None:
    tmp = Path(tempfile.gettempdir()) / "agentrmc_ui_custom.json"
    shutil.copy2(ROOT / "profiles.json", tmp)
    win = _make_window(tmp)
    try:
        idx = win.map_combo.findData("qoder")
        win.map_combo.setCurrentIndex(idx)      # 预览固定到 qoder，断言界面回显
        win.tabs.setCurrentIndex(1)
        _spin()

        # 三个手势菜单都有「自定义组合键…」入口
        for gesture in ("single", "double", "hold"):
            menu = win._build_preset_menu(38, gesture)
            labels = [a.text() for a in menu.actions()]
            assert "自定义组合键…" in labels, (gesture, labels)
            menu.deleteLater()

        # 捕捉对话框：合成 Ctrl+T，按下即录入
        dlg = gui.ComboCaptureDialog(win)
        dlg.setAttribute(Qt.WA_DontShowOnScreen)
        dlg.show()
        QTest.keyClick(dlg, Qt.Key_T, Qt.ControlModifier)
        assert dlg.mods == ["ctrl"] and dlg.vk == 0x54, (dlg.mods, dlg.vk)
        assert dlg.result() == QDialog.Accepted

        # 写回 profiles.json，单元格文案同步
        win._apply_captured("qoder", 38, "single", dlg)
        _spin()
        raw = json.loads(tmp.read_text(encoding="utf-8"))
        prof = next(p for p in raw["profiles"] if p["name"] == "qoder")
        assert prof["keys"]["38"] == "k38_custom", prof["keys"].get("38")
        assert prof["actions"]["k38_custom"]["mods"] == ["ctrl"]
        assert prof["actions"]["k38_custom"]["vk"] == 0x54
        assert win.map_rows[38].cells["single"].title_text == "CTRL+T"

        # 长按自定义：Shift+Alt+X
        dlg2 = gui.ComboCaptureDialog(win)
        dlg2.setAttribute(Qt.WA_DontShowOnScreen)
        dlg2.show()
        QTest.keyClick(dlg2, Qt.Key_X, Qt.ShiftModifier | Qt.AltModifier)
        win._apply_captured("qoder", 40, "hold", dlg2)
        _spin()
        raw = json.loads(tmp.read_text(encoding="utf-8"))
        prof = next(p for p in raw["profiles"] if p["name"] == "qoder")
        assert prof["hold"]["40"] == "k40_h_custom", prof.get("hold")
        assert prof["actions"]["k40_h_custom"]["label"] == "SHIFT+ALT+X"
        assert win.map_rows[40].cells["hold"].title_text == "SHIFT+ALT+X"

        # 单击绑到本键原生 VK：等价直通，不再注入
        dlg3 = gui.ComboCaptureDialog(win)
        dlg3.setAttribute(Qt.WA_DontShowOnScreen)
        dlg3.show()
        QTest.keyClick(dlg3, Qt.Key_Up)        # 遥控器上键 vk 38 = VK 0x26
        win._apply_captured("qoder", 38, "single", dlg3)
        _spin()
        raw = json.loads(tmp.read_text(encoding="utf-8"))
        prof = next(p for p in raw["profiles"] if p["name"] == "qoder")
        assert prof["actions"]["k38_custom"]["type"] == "none", \
            prof["actions"]["k38_custom"]
        print("PASS 自定义组合键：菜单入口 / 按键直接录入 / 写回 profiles.json")
    finally:
        win.deleteLater()
        _spin()
        tmp.unlink(missing_ok=True)


if __name__ == "__main__":
    test_rows_visible_after_tab_switch()
    test_preset_edit_roundtrip()
    test_cell_click_opens_menu()
    test_custom_combo_capture()
    print("\n全部通过")
