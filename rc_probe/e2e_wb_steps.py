"""Step-wise live probe of the WorkBuddy handoff primitives, read-back after
each step. Stages: focus (SetFocus 新建任务 + Enter), paste (Ctrl+V, NO Enter),
enter (one Enter = the product's autoEnter; may send). Run one stage per call.
"""
import sys
import time
import json
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationValuePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import (pids_for, top_window, send_key, hotkey,  # noqa: E402
                                set_clipboard, shot, user32)
from e2e_asr_stub import MARKER  # noqa: E402

STAGE = sys.argv[1] if len(sys.argv) > 1 else "focus"
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for("WorkBuddy.exe"))
print("HWND=%s size=%s" % (hwnd, size))


def focused():
    el = ui.GetFocusedElement()
    if el is None:
        return None
    out = {"type": el.GetCurrentPropertyValue(30003),
           "name": (el.CurrentName or "")[:40]}
    try:
        vp = el.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
        out["value"] = (vp.CurrentValue or "")[:90]
    except Exception:
        out["value"] = "<no value pattern>"
    return out


def composer():
    root = ui.ElementFromHandle(hwnd)
    edits = root.FindAll(4, ui.CreatePropertyCondition(30003, 50004))
    vals = []
    for i in range(edits.Length):
        el = edits.GetElement(i)
        try:
            vp = el.GetCurrentPattern(10002).QueryInterface(
                IUIAutomationValuePattern)
            vals.append((vp.CurrentValue or "")[:90])
        except Exception as e:
            vals.append("err:%s" % e)
    return vals


print("BEFORE focus=%s composers=%s" % (json.dumps(focused(), ensure_ascii=False),
                                        json.dumps(composer(), ensure_ascii=False)))
if STAGE == "focus":
    root = ui.ElementFromHandle(hwnd)
    el = root.FindFirst(4, ui.CreatePropertyCondition(30005, "新建任务"))
    if el is None:
        print("NO_CONTROL")
        sys.exit(1)
    el.SetFocus()
    time.sleep(0.3)
    print("AFTER_SETFOCUS focus=%s" % json.dumps(focused(), ensure_ascii=False))
    send_key(0x0D, False)
    time.sleep(0.02)
    send_key(0x0D, True)
    time.sleep(0.8)
    print("AFTER_ENTER focus=%s composers=%s" % (
        json.dumps(focused(), ensure_ascii=False),
        json.dumps(composer(), ensure_ascii=False)))
elif STAGE == "paste":
    set_clipboard(MARKER)
    time.sleep(0.12)
    hotkey(0x11, 0x56)
    time.sleep(0.7)
    print("AFTER_PASTE focus=%s composers=%s" % (
        json.dumps(focused(), ensure_ascii=False),
        json.dumps(composer(), ensure_ascii=False)))
elif STAGE == "enter":
    send_key(0x0D, False)
    time.sleep(0.02)
    send_key(0x0D, True)
    time.sleep(1.2)
    print("AFTER_ENTER focus=%s composers=%s" % (
        json.dumps(focused(), ensure_ascii=False),
        json.dumps(composer(), ensure_ascii=False)))
    root = ui.ElementFromHandle(hwnd)
    hit = root.FindFirst(4, ui.CreatePropertyCondition(30005, MARKER))
    print("MARKER_IN_TREE=%s" % (hit is not None))
elif STAGE == "race":
    # replicate product timings exactly: Enter#1 -> 350ms -> Ctrl+V -> 80ms -> Enter#2
    root = ui.ElementFromHandle(hwnd)
    el = root.FindFirst(4, ui.CreatePropertyCondition(30005, "新建任务"))
    if el is None:
        print("NO_CONTROL")
        sys.exit(1)
    el.SetFocus()
    time.sleep(0.3)
    send_key(0x0D, False)
    time.sleep(0.02)
    send_key(0x0D, True)
    time.sleep(0.35)
    set_clipboard(MARKER)
    time.sleep(0.05)
    hotkey(0x11, 0x56)
    time.sleep(0.08)
    send_key(0x0D, False)
    time.sleep(0.02)
    send_key(0x0D, True)
    time.sleep(1.2)
    print("AFTER_RACE focus=%s composers=%s" % (
        json.dumps(focused(), ensure_ascii=False),
        json.dumps(composer(), ensure_ascii=False)))
    root = ui.ElementFromHandle(hwnd)
    hit = root.FindFirst(4, ui.CreatePropertyCondition(30005, MARKER))
    print("MARKER_IN_TREE=%s" % (hit is not None))
elif STAGE == "wakerace":
    # fire the same wake URI HostNewTask uses, then product timings
    import ctypes
    ctypes.windll.shell32.ShellExecuteW(None, "open", "workbuddy://new-task",
                                        None, None, 1)
    time.sleep(1.2)
    root = ui.ElementFromHandle(hwnd)
    el = root.FindFirst(4, ui.CreatePropertyCondition(30005, "新建任务"))
    if el is None:
        print("NO_CONTROL")
        sys.exit(1)
    el.SetFocus()
    time.sleep(0.3)
    send_key(0x0D, False)
    time.sleep(0.02)
    send_key(0x0D, True)
    time.sleep(0.35)
    set_clipboard(MARKER)
    time.sleep(0.05)
    hotkey(0x11, 0x56)
    time.sleep(0.08)
    send_key(0x0D, False)
    time.sleep(0.02)
    send_key(0x0D, True)
    time.sleep(1.5)
    print("AFTER_WAKE_RACE focus=%s composers=%s" % (
        json.dumps(focused(), ensure_ascii=False),
        json.dumps(composer(), ensure_ascii=False)))
    root = ui.ElementFromHandle(hwnd)
    hit = root.FindFirst(4, ui.CreatePropertyCondition(30005, MARKER))
    print("MARKER_IN_TREE=%s" % (hit is not None))
shot(hwnd, r"D:\githubrepos\Agentrmc\rc_probe\e2e_wb_%s.png" % STAGE)
print("SHOT_SAVED")
