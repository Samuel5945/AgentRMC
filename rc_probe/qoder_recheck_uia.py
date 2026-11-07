"""Read-only recheck of the Qoder CN desktop UIA tree.

Earlier finding (2026-11-07): the main window exposes only ~3 title-bar
buttons; the renderer never builds an accessibility tree, so targeted
dispatch via UIA (the WorkBuddy-style path) is impossible without changing
user settings. This probe re-checks the CURRENT state after app updates:
walk the main window subtree, count elements per control type, list named
elements. NO clicks, NO keys, NO focus changes, NO settings writes.

Usage: python rc_probe/qoder_recheck_uia.py --exe="Qoder CN.exe" [--depth=6]
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
from collections import Counter
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation, IUIAutomation)

TreeScope_Children = 2
TreeScope_Descendants = 4

CTL_NAMES = {
    50000: "Button", 50001: "Calendar", 50002: "CheckBox", 50003: "ComboBox",
    50004: "Edit", 50005: "Hyperlink", 50006: "Image", 50007: "ListItem",
    50008: "List", 50009: "Menu", 50010: "MenuBar", 50011: "MenuItem",
    50012: "ProgressBar", 50013: "RadioButton", 50014: "ScrollBar",
    50015: "Slider", 50016: "Spinner", 50017: "StatusBar", 50018: "Tab",
    50019: "TabItem", 50020: "Text", 50021: "ToolBar", 50023: "Tree",
    50024: "TreeItem", 50025: "Custom", 50026: "Group", 50027: "Thumb",
    50028: "DataGrid", 50029: "DataItem", 50030: "Document", 50031: "SplitButton",
    50032: "Window", 50033: "Pane", 50034: "ScrollBar", 50037: "ToolTip",
}

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()
user32.IsWindowVisible.restype = wt.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = wt.DWORD
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
    exe = arg("exe", "Qoder CN.exe")
    depth = int(arg("depth", "5"))
    pids = pids_for(exe)
    if not pids:
        print("NOT_RUNNING: %s" % exe)
        return 1
    win = top_window(pids)
    if not win:
        print("NO_WINDOW")
        return 1
    hwnd, title, size = win
    print("TARGET=%s hwnd=%s title=%r size=%s (read-only walk)" %
          (exe, hwnd, title, size))

    ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
    root = ui.ElementFromHandle(hwnd)

    ctl_counter = Counter()
    named = []
    total = 0

    def walk(el, d):
        nonlocal total
        if d > depth or total > 4000:
            return
        try:
            ct = el.CurrentControlType
            name = el.CurrentName or ""
            aid = el.CurrentAutomationId or ""
        except Exception as e:
            named.append(("<unreadable:%s>" % e, d))
            return
        total += 1
        ctl_counter[CTL_NAMES.get(ct, str(ct))] += 1
        if name:
            named.append((name[:70], d))
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
    print("TOTAL_ELEMENTS=%d" % total)
    print("BY_CONTROL_TYPE=%s" % dict(ctl_counter.most_common()))
    print("--- named elements (first 60) ---")
    for name, d in named[:60]:
        print("  " * d + name)
    if len(named) > 60:
        print("... +%d more" % (len(named) - 60))
    return 0


if __name__ == "__main__":
    sys.exit(main())
