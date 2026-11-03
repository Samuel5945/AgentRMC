"""Find the host's 'new task' affordance in its UIA tree.

Two modes:
  default        scan Button controls for the keyword names, report Invoke support
  --all=true     scan EVERY control type in the tree for a name/automationId hit
                 (the sidebar new-task button is not a Button in some builds)

The walk self-proves: it prints visited/named counts and a control-type
histogram, so "0 hits" can be told apart from "the walk never read anything".

Reads only. --invoke would act on the app, so this tool deliberately has no
such flag; reaching the control is the thing being measured here.
"""
import sys
import os
import re
import ctypes
import ctypes.wintypes
from ctypes import byref

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation  # noqa: E402

# comtypes' generated UIAutomationClient has no IInvokeProvider, so declare it
# against its published IID (UIAutomation.h): a wrong IID fails the QI loudly
# instead of invoking something else.
from comtypes import COMMETHOD, GUID, HRESULT, IUnknown  # noqa: E402


class IInvokeProvider(IUnknown):
    _iid_ = GUID("{00225041-116c-4327-9929-00756f4458c8}")
    _methods_ = [COMMETHOD([], HRESULT, "Invoke")]

TreeScope_Descendants = 4
PID_ControlType = 30003
PID_Name = 30005
PID_AutomationId = 30011
PID_ClassName = 30012
PID_IsOffscreen = 30022
PID_HasKeyboardFocus = 30039
CID_Button = 50000
INVOKE_PATTERN = 10000
LEGACY_PATTERN = 10033

TARGETS = ("WorkBuddyAI.exe", "WorkBuddy.exe", "DeepSeek Harness.exe")
NAME_KEYWORDS = ("新任务", "新建任务", "新建会话", "新会话", "New task",
                 "New Task", "newChat", "New chat")
ID_KEYWORDS = ("new-task", "newTask", "new_task", "newchat", "new-chat")

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()
user32.SetForegroundWindow.argtypes = [ctypes.wintypes.HWND]
user32.IsWindowVisible.restype = ctypes.wintypes.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = ctypes.wintypes.DWORD
EnumWindowsProc = ctypes.WINFUNCTYPE(
    ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)


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
        got = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, byref(got))
        if got.value in pids and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            r = ctypes.wintypes.RECT()
            user32.GetWindowRect(hwnd, byref(r))
            if buf.value:
                hits.append((hwnd, buf.value, (r.right - r.left,
                                               r.bottom - r.top)))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    if not hits:
        return None
    return max(hits, key=lambda t: t[2][0] * t[2][1])


def pv(el, pid):
    try:
        return el.GetCurrentPropertyValue(pid)
    except Exception:
        return None


def has_pattern(el, pat):
    """A comtypes POINTER(IUnknown) is truthy-or-NULL; `is not None` proves
    nothing. For InvokePattern the real test is the QI to IInvokeProvider."""
    try:
        p = el.GetCurrentPattern(pat)
    except Exception:
        return False
    if not p:
        return False
    if pat == INVOKE_PATTERN:
        try:
            p.QueryInterface(IInvokeProvider)
        except Exception:
            return False
    return True


def measure(ui, hwnd):
    """One scan pass: return (named_count, [hit dicts])."""
    root = ui.ElementFromHandle(hwnd)
    if root is None:
        return -1, []
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    n = found.Length if found else 0
    named = 0
    hits = []
    for i in range(n):
        el = found.GetElement(i)
        if el is None:
            continue
        nm = pv(el, PID_Name) or ""
        if nm:
            named += 1
        if not any(k in nm for k in NAME_KEYWORDS):
            continue
        try:
            r = el.CurrentBoundingRectangle
            w, h = r.right - r.left, r.bottom - r.top
        except Exception:
            r, w, h = None, 0, 0
        hits.append({"ctl": pv(el, PID_ControlType), "name": nm, "w": w, "h": h,
                     "off": bool(pv(el, PID_IsOffscreen)),
                     "invoke": has_pattern(el, INVOKE_PATTERN)})
    return named, hits


def away_window(exclude_pid):
    """A neutral window to hand foreground to between rounds: our own app
    first, so the 'backgrounded' state does not depend on another host."""
    for exe in ("AgentRMCNative.exe", "Qoder.exe", "Qoder CN.exe"):
        pids = set(pids_for(exe)) - {exclude_pid}
        w = top_window(pids) if pids else None
        if w:
            return w[0], exe
    return None, None


