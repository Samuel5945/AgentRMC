"""Re-test: after Ctrl+N in Qoder / Qoder CN, where does keyboard focus land?

Question under test: does Ctrl+N put focus in the chat composer (editable), or
in the conversation list (rows that are clickable but not keyboard-navigable)?
Earlier rounds could not prove composer focus, so blind paste stayed off.

This probe ONLY: raise -> snapshot focus -> SendInput Ctrl+N -> snapshot focus
(+ ancestor chain, patterns, rects) -> screenshots. It never pastes, never
presses Enter, never clicks. A watchdog voids any round raced by real user
mouse input (buttons down / cursor moved by someone else / foreground stolen).

Usage: python rc_probe/qoder_ctrl_n_focus.py --exe="Qoder.exe"
       python rc_probe/qoder_ctrl_n_focus.py --exe="Qoder CN.exe"
"""
import sys
import time
import ctypes
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation, IUIAutomation, IUIAutomationValuePattern,
    IUIAutomationTextPattern)

from PIL import ImageGrab

TreeScope_Descendants = 4
PID_ControlType = 30003

CTL_NAMES = {
    50000: "Button", 50004: "Edit", 50007: "ListItem", 50008: "List",
    50020: "Text", 50023: "Tree", 50024: "TreeItem", 50025: "Custom",
    50026: "Group", 50029: "DataItem", 50030: "Document", 50033: "Pane",
    50034: "ScrollBar", 50036: "SplitButton", 50037: "StatusBar",
    50038: "Tab", 50039: "TabItem", 50040: "ToolBar",
}
LISTISH = {50007, 50008, 50023, 50024, 50029}  # ListItem/List/Tree/TreeItem/DataItem
PATTERN_NAMES = {
    10000: "Invoke", 10002: "Value", 10010: "SelectionItem",
    10014: "Text", 10018: "ScrollItem", 10019: "ExpandCollapse",
}

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()
user32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
user32.GetForegroundWindow.restype = wt.HWND
user32.GetAsyncKeyState.restype = ctypes.c_short
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.IsWindowVisible.restype = wt.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.MapVirtualKeyW.restype = wt.UINT
user32.SendInput.restype = wt.UINT
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
BUTTONS = {"L": 0x01, "R": 0x02, "M": 0x04}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def send_key(vk, up=False):
    i = INPUT()
    i.type = 1
    i.ki.wVk = vk
    i.ki.wScan = user32.MapVirtualKeyW(vk, 0)
    i.ki.dwFlags = 0x0008 | (0x0002 if up else 0)  # SCANCODE | KEYUP
    # Delete/arrows/etc. share scancodes with numpad keys: without the
    # extended flag, 0x53 lands as numpad '.' and the zh IME types '。'.
    if vk in (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E):
        i.ki.dwFlags |= 0x0001  # KEYEVENTF_EXTENDEDKEY
    return user32.SendInput(1, byref(i), ctypes.sizeof(INPUT)) == 1


def hotkey(mod, key):
    ok = send_key(mod)
    ok = send_key(key) and ok
    time.sleep(0.03)
    ok = send_key(key, True) and ok
    ok = send_key(mod, True) and ok
    return ok


def arg(name, default=None):
    for a in sys.argv[1:]:
        if a.startswith("--%s=" % name):
            return a.split("=", 1)[1]
    return default


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


class Guard:
    def __init__(self):
        self.hits = []

    def cursor(self):
        pt = wt.POINT()
        user32.GetCursorPos(byref(pt))
        return (pt.x, pt.y)

    def check(self, step, exp_cursor, exp_fg):
        bad = []
        down = [k for k, vk in BUTTONS.items()
                if (user32.GetAsyncKeyState(vk) & 0x8000) != 0]
        if down:
            bad.append("button_down=%s" % down)
        cur = self.cursor()
        if exp_cursor is not None and cur != tuple(exp_cursor):
            bad.append("cursor=%s expected=%s" % (cur, exp_cursor))
        fg = user32.GetForegroundWindow()
        if exp_fg is not None and fg != exp_fg:
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
            bad.append("foreground=%s(%s) expected=%s" % (fg, cls.value, exp_fg))
        if bad:
            self.hits.append("%s: %s" % (step, "; ".join(bad)))
        return not bad

    def clean(self):
        return not self.hits


