"""WorkBuddy 系「打开指定会话」能力验证：点侧栏会话行（按标题匹配），
读聊天区标题 Text 证明当前会话已切换；可选再点回原会话恢复视图。

只导航不发送。用法:
  python rc_probe/wb_open_session.py WorkBuddy.exe <目标标题> [恢复标题]
"""
import pathlib
import sys
import time

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,  # noqa: E402
                                             IUIAutomationInvokePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window  # noqa: E402

exe, target = sys.argv[1], sys.argv[2]
restore = sys.argv[3] if len(sys.argv) > 3 else None

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
import ctypes
import ctypes.wintypes as wt
from qoder_ctrl_n_focus import user32, pids_for  # noqa: E402

# 主窗可能收进悬浮挂件（主窗 IsWindowVisible=False）：按面积挑最大的同名窗，
# 必要时 SHOW/RESTORE 唤醒，否则 UIA 树里只有挂件。
_pids = set(pids_for(exe))
_cands = []
_CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def _enum(h, _):
    got = wt.DWORD()
    user32.GetWindowThreadProcessId(h, ctypes.byref(got))
    if got.value in _pids:
        n = user32.GetWindowTextLengthW(h)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, buf, n + 1)
        rr = wt.RECT()
        user32.GetWindowRect(h, ctypes.byref(rr))
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        # 主窗 = Chrome_WidgetWin_1 且标题是产品名；挂件/IME/候选窗都排除。
        if cls.value == "Chrome_WidgetWin_1" and buf.value in ("WorkBuddy", "WorkBuddy AI"):
            _cands.append((h, buf.value, (rr.right - rr.left, rr.bottom - rr.top)))
    return True


user32.EnumWindows(_CB(_enum), 0)
if not _cands:
    print("no main window for", exe)
    sys.exit(1)
hwnd, wtitle, size = _cands[0]
if user32.IsIconic(hwnd):
    user32.ShowWindow(hwnd, 9)
elif not user32.IsWindowVisible(hwnd):
    user32.ShowWindow(hwnd, 5)
user32.SetForegroundWindow(hwnd)
time.sleep(1.0)
print("woke window", hex(hwnd), wtitle, size)
root = ui.ElementFromHandle(hwnd)
walker = ui.ControlViewWalker

_wr = wt.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(_wr))
WIN = (_wr.left, _wr.top)
print("window origin", WIN, "size", size)


def rect_of(el):
    r = el.GetCurrentPropertyValue(30001)
    t = tuple(int(v) for v in (tuple(r) if isinstance(r, (tuple, list))
                               else (0, 0, 0, 0)))
    return (t[0] - WIN[0], t[1] - WIN[1], t[2], t[3])


def collect():
    """返回 (会话行 Button 列表, 聊天区标题 Text)。限深遍历，别全树 FindAll。"""
    rows, headers = [], []
    seen = 0

    def walk(el, d):
        nonlocal seen
        if d > 9 or seen > 4000:
            return
        seen += 1
        ct = el.GetCurrentPropertyValue(30003)
        n = el.GetCurrentPropertyValue(30005) or ""
        r = rect_of(el)
        if ct == 50000 and 0 <= r[0] < 600 and r[1] > 200 and r[2] > 300 and n:
            rows.append((n, el))
        if ct == 50020 and 600 < r[0] < 1200 and 100 < r[1] < 300 and r[2] > 200:
            headers.append(n)
        child = walker.GetFirstChildElement(el)
        while child and seen <= 4000:
            walk(child, d + 1)
            child = walker.GetNextSiblingElement(child)

    walk(root, 0)
    return rows, headers


def click_row(rows, title):
    for n, el in rows:
        if n == title or n.startswith(title + " ") or n.startswith(title):
            el.GetCurrentPattern(10000).QueryInterface(
                IUIAutomationInvokePattern).Invoke()
            return n
    return None


rows, headers = collect()
print("before header:", headers[:1])
hit = click_row(rows, target)
print("clicked row:", hit)
if hit is None:
    print("ROW NOT FOUND; rows=", [n for n, _ in rows][:8])
    sys.exit(1)
time.sleep(1.5)
rows2, headers2 = collect()
print("after header:", headers2[:1])
ok = bool(headers2) and headers2[0].startswith(target)
print("OPEN_SPECIFIED_SESSION:", "OK" if ok else "MISMATCH")
if restore:
    back = click_row(rows2, restore)
    time.sleep(1.5)
    _, headers3 = collect()
    print("restored click:", back, "header:", headers3[:1])
