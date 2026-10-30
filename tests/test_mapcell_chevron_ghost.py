"""按键映射格的 ⌄ 必须在挂进布局（有父）之后才 setVisible。

2026-10-30 现场：无父 QLabel 的 setVisible(True) 会被 Qt 当成顶层窗 show 出来，
每建一个 MapCell 就闪一个 137x51（本机 175% 下 240x89）的白色原生小窗；
启动、切 profile、刷新映射表时连闪几十个，被误判成 DWM 最小化残影。
"""
from __future__ import annotations

import inspect
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from PySide6.QtWidgets import QApplication, QLabel  # noqa: E402

import agentrc_gui as gui  # noqa: E402


def _make_cell(editable: bool):
    calls = []
    orig = QLabel.setVisible

    def spy(self, visible):
        if self.objectName() == "mapCellChev":
            calls.append((bool(visible), self.parent() is not None))
        return orig(self, visible)

    QLabel.setVisible = spy
    try:
        cell = gui.MapCell(0x74, "tap", "F5", "语音", editable=editable)
    finally:
        QLabel.setVisible = orig
    return cell, calls


def test_chev_is_parented_before_visible() -> None:
    _app = QApplication.instance() or QApplication([])
    cell, calls = _make_cell(True)
    chev = cell.findChild(QLabel, "mapCellChev")
    assert chev is not None
    assert calls == [(True, True)], calls
    assert chev.parent() is not None
    cell.deleteLater()
    print("PASS 可编辑格的 ⌄ 在有父之后才显示")


def test_readonly_cell_chev_stays_parented_and_hidden() -> None:
    _app = QApplication.instance() or QApplication([])
    cell, calls = _make_cell(False)
    chev = cell.findChild(QLabel, "mapCellChev")
    assert chev is not None
    assert calls == [(False, True)], calls
    cell.deleteLater()
    print("PASS 只读格的 ⌄ 同样先有父，且保持隐藏")


def test_source_order_add_widget_before_setvisible() -> None:
    src = inspect.getsource(gui.MapCell.__init__)
    assert src.index("lay.addWidget(chev") < src.index("chev.setVisible(self.editable)"), \
        "必须先 addWidget 再 setVisible，否则无父 QLabel 会以顶层窗闪出"
    print("PASS 源码顺序：addWidget 在 setVisible 之前")


if __name__ == "__main__":
    test_chev_is_parented_before_visible()
    test_readonly_cell_chev_stays_parented_and_hidden()
    test_source_order_add_widget_before_setvisible()
    print("\n全部通过")