def run_rounds(ui, exe, rounds):
    import time

    away_ms = int(arg("awayms") or 450)
    user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
    pids = pids_for(exe)
    win = top_window(set(pids)) if pids else None
    if not win:
        print("== %s NO-WINDOW" % exe)
        return 0, 0
    hwnd, title, size = win
    print("== %s hwnd=%d %dx%d  %d rounds  away=%dms"
          % (exe, hwnd, size[0], size[1], rounds, away_ms))
    if size[0] < 500:
        print("   ABORT: mini window only — rounds would measure nothing")
        return 0, 0
    ok = 0
    causes = []
    for rnd in range(1, rounds + 1):
        away, away_name = away_window(pids[0])
        if away:
            user32.SetForegroundWindow(away)
            time.sleep(away_ms / 1000.0)
        was_fg = (user32.GetForegroundWindow() == hwnd)
        user32.SetForegroundWindow(hwnd)
        fg_ms = -1
        for t in range(20):
            if user32.GetForegroundWindow() == hwnd:
                fg_ms = t * 100
                break
            time.sleep(0.1)
        named, hits = -1, []
        tree_ms = -1
        for t in range(60):
            named, hits = measure(ui, hwnd)
            if named >= 15:
                tree_ms = t * 100
                break
            time.sleep(0.1)
        usable = [h for h in hits
                  if h["invoke"] and not h["off"] and h["w"] > 8 and h["h"] > 8]
        if fg_ms < 0:
            cause = "FG-NOT-ACQUIRED"
        elif named < 15:
            cause = "TREE-THIN(named=%d)" % named
        elif not usable:
            cause = ("NO-USABLE-HIT(hits=%d,named=%d)" % (len(hits), named))
        else:
            cause = ""
            ok += 1
        h = usable[0] if usable else None
        print("   r%d away=%s was_fg=%d fg_ms=%-4d tree_ms=%-4d named=%-4d "
              "hits=%d%s%s%s"
              % (rnd, (away_name or "none")[:14], 1 if was_fg else 0, fg_ms,
                 tree_ms, named, len(hits),
                 (" pick=ctl%s %r %dx%d invoke=%s off=%s"
                  % (h["ctl"], h["name"][:10], h["w"], h["h"], h["invoke"],
                     h["off"])) if h else "",
                 (" allhits=%s" % [(x["ctl"], x["w"], x["h"], x["off"],
                                    x["invoke"]) for x in hits][:4])
                 if hits and not usable else "",
                 ("  FAIL: %s" % cause) if cause else ""))
        if cause:
            causes.append(cause)
    print("   RESULT %s: %d/%d usable hits%s"
          % (exe, ok, rounds, (" failures=%s" % causes) if causes else ""))
    return ok, rounds


def invoke_el(el):
    """UIA returns the pattern as IUnknown; it must be QI'd to IInvokeProvider
    before Invoke() exists on it."""
    pat = el.GetCurrentPattern(INVOKE_PATTERN)
    if pat is None:
        return "pattern=None"
    try:
        inv = pat.QueryInterface(IInvokeProvider)
    except Exception as e:
        return "QueryInterface(IInvokeProvider) failed: %r" % e
    inv.Invoke()
    return "Invoke() ok"


def focus_snapshot(ui):
    """Who owns the caret right now — the only in-app marker that survives
    PrintWindow being unavailable on these Electron windows."""
    try:
        el = ui.GetFocusedElement()
    except Exception as e:
        return {"error": repr(e)}
    if el is None:
        return {"none": True}
    try:
        r = el.CurrentBoundingRectangle
        rect = (r.left, r.top, r.right, r.bottom)
    except Exception:
        rect = None
    return {"pid": pv(el, 30002), "ctl": pv(el, PID_ControlType),
            "name": (pv(el, PID_Name) or "")[:40], "cls": pv(el, PID_ClassName),
            "aid": pv(el, PID_AutomationId), "rect": rect,
            "editable": pv(el, PID_ControlType) in (50004, 50026, 50030)}


