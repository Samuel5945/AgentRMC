"""读取前台 Agent 窗口的 UIA 树，判断能不能直接拿到聊天文本（只读，不改状态）。

用法:
    python rc_probe/uia_probe.py [进程名]
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


def windows_of(wanted: str):
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
        if exe.lower() == wanted.lower() and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            out.append((hwnd, buf.value))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return out


CTRL = {
    50000: "Button", 50001: "Calendar", 50002: "CheckBox", 50003: "ComboBox",
    50004: "Edit", 50005: "Hyperlink", 50006: "Image", 50007: "ListItem",
    50008: "List", 50009: "Menu", 50010: "MenuBar", 50011: "MenuItem",
    50012: "ProgressBar", 50013: "RadioButton", 50014: "ScrollBar",
    50015: "Slider", 50016: "Spinner", 50017: "StatusBar", 50018: "Tab",
    50019: "TabItem", 50020: "Text", 50021: "ToolBar", 50022: "ToolTip",
    50023: "Tree", 50024: "TreeItem", 50025: "Custom", 50026: "Group",
    50027: "Thumb", 50028: "DataGrid", 50029: "DataItem", 50030: "Document",
    50031: "SplitButton", 50032: "Window", 50033: "Pane", 50034: "Header",
    50035: "HeaderItem", 50036: "Table", 50037: "TitleBar", 50038: "Separator",
}


def main() -> None:
    wanted = sys.argv[1] if len(sys.argv) > 1 else "WorkBuddyAI.exe"
    wins = windows_of(wanted)
    print(f"{wanted}: {len(wins)} 个可见窗口")
    if not wins:
        return
    hwnd, title = wins[0]
    print(f"  hwnd=0x{hwnd:X} title={title!r}")

    uia_mod = comtypes.client.GetModule("UIAutomationCore.dll")
    uia = comtypes.client.CreateObject(uia_mod.CUIAutomation,
                                       interface=uia_mod.IUIAutomation)
    root = uia.ElementFromHandle(hwnd)
    cond = uia.CreateTrueCondition()
    # 树可能很大，先只取前 300 个后代
    items = root.FindAll(4, cond)  # TreeScope_Descendants
    print(f"  后代节点: {items.Length}")
    shown = 0
    texts = []
    for i in range(items.Length):
        if shown >= 160:
            break
        try:
            el = items.GetElement(i)
            ctype = el.CurrentControlType
            name = (el.CurrentName or "").strip()
            aid = el.CurrentAutomationId or ""
            cls = el.CurrentClassName or ""
            off = el.CurrentIsOffscreen
            tag = CTRL.get(ctype, hex(ctype))
            if not name and not aid:
                continue
            shown += 1
            line = f"  {tag:<26} off={int(off)} cls={cls[:22]:<22} id={aid[:24]:<24} {name[:90]!r}"
            print(line)
            if name and ("Text" in tag or "Document" in tag or len(name) > 40):
                texts.append(name)
        except Exception as e:  # noqa: BLE001
            print(f"  [!] {i}: {e}")
    print(f"\n候选文本节点 {len(texts)} 个，最后 5 条:")
    for t in texts[-5:]:
        print("   -", t[:160].replace("\n", " "))


if __name__ == "__main__":
    main()
