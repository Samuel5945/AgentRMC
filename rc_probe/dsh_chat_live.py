"""DSH 聊天页实机验收：点开一张 DSH 待办卡，确认会话记录真的读出来。
判据（读 a11y 文本，不靠物理坐标）：
  1. 聊天视图打开（「‹ 待办」返回按钮在）；
  2. chatMeta 含「DeepSeek Harness」且「共 N 条」N>0；
  3. Agent 气泡数 >0（DSH 现在支持记录，不再是空提示）；
  4. 上翻能加载更旧页（若 total>50）。
前置：装了含 DSH 读取器的新版 AgentRMC，待办里至少一张 DSH 卡。
用法：python rc_probe/dsh_chat_live.py
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
kernel32 = ctypes.windll.kernel32

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


def force_foreground(h):
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    if user32.IsIconic(h):
        user32.ShowWindow(h, 9)
    user32.ShowWindow(h, 3)  # SW_MAXIMIZE
    fg = user32.GetForegroundWindow()
    fg_tid = user32.GetWindowThreadProcessId(fg, None)
    my_tid = kernel32.GetCurrentThreadId()
    attached = bool(user32.AttachThreadInput(my_tid, fg_tid, True))
    user32.SetForegroundWindow(h)
    user32.BringWindowToTop(h)
    if attached:
        user32.AttachThreadInput(my_tid, fg_tid, False)
    time.sleep(0.8)


force_foreground(hwnd)
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


def scan_ordered(substr, limit=20000):
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
        if d >= 30:
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


def chat_meta():
    for el in scan_ordered("共 "):
        nm = el.CurrentName or ""
        if "条" in nm and "DeepSeek" in nm:
            return nm
    # 退而求其次：任何 chatMeta
    for el in scan_ordered("共 "):
        nm = el.CurrentName or ""
        if "条" in nm:
            return nm
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

# 2) 逐张点阶段标签，直到打开的聊天页 chatMeta 是 DSH。
candidates = [e for e in scan_ordered("待验收") + scan_ordered("等你处理") +
              scan_ordered("进行中")
              if e.CurrentControlType == 50020 and
              (e.CurrentName or "").strip() in ("待验收", "等你处理", "进行中")]
check(bool(candidates), "found work card stage labels")
opened = False
meta = ""
for card in candidates[:10]:
    click(*center(card))
    time.sleep(1.5)
    if not scan_ordered("‹ 待办"):
        continue
    meta = chat_meta()
    if "DeepSeek" in meta:
        opened = True
        break
    back0 = scan_ordered("‹ 待办")
    if back0:
        invoke(back0[0])
        time.sleep(0.8)
check(opened, "opened a DSH chat view", meta[:80])
if not opened:
    sys.exit(1)

# 3) 共 N 条 N>0 + 气泡渲染。
m = re.search(r"共 (\d+) 条", meta)
total = int(m.group(1)) if m else 0
check(total > 0, "DSH transcript total > 0", f"total={total}")
time.sleep(1.0)
bubbles = bubble_count()
check(bubbles > 0, "DSH bubbles rendered (no longer empty hint)", f"count={bubbles}")

# 4) 上翻加载更旧页（total>50 才有）。
if total > 50:
    before = bubble_count()
    wr = wt.RECT()
    user32.GetWindowRect(web, ctypes.byref(wr))
    user32.SetCursorPos((wr.left + wr.right) // 2, (wr.top + wr.bottom) // 2)
    time.sleep(0.2)
    for _ in range(40):
        user32.mouse_event(0x0800, 0, 0, 600, 0)
        time.sleep(0.05)
    time.sleep(2.5)
    after = bubble_count()
    check(after > before, "scroll up loads older DSH page",
          f"before={before} after={after}")
else:
    check(True, "DSH session <=50 msgs, no older page needed", f"total={total}")

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
print("E2E PASS dsh-chat-live")