"""只读+一次点击：点 AgentRMCNative 窗口里指定文字的按钮。

用法: python rc_probe/click_button.py <按钮文字>
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
import time

import comtypes.client

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR,
                                                ctypes.POINTER(wt.DWORD)]

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def main_window(wanted: str):
    found = []

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
        if exe.lower() == wanted.lower() and user32.IsWindowVisible(hwnd) and \
                user32.GetWindowTextLengthW(hwnd) > 0:
            found.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found[0] if found else None


def main() -> None:
    wanted = "AgentRMCNative.exe"
    label = sys.argv[1]
    hwnd = main_window(wanted)
    assert hwnd, f"没找到 {wanted} 的主窗口"

    uia_mod = comtypes.client.GetModule("UIAutomationCore.dll")
    uia = comtypes.client.CreateObject(uia_mod.CUIAutomation,
                                       interface=uia_mod.IUIAutomation)
    root = uia.ElementFromHandle(hwnd)
    items = root.FindAll(4, uia.CreateTrueCondition())
    for i in range(items.Length):
        el = items.GetElement(i)
        try:
            if el.CurrentControlType == 50000 and \
                    (el.CurrentName or "").strip() == label:
                acc = el.GetCurrentPattern(uia_mod.UIA_LegacyIAccessiblePatternId)
                acc.QueryInterface(
                    uia_mod.IUIAutomationLegacyIAccessiblePattern
                ).DoDefaultAction()
                print(f"已点击「{label}」")
                return
        except Exception:
            continue
    print(f"没找到按钮「{label}」（可能要先切页签）")


if __name__ == "__main__":
    main()
