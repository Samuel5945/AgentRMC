"""P2 activation measurement: SetFocus + Enter on a named Button, verify where
focus lands and whether a composer appears. No paste, no send.
--restore-session: remember the focused document's session= token before, and
after measuring click back the sidebar row whose Value carries that token
(guarded synthetic click), so the user's view ends where it started.
"""
import sys
import time
import pathlib
import re

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationValuePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import (pids_for, top_window, user32, send_key,  # noqa: E402
                                shot)
import ctypes.wintypes as wt
from ctypes import byref

exe = sys.argv[1]
name = sys.argv[2]
restore = "--restore-session" in sys.argv
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for(exe))
if user32.IsIconic(hwnd):
    user32.ShowWindow(hwnd, 9)
    time.sleep(0.8)
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.6)


def value_of(el):
    try:
        return el.GetCurrentPattern(10002).QueryInterface(
            IUIAutomationValuePattern).CurrentValue or ""
    except Exception:
        return ""


def focused():
    f = ui.GetFocusedElement()
    if f is None:
        return None, ""
    return f.GetCurrentPropertyValue(30003), value_of(f)


before_type, before_val = focused()
token = ""
m = re.search(r"session=([A-Za-z0-9:_-]+)", before_val)
if m:
    token = m.group(1)
print("BEFORE focus_type=%s session_token=%s" % (before_type, token or "<none>"))
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreatePropertyCondition(30005, name))
target = None
for i in range(arr.Length):
    el = arr.GetElement(i)
    if el.GetCurrentPropertyValue(30003) != 50000:
        continue
    if el.GetCurrentPropertyValue(30022):  # offscreen
        continue
    r = el.GetCurrentPropertyValue(30001)
    w, h = (r[2], r[3]) if isinstance(r, (tuple, list)) else (
        r.right - r.left, r.bottom - r.top)
    if w >= 8 and h >= 8:
        target = el
        break
if target is None:
    print("NO_VISIBLE_BUTTON name=%s" % name)
    sys.exit(1)
hr = target.SetFocus()
time.sleep(0.3)
ft, fv = focused()
print("SETFOCUS hr=%s landed_type=%s landed_name=%r" % (
    hr, ft, ui.GetFocusedElement().CurrentName))
send_key(0x0D, False)
time.sleep(0.02)
send_key(0x0D, True)
time.sleep(1.0)
ft, fv = focused()
print("AFTER_ENTER focus_type=%s focus_value=%r" % (ft, fv[:90]))
root = ui.ElementFromHandle(hwnd)
edits = root.FindAll(4, ui.CreatePropertyCondition(30003, 50004))
for i in range(edits.Length):
    el = edits.GetElement(i)
    print("EDIT name=%r value=%r offscreen=%s" % (
        (el.CurrentName or "")[:50], value_of(el)[:50],
        el.GetCurrentPropertyValue(30022)))
tag = exe.lower().replace(" ", "_").replace(".exe", "")
shot(hwnd, r"D:\githubrepos\Agentrmc\rc_probe\p2_activate_%s.png" % tag)
if restore and token:
    arr = root.FindAll(4, ui.CreateTrueCondition())
    back = None
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if token in value_of(el):
            back = el
            break
    if back is None:
        print("RESTORE_ROW_NOT_FOUND token=%s" % token)
    else:
        r = back.GetCurrentPropertyValue(30001)
        left, top, w, h = (tuple(r) if isinstance(r, (tuple, list)) else (
            r.left, r.top, r.right - r.left, r.bottom - r.top))
        pt = wt.POINT()
        user32.GetCursorPos(byref(pt))
        old = (pt.x, pt.y)
        user32.SetCursorPos(int(left + w / 2), int(top + h / 2))
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.05)
        user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(0.05)
        user32.SetCursorPos(*old)
        time.sleep(1.0)
        ft, fv = focused()
        print("RESTORED focus_value=%r contains_token=%s" % (
            fv[:90], token in fv))