def markers(ui, hwnd):
    named, hits = measure(ui, hwnd)
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    n = found.Length if found else 0
    names = set()
    for i in range(n):
        el = found.GetElement(i)
        nm = pv(el, PID_Name)
        if nm:
            names.add(nm)
    conv = [k for k in ("点赞", "点踩", "复制分支名", "添加到对话") if k in names]
    home = [k for k in ("日常开发", "开发网站", "技术文档", "Agent 应用")
            if k in names]
    return {"named": named, "hits": hits, "in_conversation": conv,
            "on_home": home}


def run_invoke(ui, exe):
    import time

    user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
    pids = pids_for(exe)
    win = top_window(set(pids)) if pids else None
    if not win or win[2][0] < 500:
        print("== %s SKIP (no main window)" % exe)
        return False
    hwnd, title, size = win
    print("== %s hwnd=%d %dx%d" % (exe, hwnd, size[0], size[1]))
    away, away_name = away_window(pids[0])
    if away:
        user32.SetForegroundWindow(away)
        time.sleep(0.45)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    if user32.GetForegroundWindow() != hwnd:
        print("   ABORT: foreground not acquired, Invoke would hit the wrong "
              "window")
        return False
    before = markers(ui, hwnd)
    usable = [h for h in before["hits"]
              if h["invoke"] and not h["off"] and h["w"] > 8 and h["h"] > 8]
    print("   BEFORE named=%d hits=%d usable=%d focus=%s"
          % (before["named"], len(before["hits"]), len(usable),
             focus_snapshot(ui)))
    print("   BEFORE markers in_conversation=%s on_home=%s"
          % (before["in_conversation"], before["on_home"]))
    if not usable:
        print("   VERDICT: NO-USABLE-CONTROL — nothing invoked")
        return False
    target = usable[0]
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    el = None
    for i in range(found.Length):
        e = found.GetElement(i)
        if e is None:
            continue
        nm = pv(e, PID_Name) or ""
        if any(k in nm for k in NAME_KEYWORDS) and has_pattern(e, INVOKE_PATTERN):
            try:
                r = e.CurrentBoundingRectangle
            except Exception:
                continue
            if (r.right - r.left) > 8 and (r.bottom - r.top) > 8 and not pv(
                    e, PID_IsOffscreen):
                el = e
                break
    if el is None:
        print("   VERDICT: TARGET-ELEMENT-LOST-BETWEEN-PASSES")
        return False
    res = invoke_el(el)
    print("   INVOKE sent: name=%r ctl=%s result=%s"
          % (target["name"], target["ctl"], res))
    time.sleep(1.5)
    after = markers(ui, hwnd)
    print("   AFTER  named=%d hits=%d focus=%s"
          % (after["named"], len(after["hits"]), focus_snapshot(ui)))
    print("   AFTER  markers in_conversation=%s on_home=%s"
          % (after["in_conversation"], after["on_home"]))
    left_conv = bool(before["in_conversation"]) and not after["in_conversation"]
    focus_ok = focus_snapshot(ui).get("editable", False)
    if focus_ok:
        print("   VERDICT: NEW-TASK-FOCUS-IN-INPUT (focused control is "
              "editable)")
        return True
    if left_conv or (after["on_home"] and not before["on_home"]):
        print("   VERDICT: SURFACE-CHANGED-BUT-FOCUS-NOT-EDITABLE — the page "
              "moved, caret not proven in an input")
        return False
    print("   VERDICT: NO-OBSERVABLE-CHANGE — Invoke returned without error but "
          "no surface or focus marker moved")
    return False


def click_at(cx, cy):
    """Real mouse click, because these TabItem controls expose no Invoke
    pattern. Cursor position is saved and restored."""
    user32.GetCursorPos.argtypes = [ctypes.POINTER(ctypes.wintypes.POINT)]
    pt = ctypes.wintypes.POINT()
    user32.GetCursorPos(byref(pt))
    if not user32.SetCursorPos(cx, cy):
        return "SetCursorPos blocked (err=%d)" % ctypes.get_last_error()
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    user32.SetCursorPos(pt.x, pt.y)
    return "clicked (%d,%d), cursor restored to (%d,%d)" % (cx, cy, pt.x, pt.y)


