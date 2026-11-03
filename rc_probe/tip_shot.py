"""Hover a widget in the live AgentRMC app and screenshot the Qt tooltip.

Usage: python rc_probe/tip_shot.py <uia_name> <tag>
"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import sys
import time
import pathlib
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

import comtypes.client
from PIL import ImageGrab

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32  # noqa: E402

NAME = sys.argv[1] if len(sys.argv) > 1 else "续当前会话（默认）"
TAG = sys.argv[2] if len(sys.argv) > 2 else "dispatch"
OUT = r"D:\githubrepos\Agentrmc\rc_probe\tip_%s.png" % TAG
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
pids = set(pids_for("AgentRMCNative.exe"))
COMBO, BUTTON, TOOLTP = 50003, 50000, 50023


def windows():
    out = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            cls = create_unicode_buffer(256)
            user32.GetClassNameW(h, cls, 256)
            n = user32.GetWindowTextLengthW(h)
            b = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, b, n + 1)
            out.append((h, b.value, cls.value))
        return True
    user32.EnumWindows(CB(cb), 0)
    return out


def rect(el):
    r = el.GetCurrentPropertyValue(30001)
    if isinstance(r, (tuple, list)):
        return r
    return (r.left, r.top, r.width, r.height)


def find(name, types=(COMBO, BUTTON)):
    for h, title, _ in windows():
        if not title.startswith("AgentRMC"):
            continue
        root = ui.ElementFromHandle(h)
        arr = root.FindAll(4, ui.CreateTrueCondition())
        for i in range(arr.Length):
            el = arr.GetElement(i)
            if (el.CurrentName or "").strip() == name.strip() and \
                    el.GetCurrentPropertyValue(30003) in types:
                return el
    return None


def click(el):
    l, t, w, h = rect(el)
    pt = wt.POINT()
    user32.GetCursorPos(byref(pt))
    old = (pt.x, pt.y)
    user32.SetCursorPos(int(l + w / 2), int(t + h / 2))
    time.sleep(0.1)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    user32.SetCursorPos(*old)
    time.sleep(0.8)


if NAME.startswith("at:"):
    # 坐标模式：Qt 的 QComboBox 在 UIA 里常常没有可读 Name，直接按屏幕坐标悬停。
    cx, cy = (int(v) for v in NAME[3:].split(","))
else:
    target = find(NAME)
    if target is None:
        voice_btn = find("语音")
        if voice_btn is not None:
            click(voice_btn)
            target = find(NAME)
    if target is None:
        print("NO_TARGET name=%s" % NAME)
        sys.exit(1)
    l, t, w, h = rect(target)
    cx, cy = int(l + w / 2), int(t + h / 2)

# 把鼠标所在窗口拉到前台（tooltip 只在激活窗里冒）
pt = wt.POINT(cx, cy)
under = user32.WindowFromPoint(pt)
top = user32.GetAncestor(under, 2) or under
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(top)
time.sleep(0.5)

before = {k[0] for k in windows()}
# 悬停：真实移动鼠标并保持静止，Qt 的 tooltip 计时器才会走。
for step in range(8, 0, -1):
    user32.SetCursorPos(cx - step * 14, cy - step * 5)
    time.sleep(0.02)
user32.SetCursorPos(cx, cy)
tip = None
for _ in range(50):
    time.sleep(0.1)
    fresh = [k for k in windows() if k[0] not in before]
    tip = next((k for k in fresh if "ToolTip" in k[2]), None)
    if tip is None:
        tip = next(iter(fresh), None)
    if tip is not None:
        break
if tip is None:
    ImageGrab.grab(bbox=(cx, cy, cx + 1500, cy + 420), all_screens=True).save(OUT)
    print("NO_TIP_WINDOW fallback_box=(%d,%d,%d,%d) saved=%s"
          % (cx, cy, cx + 1500, cy + 420, OUT))
    sys.exit(2)

r = wt.RECT()
user32.GetWindowRect(tip[0], byref(r))
img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True)
img.save(OUT)
print("tip_hwnd=%s class=%s rect=(%d,%d,%d,%d) size=%dx%d saved=%s"
      % (tip[0], tip[2], r.left, r.top, r.right, r.bottom,
         r.right - r.left, r.bottom - r.top, OUT))
