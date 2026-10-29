"""按键映射页布局几何探针：不画到屏幕上，只量尺寸和对齐。

    .venv\\Scripts\\python.exe rc_probe\\keymap_layout_probe.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from PySide6.QtCore import Qt, QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication, QLabel, QPushButton  # noqa: E402

import agentrc_gui as gui  # noqa: E402

APP = QApplication.instance() or QApplication(sys.argv)
APP.setStyleSheet(gui.QSS)


def spin(ms: int = 120) -> None:
    end = QTimer()
    end.setSingleShot(True)
    end.start(ms)
    while end.isActive():
        APP.processEvents()


def dump(win: gui.MainWindow, tag: str) -> None:
    vp = win.map_scroll.viewport()
    header = win.map_grid.itemAt(0).widget()
    heads = [(l.text(), l.geometry().x(), l.width())
             for l in header.findChildren(QLabel) if l.objectName() == "mapHead"]
    row = win.map_rows[38]
    cells = {g: (c.geometry().x(), c.width())
             for g, c in row.cells.items()}
    titles = []
    for g, c in row.cells.items():
        for l in c.findChildren(QLabel):
            if l.objectName() in ("mapCellTitle", "mapCellDash"):
                titles.append((g, l.geometry().y()))
    btns = [(b.text(), b.geometry().x(), b.y())
            for b in win.findChildren(QPushButton)
            if b.objectName() == "ghostSmall"
            and b.text() in ("高级配置", "重新加载")]
    vis = sum(1 for i in range(win.map_grid.count())
              if isinstance(win.map_grid.itemAt(i).widget(), gui.MapKeyRow)
              and win.map_grid.itemAt(i).widget().geometry().bottom() <= vp.height())
    print(f"[{tag}] viewport={vp.width()}x{vp.height()} "
          f"rows_visible={vis}/10 vbar_max={win.map_scroll.verticalScrollBar().maximum()}")
    print(f"[{tag}] header={heads}")
    print(f"[{tag}] cells={cells} title_y={titles}")
    print(f"[{tag}] page_buttons={btns}")


win = gui.MainWindow(with_tray=False)
win.setAttribute(Qt.WA_DontShowOnScreen)
win.show()
spin()
win.tabs.setCurrentIndex(1)
spin()

dump(win, "default")
win.resize(880, 560)
spin()
dump(win, "min880")

# 预设菜单尺寸（回归检查：菜单结构没被这次改动影响）
for name, (vk, gesture) in (("single", (38, "single")), ("double", (38, "double")),
                            ("hold", (38, "hold"))):
    m = win._build_preset_menu(vk, gesture)
    if m:
        print(f"menu[{name}] {m.sizeHint().width()}x{m.sizeHint().height()} "
              f"actions={len(m.actions())}")
        m.deleteLater()

win.deleteLater()
spin()
