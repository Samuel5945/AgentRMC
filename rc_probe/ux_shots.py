"""Screenshot both voice UIs (embedded left page + standalone panel) in the
current color scheme, for theme/size regression checks. PMv2 DPI aware.
"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import sys
import time
import pathlib
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
from PIL import ImageGrab

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32  # noqa: E402

TAG = sys.argv[1] if len(sys.argv) > 1 else "shot"
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
pids = set(pids_for("AgentRMCNative.exe"))


def windows():
    out = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            b = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, b, n + 1)
            out.append((h, b.value))
        return True
    user32.EnumWindows(CB(cb), 0)
    return out


def click(el):
    r = el.GetCurrentPropertyValue(30001)
    l, t, w, h = (tuple(r) if isinstance(r, (tuple, list))
                  else (r.left, r.top, r.right - r.left, r.bottom - r.top))
    pt = wt.POINT()
    user32.GetCursorPos(byref(pt))
    old = (pt.x, pt.y)
    user32.SetCursorPos(int(l + w / 2), int(t + h / 2))
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    user32.SetCursorPos(*old)
    time.sleep(0.7)


def click_name(hwnd, name):
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if (el.CurrentName or "") == name and \
                el.GetCurrentPropertyValue(30003) == 50000:
            click(el)
            return True
    return False


def shot(hwnd, tag):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, byref(r))
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                   all_screens=True).save(
        r"D:\githubrepos\Agentrmc\rc_probe\ux_%s_%s.png" % (TAG, tag))


wins = windows()
main = [h for h, t in wins if t == "AgentRMC Native"]
if not main:
    print("NO_MAIN")
    sys.exit(1)
hwnd = main[0]
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
time.sleep(0.5)
if click_name(hwnd, "语音"):
    print("switched to voice page")
shot(hwnd, "embedded")
if click_name(hwnd, "语音主界面"):
    time.sleep(0.8)
    panels = [h for h, t in windows() if t == "AgentRMC 语音"]
    print("standalone=%s" % bool(panels))
    if panels:
        shot(panels[0], "standalone")
print("DONE")
