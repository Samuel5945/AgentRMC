"""Decisive probe: does an MSAA (IAccessible) request make Chromium build its
accessibility tree, and is the host's 'new task' control reachable through it?

Chromium flips into full accessibility mode when something asks for IAccessible
over WM_GETOBJECT (that is what screen readers do). Plain UIA
ElementFromHandle left these windows at 8 native-frame-only descendants, so MSAA
is the test of whether any AT channel exists at all.

Reads only. Prints UIA descendant counts before and after the MSAA walk so
"tree never built" cannot be misread as "button is missing".

usage: python msaa_flip_probe.py [WorkBuddy.exe|WorkBuddyAI.exe|"DeepSeek Harness.exe"]
"""
import sys
import ctypes
import ctypes.wintypes
from ctypes import byref, create_unicode_buffer

import comtypes
import comtypes.client
from comtypes.automation import VARIANT, VT_DISPATCH, VT_I4, VT_UNKNOWN  # noqa: E402

UIA = comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation  # noqa: E402

comtypes.client.GetModule("oleacc.dll")
from comtypes.gen.Accessibility import IAccessible  # noqa: E402

TreeScope_Descendants = 4
OBJID_CLIENT = 0xFFFFFFFC
OBJID_WINDOW = 0x00000000

KEYWORDS = ("新任务", "新建任务", "New task", "New Task", "newChat", "New chat",
            "新建会话", "新会话")

user32 = ctypes.WinDLL("user32", use_last_error=True)
oleacc = ctypes.WinDLL("oleacc", use_last_error=True)

user32.IsWindowVisible.restype = ctypes.wintypes.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = ctypes.wintypes.DWORD

oleacc.AccessibleObjectFromWindow.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_void_p)]
oleacc.AccessibleObjectFromWindow.restype = ctypes.HRESULT
oleacc.AccessibleChildren.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_long,
    ctypes.POINTER(VARIANT), ctypes.POINTER(ctypes.c_long)]
oleacc.AccessibleChildren.restype = ctypes.HRESULT

EnumWindowsProc = ctypes.WINFUNCTYPE(
    ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)


def pids_for(exe):
    import subprocess

    res = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq %s" % exe, "/FO", "CSV", "/NH"],
        capture_output=True, text=True, encoding="mbcs", errors="ignore")
    out = []
    for line in (res.stdout or "").splitlines():
        parts = line.strip().split('","')
        if len(parts) >= 2 and parts[0].strip('"').lower() == exe.lower():
            try:
                out.append(int(parts[1].strip('"')))
            except ValueError:
                pass
    return out


def top_window(pids):
    hits = []

    def cb(hwnd, _l):
        got = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, byref(got))
        if got.value in pids and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if buf.value:
                hits.append((hwnd, buf.value))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return hits[0] if hits else None


def uia_count(ui, hwnd):
    root = ui.ElementFromHandle(hwnd)
    if root is None:
        return -1
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    return found.Length if found else 0


def acc_from_window(hwnd, objid):
    from comtypes import POINTER

    p = POINTER(IAccessible)()
    oleacc.AccessibleObjectFromWindow.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
        POINTER(POINTER(IAccessible))]
    hr = oleacc.AccessibleObjectFromWindow(
        hwnd, objid, byref(IAccessible._iid_), byref(p))
    if hr != 0 or not p:
        return None, hr
    return p, hr


def text_of(acc):
    try:
        return acc.accName[0] or ""
    except Exception:
        return ""


def role_of(acc):
    try:
        return acc.accRole[0]
    except Exception:
        return -1


