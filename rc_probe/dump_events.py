"""只读抓取 AgentRMCNative 事件流：切到「事件流」页签并导出全部条目。

用法: python rc_probe/dump_events.py [进程名] [-o out.txt]
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys

import comtypes.client

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR,
                                                ctypes.POINTER(wt.DWORD)]

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def windows_of(wanted: str, include_hidden: bool = False):
    out = []

    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel32.OpenProcess(0x1000, False, pid.value)
        exe = ""
        if h:
            buf = ctypes.create_unicode_buffer(512)
            n = wt.DWORD(512)
            if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
                exe = buf.value.split("\\")[-1]
            kernel32.CloseHandle(h)
        if exe.lower() != wanted.lower():
            return True
        visible = bool(user32.IsWindowVisible(hwnd))
        if visible or include_hidden:
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            out.append((hwnd, buf.value, visible))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return out


def main() -> None:
    args = [a for a in sys.argv[1:]]
    out_path = None
    if "-o" in args:
        i = args.index("-o")
        out_path = args[i + 1]
        del args[i:i + 2]
    wanted = args[0] if args else "AgentRMCNative.exe"

    wins = windows_of(wanted, include_hidden=True)
    print(f"{wanted}: {len(wins)} 个顶层窗口（含隐藏）")
    if not wins:
        return
    for hwnd, title, vis in wins:
        print(f"  hwnd=0x{hwnd:X} visible={vis} title={title!r}")

    hwnd, title, vis = next((w for w in wins if w[2]), wins[0])

    uia_mod = comtypes.client.GetModule("UIAutomationCore.dll")
    uia = comtypes.client.CreateObject(uia_mod.CUIAutomation,
                                       interface=uia_mod.IUIAutomation)
    root = uia.ElementFromHandle(hwnd)
    cond = uia.CreateTrueCondition()
    items = root.FindAll(4, cond)
    print(f"  后代节点: {items.Length}")

    lines = [f"hwnd=0x{hwnd:X} title={title!r} nodes={items.Length}"]

    # 1) 找「事件流」TabItem 并选中（让 QListWidget 显示出来）
    tab = None
    for i in range(items.Length):
        el = items.GetElement(i)
        try:
            if el.CurrentControlType == 50019 and \
                    (el.CurrentName or "").strip() == "事件流":
                tab = el
                break
        except Exception:
            continue
    if tab is not None:
        try:
            sel = tab.GetCurrentPattern(uia_mod.UIA_SelectionItemPatternId)
            if sel:
                sel.Select()
                print("  已选中「事件流」页签")
                lines.append("[tab] 事件流 已选中")
        except Exception as e:  # noqa: BLE001
            print(f"  选中页签失败: {e}")
    else:
        print("  未找到「事件流」页签（可能当前已在事件流页）")

    # 2) 重新取树，导出全部 ListItem 文本
    items = root.FindAll(4, cond)
    count = 0
    for i in range(items.Length):
        el = items.GetElement(i)
        try:
            if el.CurrentControlType != 50007:  # ListItem
                continue
            name = (el.CurrentName or "").strip()
            if not name:
                continue
            count += 1
            print(f"[{count:03d}] {name}")
            lines.append(name)
        except Exception:
            continue
    print(f"共 {count} 条事件")
    lines.append(f"[total] {count}")

    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print(f"已写入 {out_path}")


if __name__ == "__main__":
    main()
