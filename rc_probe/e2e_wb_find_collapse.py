"""List named Button/TabItem/MenuItem controls in WorkBuddy main window whose
name hints at collapsing to the widget (收起/折叠/最小化/挂件), read-only.
"""
import sys
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window  # noqa: E402

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for("WorkBuddy.exe"))
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreateTrueCondition())
print("NODES=%d" % arr.Length)
keys = ("收起", "折叠", "最小化", "挂件", "隐藏", "collapse", "minimize")
for i in range(arr.Length):
    el = arr.GetElement(i)
    try:
        nm = el.CurrentName or ""
    except Exception:
        continue
    if nm and any(k in nm.lower() for k in keys):
        try:
            ct = el.GetCurrentPropertyValue(30003)
            r = el.GetCurrentPropertyValue(30001)
            rect = (r.left, r.top, r.right, r.bottom)
        except Exception:
            ct, rect = "?", "?"
        print("CAND type=%s name=%r rect=%s" % (ct, nm[:40], rect))
