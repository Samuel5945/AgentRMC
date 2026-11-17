"""Chromium 子窗点击可行性探针。

回答一个问题：Chromium 系（DSH / Qoder 这类 Electron/CEF 窗口）能不能被
「不动系统光标」地点击？逐条试下面四条路，把结果打出来：

  A. PostMessage 到 Chrome_WidgetWin_1（顶层）
  B. PostMessage 到 Chrome_RenderWidgetHostHWND（渲染窗）
  C. UIA ElementFromPoint → 找最近的可 Invoke 元素
  D. 鼠标注入（SendInput）—— 只测「会不会生效」，不改真实光标位置前先存后还原

用法: python rc_probe/chromium_click_probe.py [x] [y]
"""
import ctypes
import sys
import time
from ctypes import wintypes as wt

ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
u = ctypes.WinDLL("user32")
ole = ctypes.WinDLL("ole32")

u.WindowFromPoint.argtypes = [wt.POINT]
u.WindowFromPoint.restype = wt.HWND
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.SendMessageW.restype = ctypes.c_ssize_t
u.GetAncestor.argtypes = [wt.HWND, wt.UINT]
u.GetAncestor.restype = wt.HWND
u.ScreenToClient.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
u.ChildWindowFromPointEx.argtypes = [wt.HWND, wt.POINT, wt.UINT]
u.ChildWindowFromPointEx.restype = wt.HWND
u.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.EnumChildWindows.argtypes = [wt.HWND, ctypes.c_void_p, wt.LPARAM]

X = int(sys.argv[1]) if len(sys.argv) > 1 else 2772
Y = int(sys.argv[2]) if len(sys.argv) > 2 else 2888

GA_ROOT = 2
GA_PARENT = 1
CWP_SKIPINVISIBLE = 0x0001
CWP_SKIPTRANSPARENT = 0x0002


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def title(h):
    n = u.GetWindowTextLengthW(h)
    b = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(h, b, n + 1)
    return b.value


def chain(h):
    out = []
    while h:
        out.append(f"{cls(h)}[{title(h)[:28]}]")
        h = u.GetParent(h)
    return out


def post_click(hwnd, sx, sy, label):
    if not hwnd:
        print(f"  {label}: 没有窗口")
        return
    p = wt.POINT(sx, sy)
    u.ScreenToClient(hwnd, ctypes.byref(p))
    r = wt.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    inside = 0 <= p.x < r.right and 0 <= p.y < r.bottom
    lp = (p.y << 16) | (p.x & 0xFFFF)
    u.PostMessageW(hwnd, 0x0200, 0, lp)   # WM_MOUSEMOVE
    u.PostMessageW(hwnd, 0x0201, 1, lp)   # WM_LBUTTONDOWN
    u.PostMessageW(hwnd, 0x0202, 0, lp)   # WM_LBUTTONUP
    print(f"  {label}: client=({p.x},{p.y}) inside={inside} cls={cls(hwnd)} posted")


def probe_uia(sx, sy):
    # 用 C++ 侧同一套：CoInitialize + CUIAutomation（这里只测 ElementFromPoint 有
    # 没有元素、有没有 Invoke，不实际点）
    try:
        import comtypes.client as cc
        from comtypes.gen import UIAutomationClient as UIA
    except Exception as e:
        print("  UIA: 拿不到 UIAutomationClient:", e)
        return
    ole.CoInitializeEx(None, 2)
    try:
        ui = cc.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
        pt = wt.POINT(sx, sy)
        el = ui.ElementFromPoint(pt)
        if not el:
            print("  UIA: ElementFromPoint 无元素")
            return
        try:
            print(f"  UIA element: name={el.CurrentName!r} type={el.CurrentControlType} "
                  f"hwnd={el.CurrentNativeWindowHandle}")
        except Exception as e:
            print("  UIA: 读元素失败", e)
        walker = ui.ControlViewWalker
        node = el
        for hop in range(6):
            if not node:
                break
            try:
                name = node.CurrentName
                ctype = node.CurrentControlType
            except Exception:
                name, ctype = "?", "?"
            pats = []
            for pid, pname in ((10000, "Invoke"), (10018, "LegacyIAccessible")):
                try:
                    node.GetCurrentPattern(pid)
                    pats.append(pname)
                except Exception:
                    pass
            print(f"    hop{hop}: type={ctype} name={name!r} patterns={pats}")
            try:
                node = walker.GetParentElement(node)
            except Exception:
                break
    finally:
        try:
            ole.CoUninitialize()
        except Exception:
            pass


def probe_sendinput(sx, sy):
    # 只回答「注入能不能生效」，不在这里真点（留给人工确认）。
    print("  SendInput: 需要真移动系统光标（与设计底线冲突），此处不执行")


root = u.WindowFromPoint(wt.POINT(X, Y))
print(f"点 ({X},{Y}) 命中: {cls(root)}[{title(root)[:40]}]")
top = u.GetAncestor(root, GA_ROOT)
print(f"顶层窗: {cls(top)}[{title(top)[:40]}]")
print("窗口链:", " <- ".join(chain(root)))

print("\n[A] PostMessage 顶层 Chrome_WidgetWin_1")
post_click(top, X, Y, "top")

print("\n[B] PostMessage 渲染子窗 Chrome_RenderWidgetHostHWND")
render = None
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def find(h, l):
    global render
    if cls(h) == "Chrome_RenderWidgetHostHWND":
        render = h
        return False
    return True


u.EnumChildWindows(top, CB(find), 0)
print("  render hwnd:", render)
post_click(render, X, Y, "render")

print("\n[C] UIA 元素树")
probe_uia(X, Y)

print("\n[D] SendInput")
probe_sendinput(X, Y)