def find_target(ui, hwnd, name_needles):
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    for i in range(found.Length):
        el = found.GetElement(i)
        nm = pv(el, PID_Name) or ""
        if not any(k in nm for k in name_needles):
            continue
        try:
            r = el.CurrentBoundingRectangle
        except Exception:
            continue
        w, h = r.right - r.left, r.bottom - r.top
        if w <= 8 or h <= 8 or pv(el, PID_IsOffscreen):
            continue
        return i, nm, pv(el, PID_ControlType), (r.left, r.top, r.right, r.bottom), \
            ((r.left + r.right) // 2, (r.top + r.bottom) // 2)
    return None


def run_click(ui, exe, needles, label):
    import time

    user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
    pids = pids_for(exe)
    win = top_window(set(pids)) if pids else None
    if not win or win[2][0] < 500:
        print("== %s SKIP (no main window)" % exe)
        return False
    hwnd, title, size = win
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.8)
    if user32.GetForegroundWindow() != hwnd:
        print("== %s ABORT: foreground not acquired" % exe)
        return False
    before = markers(ui, hwnd)
    tgt = find_target(ui, hwnd, needles)
    print("== %s %s  BEFORE named=%d focus=%s"
          % (exe, label, before["named"], focus_snapshot(ui)))
    print("   BEFORE in_conversation=%s on_home=%s"
          % (before["in_conversation"], before["on_home"]))
    if not tgt:
        print("   VERDICT: TARGET-NOT-FOUND needles=%s (no hit, nothing clicked)"
              % (needles,))
        return False
    idx, nm, ctl, rect, center = tgt
    print("   TARGET [%d] name=%r ctl=%s rect=%s center=%s"
          % (idx, nm[:24], ctl, rect, center))
    print("   click: %s" % click_at(center[0], center[1]))
    time.sleep(1.6)
    after = markers(ui, hwnd)
    fc = focus_snapshot(ui)
    print("   AFTER  named=%d focus=%s" % (after["named"], fc))
    print("   AFTER  in_conversation=%s on_home=%s"
          % (after["in_conversation"], after["on_home"]))
    entered = (not before["in_conversation"]) and bool(after["in_conversation"])
    left = bool(before["in_conversation"]) and not after["in_conversation"]
    if fc.get("editable"):
        print("   VERDICT: SEMANTICS-OK — caret is in an editable control after "
              "the click")
        return True
    if left:
        print("   VERDICT: LEFT-CONVERSATION but caret not in an editable "
              "control (%s ctl=%s)" % (fc.get("name"), fc.get("ctl")))
    elif entered:
        print("   VERDICT: ENTERED-CONVERSATION (this click was the setup, "
              "not the new-task action)")
    else:
        print("   VERDICT: NO-SURFACE-CHANGE and caret not editable")
    return False


class ISelectionItemProvider(IUnknown):
    """Declared by hand (comtypes does not generate it). The IID is the point
    of this test: QI either resolves it or it does not, regardless of which
    numeric pattern id Chromium reported as supported."""
    _iid_ = GUID("{1B96C8C1-AC3F-41DD-8345-FF3F188C472E}")
    _methods_ = [
        COMMETHOD([], HRESULT, "Select"),
        COMMETHOD([], HRESULT, "AddToSelection"),
        COMMETHOD([], HRESULT, "RemoveFromSelection"),
    ]


def supported_patterns(el):
    out = []
    for pat in range(10000, 10037):
        try:
            p = el.GetCurrentPattern(pat)
        except Exception:
            continue
        if p:
            out.append((pat, p))
    return out


def cursor_pos():
    user32.GetCursorPos.argtypes = [ctypes.POINTER(ctypes.wintypes.POINT)]
    pt = ctypes.wintypes.POINT()
    user32.GetCursorPos(byref(pt))
    return (pt.x, pt.y)


