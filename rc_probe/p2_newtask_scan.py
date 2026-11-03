"""P2 measurement: read-only UIA scan of a host's new-task affordances.

Lists named controls whose name hits new-task / new-session / project-selector
keywords, with control type + which of Invoke/ExpandCollapse/SelectionItem
patterns actually QI (a pattern listed by the provider but failing QI is not
usable). Prints named-node count so "0 hits" can be told from "tree not built".
"""
import sys
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
import comtypes.gen.UIAutomationClient as gen
from comtypes import COMMETHOD, GUID, HRESULT, IUnknown

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window, user32  # noqa: E402
import time


class IInvoke(IUnknown):
    _iid_ = GUID("{00225041-116c-4327-9929-00756f4458c8}")
    _methods_ = [COMMETHOD([], HRESULT, "Invoke")]


IExpand = getattr(gen, "IUIAutomationExpandCollapsePattern", None)
if IExpand is None:
    class IExpand(IUnknown):
        _iid_ = GUID("{9F7D7BB5-20B6-4771-A232-864A5D3F1F09}")
        _methods_ = [COMMETHOD([], HRESULT, "Expand"),
                     COMMETHOD([], HRESULT, "Collapse")]


KEYS = ("新建任务", "新任务", "新建会话", "新会话", "new task", "new chat",
        "new session", "创建任务", "发起任务", "项目", "workspace", "工作区")

exe = sys.argv[1]
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
pids = pids_for(exe)
if not pids:
    print("NOT_RUNNING %s" % exe)
    sys.exit(1)
win = top_window(pids)
if win is None:
    print("NO_WINDOW %s" % exe)
    sys.exit(1)
hwnd, title, size = win
print("HOST=%s hwnd=%s title=%r size=%s" % (exe, hwnd, title, size))
if user32.IsIconic(hwnd):
    print("WAS_MINIMIZED -> SW_RESTORE")
    user32.ShowWindow(hwnd, 9)
    time.sleep(0.8)
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.6)
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreateTrueCondition())
named = 0
hits = 0
for i in range(arr.Length):
    el = arr.GetElement(i)
    try:
        nm = el.CurrentName or ""
    except Exception:
        continue
    if not nm:
        continue
    named += 1
    low = nm.lower()
    if not any(k in low for k in KEYS):
        continue
    ct = el.GetCurrentPropertyValue(30003)
    pats = []
    for pid_, label, iface in ((10000, "Invoke", IInvoke),
                               (10005, "ExpandCollapse", IExpand)):
        try:
            el.GetCurrentPattern(pid_).QueryInterface(iface)
            pats.append(label)
        except Exception:
            pass
    r = el.GetCurrentPropertyValue(30001)
    rect = tuple(r) if isinstance(r, (tuple, list)) else (
        r.left, r.top, r.right - r.left, r.bottom - r.top)
    print("HIT type=%s name=%r rect=%s patterns=%s" % (ct, nm[:60], rect, pats))
    hits += 1
print("NAMED=%d HITS=%d" % (named, hits))
