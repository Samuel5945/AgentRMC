"""只读：WorkBuddy 系会话定向派发的前置证据。

打印：composer Edit（rect/value/可聚焦）、发送按钮、会话行 Button 的
SelectionItem/Invoke pattern 与选中态、聊天区标题 Text（= 当前会话标题）。
用法: python rc_probe/wb_session_controls.py [WorkBuddy.exe|WorkBuddyAI.exe]
"""
import pathlib
import sys

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,  # noqa: E402
                                             IUIAutomationValuePattern,
                                             IUIAutomationSelectionItemPattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window  # noqa: E402

exe = sys.argv[1] if len(sys.argv) > 1 else "WorkBuddy.exe"
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for(exe))
root = ui.ElementFromHandle(hwnd)
print(f"window {title!r} {size}")


def rect_of(el):
    r = el.GetCurrentPropertyValue(30001)
    return tuple(int(v) for v in (tuple(r) if isinstance(r, (tuple, list))
                                  else (0, 0, 0, 0)))


def name_of(el):
    return el.GetCurrentPropertyValue(30005) or ""


print("--- EDITS ---")
arr = root.FindAll(4, ui.CreatePropertyCondition(30003, 50004))
for i in range(arr.Length):
    el = arr.GetElement(i)
    try:
        vp = el.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
        val = (vp.CurrentValue or "")[:60]
        ro = vp.CurrentIsReadOnly
    except Exception:
        val, ro = "<no value pattern>", None
    print(f"Edit rect={rect_of(el)} off={el.GetCurrentPropertyValue(30022)} "
          f"readonly={ro} value={val!r} aid={el.GetCurrentPropertyValue(30011)!r}")

print("--- SESSION ROWS (Button in sidebar x<2500, y>2600) ---")
arr = root.FindAll(4, ui.CreatePropertyCondition(30003, 50000))
for i in range(arr.Length):
    el = arr.GetElement(i)
    r = rect_of(el)
    if not (r[0] < 2500 and r[1] > 2600 and r[2] > 300):
        continue
    sel = None
    try:
        sp = el.GetCurrentPattern(10010).QueryInterface(
            IUIAutomationSelectionItemPattern)
        sel = bool(sp.CurrentIsSelected)
    except Exception:
        sel = "no SelectionItem"
    inv = True
    try:
        el.GetCurrentPattern(10000)
    except Exception:
        inv = False
    print(f"Button name={name_of(el)[:40]!r} rect={r} selected={sel} invoke={inv}")

print("--- CHAT HEADER / SEND (x>2500, y<2800 or name contains 发送) ---")
walker = ui.ControlViewWalker
seen = 0


def walk(el, d):
    global seen
    if d > 9 or seen > 4000:
        return
    seen += 1
    ct = el.GetCurrentPropertyValue(30003)
    n = name_of(el)
    r = rect_of(el)
    if ct == 50020 and r[0] > 2500 and 2500 < r[1] < 2700 and n:
        print(f"HeaderText name={n[:50]!r} rect={r}")
    if ct == 50000 and ("发送" in n or "send" in n.lower()):
        print(f"SendButton name={n[:30]!r} rect={r} "
              f"off={el.GetCurrentPropertyValue(30022)}")
    child = walker.GetFirstChildElement(el)
    while child and seen <= 4000:
        walk(child, d + 1)
        child = walker.GetNextSiblingElement(child)


walk(root, 0)
print("walked", seen)
