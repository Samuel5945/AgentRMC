"""探测：RIDEV_NOLEGACY 下，SendInput 注入的键还会不会变成 WM_KEYDOWN。

流程（全程 < 3 秒，结束一定恢复传统键盘）：
  1. 建一个小窗口并抢焦点
  2. 注册 NOLEGACY（传统键盘消息关闭）
  3. SendInput 注入 F13，看窗口能否收到 WM_KEYDOWN
  4. 无论结果如何，立即恢复 INPUTSINK 注册并退出

如果第 3 步收到 F13，说明「NOLEGACY + 重注入」可行：
遥控器的原生按键不再漏给前台窗口，可以真正做到替换。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import threading
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

CS_HREDRAW, CS_VREDRAW = 0x0002, 0x0001
WS_OVERLAPPEDWINDOW = 0x00CF0000
WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN = 0x0100, 0x0101, 0x0104
WM_INPUT, WM_CLOSE = 0x00FF, 0x0010
RID_INPUT = 0x10000003
RIDEV_NOLEGACY = 0x00000030
RIDEV_INPUTSINK = 0x00000100
RIDEV_REMOVE = 0x00000001
HWND_MESSAGE = wt.HWND(-3)

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _U)]


kernel32.GetModuleHandleW.restype = wt.HMODULE
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
user32.RegisterClassW.restype = wt.ATOM
user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.RegisterRawInputDevices.restype = wt.BOOL
user32.RegisterRawInputDevices.argtypes = [ctypes.POINTER(RAWINPUTDEVICE), wt.UINT, wt.UINT]
user32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t

seen: list[str] = []
WNDPROC_REF = None
VK_F13 = 0x7C


def wndproc(hwnd, msg, wparam, lparam):
    if msg == WM_KEYDOWN:
        seen.append(f"WM_KEYDOWN vk=0x{wparam:02X}")
    elif msg == WM_KEYUP:
        seen.append(f"WM_KEYUP vk=0x{wparam:02X}")
    elif msg == WM_INPUT:
        seen.append("WM_INPUT")
    elif msg == WM_CLOSE:
        user32.DestroyWindow(hwnd)
        return 0
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


def register(hwnd, flags: int) -> bool:
    rid = RAWINPUTDEVICE(0x01, 0x06, flags, hwnd if flags != RIDEV_REMOVE else None)
    return bool(user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)))


def pump(ms: int) -> None:
    end = time.time() + ms / 1000
    msg = wt.MSG()
    while time.time() < end:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.002)


def send(vk: int, up: bool) -> None:
    inp = INPUT()
    inp.type = 1
    inp.u.ki = KEYBDINPUT(vk, 0, 0x0002 if up else 0, 0, 0x41474D43)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def main() -> None:
    global WNDPROC_REF
    hinst = kernel32.GetModuleHandleW(None)
    wc = WNDCLASSW()
    WNDPROC_REF = WNDPROC(wndproc)
    wc.style = CS_HREDRAW | CS_VREDRAW
    wc.lpfnWndProc = WNDPROC_REF
    wc.hInstance = hinst
    wc.lpszClassName = "AgentRMCNolegacyProbe"
    if not user32.RegisterClassW(ctypes.byref(wc)):
        print("RegisterClassW failed", ctypes.get_last_error())
        return
    hwnd = user32.CreateWindowExW(0, wc.lpszClassName, "AgentRMC NOLEGACY probe",
                                  WS_OVERLAPPEDWINDOW, 60, 60, 360, 110,
                                  None, None, hinst, None)
    if not hwnd:
        print("CreateWindowExW failed", ctypes.get_last_error())
        return

    def restore(*_):
        register(hwnd, RIDEV_INPUTSINK)
        print("--- restored legacy ---", flush=True)
        os._exit(0)

    try:
        user32.ShowWindow(hwnd, 5)  # SW_SHOW
        user32.SetForegroundWindow(hwnd)
        pump(150)
        print(f"foreground={user32.GetForegroundWindow() == hwnd}", flush=True)

        seen.clear()
        send(VK_F13, False)
        pump(20)
        send(VK_F13, True)
        pump(250)
        print("A. 默认模式（legacy 开）注入 F13:", seen, flush=True)

        ok = register(hwnd, RIDEV_NOLEGACY)
        print(f"B. NOLEGACY 注册={ok} err={ctypes.get_last_error()}", flush=True)
        seen.clear()
        send(VK_F13, False)
        pump(20)
        send(VK_F13, True)
        pump(300)
        print("B. NOLEGACY 下注入 F13:", seen, flush=True)
    finally:
        restore()


if __name__ == "__main__":
    threading.Timer(4.0, lambda: os._exit(2)).start()
    main()
