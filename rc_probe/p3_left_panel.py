"""Verify the left-column stack: remote card <-> embedded voice panel, and the
typed-dialogue input. Guarded clicks only; restores cursor after each click.
"""
import sys
import time
import pathlib
import ctypes
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

# 副屏不同 DPI 时 system-aware 进程的 GetWindowRect 会被虚拟化→截图错位；
# 先 PerMonitorV2 再 import 探针模块（模块里的 SetProcessDPIAware 会静默失败）。
ctypes.windll.shcore.SetProcessDpiAwareness(2)

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationValuePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32, send_key  # noqa: E402

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
pids = set(pids_for("AgentRMCNative.exe"))
wins = []
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def cb(h, l):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, byref(pid))
    if pid.value in pids and user32.IsWindowVisible(h):
        n = user32.GetWindowTextLengthW(h)
        b = create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, b, n + 1)
        wins.append((h, b.value))
    return True


user32.EnumWindows(CB(cb), 0)
main = [h for h, t in wins if t == "AgentRMC Native"]
if not main:
    print("NO_MAIN_WINDOW")
    sys.exit(1)
hwnd = main[0]
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.6)


def click(el):
    r = el.GetCurrentPropertyValue(30001)
    left, top, w, h = (tuple(r) if isinstance(r, (tuple, list))
                       else (r.left, r.top, r.right - r.left, r.bottom - r.top))
    pt = wt.POINT()
    user32.GetCursorPos(byref(pt))
    old = (pt.x, pt.y)
    user32.SetCursorPos(int(left + w / 2), int(top + h / 2))
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.SetCursorPos(*old)
    time.sleep(0.6)


def find_name(name, ct=None):
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if (el.CurrentName or "") == name and (
                ct is None or el.GetCurrentPropertyValue(30003) == ct):
            return el
    return None


def shot(tag):
    from PIL import ImageGrab
    r = wt.RECT()
    user32.GetWindowRect(hwnd, byref(r))
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                   all_screens=True).save(
        r"D:\githubrepos\Agentrmc\rc_probe\p3_%s.png" % tag)


btn = find_name("语音", 50000)
print("TOGGLE_VOICE_FOUND=%s" % (btn is not None))
if btn is not None:
    click(btn)
shot("voice_page")
root = ui.ElementFromHandle(hwnd)
dump = root.FindAll(4, ui.CreateTrueCondition())
for i in range(dump.Length):
    el = dump.GetElement(i)
    ct = el.GetCurrentPropertyValue(30003)
    if ct in (50000, 50004):
        aid = el.GetCurrentPropertyValue(30011) or ""
        if ct == 50004 or "voiceWindow" in aid or (el.CurrentName or "") in (
                "遥控器", "发送", "语音"):
            print("DUMP", ct, repr((el.CurrentName or "")[:30]), repr(aid[:80]))
arr = root.FindAll(4, ui.CreatePropertyCondition(30003, 50004))
print("TYPED_EDITS=%d" % arr.Length)
if arr.Length:
    ed = arr.GetElement(0)
    vp = ed.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
    vp.SetValue("你好")
    click(ed)
    send_key(0x0D, False)
    time.sleep(0.03)
    send_key(0x0D, True)
    time.sleep(1.5)
    tr = None
    tall = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(tall.Length):
        el = tall.GetElement(i)
        if (el.GetCurrentPropertyValue(30011) or "").endswith("voiceTranscript"):
            tr = el
            break
    print("TRANSCRIPT=%r" % (None if tr is None else tr.CurrentName))
shot("typed")
back = find_name("遥控器", 50000)
print("TOGGLE_REMOTE_FOUND=%s" % (back is not None))
if back is not None:
    click(back)
shot("remote_page")
print("DONE")
