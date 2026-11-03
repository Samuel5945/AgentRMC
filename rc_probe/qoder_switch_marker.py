"""Hunt for a *content-level* marker that proves Qoder actually switched to the
conversation whose row was clicked.

Earlier reading only proved "the clicked row got focus" — that is self-referential
and can never fail, so it is not an acceptance signal. This probe asks a
discriminating question instead: when session B is open, does B's title show up
in the message area (right of the sidebar) and A's stop showing up?

A usable marker must move in BOTH directions (B appears after switching, and
either A disappears or B was absent before). Otherwise we report NO-MARKER and
session-select stays off for this host.
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

user32 = ctypes.WinDLL("user32", use_last_error=True)
# Without this, UIA rects come back DPI-virtualised on a 200% display while
# SetCursorPos wants physical pixels: every click lands at half the intended
# offset and a "nothing happened" reading is really a mis-aim.
user32.SetProcessDPIAware()
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
                hits.append((hwnd, buf.value, (r.right - r.left,
                                               r.bottom - r.top)))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return max(hits, key=lambda t: t[2][0] * t[2][1]) if hits else None


class Guard:
    def __init__(self):
        self.hits = []

    def cursor(self):
        pt = ctypes.wintypes.POINT()
        user32.GetCursorPos(byref(pt))
        return (pt.x, pt.y)

    def buttons(self):
        return [k for k, vk in BUTTONS.items()
                if (user32.GetAsyncKeyState(vk) & 0x8000) != 0]

    def check(self, step, exp_cur, exp_fg):
        bad = []
        if self.buttons():
            bad.append("button_down=%s" % self.buttons())
        cur = self.cursor()
        if exp_cur is not None and cur != tuple(exp_cur):
            bad.append("cursor=%s expected=%s" % (cur, exp_cur))
        fg = user32.GetForegroundWindow()
        if exp_fg is not None and fg != exp_fg:
            # WorkerW / empty-title desktop windows (the wallpaper engine on this
            # machine) transiently own the foreground, and GetForegroundWindow
            # briefly returns NULL while a window restores. Neither is the user;
            # the hard signals stay: physical buttons down, or the cursor not
            # where the probe put it.
            cls = create_unicode_buffer(256)
            if fg:
                user32.GetClassNameW(fg, cls, 256)
            noise = (not fg) or (cls.value == "WorkerW" and
                                 user32.GetWindowTextLengthW(fg) == 0)
            if noise:
                user32.SetForegroundWindow(exp_fg)
                time.sleep(0.4)
                if user32.GetForegroundWindow() == exp_fg:
                    return True
            bad.append("foreground=%s(%s) expected=%s"
                       % (fg, cls.value, exp_fg))
        if bad:
            self.hits.append("%s: %s" % (step, "; ".join(bad)))
            return False
        return True


def click(x, y):
    pt = ctypes.wintypes.POINT()
    user32.GetCursorPos(byref(pt))
    origin = (pt.x, pt.y)
    user32.SetCursorPos(x, y)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    user32.SetCursorPos(origin[0], origin[1])
    return origin


def elements(ui, hwnd):
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    out = []
    for i in range(found.Length):
        el = found.GetElement(i)
        try:
            nm = (el.CurrentName or "").strip()
            ct = el.CurrentControlType
            r = el.CurrentBoundingRectangle
        except Exception:
            continue
        out.append((nm, ct, r.left, r.top, r.right, r.bottom))
    return out


def rows_of(els):
    """Qoder session rows: Hyperlink whose name is 任务“X”，Agent：…"""
    out = []
    for nm, ct, l, t, r, b in els:
        if ct != 50005 or '任务“' not in nm:
            continue
        stem = nm.split('任务“', 1)[1].split('”', 1)[0]
        if stem:
            out.append({"stem": stem, "left": l, "right": r, "top": t,
                        "bottom": b})
    return out


def content_hits(els, stem, sidebar_right):
    """Nodes in the message area (right of the sidebar) whose name contains the
    title's head. The row itself is excluded by geometry."""
    key = stem[:8]
    hits = []
    for nm, ct, l, t, r, b in els:
        if not nm or key not in nm:
            continue
        if l < sidebar_right + 10:
            continue
        hits.append((ct, nm[:44], l, t))
    return hits


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "Qoder.exe"
    ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
    pids = set(pids_for(exe))
    win = top_window(pids) if pids else None
    if not win or win[2][0] < 500:
        print("VERDICT: NO-MAIN-WINDOW (%s)" % str(win[2] if win else "none"))
        return
    hwnd, title, size = win
    print("== %s window=%r %dx%d" % (exe, title[:30], size[0], size[1]))
    g = Guard()
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.9)
    if not g.check("raise", None, hwnd):
        print("VERDICT: VOID (%s)" % g.hits)
        return

    els = elements(ui, hwnd)
    rows = rows_of(els)
    print("   rows=%d" % len(rows))
    if len(rows) < 2:
        print("VERDICT: NEEDS-2-ROWS (rows=%d, sidebar virtualized?)" % len(rows))
        return
    for r in sorted(rows, key=lambda x: x["top"]):
        print("     stem=%r row_left=%d" % (r["stem"][:30], r["left"]))
    sidebar_right = max(r["right"] for r in rows)

    # pick the row that is NOT the one whose title text is already in the
    # content area, so "before" and "after" are genuinely different states
    scored = []
    for r in rows:
        scored.append((len(content_hits(els, r["stem"], sidebar_right)), r))
    open_now = max(scored, key=lambda s: s[0])
    target = min(scored, key=lambda s: s[0])
    if open_now[0] == 0:
        print("   NOTE: no row has content hits in the baseline state "
              "(open conversation shows no title text?) — marker channel may "
              "be dead from the start")
    print("   baseline: open-candidate=%r hits=%d | switch-target=%r hits=%d"
          % (open_now[1]["stem"][:20], open_now[0], target[1]["stem"][:20],
             target[0]))

    origin = click((target[1]["left"] + target[1]["right"]) // 2,
                   (target[1]["top"] + target[1]["bottom"]) // 2)
    time.sleep(2.0)
    if not g.check("click-target", origin, hwnd):
        print("VERDICT: VOID (%s)" % g.hits)
        return
    after = elements(ui, hwnd)
    a_open = len(content_hits(after, open_now[1]["stem"], sidebar_right))
    a_target = len(content_hits(after, target[1]["stem"], sidebar_right))
    print("   after click: open-candidate hits=%d | target hits=%d"
          % (a_open, a_target))
    focus = ui.GetFocusedElement()
    print("   focus ctl=%s name=%r" % (focus.CurrentControlType if focus
                                       else None,
                                       (focus.CurrentName or "")[:36] if focus
                                       else ""))

    # restore the user's view
    click((open_now[1]["left"] + open_now[1]["right"]) // 2,
          (open_now[1]["top"] + open_now[1]["bottom"]) // 2)
    time.sleep(1.4)
    back = elements(ui, hwnd)
    b_open = len(content_hits(back, open_now[1]["stem"], sidebar_right))
    b_target = len(content_hits(back, target[1]["stem"], sidebar_right))
    print("   after click-back: open-candidate hits=%d | target hits=%d"
          % (b_open, b_target))
    if not g.check("click-back", None, hwnd):
        print("VERDICT: VOID (%s)" % g.hits)
        return

    gained = a_target > target[0] and b_target < a_target
    lost = a_open < open_now[0] and b_open > a_open
    if gained and lost:
        print("VERDICT: HARD-MARKER — content hits track the opened "
              "conversation both ways; acceptance signal is usable")
    elif gained:
        print("VERDICT: WEAK-MARKER — target's title appears when opened, but "
              "the previous one never disappears (can't prove the switch "
              "exclusively)")
    else:
        print("VERDICT: NO-MARKER — title text does not track the open "
              "conversation; session-select must stay off for this host")


if __name__ == "__main__":
    main()