def run_select_probe(ui, exe, needles):
    """Can the new-task control be activated without touching the mouse?"""
    import time

    user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
    pids = pids_for(exe)
    win = top_window(set(pids)) if pids else None
    if not win or win[2][0] < 500:
        print("== %s SKIP (no main window)" % exe)
        return False
    hwnd, title, size = win
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.8)
    if user32.GetForegroundWindow() != hwnd:
        print("== %s ABORT: foreground not acquired" % exe)
        return False
    before = markers(ui, hwnd)
    tgt = find_target(ui, hwnd, needles)
    cur0 = cursor_pos()
    print("== %s BEFORE named=%d focus=%s cursor=%s"
          % (exe, before["named"], focus_snapshot(ui), cur0))
    print("   BEFORE in_conversation=%s on_home=%s"
          % (before["in_conversation"], before["on_home"]))
    if not tgt:
        print("   VERDICT: TARGET-NOT-FOUND (nothing attempted)")
        return False
    idx, nm, ctl, rect, center = tgt
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    el = found.GetElement(idx)
    sup = supported_patterns(el)
    print("   TARGET [%d] name=%r ctl=%s rect=%s supported_pattern_ids=%s"
          % (idx, nm[:20], ctl, rect, [p[0] for p in sup]))
    provider = None
    for pat, obj in sup:
        try:
            q = obj.QueryInterface(ISelectionItemProvider)
        except Exception as e:
            print("     QI(100%d -> ISelectionItemProvider) failed: %s"
                  % (pat, type(e).__name__))
            continue
        print("     QI(100%d -> ISelectionItemProvider) OK" % pat)
        provider = (pat, q)
        break
    if provider is None:
        print("   VERDICT: NO-SELECTIONITEM-PROVIDER — Select() unavailable, "
              "mouse click stays the only primitive")
        return False
    pat, prov = provider
    try:
        prov.Select()
        acted = "Select() returned OK (pattern id %d)" % pat
    except Exception as e:
        print("   VERDICT: SELECT-FAILED %r" % e)
        return False
    print("   action: %s" % acted)
    time.sleep(1.6)
    cur1 = cursor_pos()
    after = markers(ui, hwnd)
    fc = focus_snapshot(ui)
    print("   AFTER  named=%d focus=%s cursor=%s delta=(%d,%d)"
          % (after["named"], fc, cur1, cur1[0] - cur0[0], cur1[1] - cur0[1]))
    print("   AFTER  in_conversation=%s on_home=%s"
          % (after["in_conversation"], after["on_home"]))
    moved = cur1 != cur0
    left = bool(before["in_conversation"]) and not after["in_conversation"]
    home = after["on_home"] and not before["on_home"]
    if fc.get("editable") and (left or home) and not moved:
        print("   VERDICT: SELECT-WORKS-NO-MOUSE — new task opened, caret in "
              "editable control, cursor unmoved")
        return True
    print("   VERDICT: PARTIAL editable=%s left_conv=%s home=%s cursor_moved=%s"
          % (fc.get("editable"), left, home, moved))
    return False


def press_key(vk):
    user32.keybd_event(vk, 0, 0, 0)
    user32.keybd_event(vk, 0, 0x0002, 0)  # KEYUP


def run_focuskey(ui, exe, needles):
    """No-mouse activation: UIA SetFocus() on the target, then press Enter.

    Deliberately avoids pattern providers (my hand-declared IIDs cannot even
    QI against a Button's real InvokePattern), using only SetFocus + a key.
    """
    import time

    user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
    pids = pids_for(exe)
    win = top_window(set(pids)) if pids else None
    if not win or win[2][0] < 500:
        print("== %s SKIP (no main window — protocol-wake it first)" % exe)
        return False
    hwnd, title, size = win
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.8)
    if user32.GetForegroundWindow() != hwnd:
        print("== %s ABORT: foreground not acquired" % exe)
        return False
    before = markers(ui, hwnd)
    tgt = find_target(ui, hwnd, needles)
    cur0 = cursor_pos()
    print("== %s BEFORE named=%d focus=%s cursor=%s"
          % (exe, before["named"], focus_snapshot(ui), cur0))
    print("   BEFORE in_conversation=%s on_home=%s"
          % (before["in_conversation"], before["on_home"]))
    if not tgt:
        print("   VERDICT: TARGET-NOT-FOUND (nothing attempted)")
        return False
    idx, nm, ctl, rect, center = tgt
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    el = found.GetElement(idx)
    try:
        el.SetFocus()
        sf = "SetFocus() ok"
    except Exception as e:
        print("   VERDICT: SETFOCUS-FAILED %r" % e)
        return False
    time.sleep(0.4)
    fc1 = focus_snapshot(ui)
    print("   %s -> focus now=%s" % (sf, fc1))
    if (fc1.get("name") or "")[:6] not in nm[:6] and fc1.get("ctl") != ctl:
        print("   VERDICT: SETFOCUS-DID-NOT-LAND (focus is elsewhere) — Enter "
              "would go to the wrong control, aborting before any key")
        return False
    press_key(0x0D)  # VK_RETURN
    print("   key: VK_RETURN sent (no mouse used)")
    time.sleep(1.6)
    cur1 = cursor_pos()
    after = markers(ui, hwnd)
    fc = focus_snapshot(ui)
    print("   AFTER  named=%d focus=%s cursor=%s delta=(%d,%d)"
          % (after["named"], fc, cur1, cur1[0] - cur0[0], cur1[1] - cur0[1]))
    print("   AFTER  in_conversation=%s on_home=%s"
          % (after["in_conversation"], after["on_home"]))
    left = bool(before["in_conversation"]) and not after["in_conversation"]
    home = bool(after["on_home"]) and not before["on_home"]
    if fc.get("editable") and (left or home) and cur1 == cur0:
        print("   VERDICT: NO-MOUSE-ACTIVATION-OK — new task opened, caret in "
              "editable control, cursor never moved")
        return True
    print("   VERDICT: PARTIAL editable=%s left_conv=%s home=%s cursor_delta=%s"
          % (fc.get("editable"), left, home,
             (cur1[0] - cur0[0], cur1[1] - cur0[1])))
    return False


