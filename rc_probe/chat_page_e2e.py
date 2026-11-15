"""聊天页端到端探针（电脑外壳，2026-11-13 晚改版）。

判据（读页面 dbg 读数，不靠物理坐标——DPI 下不可靠）：
  1. 单击待办卡 → 聊天视图打开（返回按钮在）；
  2. 打开即最新：scrollTop == scrollHeight-clientHeight 且最后气泡贴列表底；
  3. 最大化窗口让内容填不满 → maybeAutoLoad 自动触发 chatOlder→pushChat→
     旧页 prepend，气泡数增长（桌面旧页加载全链）；
  4. 返回待办列表。
前置：新版 AgentRMC 在跑、待办列表至少一条卡（dbg 版外壳）。
用法: python rc_probe/chat_page_e2e.py
"""
import ctypes
import ctypes.wintypes as wt
import io
import re
import sys
import time

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
ctypes.windll.shcore.SetProcessDpiAwareness(2)
user32 = ctypes.windll.user32

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,  # noqa: E402
                                             IUIAutomationInvokePattern)

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
walker = ui.ControlViewWalker
FAILS = []


def check(cond, what, extra=""):
    print(("PASS " if cond else "FAIL ") + what + (f" | {extra}" if extra else ""))
    if not cond:
        FAILS.append(what)


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
    print("AgentRMC main window not visible")
    sys.exit(1)
hwnd = tops[0]
user32.ShowWindow(hwnd, 3)  # SW_MAXIMIZE：高窗口触发自动续载
user32.SetForegroundWindow(hwnd)
time.sleep(1.2)

_children = []


@ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def _child_enum(h, _):
    cls = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, cls, 64)
    _children.append((h, cls.value))
    return True


user32.EnumChildWindows(hwnd, _child_enum, 0)
web = next((h for h, c in _children if c == "Chrome_WidgetWin_1"), None)
if web is None:
    print("WebView2 Chrome child not found")
    sys.exit(1)
root = ui.ElementFromHandle(web)


def scan_ordered(substr, limit=15000):
    out = []
    seen = 0

    def walk(el, d):
        nonlocal seen
        if seen >= limit:
            return
        seen += 1
        try:
            nm = el.CurrentName or ""
        except Exception:
            nm = ""
        if nm and substr in nm:
            out.append(el)
        if d >= 28:
            return
        try:
            child = walker.GetFirstChildElement(el)
        except Exception:
            return
        while child:
            walk(child, d + 1)
            try:
                child = walker.GetNextSiblingElement(child)
            except Exception:
                break

    walk(root, 0)
    return out


def center(el):
    r = el.CurrentBoundingRectangle
    return ((r.left + r.right) // 2, (r.top + r.bottom) // 2)


def click(x, y):
    user32.SetCursorPos(x, y)
    time.sleep(0.15)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.7)


def invoke(el):
    try:
        el.GetCurrentPattern(10000).QueryInterface(IUIAutomationInvokePattern).Invoke()
        return True
    except Exception:
        try:
            click(*center(el))
            return True
        except Exception:
            return False


def dbg_meta():
    for el in scan_ordered("[dbg"):
        try:
            return el.CurrentName or ""
        except Exception:
            pass
    return ""


def bubble_count():
    return len([e for e in scan_ordered("Agent")
                if e.CurrentControlType == 50020 and
                (e.CurrentName or "").strip() == "Agent"])


# 1) 待办页。
tabs = [e for e in scan_ordered("待我处理") if e.CurrentControlType == 0xC350]
if tabs:
    invoke(tabs[0])
    time.sleep(0.8)

# 2) 点阶段标签进聊天，候选重试直到命中支持记录的宿主（DSH 无气泡）。
candidates = [e for e in scan_ordered("待验收") + scan_ordered("等你处理") +
              scan_ordered("进行中")
              if e.CurrentControlType == 50020 and
              (e.CurrentName or "").strip() in ("待验收", "等你处理", "进行中")]
check(bool(candidates), "found a work card stage label")
opened = False
for card in candidates[:6]:
    click(*center(card))
    time.sleep(1.5)
    if not scan_ordered("‹ 待办"):
        continue
    n = len([e for e in scan_ordered("Agent")
             if e.CurrentControlType == 50020 and
             (e.CurrentName or "").strip() == "Agent"])
    if n > 0:
        opened = True
        break
    back0 = scan_ordered("‹ 待办")
    if back0:
        invoke(back0[0])
        time.sleep(0.8)
check(opened, "chat view opened with transcript (supported host)")
if not opened:
    sys.exit(1)

# 3) 打开即最新：读 dbg 数值判定（几何坐标受 DPI/窗口高度干扰）。
time.sleep(1.2)
meta = dbg_meta()
m = re.search(r"\[dbg (\d+)/(\d+)/(\d+)", meta)
if m:
    st, sh, ch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    check(abs(st - (sh - ch)) <= 2 and sh > ch,
          "open pins to latest (scrollTop == max)", f"st={st} sh={sh} ch={ch}")
else:
    check(False, "dbg readable", meta[:80])
bubbles = [e for e in scan_ordered("Agent")
           if e.CurrentControlType == 50020 and (e.CurrentName or "").strip() == "Agent"]
check(bool(bubbles), "bubbles rendered", f"count={len(bubbles)}")

# 4) 滚轮上翻触发旧页加载（列表已贴底，上翻有效；旧版列表卡顶所以翻不动）。
before = bubble_count()
wr = wt.RECT()
user32.GetWindowRect(web, ctypes.byref(wr))
cx = (wr.left + wr.right) // 2
cy = (wr.top + wr.bottom) // 2
user32.SetCursorPos(cx, cy)
time.sleep(0.2)
for _ in range(40):
    user32.mouse_event(0x0800, 0, 0, 600, 0)  # WHEEL 向上，每次 5 格
    time.sleep(0.05)
time.sleep(2.5)
after = bubble_count()
meta2 = dbg_meta()
m2 = re.search(r"共 (\d+) 条", meta2)
total = int(m2.group(1)) if m2 else 0
if total > 50:
    check(after > before, "scroll up loads older page (chatOlder→pushChat)",
          f"before={before} after={after} total={total}")
else:
    check(True, "session <=50 msgs, no older page needed", f"total={total}")

# 5) 返回。
back = scan_ordered("‹ 待办")
if back:
    invoke(back[0])
    time.sleep(0.8)
check(bool(scan_ordered("待我处理")), "back to work list")

print()
if FAILS:
    print(f"E2E FAIL: {FAILS}")
    sys.exit(1)
print("E2E PASS chat-page")