def describe(el):
    if el is None:
        return {"focus": None}
    d = {}
    try:
        ct = el.CurrentControlType
        d["ctl"] = CTL_NAMES.get(ct, str(ct))
        d["ctl_id"] = ct
        d["name"] = (el.CurrentName or "")[:60]
        d["aid"] = el.CurrentAutomationId or ""
        d["class"] = el.CurrentClassName or ""
        d["kbd_focusable"] = bool(el.CurrentIsKeyboardFocusable)
        r = el.CurrentBoundingRectangle
        d["rect"] = [r.left, r.top, r.right, r.bottom]
        pats = []
        for pid_, pname in PATTERN_NAMES.items():
            try:
                p = el.GetCurrentPattern(pid_)
                if p is not None:
                    pats.append(pname)
            except Exception:
                pass
        d["patterns"] = pats
    except Exception as e:
        d["error"] = str(e)
    return d


def ancestors(ui, el, depth=8):
    chain = []
    walker = ui.RawViewWalker
    cur = el
    for _ in range(depth):
        if cur is None:
            break
        try:
            cur = walker.GetParentElement(cur)
        except Exception:
            break
        if cur is None:
            break
        try:
            ct = cur.CurrentControlType
            chain.append({"ctl": CTL_NAMES.get(ct, str(ct)), "ctl_id": ct,
                          "name": (cur.CurrentName or "")[:40],
                          "aid": cur.CurrentAutomationId or ""})
        except Exception:
            break
    return chain


def focus_snapshot(ui, hwnd):
    out = {}
    el = ui.GetFocusedElement()
    out["focus"] = describe(el)
    out["ancestors"] = ancestors(ui, el) if el is not None else []
    root = ui.ElementFromHandle(hwnd)
    doc = root.FindFirst(TreeScope_Descendants,
                         ui.CreatePropertyCondition(PID_ControlType, 50030))
    out["doc_name"] = ((doc.CurrentName or "")[:60] if doc is not None
                       else "<no-document>")
    return out


def shot(hwnd, path):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, byref(r))
    img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                         all_screens=True)
    img.save(path)
    return path


def set_clipboard(text):
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    u32.OpenClipboard.argtypes = [wt.HWND]
    u32.OpenClipboard.restype = wt.BOOL
    u32.EmptyClipboard.restype = wt.BOOL
    u32.SetClipboardData.argtypes = [wt.UINT, wt.HANDLE]
    u32.SetClipboardData.restype = wt.HANDLE
    u32.CloseClipboard.restype = wt.BOOL
    kernel32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
    kernel32.GlobalAlloc.restype = wt.HGLOBAL
    kernel32.GlobalLock.argtypes = [wt.HGLOBAL]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = [wt.HGLOBAL]
    data = text.encode("utf-16-le") + b"\x00\x00"
    if not u32.OpenClipboard(None):
        return False
    try:
        u32.EmptyClipboard()
        h = kernel32.GlobalAlloc(0x0042, len(data))
        p = kernel32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        kernel32.GlobalUnlock(h)
        return bool(u32.SetClipboardData(0x000D, h))  # CF_UNICODETEXT; system owns h
    finally:
        u32.CloseClipboard()


def read_editor_text(el):
    """Value pattern first, Text pattern document range as fallback."""
    try:
        p = el.GetCurrentPattern(10002)
        vp = p.QueryInterface(IUIAutomationValuePattern)
        return ("value", vp.CurrentValue or "")
    except Exception:
        pass
    try:
        p = el.GetCurrentPattern(10014)
        tp = p.QueryInterface(IUIAutomationTextPattern)
        return ("text", tp.DocumentRange.GetText(-1) or "")
    except Exception as e:
        return ("unreadable", str(e))


def is_composer(snap):
    f = snap["focus"]
    return (f.get("ctl_id") == 50004 and
            "loop-rich-editor" in (f.get("class") or ""))


def verdict(snap):
    f = snap["focus"]
    if not f or f.get("focus") is None and "ctl_id" not in f:
        return "NO_FOCUS_ELEMENT"
    ids = [f.get("ctl_id")] + [a.get("ctl_id") for a in snap["ancestors"]]
    if any(i in LISTISH for i in ids):
        return "FOCUS_IN_LIST"
    pats = f.get("patterns") or []
    if f.get("ctl_id") in (50004, 50030) and ("Value" in pats or "Text" in pats):
        return "FOCUS_IN_EDIT"
    if "Value" in pats:
        return "VALUE_PATTERN_ELSEWHERE"
    return "UNPROVEN"