CONV_RE = re.compile(r"(分钟前|小时前|天前|昨天)$")
WAKE_URI = {"WorkBuddy.exe": "workbuddy://new-task",
            "WorkBuddyAI.exe": "workbuddy-ai://new-task"}


def find_conv_button(ui, hwnd):
    """Sidebar conversation entries are Buttons whose name ends in a relative
    timestamp. Matching on that shape survives the timestamp drifting (the
    lesson from the AI run where '45分钟前' had become '1小时前')."""
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    for i in range(found.Length):
        el = found.GetElement(i)
        if pv(el, PID_ControlType) != 50000:
            continue
        nm = (pv(el, PID_Name) or "").strip()
        if not nm or nm in ("新建任务", "发现应用", "消息中心"):
            continue
        if not CONV_RE.search(nm):
            continue
        try:
            r = el.CurrentBoundingRectangle
        except Exception:
            continue
        if (r.right - r.left) < 100 or (r.bottom - r.top) < 10:
            continue
        if pv(el, PID_IsOffscreen):
            continue
        return i, nm
    return None


def activate_by_name(ui, hwnd, needles=None, conv=False, timeout_ms=2500):
    """SetFocus then Enter. Returns (ok, ms, detail)."""
    import time

    for _ in range(25):
        tgt = find_conv_button(ui, hwnd) if conv else find_target(
            ui, hwnd, needles)
        if tgt:
            break
        time.sleep(0.1)
    else:
        return False, -1, "target-never-found"
    idx = tgt[0]
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    el = found.GetElement(idx)
    try:
        el.SetFocus()
    except Exception as e:
        return False, -1, "SetFocus raised %r" % e
    press_key(0x0D)
    t0 = time.time()
    return True, int((time.time() - t0) * 1000), "name=%r ctl=%s" % (
        tgt[1][:28], pv(el, PID_ControlType))


