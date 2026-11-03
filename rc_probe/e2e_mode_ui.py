"""Operate the new per-host dispatch-mode combo in the voice panel via UIA+keys
and prove the UI write path: config ui.dispatch_mode_<pin> must become current.
Opens the panel via the main window's 语音主界面 button if needed.
Combos in the panel, top-to-bottom: agentTarget, dispatchMode, backend.
"""
import sys
import json
import time
import pathlib
import ctypes
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationValuePattern)
from comtypes import COMMETHOD, GUID, HRESULT, IUnknown

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, send_key, shot, user32  # noqa: E402


class IInvokeProvider(IUnknown):
    _iid_ = GUID("{00225041-116c-4327-9929-00756f4458c8}")
    _methods_ = [COMMETHOD([], HRESULT, "Invoke")]


def windows():
    hits = []
    pids = set(pids_for("AgentRMCNative.exe"))
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            b = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, b, n + 1)
            if b.value:
                hits.append((h, b.value))
        return True
    user32.EnumWindows(CB(cb), 0)
    return hits


def top_of(el):
    r = el.GetCurrentPropertyValue(30001)
    return r[1] if isinstance(r, (tuple, list)) else r.top


ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
wins = windows()
panel = [h for h, t in wins if "语音" in t]
if not panel:
    main = [h for h, t in wins if t == "AgentRMC Native"]
    if not main:
        print("NO_MAIN_WINDOW")
        sys.exit(1)
    root = ui.ElementFromHandle(main[0])
    btn = root.FindFirst(4, ui.CreatePropertyCondition(30005, "语音主界面"))
    if btn is None:
        print("NO_PANEL_BUTTON")
        sys.exit(1)
    btn.GetCurrentPattern(10000).QueryInterface(IInvokeProvider).Invoke()
    time.sleep(1.0)
    wins = windows()
    panel = [h for h, t in wins if "语音" in t]
if not panel:
    print("PANEL_NOT_OPEN wins=%s" % wins)
    sys.exit(1)
hwnd = panel[0]
print("PANEL hwnd=%s" % hwnd)
root = ui.ElementFromHandle(hwnd)
arr = root.FindAll(4, ui.CreatePropertyCondition(30003, 50003))
combos = []
for i in range(arr.Length):
    el = arr.GetElement(i)
    try:
        vp = el.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
        val = vp.CurrentValue
    except Exception:
        val = "?"
    combos.append((top_of(el), el, val))
combos.sort(key=lambda t: t[0])
print("COMBOS=%s" % [c[2] for c in combos])
if len(combos) < 2:
    print("NOT_ENOUGH_COMBOS")
    sys.exit(1)
mode = combos[1][1]
print("MODE_BEFORE=%r" % combos[1][2])
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.4)
# Qt 的 UIA SetFocus 对 combo 不落键盘焦点（实测落到 ✕）：用 Tab 走真实
# 键盘焦点，UIA 复查落点后再用 Down 改值。全程不动鼠标。
mode_top = top_of(mode)
focused_ok = False
for _ in range(20):
    send_key(0x09, False)
    time.sleep(0.03)
    send_key(0x09, True)
    time.sleep(0.15)
    f = ui.GetFocusedElement()
    try:
        if f.GetCurrentPropertyValue(30003) == 50003 and \
                abs(top_of(f) - mode_top) <= 2:
            focused_ok = True
            break
    except Exception:
        pass
print("TAB_FOCUS_ON_MODE=%s" % focused_ok)
if not focused_ok:
    print("TAB_NAV_FAILED")
    sys.exit(1)
for _ in range(2):
    send_key(0x28, False)
    time.sleep(0.05)
    send_key(0x28, True)
    time.sleep(0.25)
try:
    vp = mode.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
    print("MODE_AFTER=%r" % vp.CurrentValue)
except Exception as e:
    print("MODE_AFTER_ERR=%s" % e)
cfg = json.loads(pathlib.Path(r"D:\apps\AgentRMC\config.json")
                 .read_text(encoding="utf-8"))
print("CONFIG_MODE=%r pin=%r" % (
    cfg["ui"].get("dispatch_mode_" + cfg["ui"].get("agent_target", "")),
    cfg["ui"].get("agent_target")))
shot(hwnd, r"D:\githubrepos\Agentrmc\rc_probe\e2e_mode_ui.png")
print("SHOT_SAVED")