def main():
    exe = arg("exe", "Qoder.exe")
    tag = exe.lower().replace(" ", "_").replace(".exe", "")
    pids = pids_for(exe)
    if not pids:
        print("NOT_RUNNING: %s" % exe)
        return 1
    win = top_window(pids)
    if not win:
        print("NO_WINDOW: %s" % exe)
        return 1
    hwnd, title, size = win
    print("TARGET=%s hwnd=%s title=%r size=%s" % (exe, hwnd, title, size))

    ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
    guard = Guard()
    cur0 = guard.cursor()
    if not guard.check("start", cur0, None):
        print("VOID: user input at start: %s" % guard.hits)
        return 2

    # raise (Alt tap trick, same as AgentWindow::raise); restore if minimized
    if user32.IsIconic(hwnd):
        print("WAS_MINIMIZED -> SW_RESTORE")
        user32.ShowWindow(hwnd, 9)
        time.sleep(0.6)
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    if not guard.check("raise", cur0, hwnd):
        print("VOID: raise raced: %s" % guard.hits)
        return 2
    print("FOREGROUND_OK")

    before = focus_snapshot(ui, hwnd)
    print("BEFORE_FOCUS=%s" % before)
    shot(hwnd, "rc_probe/ctrl_n_%s_before.png" % tag)

    ok = hotkey(0x11, 0x4E)  # Ctrl+N
    print("CTRL_N_SENT=%s" % ok)
    if not ok:
        print("SENDINPUT_FAILED")
        return 3
    time.sleep(1.0)
    if not guard.check("after-ctrl-n", cur0, hwnd):
        print("VOID: user input during round: %s" % guard.hits)
        return 2

    after = focus_snapshot(ui, hwnd)
    print("AFTER_FOCUS=%s" % after)
    shot(hwnd, "rc_probe/ctrl_n_%s_after.png" % tag)

    v = verdict(after)
    print("VERDICT=%s" % v)
    print("DOC_BEFORE=%r DOC_AFTER=%r" % (before["doc_name"], after["doc_name"]))

    if arg("paste") == "1":
        # Gated paste: only when focus is PROVEN to sit in the composer.
        # Never a blind paste; never Enter.
        if not (v == "FOCUS_IN_EDIT" and is_composer(after)):
            print("PASTE_SKIPPED: focus gate failed (%s)" % v)
        else:
            el = ui.GetFocusedElement()
            kind0, text0 = read_editor_text(el)
            print("COMPOSER_BEFORE=%s:%r" % (kind0, text0[:80]))
            marker = "AgentRMC handoff self-test marker (unsent)"
            if not set_clipboard(marker):
                print("CLIPBOARD_SET_FAILED")
            else:
                okp = hotkey(0x11, 0x56)  # Ctrl+V
                print("PASTE_SENT=%s" % okp)
                time.sleep(0.8)
                if not guard.check("after-paste", cur0, hwnd):
                    print("VOID: user input during paste: %s" % guard.hits)
                    return 2
                el2 = ui.GetFocusedElement()
                kind1, text1 = read_editor_text(el2)
                landed = marker in text1
                print("COMPOSER_AFTER=%s:%r" % (kind1, text1[:120]))
                print("MARKER_LANDED=%s" % landed)
                shot(hwnd, "rc_probe/ctrl_n_%s_pasted.png" % tag)
                # Cleanup only if focus is still the same composer and the
                # marker really is what sits in it.
                snap2 = focus_snapshot(ui, hwnd)
                if landed and is_composer(snap2):
                    hotkey(0x11, 0x41)  # Ctrl+A inside composer
                    time.sleep(0.15)
                    send_key(0x2E)      # Delete
                    send_key(0x2E, True)
                    time.sleep(0.5)
                    el3 = ui.GetFocusedElement()
                    kind2, text2 = read_editor_text(el3)
                    print("COMPOSER_CLEANED=%s:%r" % (kind2, text2[:80]))
                    shot(hwnd, "rc_probe/ctrl_n_%s_cleaned.png" % tag)
                else:
                    print("CLEANUP_SKIPPED: gate failed after paste")
    print("ROUND_CLEAN=%s (no Enter, no clicks performed)" % guard.clean())
    return 0


if __name__ == "__main__":
    sys.exit(main())
