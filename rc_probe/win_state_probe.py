"""Main window state probe: maximize button / F11 fullscreen / taskbar coverage.

Launches a given AgentRMCNative.exe, then for each state records the window
rect against the monitor work area (frameless maximize covering the taskbar is
the classic failure) and saves a screenshot. Physical-pixel safe: button coords
scale by GetDpiForWindow. Keys go via PostMessage so focus isn't needed.

Usage: python win_state_probe.py <exe-path> [shot-prefix]
"""
import ctypes
import subprocess
import sys
import time
import pathlib

# -I 隔离模式不带 user site：PIL 装在用户目录，显式挂进来。
sys.path.insert(0, r"C:\Users\PC\AppData\Roaming\Python\Python312\site-packages")
ctypes.windll.shcore.SetProcessDpiAwareness(2)
from ctypes import byref, wintypes  # noqa: E402

from PIL import ImageGrab  # noqa: E402

user32 = ctypes.windll.user32

EXE = sys.argv[1]
PREFIX = sys.argv[2] if len(sys.argv) > 2 else "winstate"
SHOTS = pathlib.Path(__file__).parent / "shots"
SHOTS.mkdir(exist_ok=True)

WM_KEYDOWN, WM_KEYUP, VK_F11 = 0x0100, 0x0101, 0x7A
SW_MAXIMIZE, SW_RESTORE = 3, 9


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_ulong),
                ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT),
                ("dwFlags", wintypes.DWORD)]


def find_win():
    return user32.FindWindowW(None, "AgentRMC Native")


def rect_of(h):
    r = wintypes.RECT()
    user32.GetWindowRect(h, byref(r))
    return (r.left, r.top, r.right, r.bottom)


def as_rect(r):
    return (r.left, r.top, r.right, r.bottom)


def areas_of(h):
    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(MONITORINFO)
    user32.GetMonitorInfoW(user32.MonitorFromWindow(h, 2), byref(mi))
    return (as_rect(mi.rcMonitor), as_rect(mi.rcWork))


def shoot(h, name):
    img = ImageGrab.grab(bbox=rect_of(h))
    img.save(SHOTS / f"{PREFIX}_{name}.png")


def covers_work(rect, work, tol=4):
    """True = window exactly fills the work area (taskbar still visible)."""
    return all(abs(a - b) <= tol for a, b in zip(rect, work))


def covers_monitor(rect, monitor, tol=4):
    return all(abs(a - b) <= tol for a, b in zip(rect, monitor))


def click_max_button(h):
    """Click the title-bar maximize button: 3rd from the right edge.
    Layout: right margin 10, buttons 28x24, spacing 10 -> center at
    right-62 logical; title bar button row center at y=24 logical."""
    dpi = user32.GetDpiForWindow(h)
    scale = dpi / 96.0
    left, top, right, _ = rect_of(h)
    x = right - int(62 * scale)
    y = top + int(24 * scale)
    user32.SetCursorPos(x, y)
    time.sleep(0.15)
    user32.mouse_event(2, 0, 0, 0, 0)   # down
    user32.mouse_event(4, 0, 0, 0, 0)   # up
    time.sleep(0.8)


def send_f11(h):
    user32.PostMessageW(h, WM_KEYDOWN, VK_F11, 0)
    time.sleep(0.05)
    user32.PostMessageW(h, WM_KEYUP, VK_F11, 0)
    time.sleep(0.8)


subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
               capture_output=True)
time.sleep(0.8)
subprocess.Popen([EXE], cwd=str(pathlib.Path(EXE).parent))

hwnd = None
for _ in range(40):
    time.sleep(0.25)
    hwnd = find_win()
    if hwnd and user32.IsWindowVisible(hwnd):
        break
if not hwnd or not user32.IsWindowVisible(hwnd):
    print("PROBE_FAIL window not found")
    sys.exit(1)
time.sleep(2.5)  # 主题/背景加载完再开始

monitor, work = areas_of(hwnd)
results = []

# 1) 启动态：普通窗口
r0 = rect_of(hwnd)
results.append(("normal_visible", r0[2] - r0[0] > 600 and r0[3] - r0[1] > 400,
                f"rect={r0}"))
shoot(hwnd, "1_normal")

# 2) 点最大化按钮 → 应恰好铺满工作区（自动隐藏任务栏的机器上 workarea
#    就等于整屏，所以只对 work 断言；盖任务栏的老 bug 表现为超出 work）
click_max_button(hwnd)
r1 = rect_of(hwnd)
ok_max = covers_work(r1, work)
results.append(("maximize_button_fills_workarea", ok_max,
                f"rect={r1} work={work} monitor={monitor}"))
shoot(hwnd, "2_maximized")

# 3) 再点还原
click_max_button(hwnd)
r2 = rect_of(hwnd)
results.append(("restore_button_back_to_normal",
                abs(r2[2] - r0[2]) <= 8 and abs(r2[3] - r0[3]) <= 8,
                f"rect={r2} vs {r0}"))

# 4) F11 全屏 → 应铺满整块显示器
send_f11(hwnd)
r3 = rect_of(hwnd)
ok_fs = covers_monitor(r3, monitor)
results.append(("f11_fullscreen_covers_monitor", ok_fs,
                f"rect={r3} monitor={monitor}"))
shoot(hwnd, "3_fullscreen")

# 5) 再按 F11 退出
send_f11(hwnd)
r4 = rect_of(hwnd)
results.append(("f11_exit_fullscreen",
                abs(r4[2] - r0[2]) <= 8 and abs(r4[3] - r0[3]) <= 8,
                f"rect={r4}"))

failed = [x for x in results if not x[1]]
for name, ok, detail in results:
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
print("PROBE_%s" % ("PASS" if not failed else "FAIL"))

# 留着应用给用户看；不需要清理。
