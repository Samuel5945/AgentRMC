"""Why did focusComposer fail? List WorkBuddy Edits (rect/value), SetFocus the
first visible one, read back where focus landed. Read-only + one SetFocus.
"""
import sys
import json
import time
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationValuePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window  # noqa: E402

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for("WorkBuddy.exe"))
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreatePropertyCondition(30003, 50004))
print("EDITS=%d" % arr.Length)
first = None
for i in range(arr.Length):
    el = arr.GetElement(i)
    r = el.GetCurrentPropertyValue(30001)
    # comtypes marshals UIA BoundingRectangle as (left, top, width, height)
    rect = tuple(r) if isinstance(r, (tuple, list)) else (
        r.left, r.top, r.right - r.left, r.bottom - r.top)
    try:
        vp = el.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
        val = (vp.CurrentValue or "")[:40]
    except Exception:
        val = "<no value>"
    off = el.GetCurrentPropertyValue(30022)
    print("EDIT rect=%s offscreen=%s value=%r" % (rect, off, val))
    if first is None and rect[2] >= 8 and rect[3] >= 8:
        first = el
if first is None:
    print("NO_VISIBLE_EDIT")
    sys.exit(1)
from qoder_ctrl_n_focus import user32  # noqa: E402
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.5)
hr = first.SetFocus()
time.sleep(0.4)
f = ui.GetFocusedElement()
import ctypes
import ctypes.wintypes as wt
from ctypes import byref
wpid = wt.DWORD()
ctypes.windll.user32.GetWindowThreadProcessId(hwnd, byref(wpid))
print("SETFOCUS_HR=%s focused_type=%s focused_pid=%s window_pid=%s" % (
    hr, f.GetCurrentPropertyValue(30003), f.GetCurrentPropertyValue(30002),
    wpid.value))
