"""自检：SendInput 注入的按键能不能被普通窗口收到。

开一个 Qt 输入框，800ms 后注入 "abc"，再读回输入框内容。
用法: python rc_probe/inject_test.py
"""
import ctypes
import sys
import time
from ctypes import wintypes as wt

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication, QLineEdit

sys.path.insert(0, ".")
import agentrc as core  # noqa: E402

u = ctypes.WinDLL("user32")
KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_KEYUP = 0x0002
INPUT_KEYBOARD = 1
MAPVK_VK_TO_VSC = 0


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _U)]


u.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
u.MapVirtualKeyW.argtypes = [wt.UINT, wt.UINT]


def send(vk, up=False):
    scan = u.MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 0xFF
    i = INPUT()
    i.type = INPUT_KEYBOARD
    i.u.ki = KEYBDINPUT(0, scan, KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0), 0, 0)
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


app = QApplication(sys.argv)
box = QLineEdit()
box.setWindowTitle("inject-test")
box.resize(320, 60)
box.show()
box.activateWindow()
box.setFocus()


def do_inject():
    hwnd = int(box.winId())
    fg = int(u.GetForegroundWindow())
    print(f"前台窗口={fg} 本窗口={hwnd} 焦点匹配={fg == hwnd}")
    u.SetForegroundWindow(hwnd)
    time.sleep(0.15)
    print(f"置前后前台={int(u.GetForegroundWindow())}")
    for ch in "abc":
        vk = ord(ch.upper())
        send(vk)
        time.sleep(0.03)
        send(vk, up=True)
        time.sleep(0.03)


def report():
    text = box.text()
    print(f"输入框内容: {text!r}")
    print("结论:", "注入有效，窗口收到了按键" if text else "注入无效，窗口没收到")
    app.quit()


QTimer.singleShot(900, do_inject)
QTimer.singleShot(1800, report)
sys.exit(app.exec())
