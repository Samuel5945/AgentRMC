"""Collect product-chain evidence after the stub transcription:
work-items queue, Qoder composer value (should be empty = sent), whether the
marker text exists as a conversation message, and a screenshot.
"""
import sys
import json
import time
import pathlib
import urllib.request

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import (pids_for, top_window, focus_snapshot,  # noqa: E402
                                shot, user32)
from e2e_asr_stub import MARKER  # noqa: E402

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with OPENER.open("http://127.0.0.1:45915/work-items", timeout=3) as r:
    items = json.loads(r.read().decode("utf-8"))
print("WORK_ITEMS=" + json.dumps(items, ensure_ascii=False)[:1200])

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
EXE = sys.argv[1] if len(sys.argv) > 1 else "Qoder.exe"
TAG = EXE.lower().replace(" ", "_").replace(".exe", "")
hwnd, title, size = top_window(pids_for(EXE))
print("HOST=%s HWND=%s title=%r" % (EXE, hwnd, title))
snap = focus_snapshot(ui, hwnd)
print("FOCUS=" + json.dumps(snap, ensure_ascii=False)[:600])
root = ui.ElementFromHandle(hwnd)
cond = ui.CreatePropertyCondition(30005, MARKER)
el = root.FindFirst(4, cond)
print("MARKER_IN_TREE=%s" % (el is not None))
if el is not None:
    print("MARKER_CTL=%s name=%r" % (el.CurrentControlType, el.CurrentName))
# composer value via focused element if it is the editor
f = ui.GetFocusedElement()
try:
    vp = f.GetCurrentPattern(10002).QueryInterface(
        comtypes.gen.UIAutomationClient.IUIAutomationValuePattern)
    print("COMPOSER_VALUE=%r" % vp.CurrentValue)
except Exception as e:
    print("COMPOSER_VALUE_ERR=%s" % e)
shot(hwnd, r"D:\githubrepos\Agentrmc\rc_probe\e2e_handoff_sent_%s.png" % TAG)
print("SHOT_SAVED")
