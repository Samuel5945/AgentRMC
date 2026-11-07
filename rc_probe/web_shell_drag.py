"""验证 Web 顶栏拖动：按住顶栏空白处走一段，报告窗口位移。

用法: python rc_probe/web_shell_drag.py [dx] [dy]
坐标是物理像素；进程 SetProcessDpiAwareness(2)，SetCursorPos 吃物理坐标。
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)
user32 = ctypes.windll.user32

dx = int(sys.argv[1]) if len(sys.argv) > 1 else 200
dy = int(sys.argv[2]) if len(sys.argv) > 2 else 80

pid = wt.DWORD()
wins = []
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def cb(h, _):
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if user32.IsWindowVisible(h):
        n = user32.GetWindowTextLengthW(h)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, buf, n + 1)
        if buf.value == "AgentRMC Native":
            rr = wt.RECT()
            user32.GetWindowRect(h, ctypes.byref(rr))
            wins.append((h, (rr.left, rr.top, rr.right, rr.bottom), pid.value))
    return True


user32.EnumWindows(CB(cb), 0)
screen_area = 3840 * 2160
cands = [w for w in wins if (w[1][2] - w[1][0]) * (w[1][3] - w[1][1]) < screen_area]
if not cands:
    print("主窗没找到")
    sys.exit(1)
h, box, p = max(cands, key=lambda w: (w[1][2] - w[1][0]) * (w[1][3] - w[1][1]))
if user32.IsZoomed(h):
    user32.ShowWindow(h, 9)  # SW_RESTORE
    time.sleep(0.5)
r = wt.RECT()
user32.GetWindowRect(h, ctypes.byref(r))
print(f"hwnd={h:#x} pid={p} rect=({r.left},{r.top},{r.right},{r.bottom})")
user32.SetForegroundWindow(h)
time.sleep(0.3)

sx, sy = r.left + 600, r.top + 40
user32.SetCursorPos(sx, sy)
time.sleep(0.2)
user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
time.sleep(0.15)
steps = 12
for i in range(1, steps + 1):
    user32.SetCursorPos(sx + dx * i // steps, sy + dy * i // steps)
    time.sleep(0.04)
time.sleep(0.2)
user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
time.sleep(0.5)

r2 = wt.RECT()
user32.GetWindowRect(h, ctypes.byref(r2))
print(f"after=({r2.left},{r2.top},{r2.right},{r2.bottom})")
print(f"moved {r2.left - r.left} {r2.top - r.top} (期望 {dx} {dy})")
user32.SetCursorPos(sx + dx, sy + dy + 200)
