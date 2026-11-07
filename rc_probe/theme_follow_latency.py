"""量「跟随系统深浅色」的端到端延迟：翻一次 AppsUseLightTheme + 广播，
轮询主窗像素直到变色，再翻回来还原。只动 AppsUseLightTheme（任务栏跟的
SystemUsesLightTheme 不碰），结束恢复原值。

用法: python rc_probe/theme_follow_latency.py
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time
import winreg

ctypes.windll.shcore.SetProcessDpiAwareness(2)
from PIL import Image  # noqa: E402

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

KEY = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"

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
            wins.append((h, (rr.left, rr.top, rr.right, rr.bottom)))
    return True


user32.EnumWindows(CB(cb), 0)
screen_area = 3840 * 2160
cands = [w for w in wins if (w[1][2] - w[1][0]) * (w[1][3] - w[1][1]) < screen_area]
if not cands:
    print("主窗没找到")
    sys.exit(1)
h, box = max(cands, key=lambda w: (w[1][2] - w[1][0]) * (w[1][3] - w[1][1]))
w, hh = box[2] - box[0], box[3] - box[1]
print(f"hwnd={h:#x} size={w}x{hh}")
user32.SetForegroundWindow(h)
time.sleep(0.4)


class BI(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


dc0 = user32.GetDC(0)
bmp = gdi32.CreateCompatibleBitmap(dc0, w, hh)
dc = gdi32.CreateCompatibleDC(dc0)
gdi32.SelectObject(dc, bmp)
bmi = BI()
bmi.biSize = ctypes.sizeof(BI)
bmi.biWidth, bmi.biHeight = w, -hh
bmi.biPlanes, bmi.biBitCount, bmi.biCompression = 1, 32, 0
buf = ctypes.create_string_buffer(w * hh * 4)


def pixel(x, y):
    # 屏幕抓 1x1：不碰 app 的 UI 线程。PrintWindow 会派 WM_PRINT 让主窗同步
    # 重绘整窗（含 WebView2），轮询它本身就把被测延迟拖慢（实测污染 ~1s）。
    from PIL import ImageGrab
    return ImageGrab.grab(bbox=(box[0] + x, box[1] + y, box[0] + x + 1,
                                box[1] + y + 1),
                          all_screens=True).getpixel((0, 0))[:3]


def read_pref():
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as k:
        return winreg.QueryValueEx(k, "AppsUseLightTheme")[0]


def write_pref(v):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE) as k:
        winreg.SetValueEx(k, "AppsUseLightTheme", 0, winreg.REG_DWORD, v)


def broadcast():
    area = ctypes.create_unicode_buffer("ImmersiveColorSet")
    res = wt.DWORD()
    t = time.perf_counter()
    user32.SendMessageTimeoutW(0xFFFF, 0x001A, 0,
                               ctypes.cast(area, ctypes.c_void_p),
                               0x0002, 3000, ctypes.byref(res))
    return (time.perf_counter() - t) * 1000


def wait_change(base, timeout=20.0):
    t = time.perf_counter()
    while time.perf_counter() - t < timeout:
        p = pixel(10, 300)
        if p is not None and p != base:
            return (time.perf_counter() - t) * 1000, p
        time.sleep(0.04)
    return None, pixel(10, 300)


orig = read_pref()
base = pixel(10, 300)
print(f"原值={orig} 基线像素={base}")

import threading  # noqa: E402


def flip_and_measure(target, baseline):
    """广播在后台线程跑（它自己要等所有顶层窗应答，本机近 3 秒），
    主线程从写注册表那一刻起轮询窗口像素——这才是用户感知的延迟。"""
    write_pref(target)
    print(f"  write epoch ms={time.time() * 1000:.0f}")
    t0 = time.perf_counter()
    th = None
    if "--nobroadcast" not in sys.argv:
        th = threading.Thread(target=broadcast)
        th.start()
    ms, p = wait_change(baseline)
    if th is not None:
        th.join()
    return ms, p


ms, p = flip_and_measure(1 - orig, base)
print(f"翻到 {1 - orig}: 写注册表起窗口变色 {ms if ms is None else round(ms)}ms -> {p}")

ms2, p2 = flip_and_measure(orig, p if p is not None else base)
print(f"还原 {orig}: 写注册表起窗口变色 {ms2 if ms2 is None else round(ms2)}ms -> {p2}")
print("现值=", read_pref())
gdi32.DeleteDC(dc)
gdi32.DeleteObject(bmp)
