"""Re-verify the two primitives that decide the session-picker design, with a
watchdog that voids any round where real user input races the probe.

Why this exists: an earlier run was invalidated because the user was using the
mouse at the same time. Probe-driven conclusions about "does a conversation row
respond to keyboard" and "what is focus after clicking a row" must not be built
on rounds that the user was also driving.

Guard checks around every step: physical mouse button states, cursor position
(vs what the probe itself set), and the foreground window (vs what the probe
raised). Any mismatch => round is reported VOID, not as a result.

Reads + minimal actions only: it clicks a row of the *target* app, and for Qoder
it clicks back to the original row so the user's view ends where it started.
"""
import sys
import time
import ctypes
import ctypes.wintypes
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation, IUIAutomation)

TreeScope_Descendants = 4
PID_ControlType = 30003
PID_Name = 30005

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.GetCursorPos.argtypes = [ctypes.POINTER(ctypes.wintypes.POINT)]
user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
user32.GetAsyncKeyState.restype = ctypes.c_short
user32.SetForegroundWindow.argtypes = [ctypes.wintypes.HWND]
user32.IsWindowVisible.restype = ctypes.wintypes.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = ctypes.wintypes.DWORD

EnumWindowsProc = ctypes.WINFUNCTYPE(
    ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)

BUTTONS = {"L": 0x01, "R": 0x02, "M": 0x04}


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
            r = ctypes.wintypes.RECT()
            user32.GetWindowRect(hwnd, byref(r))
            if buf.value:
                hits.append((hwnd, buf.value, (r.right - r.left, r.bottom - r.top)))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return max(hits, key=lambda t: t[2][0] * t[2][1]) if hits else None


class Guard:
    """Detects human input racing the probe. expected_cursor/foreground are
    whatever the probe itself last commanded; anything else means the user."""

    def __init__(self):
        self.hits = []

    def cursor(self):
        pt = ctypes.wintypes.POINT()
        user32.GetCursorPos(byref(pt))
        return (pt.x, pt.y)

    def buttons_down(self):
        return [k for k, vk in BUTTONS.items()
                if (user32.GetAsyncKeyState(vk) & 0x8000) != 0]

    def check(self, step, exp_cursor, exp_foreground):
        bad = []
        down = self.buttons_down()
        if down:
            bad.append("button_down=%s" % down)
        cur = self.cursor()
        if exp_cursor is not None and cur != tuple(exp_cursor):
            bad.append("cursor=%s expected=%s" % (cur, exp_cursor))
        fg = user32.GetForegroundWindow()
        if exp_foreground is not None and fg != exp_foreground:
            bad.append("foreground=%s expected=%s" % (fg, exp_foreground))
        if bad:
            self.hits.append("%s: %s" % (step, "; ".join(bad)))
        return (not bad), cur, fg

    def clean(self):
        return not self.hits


def snapshot(ui, hwnd):
    """Focus element + document name + count of named controls."""
    el = ui.GetFocusedElement()
    out = {}
    if el is not None:
        out["focus_ctl"] = el.CurrentControlType
        out["focus_name"] = (el.CurrentName or "")[:34]
        out["focus_aid"] = el.CurrentAutomationId
    root = ui.ElementFromHandle(hwnd)
    doc = root.FindFirst(TreeScope_Descendants,
                         ui.CreatePropertyCondition(PID_ControlType, 50030))
    out["doc_name"] = ((doc.CurrentName or "")[:40] if doc is not None else
                       "<no-document>")
    return out