def children(acc, cap=200):
    try:
        n = acc.accChildCount
    except Exception:
        return [], "accChildCount raised"
    if not n:
        return [], "0 children"
    arr = (VARIANT * min(n, cap))()
    got = ctypes.c_long(0)
    hr = oleacc.AccessibleChildren(
        ctypes.cast(acc, ctypes.c_void_p), 0, min(n, cap), arr, byref(got))
    if hr != 0:
        return [], "AccessibleChildren hr=0x%X" % (hr & 0xFFFFFFFF)
    out = []
    for i in range(got.value):
        v = arr[i]
        if v.vt == VT_DISPATCH:
            try:
                out.append(v.value.queryInterface(IAccessible))
            except Exception as e:
                out.append(("QI-FAIL", str(e)))
        elif v.vt == VT_UNKNOWN:
            try:
                out.append(v.value.queryInterface(IAccessible))
            except Exception:
                out.append(("VT_UNKNOWN", None))
        elif v.vt == VT_I4:
            try:
                nm = acc.accName[v.value]
            except Exception:
                nm = ""
            out.append(("CHILD_ID", v.value, nm or ""))
        else:
            out.append(("VT_%s" % v.vt, None))
    return out, None


def walk(acc, depth, rows, hits, max_depth=8, max_nodes=1500):
    stack = [(acc, depth)]
    seen = 0
    while stack and seen < max_nodes:
        cur, d = stack.pop()
        if isinstance(cur, tuple):
            if cur[0] == "CHILD_ID" and any(k in cur[2] for k in KEYWORDS):
                hits.append(cur[2])
                rows.append((d, "id", cur[2]))
            continue
        seen += 1
        nm = text_of(cur)
        rl = role_of(cur)
        rows.append((d, rl, nm))
        if nm and any(k in nm for k in KEYWORDS):
            hits.append(nm)
            print("      HIT depth=%d role=%s name=%r" % (d, rl, nm))
        if d >= max_depth:
            continue
        kids, err = children(cur)
        if err:
            continue
        for k in kids:
            if isinstance(k, tuple):
                if k[0] == "CHILD_ID":
                    stack.append((k, d + 1))
            else:
                stack.append((k, d + 1))
    return seen


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "WorkBuddy.exe"
    compts_ok = True
    try:
        comtypes.CoInitialize()
    except Exception as e:
        print("CoInitialize failed: %s" % e)
        compts_ok = False
    if not compts_ok:
        return
    pids = pids_for(exe)
    if not pids:
        print("VERDICT: NOT-RUNNING")
        return
    win = top_window(set(pids))
    if not win:
        print("VERDICT: NO-WINDOW")
        return
    hwnd, title = win
    ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
    print("== %s window=%r hwnd=%s" % (exe, title[:44], hwnd))
    print("  UIA descendants BEFORE MSAA = %d" % uia_count(ui, hwnd))

    acc, hr = acc_from_window(hwnd, OBJID_CLIENT)
    print("  AFO(OBJID_CLIENT) hr=0x%X -> %s"
          % (hr & 0xFFFFFFFF, "IAccessible" if acc else "none"))
    if acc is None:
        acc, hr = acc_from_window(hwnd, OBJID_WINDOW)
        print("  AFO(OBJID_WINDOW) hr=0x%X -> %s"
              % (hr & 0xFFFFFFFF, "IAccessible" if acc else "none"))
    if acc is None:
        print("VERDICT: MSAA-UNAVAILABLE -> no AT channel; UIA/MSAA route dead")
        return

    rows, hits = [], []
    visited = walk(acc, 0, rows, hits)
    named = [r for r in rows if r[2]]
    print("  MSAA nodes visited=%d named=%d" % (visited, len(named)))
    print("  sample names: %s" % [n for _, _, n in named[:12]])
    print("  UIA descendants AFTER MSAA = %d" % uia_count(ui, hwnd))
    if hits:
        print("VERDICT: AT-TREE-EXPOSED + keyword nodes=%d -> %s"
              % (len(hits), hits[:6]))
    elif len(named) > 12:
        print("VERDICT: AT-TREE-EXPOSED, no keyword node (%d named) -> "
              "reachable, need the real label" % len(named))
    else:
        print("VERDICT: AT-TREE-NOT-BUILT (%d named of %d visited) -> "
              "no UIA/MSAA route for this host" % (len(named), visited))


if __name__ == "__main__":
    main()
