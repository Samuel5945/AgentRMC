"""Re-run the current-session handoff while polling WorkBuddy's composer value
every 200ms, to see whether the paste lands, gets wiped, or Enter is swallowed.
Also polls AgentRMC windows' UIA text for the pet bubble verdict.
"""
import sys
import time
import json
import pathlib
import subprocess

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationValuePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window, send_key  # noqa: E402
from e2e_asr_stub import MARKER  # noqa: E402

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, _, _ = top_window(pids_for("WorkBuddy.exe"))


def composer():
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreatePropertyCondition(30003, 50004))
    out = []
    for i in range(arr.Length):
        el = arr.GetElement(i)
        try:
            vp = el.GetCurrentPattern(10002).QueryInterface(
                IUIAutomationValuePattern)
            out.append(vp.CurrentValue or "")
        except Exception:
            out.append("<err>")
    return out


def bubbles():
    texts = []
    for h, t in []:
        pass
    import ctypes
    import ctypes.wintypes as wt
    from ctypes import byref, create_unicode_buffer
    from qoder_ctrl_n_focus import user32
    hits = []
    pids = set(pids_for("AgentRMCNative.exe"))
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            hits.append(h)
        return True
    user32.EnumWindows(CB(cb), 0)
    for h in hits:
        try:
            root = ui.ElementFromHandle(h)
            arr = root.FindAll(4, ui.CreatePropertyCondition(30003, 50020))
            for i in range(arr.Length):
                nm = arr.GetElement(i).CurrentName or ""
                if any(k in nm for k in ("已交给", "已复制", "没找到", "没证明",
                                         "手动")):
                    texts.append(nm)
        except Exception:
            pass
    return texts


# fire the global voice hotkey once (silence auto-stop -> stub -> handoff)
for vk in (0x11, 0x12, 0x56):
    send_key(vk, False)
    time.sleep(0.02)
for vk in (0x56, 0x12, 0x11):
    send_key(vk, True)
    time.sleep(0.02)
print("TAP_SENT")
t0 = time.time()
seen = set()
while time.time() - t0 < 20:
    vals = composer()
    tag = json.dumps(["MARKER" if MARKER in v else
                      ("placeholder" if "今天帮你" in v else repr(v[:30]))
                      for v in vals], ensure_ascii=False)
    if tag not in seen:
        seen.add(tag)
        print("t=%4.1f composer=%s" % (time.time() - t0, tag))
    for b in bubbles():
        if b not in seen:
            seen.add(b)
            print("t=%4.1f BUBBLE=%s" % (time.time() - t0, b))
    time.sleep(0.2)
print("FINAL composer=%s" % json.dumps(composer(), ensure_ascii=False)[:200])
