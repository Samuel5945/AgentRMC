"""Where did the marker land in WorkBuddy? Dump every node whose name equals
the marker (rect/offscreen/type via property interface), plus every Edit
control's Value, plus named session-row candidates. Read-only.
"""
import sys
import json
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationValuePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window  # noqa: E402
from e2e_asr_stub import MARKER  # noqa: E402

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for(sys.argv[1] if len(sys.argv) > 1
                                       else "WorkBuddy.exe"))
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreatePropertyCondition(30005, MARKER))
print("MARKER_HITS=%d" % arr.Length)
for i in range(arr.Length):
    el = arr.GetElement(i)
    def prop(pid_):
        try:
            return el.GetCurrentPropertyValue(pid_)
        except Exception as e:
            return "err:%s" % e
    r = prop(30001)
    try:
        r = (r.left, r.top, r.right, r.bottom)
    except Exception:
        pass
    print("HIT type=%s offscreen=%s rect=%s" % (prop(30003), prop(30022), r))
edits = root.FindAll(4, ui.CreatePropertyCondition(30003, 50004))
print("EDITS=%d" % edits.Length)
for i in range(edits.Length):
    el = edits.GetElement(i)
    try:
        vp = el.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
        val = vp.CurrentValue
    except Exception as e:
        val = "err:%s" % e
    try:
        nm = el.CurrentName
    except Exception:
        nm = "?"
    print("EDIT name=%r value=%r" % (nm, (val or "")[:90]))
