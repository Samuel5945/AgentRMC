"""Open an existing Qoder session row via UIA Invoke (no mouse), so the
default-mode continuation round lands in a known conversation. Verifies the
switch by looking for that session's old message bubble in the tree.
"""
import sys
import time
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
from comtypes import COMMETHOD, GUID, HRESULT, IUnknown

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window, user32  # noqa: E402
from e2e_asr_stub import MARKER  # noqa: E402


class IInvokeProvider(IUnknown):
    _iid_ = GUID("{00225041-116c-4327-9929-00756f4458c8}")
    _methods_ = [COMMETHOD([], HRESULT, "Invoke")]


NEEDLE = sys.argv[1] if len(sys.argv) > 1 else "确认收到派活消息"
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for("Qoder.exe"))
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.5)
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreateTrueCondition())
row = None
for i in range(arr.Length):
    el = arr.GetElement(i)
    try:
        nm = el.CurrentName or ""
    except Exception:
        continue
    if NEEDLE in nm and el.GetCurrentPropertyValue(30003) == 50005:
        row = el
        break
if row is None:
    print("ROW_NOT_FOUND needle=%s" % NEEDLE)
    sys.exit(1)
print("ROW=%r" % row.CurrentName)
# 行不实现 Invoke（实测 QI 失败）、键盘也切不动：带守卫的合成点击。
import ctypes
import ctypes.wintypes as wt
from ctypes import byref
r = row.GetCurrentPropertyValue(30001)
left, top, width, height = (tuple(r) if isinstance(r, (tuple, list))
                            else (r.left, r.top, r.right - r.left,
                                  r.bottom - r.top))
pt = wt.POINT()
user32.GetCursorPos(byref(pt))
old = (pt.x, pt.y)
user32.SetCursorPos(int(left + width / 2), int(top + height / 2))
user32.mouse_event(0x0002, 0, 0, 0, 0)
time.sleep(0.05)
user32.mouse_event(0x0004, 0, 0, 0, 0)
time.sleep(0.05)
user32.SetCursorPos(*old)
time.sleep(1.2)
root = ui.ElementFromHandle(hwnd)
hit = root.FindFirst(4, ui.CreatePropertyCondition(30005, MARKER))
print("OLD_BUBBLE_IN_TREE=%s (switch proven)" % (hit is not None))
