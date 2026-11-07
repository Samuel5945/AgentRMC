"""在 WorkBuddy 桌面自建一条牺牲会话（新建任务+粘贴+Enter），等 ai-title 落盘后
打印 session id 与标题，供路由活体 harness 定向派发验收。只碰新建的这条会话。
用法: python rc_probe/wb_make_spare.py [WorkBuddy.exe|WorkBuddyAI.exe]
"""
import ctypes
import ctypes.wintypes as wt
import glob
import json
import os
import pathlib
import sys
import time

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,  # noqa: E402
                                             IUIAutomationInvokePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32  # noqa: E402

exe = sys.argv[1] if len(sys.argv) > 1 else "WorkBuddy.exe"
home = ".workbuddy-ai" if "AI" in exe else ".workbuddy"
base = os.path.join("C:/Users/PC", home, "projects")
marker = f"AGENTRMC-WBSPARE-{int(time.time())}"
text = f"链路探针 {marker}：只回复「收到」，不要调用任何工具。"

k32 = ctypes.windll.kernel32
k32.GlobalAlloc.restype = wt.HGLOBAL
k32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
k32.GlobalLock.restype = ctypes.c_void_p
k32.GlobalLock.argtypes = [wt.HGLOBAL]
k32.GlobalUnlock.argtypes = [wt.HGLOBAL]
user32.GetClipboardData.restype = wt.HANDLE
user32.GetClipboardData.argtypes = [wt.UINT]
user32.SetClipboardData.restype = wt.HANDLE
user32.SetClipboardData.argtypes = [wt.UINT, wt.HANDLE]

uri = "workbuddy-ai://new-task" if "AI" in exe else "workbuddy://new-task"
ctypes.windll.shell32.ShellExecuteW(None, "open", uri, None, None, 1)
time.sleep(1.5)

_pids = set(pids_for(exe))
_cands = []
_CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def _enum(h, _):
    g = wt.DWORD()
    user32.GetWindowThreadProcessId(h, ctypes.byref(g))
    if g.value in _pids:
        n = user32.GetWindowTextLengthW(h)
        b = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, b, n + 1)
        c = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, c, 64)
        r = wt.RECT()
        user32.GetWindowRect(h, ctypes.byref(r))
        if c.value == "Chrome_WidgetWin_1" and b.value in ("WorkBuddy", "WorkBuddy AI") \
                and (r.right - r.left) > 500:
            _cands.append((h, r.left, r.top))
    return True


user32.EnumWindows(_CB(_enum), 0)
if not _cands:
    print("no main window")
    sys.exit(1)
hwnd, ox, oy = _cands[0]
user32.SetForegroundWindow(hwnd)
time.sleep(0.5)
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
root = ui.ElementFromHandle(hwnd)


def collect():
    arr = root.FindAll(4, ui.CreateTrueCondition())
    tables, edits = [], []
    for i in range(arr.Length):
        el = arr.GetElement(i)
        ct = el.GetCurrentPropertyValue(30003)
        nm = el.GetCurrentPropertyValue(30005) or ""
        r = el.GetCurrentPropertyValue(30001)
        t = tuple(int(v) for v in (tuple(r) if isinstance(r, (tuple, list))
                                  else (0, 0, 0, 0)))
        if ct == 50019 and nm == "新建任务":
            tables.append(el)
        if ct == 50004 and t[2] > 200 and t[3] > 40:
            edits.append(el)
    return tables, edits


tables, edits = collect()
if not tables:
    print("no 新建任务 control")
    sys.exit(1)
try:
    tables[0].GetCurrentPattern(10000).QueryInterface(
        IUIAutomationInvokePattern).Invoke()
except Exception:
    tables[0].SetFocus()
    user32.keybd_event(0x0D, 0, 0, 0)
    user32.keybd_event(0x0D, 0, 2, 0)
time.sleep(1.5)
_, edits = collect()
if not edits:
    print("no composer")
    sys.exit(1)
edits[0].SetFocus()
time.sleep(0.3)
user32.OpenClipboard(None)
user32.EmptyClipboard()
data = ctypes.create_unicode_buffer(text)
h = k32.GlobalAlloc(0x0042, ctypes.sizeof(data))
p = k32.GlobalLock(h)
ctypes.memmove(p, data, ctypes.sizeof(data))
k32.GlobalUnlock(h)
user32.SetClipboardData(13, h)
user32.CloseClipboard()
user32.keybd_event(0x11, 0, 0, 0)
user32.keybd_event(0x56, 0, 0, 0)
user32.keybd_event(0x56, 0, 2, 0)
user32.keybd_event(0x11, 0, 2, 0)
time.sleep(0.5)
user32.keybd_event(0x0D, 0, 0, 0)
user32.keybd_event(0x0D, 0, 2, 0)

sid = None
t0 = time.time()
while time.time() - t0 < 60 and not sid:
    for pth in glob.glob(base + "/*/*.jsonl"):
        try:
            if marker in open(pth, encoding="utf-8").read():
                sid = os.path.basename(pth)[:-6]
                break
        except OSError:
            continue
    if not sid:
        time.sleep(1.0)
if not sid:
    print("no receipt for spare creation")
    sys.exit(1)
title = None
t0 = time.time()
while time.time() - t0 < 25 and not title:
    for pth in glob.glob(base + "/*/" + sid + ".jsonl"):
        for line in open(pth, encoding="utf-8"):
            if '"ai-title"' in line:
                d = json.loads(line)
                if d.get("type") == "ai-title":
                    title = d.get("aiTitle")
    if not title:
        time.sleep(1.0)
print("SPARE_SID:", sid)
print("SPARE_TITLE:", title)
