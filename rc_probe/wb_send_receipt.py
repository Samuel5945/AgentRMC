"""WorkBuddy 系「按会话发送 + 回执」能力验证（会真发两条无害探针消息）。

流程：新建任务 -> 发探针1 -> 从 projects/*.jsonl 拿回执与新 session id ->
切回原会话 -> 再按标题打开探针会话（发送前用聊天区标题证明归属）-> 发探针2 ->
回执必须落在探针会话的 jsonl、且原会话 jsonl 无探针2 -> 切回原会话恢复视图。

探针文案要求宿主只回「收到」、不调工具。剪贴板用前保存、用后还原。
用法: python rc_probe/wb_send_receipt.py [WorkBuddy.exe|WorkBuddyAI.exe] <原会话标题>
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
                                             IUIAutomationInvokePattern,
                                             IUIAutomationValuePattern,
                                             IUIAutomationScrollPattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32  # noqa: E402

exe = sys.argv[1] if len(sys.argv) > 1 else "WorkBuddy.exe"
orig_title = sys.argv[2] if len(sys.argv) > 2 else "设计一套新的OS11景深原案"
reuse_sid = sys.argv[3] if len(sys.argv) > 3 else None
home = ".workbuddy-ai" if "AI" in exe else ".workbuddy"
base = os.path.join("C:/Users/PC", home, "projects")

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)

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
        if c.value == "Chrome_WidgetWin_1" and b.value in ("WorkBuddy", "WorkBuddy AI"):
            _cands.append(h)
    return True


user32.EnumWindows(_CB(_enum), 0)
if not _cands:
    print("no main window")
    sys.exit(1)
hwnd = _cands[0]
if user32.IsIconic(hwnd):
    user32.ShowWindow(hwnd, 9)
elif not user32.IsWindowVisible(hwnd):
    user32.ShowWindow(hwnd, 5)
user32.SetForegroundWindow(hwnd)
time.sleep(1.0)
root = ui.ElementFromHandle(hwnd)
walker = ui.ControlViewWalker
_wr = wt.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(_wr))
WIN = (_wr.left, _wr.top)


def rect_of(el):
    r = el.GetCurrentPropertyValue(30001)
    t = tuple(int(v) for v in (tuple(r) if isinstance(r, (tuple, list))
                               else (0, 0, 0, 0)))
    return (t[0] - WIN[0], t[1] - WIN[1], t[2], t[3])


def collect():
    rows, headers, edits, tables, allbtn = [], [], [], [], []
    seen = 0

    def walk(el, d):
        nonlocal seen
        if d > 10 or seen > 5000:
            return
        seen += 1
        ct = el.GetCurrentPropertyValue(30003)
        n = el.GetCurrentPropertyValue(30005) or ""
        r = rect_of(el)
        if ct == 50000 and n:
            allbtn.append((n, el))
            if 0 <= r[0] < 600 and r[1] > 200 and r[2] > 300:
                rows.append((n, el))
        if ct == 50020 and 600 < r[0] < 1200 and 100 < r[1] < 300 and r[2] > 200:
            headers.append(n)
        if ct == 50004 and r[2] > 200 and r[3] > 40:
            edits.append(el)
        if ct == 50019 and n == "新建任务":
            tables.append(el)
        child = walker.GetFirstChildElement(el)
        while child and seen <= 5000:
            walk(child, d + 1)
            child = walker.GetNextSiblingElement(child)

    walk(root, 0)
    return rows, headers, edits, tables, allbtn


def scroll_sidebar_top():
    seen = 0

    def walk(el, d):
        nonlocal seen
        if d > 10 or seen > 5000:
            return
        seen += 1
        r = rect_of(el)
        if r[2] > 300 and r[3] > 400 and r[0] < 600:
            try:
                sp = el.GetCurrentPattern(10004).QueryInterface(
                    IUIAutomationScrollPattern)
                sp.SetScrollPercent(0, 0)
                return
            except Exception:
                pass
        child = walker.GetFirstChildElement(el)
        while child and seen <= 5000:
            walk(child, d + 1)
            child = walker.GetNextSiblingElement(child)

    walk(root, 0)
    time.sleep(0.4)
    # Chromium 的侧栏是虚拟化列表：滚出视口的行整棵子树都不存在，ScrollPattern
    # 也不一定有。滚轮物理滚到顶最可靠；指针用前保存用后还原。
    pt = wt.POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    user32.SetCursorPos(WIN[0] + 280, WIN[1] + 500)
    time.sleep(0.2)
    for _ in range(6):
        user32.mouse_event(0x0800, 0, 0, 120, 0)
        time.sleep(0.12)
    user32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.8)


def expand_group(prefix):
    """侧栏分组（任务/空间…）切走后会折叠，折叠时组内行整棵不存在。
    点组标题 Text 的中心展开；指针用前保存用后还原。"""
    found = []
    seen = 0

    def walk(el, d):
        nonlocal seen
        if d > 10 or seen > 5000:
            return
        seen += 1
        if el.GetCurrentPropertyValue(30003) == 50020:
            n = el.GetCurrentPropertyValue(30005) or ""
            r = rect_of(el)
            if n.startswith(prefix) and 0 <= r[0] < 600 and r[2] > 40:
                found.append(r)
        child = walker.GetFirstChildElement(el)
        while child and seen <= 5000:
            walk(child, d + 1)
            child = walker.GetNextSiblingElement(child)

    walk(root, 0)
    if not found:
        print("group header not found:", prefix)
        return False
    r = found[0]
    pt = wt.POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    user32.SetCursorPos(WIN[0] + r[0] + r[2] // 2, WIN[1] + r[1] + r[3] // 2)
    time.sleep(0.15)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    user32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.8)
    return True


def clean_row_name(n):
    import re
    return re.sub(r"\s+(刚刚|\d+\s*(分钟|小时|天|月)前|更多)$", "", n).strip()


def click_row(rows, title, allbtn=()):
    for n, el in list(rows) + list(allbtn):
        nc = clean_row_name(n)
        # 侧栏行标题可能是截断的首句或旧 ai-title：用前 12 字双向匹配。
        if nc == title or nc.startswith(title[:12]) or title.startswith(nc[:12]):
            try:
                el.GetCurrentPattern(10000).QueryInterface(
                    IUIAutomationInvokePattern).Invoke()
                return n
            except Exception:
                el.SetFocus()
                user32.keybd_event(0x0D, 0, 0, 0)
                user32.keybd_event(0x0D, 0, 2, 0)
                return n
    return None


def clipboard_save():
    if not user32.OpenClipboard(None):
        return None
    try:
        h = user32.GetClipboardData(13)  # CF_UNICODETEXT
        if not h:
            return None
        p = k32.GlobalLock(h)
        s = ctypes.wstring_at(p) if p else None
        k32.GlobalUnlock(h)
        return s
    finally:
        user32.CloseClipboard()


def clipboard_set(text):
    user32.OpenClipboard(None)
    user32.EmptyClipboard()
    data = ctypes.create_unicode_buffer(text)
    h = k32.GlobalAlloc(0x0042, ctypes.sizeof(data))
    p = k32.GlobalLock(h)
    ctypes.memmove(p, data, ctypes.sizeof(data))
    k32.GlobalUnlock(h)
    user32.SetClipboardData(13, h)
    user32.CloseClipboard()


def paste_into(edit, text):
    edit.SetFocus()
    time.sleep(0.3)
    clipboard_set(text)
    user32.keybd_event(0x11, 0, 0, 0)
    user32.keybd_event(0x56, 0, 0, 0)
    user32.keybd_event(0x56, 0, 2, 0)
    user32.keybd_event(0x11, 0, 2, 0)
    time.sleep(0.5)
    try:
        vp = edit.GetCurrentPattern(10002).QueryInterface(IUIAutomationValuePattern)
        return vp.CurrentValue or ""
    except Exception:
        return "<no readback>"


def press_enter():
    user32.keybd_event(0x0D, 0, 0, 0)
    user32.keybd_event(0x0D, 0, 2, 0)


def wait_receipt(marker, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for p in glob.glob(base + "/*/*.jsonl"):
            if p.endswith(".file-rollback.ndjson"):
                continue
            try:
                with open(p, encoding="utf-8") as f:
                    for line in f:
                        if marker in line:
                            d = json.loads(line)
                            return p, d.get("sessionId"), d.get("type")
            except Exception:
                continue
        time.sleep(1.0)
    return None, None, None


saved_clip = clipboard_save()
stamp = int(time.time())
try:
    if reuse_sid:
        hits = glob.glob(base + "/*/" + reuse_sid + ".jsonl")
        if not hits:
            print("reuse session not found:", reuse_sid)
            sys.exit(1)
        p1, s1 = hits[0], reuse_sid
        probe_title = None
        for line in open(p1, encoding="utf-8"):
            if '"ai-title"' in line:
                d = json.loads(line)
                if d.get("type") == "ai-title":
                    probe_title = d.get("aiTitle")
        print("reuse session:", s1, repr(probe_title))
        rows, headers, _, _, allbtn = collect()
        print("start header:", headers[:1])
    else:
        rows, headers, edits, tables, allbtn = collect()
        print("start header:", headers[:1], "edits:", len(edits), "newtask:", len(tables))
        if not tables or not edits:
            print("LANDING CONTROLS NOT FOUND")
            sys.exit(1)
        try:
            tables[0].GetCurrentPattern(10000).QueryInterface(
                IUIAutomationInvokePattern).Invoke()
        except Exception:
            tables[0].SetFocus()
            press_enter()
        time.sleep(1.5)
        rows, headers, edits, _, allbtn = collect()
        if not edits:
            print("NO COMPOSER AFTER NEW TASK")
            sys.exit(1)
        m1 = f"AGENTRMC-PROBE-1-{stamp}"
        readback = paste_into(edits[0], f"链路探针 {m1}：请只回复「收到」，不要调用任何工具、不要读写文件。")
        print("composer readback:", repr(readback[:60]))
        press_enter()
        p1, s1, t1 = wait_receipt(m1)
        print("RECEIPT1:", p1, s1, t1)
        if not s1:
            print("NO RECEIPT FOR PROBE1 — stop")
            sys.exit(1)
        time.sleep(2.0)
        # ai-title 是异步生成的：等它落盘，侧栏行与聊天区标题才同名。
        probe_title = None
        t0 = time.time()
        while time.time() - t0 < 20 and not probe_title:
            for line in open(p1, encoding="utf-8"):
                if '"ai-title"' in line:
                    d = json.loads(line)
                    if d.get("type") == "ai-title":
                        probe_title = d.get("aiTitle")
            if not probe_title:
                time.sleep(1.0)
        rows, headers, _, _, allbtn = collect()
        if not probe_title:
            probe_title = headers[0] if headers else None
        print("probe session title:", repr(probe_title))
    # 切回原会话，再按标题定向打开探针会话：发送前证明输入框归属
    back = click_row(rows, orig_title, allbtn)
    time.sleep(1.5)
    _, h_orig, _, _, allbtn = collect()
    print("switched back:", back, h_orig[:1])
    scroll_sidebar_top()
    expand_group("任务")
    rows, _, _, _, allbtn = collect()
    opened = click_row(rows, probe_title or m1, allbtn)
    time.sleep(1.5)
    _, h_now, edits, _, allbtn = collect()
    proof = bool(h_now) and bool(probe_title) and h_now[0].startswith(probe_title[:12])
    print("reopen clicked:", opened, "PRESEND PROOF header:", h_now[:1], "ok=", proof)
    if not proof or not edits:
        print("PROOF FAILED — not sending probe2")
        sys.exit(1)
    m2 = f"AGENTRMC-PROBE-2-{stamp}"
    paste_into(edits[0], f"链路探针 {m2}：请只回复「收到」，不要调用任何工具、不要读写文件。")
    press_enter()
    p2, s2, t2 = wait_receipt(m2)
    print("RECEIPT2:", p2, s2, t2)
    print("TARGETED_SEND:", "OK" if s2 == s1 else "WRONG SESSION")
    if p2 and p1:
        print("same file:", os.path.normpath(p1) == os.path.normpath(p2))
    # 别的会话不得收到探针2
    leak = False
    for p in glob.glob(base + "/*/*.jsonl"):
        if p.endswith(".file-rollback.ndjson") or (p2 and os.path.normpath(p) == os.path.normpath(p2)):
            continue
        try:
            if m2 in open(p, encoding="utf-8").read():
                leak = True
                print("LEAK INTO:", p)
        except Exception:
            pass
    print("NO_LEAK:", not leak)
finally:
    try:
        rows, _, _, _, allbtn = collect()
        print("restore:", click_row(rows, orig_title, allbtn))
    except Exception as e:
        print("restore failed:", e)
    if saved_clip is not None:
        clipboard_set(saved_clip)
        print("clipboard restored")
