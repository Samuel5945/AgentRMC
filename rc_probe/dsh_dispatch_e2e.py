"""DSH 定向派发端到端：外壳卡片派发行 → native 队列 → 插件 followup → 回执落账。

真实入口全链（2026-11-07，第 3 批 DSH 线验收）：
  1. 外壳选中牺牲会话的待办卡（按卡上探针标记文本定位）；
  2. 派发行（dpText）粘贴带标记指令 → 「派回会话」；
  3. native SessionDispatch 入队 → DSH 插件 1.5s 轮询取件 → agents.get() →
     followup 注入 → ack ok → native 回执落账（卡转 progress）；
  4. 轮次真跑 → stop 通知 → 卡转 review 带回复正文；
  5. zstd 全库扫描：标记只落在牺牲会话。

前置：新版 AgentRMC 与 DeepSeek Harness 都在跑；待办库里存在含 <旧标记> 的卡。
用法: python rc_probe/dsh_dispatch_e2e.py [卡片文本片段，默认探针卡标记]
"""
import ctypes
import ctypes.wintypes as wt
import glob
import json
import os
import sys
import time
import urllib.request

ctypes.windll.shcore.SetProcessDpiAwareness(2)
user32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
k32.GlobalAlloc.restype = wt.HGLOBAL
k32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
k32.GlobalLock.restype = ctypes.c_void_p
k32.GlobalLock.argtypes = [wt.HGLOBAL]
k32.GlobalUnlock.argtypes = [wt.HGLOBAL]
user32.SetClipboardData.restype = wt.HANDLE
user32.SetClipboardData.argtypes = [wt.UINT, wt.HANDLE]

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,  # noqa: E402
                                             IUIAutomationInvokePattern)

CARD_HINT = sys.argv[1] if len(sys.argv) > 1 else "收到 AgentRMC-P1-9K3T"
TARGET_SESSION = "session-9c08421f-080c-4a79-9325-c23464226a62"
MARKER = f"AGENTRMC-D2-{int(time.time())}"
TEXT = f"定向派发二段 {MARKER}：只回复「收到」，不要调用任何工具。"

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)


def http_json(path):
    req = urllib.request.Request("http://127.0.0.1:45915" + path)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=3) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


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
    time.sleep(0.4)


def ctrl_v():
    user32.keybd_event(0x11, 0, 0, 0)
    user32.keybd_event(0x56, 0, 0, 0)
    user32.keybd_event(0x56, 0, 2, 0)
    user32.keybd_event(0x11, 0, 2, 0)
    time.sleep(0.4)


def center(el):
    r = el.CurrentBoundingRectangle
    return (r.left + (r.right - r.left) // 2, r.top + (r.bottom - r.top) // 2)


# —— 找 AgentRMC 主窗并拉前台（WebView2 的 a11y 树要前台被查询才建）——
_CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
tops = []


def enum_(h, _):
    if user32.IsWindowVisible(h):
        n = user32.GetWindowTextLengthW(h)
        b = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, b, n + 1)
        if b.value == "AgentRMC Native":
            tops.append(h)
    return True


user32.EnumWindows(_CB(enum_), 0)
if not tops:
    print("AgentRMC window not found")
    sys.exit(1)
hwnd = tops[0]
if user32.IsIconic(hwnd):
    user32.ShowWindow(hwnd, 9)
user32.SetForegroundWindow(hwnd)
time.sleep(1.2)

# WebView2 的 a11y 树挂在 Chrome_WidgetWin_1 子窗上，从顶层窗走树是空的
# （WorkBuddy 同款坑）；要先枚举子窗、对 Chrome 子窗 ElementFromHandle。
_children = []


@ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def _child_enum(h, _):
    cls = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, cls, 64)
    _children.append((h, cls.value))
    return True


ctypes.windll.user32.EnumChildWindows(hwnd, _child_enum, 0)
web = next((h for h, c in _children if c == "Chrome_WidgetWin_1"), None)
if web is None:
    print("WebView2 Chrome child not found")
    sys.exit(1)
root = ui.ElementFromHandle(web)
walker = ui.ControlViewWalker