def wait_state(ui, hwnd, want_conv, timeout_ms=4000):
    """Poll until the conversation/home markers say what we expect."""
    import time

    for _ in range(timeout_ms // 100):
        st = markers(ui, hwnd)
        if want_conv and st["in_conversation"]:
            return True, st
        if not want_conv and not st["in_conversation"] and st["on_home"]:
            fc = focus_snapshot(ui)
            if fc.get("editable"):
                return True, st
        time.sleep(0.1)
    return False, markers(ui, hwnd)


def activate_conv_setup(ui, hwnd, exe):
    """Scaffolding only: get the app into a conversation so the measured
    keyboard activation has a visible before/after. In the AI build the list
    row ignores Enter *and* Space under UIA focus, so fall back to one real
    click there; the cursor baseline for the measurement is taken after this.
    """
    import time

    tgt = find_conv_button(ui, hwnd)
    if not tgt:
        return False, "no-conversation-row"
    idx, nm = tgt
    root = ui.ElementFromHandle(hwnd)
    found = root.FindAll(TreeScope_Descendants, ui.CreateTrueCondition())
    el = found.GetElement(idx)
    try:
        el.SetFocus()
        press_key(0x0D)
        time.sleep(0.8)
        if markers(ui, hwnd)["in_conversation"]:
            return True, "keyboard"
    except Exception:
        pass
    r = el.CurrentBoundingRectangle
    res = click_at((r.left + r.right) // 2, (r.top + r.bottom) // 2)
    time.sleep(1.2)
    got = bool(markers(ui, hwnd)["in_conversation"])
    return got, "click(%s) %s" % (nm[:18], res.split(",")[0])


def run_activation_rounds(ui, exe, rounds):
    import time

    user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
    pids = pids_for(exe)
    if not pids:
        print("== %s SKIP (not running)" % exe)
        return 0, rounds
    win = top_window(set(pids))
    if not win:
        print("== %s SKIP (no window)" % exe)
        return 0, rounds
    hwnd = win[0]
    if win[2][0] < 500:
        uri = WAKE_URI.get(exe)
        if not uri:
            print("== %s SKIP (mini window and no known wake URI)" % exe)
            return 0, rounds
        os.startfile(uri)
        time.sleep(4.0)
    ok = 0
    causes = []
    print("== %s  %d activation rounds" % (exe, rounds))
    for rnd in range(1, rounds + 1):
        cause = ""
        detail = {}
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.7)
        if user32.GetForegroundWindow() != hwnd:
            cause = "FG-FAIL"
        else:
            named, _ = measure(ui, hwnd)
            detail["named"] = named
            if named < 15:
                cause = "TREE-THIN(%d)" % named
            else:
                setup_ok, how = activate_conv_setup(ui, hwnd, exe)
                detail["setup"] = how
                st1 = markers(ui, hwnd)
                if not setup_ok:
                    cause = "CONV-SETUP-FAIL(%s, conv=%s)" % (how,
                                                              st1["in_conversation"])
                else:
                    # baseline AFTER the scaffolding: only the measured step
                    # may not touch the mouse
                    cur0 = cursor_pos()
                    a2, ms2, d2 = activate_by_name(ui, hwnd,
                                                   needles=("新建任务",))
                    detail["new_act"] = d2
                    got_home, st2 = wait_state(ui, hwnd, want_conv=False)
                    cur1 = cursor_pos()
                    if not a2:
                        cause = "NEWTASK-SETUP-FAIL(%s)" % d2
                    elif cur1 != cur0:
                        cause = "CURSOR-MOVED(%s->%s)" % (cur0, cur1)
                    elif not got_home:
                        cause = ("NEWTASK-ENTER-NOEFFECT(conv=%s home=%s "
                                 "focus=%s)" % (st2["in_conversation"],
                                                st2["on_home"],
                                                focus_snapshot(ui).get("ctl")))
                    else:
                        ok += 1
        print("   r%d ok=%d %s%s" % (rnd, 1 if not cause else 0,
                                     " ".join("%s=%s" % kv for kv in
                                              detail.items()),
                                     ("  FAIL: " + cause) if cause else ""))
        if cause:
            causes.append(cause)
    print("   RESULT %s: %d/%d%s" % (exe, ok, rounds,
                                     (" failures=%s" % causes) if causes else ""))
    return ok, rounds


def main():
    scan_all = arg("all") == "true"
    do_raise = arg("raise") == "true"
    only = arg("only")
    rounds = int(arg("rounds") or 0)
    ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
    if int(arg("activate-rounds") or 0):
        n = int(arg("activate-rounds"))
        t_ok = t_tot = 0
        for exe in TARGETS:
            if only and only.lower() not in exe.lower():
                continue
            o, r = run_activation_rounds(ui, exe, n)
            t_ok += o
            t_tot += r
        print("ACTIVATION TOTAL %d/%d" % (t_ok, t_tot))
        return
    if arg("focuskey") == "true":
        needles = tuple((arg("needles") or "新建任务").split(","))
        for exe in TARGETS:
            if only and only.lower() not in exe.lower():
                continue
            run_focuskey(ui, exe, needles)
        return
    if arg("select-probe"):
        needles = tuple(arg("select-probe").split(","))
        for exe in TARGETS:
            if only and only.lower() not in exe.lower():
                continue
            run_select_probe(ui, exe, needles)
        return
    if arg("click-once"):
        needles = tuple(arg("click-once").split(","))
        label = arg("label") or "click %s" % (needles,)
        for exe in TARGETS:
            if only and only.lower() not in exe.lower():
                continue
            run_click(ui, exe, needles, label)
        return
    if arg("invoke-once") == "true":
        okc = 0
        tot = 0
        for exe in TARGETS:
            if only and only.lower() not in exe.lower():
                continue
            tot += 1
            if run_invoke(ui, exe):
                okc += 1
        print("INVOKE RESULT %d/%d" % (okc, tot))
        return
    if rounds:
        total_ok = total = 0
        for exe in TARGETS:
            if only and only.lower() not in exe.lower():
                continue
            pids = pids_for(exe)
            win = top_window(set(pids)) if pids else None
            if not win or win[2][0] < 500:
                print("== %-22s SKIP (no main window open — open it or "
                      "protocol-wake it first)" % exe)
                continue
            o, r = run_rounds(ui, exe, rounds)
            total_ok += o
            total += r
        print("TOTAL %d/%d" % (total_ok, total))
        return
    for exe in TARGETS:
        if only and only.lower() not in exe.lower():
            continue
        pids = pids_for(exe)
        win = top_window(set(pids)) if pids else None
        if not win:
            print("== %-22s VERDICT: NO-VISIBLE-WINDOW (nothing to scan)" % exe)
            continue
        hwnd, title, size = win
        print("== %s  window=%r %dx%d" % (exe, title[:36], size[0], size[1]))
        if size[0] < 500:
            print("   VERDICT: MINI-WINDOW-ONLY (%dx%d) — main surface is not "
                  "open, a 0-hit scan here means nothing, not absence" % size)
            continue
        if do_raise:
            import time

            user32.SetForegroundWindow(hwnd)
            time.sleep(1.2)
            print("   (raised to foreground, settled 1.2s)")
        root = ui.ElementFromHandle(hwnd)
        cond = (ui.CreateTrueCondition() if scan_all else
                ui.CreatePropertyCondition(PID_ControlType, CID_Button))
        found = root.FindAll(TreeScope_Descendants, cond)
        n = found.Length if found else 0
        hist = {}
        named = 0
        hits = []
        for i in range(n):
            el = found.GetElement(i)
            if el is None:
                continue
            ct = pv(el, PID_ControlType)
            hist[ct] = hist.get(ct, 0) + 1
            nm = (pv(el, PID_Name) or "")
            aid = (pv(el, PID_AutomationId) or "")
            if nm:
                named += 1
            name_hit = any(k in nm for k in NAME_KEYWORDS)
            id_hit = any(k in aid for k in ID_KEYWORDS)
            if name_hit or id_hit:
                r = el.CurrentBoundingRectangle
                hits.append({"i": i, "ctl": ct, "name": nm, "aid": aid,
                             "cls": pv(el, PID_ClassName),
                             "offscreen": pv(el, PID_IsOffscreen),
                             "focus": pv(el, PID_HasKeyboardFocus),
                             "invoke": has_pattern(el, INVOKE_PATTERN),
                             "legacy": has_pattern(el, LEGACY_PATTERN),
                             "rect": (r.left, r.top, r.right, r.bottom)})
        print("   scanned=%d named=%d hit=%d  scope=%s"
              % (n, named, len(hits), "all-control-types" if scan_all
                 else "Button only"))
        print("   control types: %s" % ", ".join(
            "%s:%d" % kv for kv in sorted(hist.items(), key=lambda kv: -kv[1])[:8]))
        for h in hits[:25]:
            print("   [%d] ctl=%s name=%r aid=%r cls=%r off=%s focus=%s "
                  "invoke=%s legacy=%s rect=%s"
                  % (h["i"], h["ctl"], h["name"][:34], h["aid"], h["cls"],
                     h["offscreen"], h["focus"], h["invoke"], h["legacy"],
                     h["rect"]))
        if named == 0:
            print("   VERDICT: WALK-RAN-BUT-NOTHING-NAMED — tree not populated, "
                  "absence is NOT established")
        elif not hits:
            print("   VERDICT: NO-HIT among %d named controls — the app does "
                  "not expose a 新任务 control by name/id" % named)
        else:
            print("   VERDICT: %d control(s) matched; Invoke-capable=%d"
                  % (len(hits), sum(1 for h in hits if h["invoke"])))


if __name__ == "__main__":
    main()
