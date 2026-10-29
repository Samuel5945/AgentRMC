"""验证：低层键盘钩子吞掉按键时，Raw Input 是否仍能收到该键。

用法: .venv\\Scripts\\python.exe rc_probe/llhook_raw_probe.py
只在本进程内注入 F13 并监听，不碰真实键盘。
"""
import ctypes as C
import ctypes.wintypes as W
import sys
from pathlib import Path as _P
sys.path.insert(0, str(_P(__file__).resolve().parent.parent))
import agentrc as _core

user32 = _core.user32
kernel32 = C.WinDLL("kernel32", use_last_error=True)

WM_INPUT = 0x00FF
RIDEV_INPUTSINK = 0x100
RID_INPUT = 0x10000003
WH_KEYBOARD_LL = 13
LLKHF_INJECTED = 0x10


RAWINPUTDEVICE = _core.RAWINPUTDEVICE
RAWINPUTHEADER = _core.RAWINPUTHEADER


class KBDLLHOOKSTRUCT(C.Structure):
    _fields_ = [("vkCode", W.DWORD), ("scanCode", W.DWORD),
                ("flags", W.DWORD), ("time", W.DWORD),
                ("dwExtraInfo", C.c_size_t)]


WNDPROC = _core.WNDPROC
HOOKPROC = C.WINFUNCTYPE(C.c_ssize_t, C.c_int, W.WPARAM, W.LPARAM)

got = []
blocked = []


def wndproc(hwnd, msg, wparam, lparam):
    if msg == WM_INPUT:
        size = W.UINT()
        user32.GetRawInputData(lparam, RID_INPUT, None, C.byref(size),
                               C.sizeof(RAWINPUTHEADER))
        buf = C.create_string_buffer(size.value)
        n = user32.GetRawInputData(lparam, RID_INPUT, buf, C.byref(size),
                                   C.sizeof(RAWINPUTHEADER))
        if n:
            hdr = C.cast(buf, C.POINTER(RAWINPUTHEADER)).contents
            if hdr.dwType == 1:
                kb = C.cast(C.byref(buf, C.sizeof(RAWINPUTHEADER) + 6),
                            C.POINTER(W.WORD))[0]
                got.append(kb)
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


def hook(ncode, wparam, lparam):
    if ncode == 0:
        kb = C.cast(lparam, C.POINTER(KBDLLHOOKSTRUCT)).contents
        if kb.vkCode == 0x7C:          # F13
            blocked.append(kb.flags & LLKHF_INJECTED != 0)
            return 1                   # 吞掉
    return user32.CallNextHookEx(None, ncode, wparam, lparam)


def main():
    wc = _core.WNDCLASSW()
    wproc = WNDPROC(wndproc)
    wc.lpfnWndProc = wproc
    wc.hInstance = kernel32.GetModuleHandleW(None)
    wc.lpszClassName = "AgentRMCProbe"
    user32.RegisterClassW(C.byref(wc))
    hwnd = user32.CreateWindowExW(0, "AgentRMCProbe", "", 0, 0, 0, 0, 0,
                                  None, None, wc.hInstance, None)
    rid = RAWINPUTDEVICE(1, 6, RIDEV_INPUTSINK, hwnd)
    user32.RegisterRawInputDevices(C.byref(rid), 1, C.sizeof(rid))

    time = __import__("time")
    msg = W.MSG()

    def spin(seconds):
        end = time.time() + seconds
        while time.time() < end:
            while user32.PeekMessageW(C.byref(msg), None, 0, 0, 1):
                user32.TranslateMessage(C.byref(msg))
                user32.DispatchMessageW(C.byref(msg))
            time.sleep(0.005)

    def tap():
        _core._send_key(0x7C)
        _core._send_key(0x7C, up=True)

    user32.CallNextHookEx.argtypes = [W.HHOOK, C.c_int, W.WPARAM, W.LPARAM]
    user32.CallNextHookEx.restype = C.c_ssize_t

    tap()                     # 对照组：无钩子
    spin(0.3)
    print("control raw:", got)
    got.clear()

    hproc = HOOKPROC(hook)
    hook_handle = user32.SetWindowsHookExW(WH_KEYBOARD_LL, hproc, None, 0)
    print("hook installed:", bool(hook_handle))
    tap()
    spin(0.3)
    print("hook saw:", blocked)
    print("with-hook raw:", got)
    print("RESULT:", "raw survives hook" if got else "raw blocked too")
    user32.UnhookWindowsHookEx(hook_handle)


if __name__ == "__main__":
    main()
