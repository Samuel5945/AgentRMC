"""第 3 批路由端到端：在外壳待办页选中会话卡后，用「派回会话」输入行派发。

目标会话不在前台（验证定向而非续当前会话）。回执断言：标记落目标 jsonl、
待办库出现 dispatch 来源/进行中、本会话零泄漏。
用法: python rc_probe/route_send.py <目标session id> [本会话id]
"""
import ctypes
import ctypes.wintypes as wt
import glob
import json
import os
import sys
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)
user32 = ctypes.windll.user32
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

target = sys.argv[1]
own = sys.argv[2] if len(sys.argv) > 2 else "1875baef-e060-8dea-a866-4203523b7d8e"
base = "C:/Users/PC/.qoder-cn/projects"
store = "C:/Users/PC/AppData/Roaming/LIG/AgentRMCNative/work-items.json"
marker = f"AGENTRMC-ROUTE-{int(time.time())}"
text = f"定向派发探针 {marker}：只回复「收到」，不要调用任何工具。"


def clip_save():
    if not user32.OpenClipboard(None):
        return None
    try:
        h = user32.GetClipboardData(13)
        if not h:
            return None
        p = k32.GlobalLock(h)
        s = ctypes.wstring_at(p) if p else None
        k32.GlobalUnlock(h)
        return s
    finally:
        user32.CloseClipboard()


def clip_set(t):
    user32.OpenClipboard(None)
    user32.EmptyClipboard()
    data = ctypes.create_unicode_buffer(t)
    h = k32.GlobalAlloc(0x0042, ctypes.sizeof(data))
    p = k32.GlobalLock(h)
    ctypes.memmove(p, data, ctypes.sizeof(data))
    k32.GlobalUnlock(h)
    user32.SetClipboardData(13, h)
    user32.CloseClipboard()


def click(x, y):
    user32.SetCursorPos(x, y)
    time.sleep(0.15)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.3)


def contains(sid, m):
    for p in glob.glob(base + "/*/" + sid + ".jsonl"):
        try:
            if m in open(p, encoding="utf-8").read():
                return p
        except OSError:
            pass
    return None


tops = []
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def cb(h, _):
    if user32.IsWindowVisible(h):
        n = user32.GetWindowTextLengthW(h)
        b = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, b, n + 1)
        if b.value == "AgentRMC Native":
            r = wt.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            tops.append((h, r.left, r.top))
    return True


user32.EnumWindows(CB(cb), 0)
h, ox, oy = tops[0]
user32.SetForegroundWindow(h)
time.sleep(0.4)
saved = clip_save()
try:
    ix = int(sys.argv[3]) if len(sys.argv) > 3 else 1272
    iy = int(sys.argv[4]) if len(sys.argv) > 4 else 1357
    bx = int(sys.argv[5]) if len(sys.argv) > 5 else 1972
    by = int(sys.argv[6]) if len(sys.argv) > 6 else 1357
    click(ox + ix, oy + iy)   # 派回会话输入框
    clip_set(text)
    user32.keybd_event(0x11, 0, 0, 0)
    user32.keybd_event(0x56, 0, 0, 0)
    user32.keybd_event(0x56, 0, 2, 0)
    user32.keybd_event(0x11, 0, 2, 0)
    time.sleep(0.4)
    click(ox + bx, oy + by)   # 派回会话按钮
    t0 = time.time()
    hit = None
    while time.time() - t0 < 90 and not hit:
        hit = contains(target, marker)
        time.sleep(1.0)
    print("RECEIPT_IN_TARGET:", hit)
    print("LEAK_IN_OWN_SESSION:", bool(contains(own, marker)))
    d = json.load(open(store, encoding="utf-8"))
    items = d.get("items", d) if isinstance(d, dict) else d
    for it in items:
        if it.get("session_id") == target and not it.get("done"):
            print("ITEM:", it.get("source"), it.get("stage"),
                  (it.get("message") or "")[:50])
    print("marker:", marker)
finally:
    if saved is not None:
        clip_set(saved)
        print("clipboard restored")
