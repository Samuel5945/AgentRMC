"""桌面外壳「停止生成」活体验收（2026-11-20）。

只读断言：进待办会话页，读发送键的 UIA Name。
- 会话在跑 → 「停止生成」
- 没在跑   → 「派发 / 发送 / 发首条」
不点击（点了会真停掉正在跑的轮次）。

用法：python rc_probe/desktop_stop_key_probe.py [待办标题片段]
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation,
    IUIAutomation,
    UIA_ButtonControlTypeId,
    TreeScope_Descendants,
)

user32 = ctypes.WinDLL("user32", use_last_error=True)
AUTOMATION = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
SEND_LABELS = ("停止生成", "派发", "发送", "发首条")


def find_window():
    """枚举顶层窗，取「有 UIA 子树」的那个 AgentRMC 窗口。

    本机实测（2026-11-20）会同时存在 'AgentRMC Native'（真主窗，有 WebView2 子树）
    和 'AgentRMCNative'（无子树），按标题精确取前者。
    """
    found = []

    def enum_(h, _):
        if user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if "AgentRMC" in buf.value:
                found.append((h, buf.value))
        return True

    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    user32.EnumWindows(CB(enum_), 0)
    best = None
    for hwnd, title in found:
        el = AUTOMATION.ElementFromHandle(hwnd)
        n = el.FindAll(TreeScope_Descendants,
                       AUTOMATION.CreateTrueCondition()).Length
        if n > 0 and (best is None or n > best[2]):
            best = (hwnd, title, n)
    return best


def elements(root):
    desc = root.FindAll(TreeScope_Descendants,
                        AUTOMATION.CreateTrueCondition())
    return [desc.GetElement(i) for i in range(desc.Length)]


def main() -> int:
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    win = find_window()
    if not win:
        print("找不到 AgentRMC 主窗（应用没开？）")
        return 2
    hwnd, title, n = win
    print("window: %s (%d nodes)" % (title, n))
    user32.ShowWindow(hwnd, 3)
    user32.SetForegroundWindow(hwnd)
    time.sleep(1.2)

    root = AUTOMATION.ElementFromHandle(hwnd)
    els = elements(root)

    # 1) 进会话页：找标题含 want 的待办卡，Invoke/Select
    if want:
        card = None
        for el in els:
            nm = (el.CurrentName or "").strip()
            if want in nm:
                card = el
                break
        if card is None:
            print("找不到标题含 %r 的待办卡" % want)
            return 3
        opened = False
        for pattern in ("Invoke", "Select"):
            try:
                getattr(card, pattern)()
                print("opened via", pattern)
                opened = True
                break
            except Exception:
                continue
        if not opened:
            print("卡片拿不到 Invoke/Select：请手动点开会话页后再跑")
            return 3
        time.sleep(1.5)
        els = elements(AUTOMATION.ElementFromHandle(hwnd))

    # 2) 读发送键
    keys = []
    for el in els:
        try:
            if el.CurrentControlType != UIA_ButtonControlTypeId:
                continue
        except Exception:
            continue
        nm = (el.CurrentName or "").strip()
        if nm in SEND_LABELS:
            keys.append(nm)
    print("send keys:", keys)
    if not keys:
        print("FAIL 会话页没找到发送键（可能还停在列表页）")
        return 1
    if "停止生成" in keys:
        print("PASS 会话在跑 → 发送键是「停止生成」")
        return 0
    print("INFO 会话没在跑（或 live 账已过期）→ 发送键是 %s" % keys[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
