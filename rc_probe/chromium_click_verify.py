"""Chromium 点击「是否真生效」的严格验收。

⚠️ 关键：AgentRMC 的指针自己画在屏幕上，会贡献 7~19% 的像素差，直接前后截图比对
会把「点不动」误判成「生效」（2026-11-17 踩过这个坑）。所以流程必须是：
  隐藏指针 → 截 before → 点击 → 等动画结束 → 再隐藏指针 → 截 after → 比对。

用法: python rc_probe/chromium_click_verify.py <按钮名> [backend]
backend: auto | uia | post
"""
import ctypes
import json
import pathlib
import sys
import time
import urllib.request
from ctypes import wintypes as wt

from PIL import Image, ImageChops, ImageGrab

ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
u = ctypes.WinDLL("user32")
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.IsWindowVisible.argtypes = [wt.HWND]
u.IsIconic.argtypes = [wt.HWND]

OUT = pathlib.Path(__file__).parent / "out"
OUT.mkdir(exist_ok=True)
NAME = sys.argv[1] if len(sys.argv) > 1 else "插件"
BACKEND = sys.argv[2] if len(sys.argv) > 2 else "auto"
BRIDGE = "http://127.0.0.1:45915"


def post(path, payload):
    req = urllib.request.Request(
        BRIDGE + path, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode())


def get(path):
    with urllib.request.urlopen(BRIDGE + path, timeout=10) as resp:
        return json.loads(resp.read().decode())


def dsh_window():
    found = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        n = u.GetWindowTextLengthW(h)
        b = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(h, b, n + 1)
        if "DSH" in b.value and u.IsWindowVisible(h) and not u.IsIconic(h):
            found.append(h)
        return True

    u.EnumWindows(CB(cb), 0)
    return found[0] if found else None


hwnd = dsh_window()
if not hwnd:
    print("找不到可见的 DSH 窗口")
    raise SystemExit(1)
u.SetForegroundWindow(hwnd)
time.sleep(1.0)
r = wt.RECT()
u.GetWindowRect(hwnd, ctypes.byref(r))
bbox = (r.left, r.top, r.right, r.bottom)

import comtypes.client as cc
from comtypes.gen import UIAutomationClient as UIA

ctypes.WinDLL("ole32").CoInitializeEx(None, 2)
ui = cc.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
root = ui.ElementFromHandle(hwnd)
found = root.FindAll(4, ui.CreateTrueCondition())
target = None
for i in range(min(found.Length, 1500)):
    el = found.GetElement(i)
    try:
        if el.CurrentName == NAME:
            target = el
            break
    except Exception:
        continue
if target is None:
    print(f"UIA 里找不到名为 {NAME!r} 的元素（树可能还在惰性构建，稍后重试）")
    raise SystemExit(2)
br = target.CurrentBoundingRectangle
cx, cy = (br.left + br.right) // 2, (br.top + br.bottom) // 2
print(f"目标 {NAME!r} 中心 ({cx},{cy})")

post("/cursor/hide", {"fade_ms": 0})
time.sleep(1.0)
before = ImageGrab.grab(bbox=bbox, all_screens=True)
before.save(OUT / "ccv_before.png")

res = post("/cursor/click", {"x": cx, "y": cy, "agent": "验收", "button": "left",
                             "backend": BACKEND, "duration_ms": 0, "hold_ms": 0})
print("click ->", res.get("ok"), res.get("clicked"))
time.sleep(1.5)
st = get("/cursor/state")
lc = st.get("last_click") or {}
print("last_click:", json.dumps(lc, ensure_ascii=False))

# 必须再隐藏一次：指针可能还亮着，它的像素不算目标的变化
post("/cursor/hide", {"fade_ms": 0})
time.sleep(1.2)
after = ImageGrab.grab(bbox=bbox, all_screens=True)
after.save(OUT / "ccv_after.png")

diff = ImageChops.difference(before.convert("RGB"), after.convert("RGB"))
changed = sum(1 for px in diff.getdata() if px != (0, 0, 0))
total = before.width * before.height
pct = changed * 100.0 / total
print(f"严格比对（前后都无指针）: {changed}/{total} = {pct:.3f}%")
print("结论:", "生效（画面变了）" if pct > 0.1 else "未生效（画面没动）")
