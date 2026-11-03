"""P2 semantics-2: prove Qoder project-dropdown selection + readback.
Expand selector -> options live in a separate popup HWND -> click option by
name (guarded) -> read selector label -> select back original -> read again.
"""
import sys
import time
import pathlib
import ctypes
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
import comtypes.gen.UIAutomationClient as gen

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window, user32  # noqa: E402

IExpand = getattr(gen, "IUIAutomationExpandCollapsePattern")
SELECTOR = "切换或清空当前工作区，当前为 Agentrmc"
TARGET = sys.argv[1] if len(sys.argv) > 1 else "lig-site"
BACK = sys.argv[2] if len(sys.argv) > 2 else "Agentrmc"
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
pids = set(pids_for("Qoder.exe"))
hwnd, title, size = top_window(pids_for("Qoder.exe"))
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.5)
from qoder_ctrl_n_focus import hotkey  # noqa: E402
hotkey(0x11, 0x4E)  # ctrl+n -> landing（选择器只在落地页）
time.sleep(1.0)


def windows():
    out = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            out.append(h)
        return True
    user32.EnumWindows(CB(cb), 0)
    return out


def selector_label():
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(arr.Length):
        el = arr.GetElement(i)
        nm = el.CurrentName or ""
        if nm.startswith("切换或清空当前工作区"):
            return nm
    return "<none>"


def click_option(name):
    for h in windows():
        root = ui.ElementFromHandle(h)
        arr = root.FindAll(4, ui.CreatePropertyCondition(30005, name))
        for i in range(arr.Length):
            el = arr.GetElement(i)
            if el.GetCurrentPropertyValue(30022):
                # 选项可能在滚动区外：ScrollItem 滚进视口再点。
                try:
                    si = el.GetCurrentPattern(10017).QueryInterface(
                        getattr(gen, "IUIAutomationScrollItemPattern"))
                    si.ScrollIntoView()
                    time.sleep(0.4)
                except Exception:
                    continue
            r = el.GetCurrentPropertyValue(30001)
            left, top, w, hh = (tuple(r) if isinstance(r, (tuple, list)) else (
                r.left, r.top, r.right - r.left, r.bottom - r.top))
            if w < 8 or hh < 8:
                continue
            pt = wt.POINT()
            user32.GetCursorPos(byref(pt))
            old = (pt.x, pt.y)
            user32.SetCursorPos(int(left + w / 2), int(top + hh / 2))
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.05)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.05)
            user32.SetCursorPos(*old)
            return True
    return False


def expand():
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if (el.CurrentName or "").startswith("切换或清空当前工作区"):
            try:
                el.GetCurrentPattern(10005).QueryInterface(IExpand).Expand()
            except Exception:
                pass  # 已展开或指针失效：popup 可能还开着
            time.sleep(0.7)
            return True
    return False


print("LABEL_BEFORE=%r" % selector_label())
print("EXPAND=%s" % expand())
print("CLICK_%s=%s" % (TARGET, click_option(TARGET)))
time.sleep(0.8)
print("LABEL_AFTER=%r" % selector_label())
print("EXPAND_BACK=%s" % expand())
print("CLICK_%s=%s" % (BACK, click_option(BACK)))
time.sleep(0.8)
print("LABEL_RESTORED=%r" % selector_label())
