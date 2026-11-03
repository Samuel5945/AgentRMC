"""Invoke WorkBuddy's 最小化 button via UIA and report the resulting window
rect (does it become the 314x50 widget?). Read-back only afterwards.
"""
import sys
import time
import pathlib

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
from comtypes import COMMETHOD, GUID, HRESULT, IUnknown


class IInvokeProvider(IUnknown):
    _iid_ = GUID("{00225041-116c-4327-9929-00756f4458c8}")
    _methods_ = [COMMETHOD([], HRESULT, "Invoke")]

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window, user32  # noqa: E402
import ctypes.wintypes as wt
from ctypes import byref

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hwnd, title, size = top_window(pids_for("WorkBuddy.exe"))
print("BEFORE size=%s" % (size,))
root = ui.ElementFromHandle(hwnd)
el = root.FindFirst(4, ui.CreatePropertyCondition(30005, "最小化"))
if el is None:
    print("NO_MINIMIZE_CONTROL")
    sys.exit(1)
try:
    user32.PostMessageW(hwnd, 0x0112, 0xF020, 0)  # WM_SYSCOMMAND / SC_MINIMIZE
    print("MINIMIZE_POSTED")
except Exception as e:
    print("MINIMIZE_ERR=%s" % e)
    sys.exit(1)
time.sleep(1.0)
r = wt.RECT()
user32.GetWindowRect(hwnd, byref(r))
print("AFTER rect=(%d,%d,%d,%d) size=(%d,%d)" % (
    r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top))
hits = []
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
pids = set(pids_for("WorkBuddy.exe"))
def cb(h, l):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, byref(pid))
    if pid.value in pids and user32.IsWindowVisible(h):
        rr = wt.RECT()
        user32.GetWindowRect(h, byref(rr))
        hits.append((h, rr.right - rr.left, rr.bottom - rr.top))
    return True
user32.EnumWindows(CB(cb), 0)
print("VISIBLE_WINDOWS=%s" % hits)