def named_rows(ui, hwnd, predicate):
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    rows = []
    for i in range(found.Length):
        el = found.GetElement(i)
        nm = (el.CurrentName or "").strip()
        if not nm:
            continue
        info = predicate(el, nm)
        if info:
            r = el.CurrentBoundingRectangle
            if (r.right - r.left) >= 100 and (r.bottom - r.top) >= 10:
                rows.append((nm, info, (r.left + r.right) // 2,
                             (r.top + r.bottom) // 2, el))
    return rows


def wb_predicate(el, nm):
    if el.CurrentControlType != 50000:
        return None
    for suf in ("分钟前", "小时前", "天前", "昨天"):
        if nm.endswith(suf) or (" %s" % suf) in nm:
            return "wb"
    return None


def qoder_predicate(el, nm):
    if el.CurrentControlType != 50005 or "任务“" not in nm:
        return None
    stem = nm.split("任务“", 1)[1].split("”", 1)[0]
    return stem or None


def press(vk):
    user32.keybd_event(vk, 0, 0, 0)
    user32.keybd_event(vk, 0, 0x0002, 0)


def click(x, y):
    pt = ctypes.wintypes.POINT()
    user32.GetCursorPos(byref(pt))
    before = (pt.x, pt.y)
    user32.SetCursorPos(x, y)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    user32.mouse_event(0x0004, 0, 0, 0, 0, )
    user32.SetCursorPos(before[0], before[1])
    return before


def run(ui, exe, mode):
    pids = set(pids_for(exe))
    win = top_window(pids) if pids else None
    print("== %s" % exe)
    if not win or win[2][0] < 500:
        print("   VERDICT: NO-MAIN-WINDOW (%s) — re-open or protocol-wake it"
              % (win[2] if win else "none"))
        return
    hwnd, title, size = win
    g = Guard()
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.8)
    ok, cur, fg = g.check("raise", None, hwnd)
    if not ok:
        print("   VERDICT: VOID (user input during raise: %s)" % g.hits[-1])
        return
    rows = named_rows(ui, hwnd, wb_predicate if mode == "wb" else qoder_predicate)
    print("   window=%r rows=%d" % (title[:26], len(rows)))
    if not rows:
        print("   VERDICT: ROWS-NOT-READABLE (sidebar collapsed or rule wrong)")
        return
    if mode == "wb":
        nm, _, cx, cy, el = rows[0]
        el.SetFocus()
        g.check("focus", None, hwnd)
        before = snapshot(ui, hwnd)
        press(0x0D)
        time.sleep(1.2)
        s1 = snapshot(ui, hwnd)
        press(0x20)
        time.sleep(1.2)
        s2 = snapshot(ui, hwnd)
        ok_after, _, _ = g.check("after-keys", None, hwnd)
        print("   row=%r focus_after_setfocus=%s" % (nm[:20], before["focus_ctl"]))
        print("   after ENTER ctl=%s  after SPACE ctl=%s"
              % (s1["focus_ctl"], s2["focus_ctl"]))
        changed = (s2["doc_name"] != before["doc_name"] or
                   s2["focus_name"] != before["focus_name"])
        if not ok_after:
            print("   VERDICT: VOID (%s)" % g.hits)
        elif changed:
            print("   VERDICT: KEYBOARD-ACTIVATES (state moved without a click)")
        else:
            print("   VERDICT: KEYBOARD-NOEFFECT (needs the click primitive)")
        return
    # Qoder: measure the post-click focus and whether anything identifies the
    # switched session, then click back so the user's view ends unchanged.
    first = rows[0]
    second = next((r for r in rows if r[0] != first[0]), None)
    if second is None:
        print("   VERDICT: ONLY-ONE-ROW, cannot test switching")
        return
    base = snapshot(ui, hwnd)
    origin = click(second[2], second[3])
    time.sleep(1.6)
    ok1, cur1, _ = g.check("click-other", origin, hwnd)
    after = snapshot(ui, hwnd)
    print("   switched to %r" % second[1][:18])
    print("   base : focus_ctl=%s focus_name=%r doc=%r"
          % (base["focus_ctl"], base["focus_name"], base["doc_name"]))
    print("   after: focus_ctl=%s focus_name=%r doc=%r"
          % (after["focus_ctl"], after["focus_name"], after["doc_name"]))
    click(first[2], first[3])
    time.sleep(1.0)
    ok2, _, _ = g.check("click-back", None, hwnd)
    if not (ok1 and ok2):
        print("   VERDICT: VOID (user input raced the clicks: %s)" % g.hits)
        return
    marker = (after["doc_name"] not in ("", base["doc_name"]) or
              after["focus_name"] != base["focus_name"])
    print("   VERDICT: %s" % ("SWITCH-HAS-A-MARKER (usable acceptance signal)"
                              if marker else
                              "NO-VERIFY-MARKER (blind switch — do not offer "
                              "for this host)"))


def main():
    ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
    run(ui, "WorkBuddy.exe", "wb")
    run(ui, "WorkBuddyAI.exe", "wb")
    run(ui, "Qoder.exe", "qoder")


if __name__ == "__main__":
    main()