def find_by_name_substr(substr, name_prop=None, depth=0, limit=4000):
    """限深全树扫，返回 Name 含 substr 的元素列表（Name 可能没建好，容忍空）。"""
    out = []
    stack = [(root, 0)]
    seen = 0
    while stack and seen < limit:
        el, d = stack.pop()
        seen += 1
        try:
            nm = el.CurrentName or ""
        except Exception:
            nm = ""
        if nm and substr in nm and (name_prop is None or name_prop in nm):
            out.append(el)
        if d >= depth_max:
            continue
        child = walker.GetFirstChildElement(el)
        while child:
            stack.append((child, d + 1))
            child = walker.GetNextSiblingElement(child)
    return out


depth_max = 22

# 1) 选中目标卡：卡上有探针轮次的回复文本，全库唯一。
rows = find_by_name_substr(CARD_HINT)
if not rows:
    print(f"card row not found by name hint: {CARD_HINT}")
    sys.exit(1)
el = rows[0]
p = center(el)
click(*p)
time.sleep(0.8)
try:
    print(f"card selected at {p}: {el.CurrentName[:60]}")
except Exception:
    print(f"card clicked at {p} (element went stale after re-render)")

# 2) 派发行输入框（placeholder 开头「派回这条会话」，Edit 控件 0xC354）。
edits = [e for e in find_by_name_substr("派回这条会话")
         if e.CurrentControlType == 0xC354]  # UIA_EditControlTypeId
if not edits:
    print("dispatch input not found (dpWrap hidden? card not selected?)")
    sys.exit(1)
dp = edits[0]
dp_rect = center(dp)
saved = clip_save()
try:
    click(*dp_rect)
    clip_set(TEXT)
    ctrl_v()
    # 3) 「派回会话」按钮：优先 UIA Invoke，找不到就点坐标。
    btns = [b for b in find_by_name_substr("派回会话")
            if b.CurrentControlType == 0xC350]  # UIA_ButtonControlTypeId
    if btns:
        try:
            btns[0].GetCurrentPatternAs(IUIAutomationInvokePattern._iid_).Invoke()
            print("dispatch sent via UIA Invoke")
        except Exception:
            click(*center(btns[0]))
            print("dispatch sent via click")
    else:
        print("send button not found")
        sys.exit(1)
finally:
    if saved is not None:
        clip_set(saved)

# 4) 回执落账：卡应转 progress 且 message 含标记（native ack-ok → receipt）。
print("marker:", MARKER)
t0 = time.time()
progress = None
while time.time() - t0 < 20 and progress is None:
    try:
        items = http_json("/work-items").get("items", [])
        for it in items:
            if it.get("session_id") == TARGET_SESSION and \
                    MARKER in str(it.get("message", "")):
                progress = it
                break
    except Exception:
        pass
    time.sleep(1.0)
print("RECEIPT_CARD:",
      json.dumps({k: progress.get(k) for k in ("stage", "message", "updated_at")},
                 ensure_ascii=False) if progress else None)

# 5) zstd 全库零泄漏 + 目标命中（插件真注入了）。
time.sleep(4)
hits = []
import zstandard
dctx = zstandard.ZstdDecompressor()
for f in glob.glob(os.path.expanduser("~/.dsh/sessions/*/session-*/*.zstd")):
    try:
        with open(f, "rb") as fh, dctx.stream_reader(fh, read_across_frames=True) as r:
            if MARKER in r.read().decode("utf-8", "replace"):
                hits.append(f)
    except Exception as e:
        print("scan err", f, e)
print("ZSTD_TARGET_HIT:", any(TARGET_SESSION in f for f in hits))
print("ZSTD_LEAK:", [os.path.basename(os.path.dirname(f)) for f in hits
                    if TARGET_SESSION not in f])

# 6) 轮次跑完后卡转 review 带回复正文（egg-alpha 可能要 1 分钟）。
t1 = time.time()
review = None
while time.time() - t1 < 150 and review is None:
    try:
        items = http_json("/work-items").get("items", [])
        for it in items:
            if it.get("session_id") == TARGET_SESSION and \
                    it.get("stage") == "review" and \
                    MARKER not in str(it.get("message", "")):
                review = it
                break
    except Exception:
        pass
    time.sleep(2.0)
print("REVIEW_CARD:",
      json.dumps({k: review.get(k) for k in ("message", "updated_at")},
                 ensure_ascii=False) if review else None)
print("E2E:", "PASS" if progress and hits else "FAIL")
