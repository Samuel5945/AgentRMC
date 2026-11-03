"""P2 semantics-2 prerequisite: measure Qoder new-task landing project dropdown.
stage scan: ctrl+n -> list ExpandCollapse-capable controls (name/rect/type).
stage expand <name>: expand that control, list resulting option names, Escape.
No selection is committed.
"""
import sys
import time
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
import comtypes.gen.UIAutomationClient as gen

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import (pids_for, top_window, user32, hotkey,  # noqa: E402
                                shot)

IExpand = getattr(gen, "IUIAutomationExpandCollapsePattern")
exe = "Qoder.exe"
stage = sys.argv[1] if len(sys.argv) > 1 else "scan"
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for(exe))
if user32.IsIconic(hwnd):
    user32.ShowWindow(hwnd, 9)
    time.sleep(0.8)
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.6)
if stage == "scan":
    hotkey(0x11, 0x4E)  # ctrl+n -> landing
    time.sleep(1.0)
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreateTrueCondition())
cands = []
for i in range(arr.Length):
    el = arr.GetElement(i)
    nm = el.CurrentName or ""
    try:
        el.GetCurrentPattern(10005).QueryInterface(IExpand)
    except Exception:
        continue
    r = el.GetCurrentPropertyValue(30001)
    rect = tuple(r) if isinstance(r, (tuple, list)) else (
        r.left, r.top, r.right - r.left, r.bottom - r.top)
    cands.append((nm, el.GetCurrentPropertyValue(30003), rect, el))
    print("EXPANDABLE type=%s name=%r rect=%s" % (
        el.GetCurrentPropertyValue(30003), nm[:50], rect))
if stage.startswith("expand"):
    want = sys.argv[2]
    target = None
    for nm, ct, rect, el in cands:
        if nm == want:
            target = el
            break
    if target is None:
        print("NO_MATCH name=%s" % want)
        sys.exit(1)
    pat = target.GetCurrentPattern(10005).QueryInterface(IExpand)
    pat.Expand()
    time.sleep(0.8)
    root = ui.ElementFromHandle(hwnd)
    opts = root.FindAll(4, ui.CreateTrueCondition())
    seen = 0
    for i in range(opts.Length):
        el = opts.GetElement(i)
        ct = el.GetCurrentPropertyValue(30003)
        nm = el.CurrentName or ""
        if ct in (50007, 50024, 50003, 50020) and nm and \
                not el.GetCurrentPropertyValue(30022):
            print("OPTION type=%s name=%r" % (ct, nm[:60]))
            seen += 1
            if seen >= 30:
                break
    shot(hwnd, r"D:\githubrepos\Agentrmc\rc_probe\p2_qoder_dropdown.png")
    user32.keybd_event(0x1B, 0, 0, 0)
    user32.keybd_event(0x1B, 0, 2, 0)
    print("ESCAPED (no selection committed)")
