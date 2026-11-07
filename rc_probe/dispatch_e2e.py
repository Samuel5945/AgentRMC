"""走 AgentRMC 真实派发入口（外壳左栏语音形态的打字框+发送）做端到端验收。

前置：用户已在 Qoder CN 桌面新建并前台停在一个可牺牲会话。
护栏：发送前断言前台进程是 Qoder CN；发送后断言标记落在目标会话 jsonl、
且本会话（1875baef）jsonl 不含标记；剪贴板用前存用后还。
用法: python rc_probe/dispatch_e2e.py <目标session id> [本会话id]
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
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, ctypes.c_wchar_p,
                                          ctypes.POINTER(wt.DWORD)]

target = sys.argv[1]
own = sys.argv[2] if len(sys.argv) > 2 else "1875baef-e060-8dea-a866-4203523b7d8e"
base = "C:/Users/PC/.qoder-cn/projects"
marker = f"AGENTRMC-DISPATCH-{int(time.time())}"
text = f"链路探针 {marker}：只回复「收到」，不要调用任何工具。"


def fg_exe():
    h = user32.GetForegroundWindow()
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    hp = k32.OpenProcess(0x0410, False, pid.value)
    if not hp:
        return ""
    buf = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(1024)
    ok = k32.QueryFullProcessImageNameW(hp, 0, buf, ctypes.byref(size))
    k32.CloseHandle(hp)
    return os.path.basename(buf.value) if ok else ""


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


def ctrl_v():
    user32.keybd_event(0x11, 0, 0, 0)
    user32.keybd_event(0x56, 0, 0, 0)
    user32.keybd_event(0x56, 0, 2, 0)
    user32.keybd_event(0x11, 0, 2, 0)
    time.sleep(0.4)


def contains(sid, m):
    for p in glob.glob(base + "/*/" + sid + ".jsonl"):
        try:
            if m in open(p, encoding="utf-8").read():
                return p
        except OSError:
            pass
    return None


# AgentRMC 主窗原点（点击坐标是窗内物理像素）
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
if not tops:
    print("AgentRMC window not found")
    sys.exit(1)
h, ox, oy = tops[0]
# 先把 Qoder CN 拉到前台（不改变它内部当前会话＝用户停好的牺牲会话），
# 再做前台护栏；刚才点 AgentRMC 截图会把前台抢走。
qtops = []


def qcb(h, _):
    n = user32.GetWindowTextLengthW(h)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(h, b, n + 1)
    c = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, c, 64)
    if c.value == "Chrome_WidgetWin_1" and b.value in ("Qoder CN", "Qoder"):
        qtops.append(h)
    return True


user32.EnumWindows(CB(qcb), 0)
if qtops:
    user32.SetForegroundWindow(qtops[0])
    time.sleep(0.6)
fg0 = fg_exe()
print("foreground before touching AgentRMC:", fg0)
if fg0.lower() not in ("qoder cn.exe", "qoder.exe"):
    print("FOREGROUND GUARD FAILED — spare session not in front, abort")
    sys.exit(1)
user32.SetForegroundWindow(h)
time.sleep(0.4)
saved = clip_save()
try:
    click(ox + 260, oy + 1243)   # 打字框
    clip_set(text)
    ctrl_v()
    from PIL import ImageGrab
    r = wt.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                   all_screens=True).save("rc_probe/dispatch_pasted.png")
    print("pasted screenshot: rc_probe/dispatch_pasted.png")
    click(ox + 515, oy + 1243)   # 发送
    t0 = time.time()
    hit = None
    while time.time() - t0 < 60 and not hit:
        hit = contains(target, marker)
        time.sleep(1.0)
    print("RECEIPT_IN_TARGET:", hit)
    print("LEAK_IN_OWN_SESSION:", bool(contains(own, marker)))
    print("marker:", marker)
finally:
    if saved is not None:
        clip_set(saved)
        print("clipboard restored")
