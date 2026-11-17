"""验证 Chromium 元素上的 UIA Invoke 是否真能触发动作。

用 DSH（Chromium）窗口做实验：取一个已知会改变界面的控件（页签），Invoke 之后
截图比对。若 Invoke 生效，就说明「Chromium 点不动」的解法是走 UIA + pid 判归属，
而不是退回 PostMessage。

用法: python rc_probe/chromium_invoke_test.py
"""
import ctypes
import pathlib
import time
from ctypes import wintypes as wt

from PIL import ImageGrab

ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
u = ctypes.WinDLL("user32")
ole = ctypes.WinDLL("ole32")
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]

OUT = pathlib.Path(__file__).parent / "out"
OUT.mkdir(exist_ok=True)


def find_window(title_part):
    result = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        n = u.GetWindowTextLengthW(h)
        b = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(h, b, n + 1)
        if title_part in b.value and u.IsWindowVisible(h):
            result.append(h)
        return True

    u.EnumWindows(CB(cb), 0)
    return result[0] if result else None


hwnd = find_window("DSH 本地构建")
if not hwnd:
    print("找不到 DSH 窗口")
    raise SystemExit(1)
u.ShowWindow(hwnd, 9)
u.SetForegroundWindow(hwnd)
time.sleep(1.0)
r = wt.RECT()
u.GetWindowRect(hwnd, ctypes.byref(r))
print("DSH window", (r.left, r.top, r.right, r.bottom))

before = OUT / "chromium_invoke_before.png"
after = OUT / "chromium_invoke_after.png"
ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).save(before)

import comtypes.client as cc
from comtypes.gen import UIAutomationClient as UIA

ole.CoInitializeEx(None, 2)
try:
    ui = cc.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
    # 遍历窗口内所有可 Invoke 且有名字的元素，挑一个 TabItem/Button 试
    root = ui.ElementFromHandle(hwnd)
    cond = ui.CreateTrueCondition()
    found = root.FindAll(4, cond)  # TreeScope_Descendants = 4
    print("descendants:", found.Length if found else 0)
    candidates = []
    for i in range(min(found.Length, 800)):
        el = found.GetElement(i)
        try:
            ctype = el.CurrentControlType
            name = el.CurrentName
        except Exception:
            continue
        if not name:
            continue
        # 50019 TabItem / 50000 Button / 50011 MenuItem
        if ctype in (50019, 50000, 50011):
            try:
                el.GetCurrentPattern(10000)  # Invoke
                candidates.append((i, ctype, name, el))
            except Exception:
                pass
    print("可 Invoke 的候选:", [(c[1], c[2]) for c in candidates][:12])
    if not candidates:
        print("没有可 Invoke 候选")
    else:
        idx, ctype, name, el = candidates[0]
        print(f"Invoke 目标: type={ctype} name={name!r}")
        pat = el.GetCurrentPattern(10000)
        try:
            pat.Invoke()
            print("Invoke() 返回成功")
        except Exception as e:
            print("Invoke() 抛错:", e)
        time.sleep(1.2)
        ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                       all_screens=True).save(after)
        print("已截图比对:", before.name, "->", after.name)
finally:
    try:
        ole.CoUninitialize()
    except Exception:
        pass
