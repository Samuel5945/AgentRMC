"""第二组探测：NOLEGACY 的抑制范围是「本进程」还是「全系统」，以及别的进程注入能不能穿过去。

结论矩阵：
  A 进程注入 → B 窗口收到  = 只抑制注册进程，原生泄漏无解
  B 进程注入 → B 窗口收到  = 重注入可由独立进程完成（可做替换）
  两者都收不到            = 全局抑制，用户态无解（需要驱动）
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

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
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.GetForegroundWindow.restype = wt.HWND
user32.RegisterRawInputDevices.restype = wt.BOOL
user32.RegisterRawInputDevices.argtypes = [ctypes.POINTER(RAWINPUTDEVICE), wt.UINT, wt.UINT]
user32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
HWND_MESSAGE = wt.HWND(-3)


def send(vk: int, up: bool) -> None:
    inp = INPUT()
    inp.type = 1
    inp.u.ki = KEYBDINPUT(vk, 0, 0x0002 if up else 0, 0, 0x41474D43)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def register(hwnd, flags: int) -> bool:
    rid = RAWINPUTDEVICE(0x01, 0x06, flags, hwnd)
    return bool(user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)))


def pump(ms: int) -> None:
    end = time.time() + ms / 1000
    msg = wt.MSG()
    while time.time() < end:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.002)


CHILD = r'''
import ctypes, ctypes.wintypes as wt, sys, time, pathlib
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
class WC(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]
class KB(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]
class U(ctypes.Union):
    _fields_ = [("ki", KB), ("pad", ctypes.c_byte * 32)]
class INP(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", U)]
kernel32.GetModuleHandleW.restype = wt.HMODULE
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.GetForegroundWindow.restype = wt.HWND
user32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INP), ctypes.c_int]
seen = []
def proc(hwnd, msg, wp, lp):
    if msg == 0x0100: seen.append("down")
    if msg == 0x0101: seen.append("up")
    return user32.DefWindowProcW(hwnd, msg, wp, lp)
REF = WNDPROC(proc)
ready, go, out = map(pathlib.Path, sys.argv[1:4])
wc = WC(); wc.lpfnWndProc = REF; wc.hInstance = kernel32.GetModuleHandleW(None)
wc.lpszClassName = "AgentRMCNolegacyChild"
user32.RegisterClassW(ctypes.byref(wc))
hwnd = user32.CreateWindowExW(0, wc.lpszClassName, "probe child", 0x00CF0000, 100, 100, 320, 90, None, None, wc.hInstance, None)
user32.ShowWindow(hwnd, 5); user32.SetForegroundWindow(hwnd)
ready.write_text("1")
end = time.time() + 2.5
sent = False
msg = wt.MSG()
while time.time() < end:
    while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        user32.TranslateMessage(ctypes.byref(msg)); user32.DispatchMessageW(ctypes.byref(msg))
    if not sent and go.exists():
        for up in (False, True):
            inp = INP(); inp.type = 1
            inp.u.ki = KB(0x7C, 0, 2 if up else 0, 0, 0x42424242)
            user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INP))
            time.sleep(0.02)
        sent = True
        end = time.time() + 0.25
    time.sleep(0.002)
out.write_text("fg=" + str(user32.GetForegroundWindow() == hwnd) + " seen=" + ",".join(seen))
'''


def main() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="rmc_probe_"))
    ready, go, out = tmp / "ready", tmp / "go", tmp / "out"
    hinst = kernel32.GetModuleHandleW(None)

    def _noop(hwnd, msg, wp, lp):
        return user32.DefWindowProcW(hwnd, msg, wp, lp)

    ref = WNDPROC(_noop)
    wc = WNDCLASSW()
    wc.lpfnWndProc = ref
    wc.hInstance = hinst
    wc.lpszClassName = "AgentRMCNolegacyParent"
    user32.RegisterClassW(ctypes.byref(wc))
    hwnd = user32.CreateWindowExW(0, wc.lpszClassName, "parent", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, None, hinst, None)
    child = subprocess.Popen([sys.executable, "-c", CHILD, str(ready), str(go), str(out)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        t0 = time.time()
        while not ready.exists() and time.time() - t0 < 4:
            time.sleep(0.02)
        print("child ready:", ready.exists(), flush=True)
        pump(150)
        ok = register(hwnd, 0x30)  # NOLEGACY
        print("NOLEGACY:", ok, "err", ctypes.get_last_error(), flush=True)
        pump(100)
        send(0x7C, False)
        pump(20)
        send(0x7C, True)
        print("A->B sent", flush=True)
        go.write_text("1")
        pump(500)
    finally:
        register(hwnd, 0x00000100)  # restore INPUTSINK
        print("restored legacy", flush=True)
        child.wait(timeout=4)
    print("child result:", out.read_text() if out.exists() else "missing", flush=True)


if __name__ == "__main__":
    main()
