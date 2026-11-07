"""ZCode desktop UIA probe: does a foreground query build the Chromium tree?

WorkBuddy lesson: the full a11y tree is built only AFTER an assistive client
queries it in the foreground. This probe: raise -> wait -> walk (with retry),
NO clicks, NO keys, NO settings writes. Read-only dispatch feasibility check.

Usage: python rc_probe/zcode_uia_probe.py [--depth=8] [--tries=4]
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time
from collections import Counter
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation, IUIAutomation)

CTL_NAMES = {
    50000: "Button", 50002: "CheckBox", 50003: "ComboBox", 50004: "Edit",
    50005: "Hyperlink", 50006: "Image", 50007: "ListItem", 50008: "List",
    50011: "MenuItem", 50020: "Text", 50023: "Tree", 50024: "TreeItem",
    50025: "Custom", 50026: "Group", 50029: "DataItem", 50030: "Document",
    50033: "Pane", 50032: "Window", 50039: "TabItem", 50038: "Tab",
}

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()
user32.IsWindowVisible.restype = wt.BOOL
user32.IsIconic.restype = wt.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.SetForegroundWindow.argtypes = [wt.HWND]
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def arg(name, default=None):
    for a in sys.argv[1:]:
        if a.startswith("--%s=" % name):
            return a.split("=", 1)[1]
    return default


def pids_for(exe):
    res = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq %s" % exe, "/FO", "CSV", "/NH"],
        capture_output=True, text=True, encoding="mbcs", errors="ignore")
    out = set()
    for line in (res.stdout or "").splitlines():
        parts = line.strip().split('","')
        if len(parts) >= 2 and parts[0].strip('"').lower() == exe.lower():
            try:
                out.add(int(parts[1].strip('"')))
            except ValueError:
                pass
    return out


def top_window(pids):
    hits = []

    def cb(hwnd, _l):
        got = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, byref(got))
        if got.value in pids and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            r = wt.RECT()
            user32.GetWindowRect(hwnd, byref(r))
            if buf.value:
                hits.append((hwnd, buf.value,
                             (r.right - r.left, r.bottom - r.top)))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return max(hits, key=lambda t: t[2][0] * t[2][1]) if hits else None


def main():
    depth = int(arg("depth", "8"))
    tries = int(arg("tries", "4"))
    pids = pids_for("ZCode.exe")
    if not pids:
        print("NOT_RUNNING")
        return 1
    win = top_window(pids)
    if not win:
        print("NO_WINDOW")
        return 1
    hwnd, title, size = win
    print("TARGET hwnd=%s title=%r size=%s" % (hwnd, title, size))

    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)
        time.sleep(0.6)
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.8)
    print("FOREGROUND=%s" % (user32.GetForegroundWindow() == hwnd))

    ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
    root = ui.ElementFromHandle(hwnd)

    for attempt in range(1, tries + 1):
        ctl_counter = Counter()
        named = []
        total = [0]

        def walk(el, d):
            if d > depth or total[0] > 6000:
                return
            try:
                ct = el.CurrentControlType
                name = el.CurrentName or ""
                aid = el.CurrentAutomationId or ""
            except Exception:
                return
            total[0] += 1
            ctl_counter[CTL_NAMES.get(ct, str(ct))] += 1
            if name:
                named.append((name[:70], d, ct))
            try:
                walker = ui.RawViewWalker
                child = walker.GetFirstChildElement(el)
                while child is not None:
                    walk(child, d + 1)
                    try:
                        child = walker.GetNextSiblingElement(child)
                    except Exception:
                        break
            except Exception:
                pass

        walk(root, 0)
        print("TRY %d: TOTAL=%d TYPES=%s" % (attempt, total[0],
                                             dict(ctl_counter.most_common(12))))
        if total[0] >= 15:
            print("--- named sample (first 50) ---")
            for name, d, ct in named[:50]:
                print("  " * min(d, 8) + "[%s] %s" % (CTL_NAMES.get(ct, ct), name))
            if len(named) > 50:
                print("... +%d more" % (len(named) - 50))
            break
        time.sleep(1.2)
    else:
        print("TREE_STAYED_BARREN after %d tries" % tries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
