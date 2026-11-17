"""Chromium UIA 元素的归属判据探针。

C++ 侧 invokeAt() 现在要求 UIA 元素报出 CurrentNativeWindowHandle，而 Chromium 系
一律报 0（实测 DSH 窗口）——于是整条 UIA 路被挡掉，只能退回 PostMessage（Chromium
不认）。这里验证「用 ProcessId 判归属」是否可行：同一个点上的元素 pid 是否等于
目标窗口的 pid，以及 Invoke 能不能真调起来。

用法: python rc_probe/chromium_uia_owner.py [x] [y]
"""
import ctypes
import sys
from ctypes import wintypes as wt

ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
u = ctypes.WinDLL("user32")
ole = ctypes.WinDLL("ole32")

u.WindowFromPoint.argtypes = [wt.POINT]
u.WindowFromPoint.restype = wt.HWND
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

X = int(sys.argv[1]) if len(sys.argv) > 1 else 2772
Y = int(sys.argv[2]) if len(sys.argv) > 2 else 2888


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def title(h):
    n = u.GetWindowTextLengthW(h)
    b = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(h, b, n + 1)
    return b.value


hwnd = u.WindowFromPoint(wt.POINT(X, Y))
pid = wt.DWORD()
u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
print(f"目标窗口 {cls(hwnd)}[{title(hwnd)[:36]}] pid={pid.value}")

import comtypes.client as cc
from comtypes.gen import UIAutomationClient as UIA

ole.CoInitializeEx(None, 2)
try:
    ui = cc.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
    el = ui.ElementFromPoint(wt.POINT(X, Y))
    print("ElementFromPoint ->", "有元素" if el else "无元素")
    walker = ui.ControlViewWalker
    node = el
    for hop in range(8):
        if not node:
            break
        try:
            name = node.CurrentName
            ctype = node.CurrentControlType
            ep = node.CurrentProcessId
            eh = node.CurrentNativeWindowHandle
            r = node.CurrentBoundingRectangle
            rect = (r.left, r.top, r.right, r.bottom)
        except Exception as e:
            print(f"  hop{hop}: 读属性失败 {e}")
            break
        print(f"  hop{hop}: pid={ep} hwnd={eh} type={ctype} name={name!r} rect={rect}")
        print(f"          pid 与目标一致: {ep == pid.value}")
        try:
            node = walker.GetParentElement(node)
        except Exception:
            break
finally:
    try:
        ole.CoUninitialize()
    except Exception:
        pass
