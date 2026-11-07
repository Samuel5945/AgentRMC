"""WebView2 原型：在主窗右半区按「截图坐标」点一下，然后重截一张。

坐标用的是 rc_probe/web_shell_screen.png 的像素（= 物理像素），脚本自己加上
窗口原点换成屏幕坐标；进程已 SetProcessDpiAwareness(2)，SetCursorPos 吃的
就是物理坐标，不会踩 DPI 缩放那一坑。

用法: python rc_probe/web_shell_click.py <image_x> <image_y> [tag]
"""
import ctypes
import ctypes.wintypes as wt
import pathlib
import sys
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)
from PIL import ImageGrab  # noqa: E402

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

x, y = int(sys.argv[1]), int(sys.argv[2])
tag = sys.argv[3] if len(sys.argv) > 3 else "click"

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
for h_, box_, p_ in wins:
    print(f"候选 hwnd={h_:#x} rect={box_} pid={p_}")
# 同名窗口可能有多个（全屏遮罩也算），主窗按面积挑：不要整屏那种。
screen_area = 3840 * 2160
cands = [w for w in wins if (w[1][2] - w[1][0]) * (w[1][3] - w[1][1]) < screen_area]
if not cands:
    print("主窗没找到（只有整屏同名窗？）")
    sys.exit(1)
h, box, _ = max(cands, key=lambda w: (w[1][2] - w[1][0]) * (w[1][3] - w[1][1]))
print(f"选中 hwnd={h:#x} rect={box}")
user32.ShowWindow(h, 5)
user32.SetForegroundWindow(h)
time.sleep(0.3)
r = wt.RECT()
user32.GetWindowRect(h, ctypes.byref(r))
sx, sy = r.left + x, r.top + y
print(f"点击 图像({x},{y}) -> 屏幕({sx},{sy})")
user32.SetCursorPos(sx, sy)
time.sleep(0.15)
user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
time.sleep(0.08)  # 真人点击约 80–150ms；零间隔偶尔会被 Chromium 当成长按外的杂讯
user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
time.sleep(0.6)

w, hh = r.right - r.left, r.bottom - r.top
shot = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True)
out = pathlib.Path("rc_probe") / f"web_shell_{tag}.png"
shot.save(out)
print("saved", out, shot.size)
