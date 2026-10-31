"""验证修复：启动建表 + 多次重建 keymap + 最小化/还原，全程不应出现顶层小窗。"""
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication
import agentrc_gui as gui

app = QApplication(sys.argv)
app.setStyleSheet(gui.QSS)
win = gui.MainWindow(with_tray=True)
win.show()
QTimer.singleShot(1200, win._refresh_keymap)
QTimer.singleShot(2000, win._refresh_keymap)
QTimer.singleShot(2800, lambda: win.map_combo.setCurrentIndex(
    (win.map_combo.currentIndex() + 1) % max(1, win.map_combo.count())))
QTimer.singleShot(3600, win.showMinimized)
QTimer.singleShot(4800, win.showNormal)
QTimer.singleShot(6000, app.quit)
app.exec()
print("verify done", flush=True)
