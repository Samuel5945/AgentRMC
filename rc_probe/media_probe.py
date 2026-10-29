"""只读探针：耳机/多媒体键在 Windows 的三条路径上是否可见。

同时挂低层键盘钩子 + Raw Input（键盘集合、消费类集合），按到的键逐行写日志。
用法: .venv\\Scripts\\python.exe rc_probe/media_probe.py [秒数]
结果: rc_probe/media_probe.log
"""
from __future__ import annotations

import ctypes as C
import ctypes.wintypes as wt
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG = HERE / "media_probe.log"
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 40

user32 = C.WinDLL("user32", use_last_error=True)
kernel32 = C.WinDLL("kernel32", use_last_error=True)

WM_INPUT = 0x00FF
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIDI_DEVICENAME = 0x20000007
RIM_TYPEKEYBOARD = 1
RIM_TYPEHID = 2
WH_KEYBOARD_LL = 13
WM_KEYDOWN, WM_SYSKEYDOWN = 0x0100, 0x0104
LLKHF_INJECTED = 0x10

SPECIAL = {
    0xAD: "MUTE", 0xAE: "VOL-", 0xAF: "VOL+",
    0xB0: "NEXT", 0xB1: "PREV", 0xB2: "STOP", 0xB3: "PLAY",
}


class RAWINPUTDEVICE(C.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]


class RAWINPUTHEADER(C.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE), ("wParam", wt.WPARAM)]


class RAWKEYBOARD(C.Structure):
    _fields_ = [("MakeCode", wt.USHORT), ("Flags", wt.USHORT), ("Reserved", wt.USHORT),
                ("VKey", wt.USHORT), ("Message", wt.UINT),
                ("ExtraInformation", wt.ULONG)]


class RAWHID(C.Structure):
    _fields_ = [("dwSizeHid", wt.DWORD), ("dwCount", wt.DWORD),
                ("bRawData", wt.BYTE * 128)]


class KBDLLHOOKSTRUCT(C.Structure):
    _fields_ = [("vkCode", wt.DWORD), ("scanCode", wt.DWORD), ("flags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", C.c_size_t)]


WNDPROC = C.WINFUNCTYPE(C.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
HOOKPROC = C.WINFUNCTYPE(C.c_ssize_t, C.c_int, wt.WPARAM, wt.LPARAM)


class WNDCLASSW(C.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", C.c_int), ("cbWndExtra", C.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = C.c_ssize_t
user32.RegisterClassW.restype = wt.ATOM
user32.RegisterClassW.argtypes = [C.POINTER(WNDCLASSW)]
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                   C.c_int, C.c_int, C.c_int, C.c_int,
                                   wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.RegisterRawInputDevices.argtypes = [C.POINTER(RAWINPUTDEVICE), wt.UINT, wt.UINT]
user32.RegisterRawInputDevices.restype = wt.BOOL
user32.GetRawInputData.argtypes = [wt.HANDLE, wt.UINT, C.c_void_p,
                                   C.POINTER(wt.UINT), wt.UINT]
user32.GetRawInputData.restype = wt.UINT
user32.GetRawInputDeviceInfoW.argtypes = [wt.HANDLE, wt.UINT, C.c_void_p,
                                          C.POINTER(wt.UINT)]
user32.GetRawInputDeviceInfoW.restype = wt.UINT
user32.SetWindowsHookExW.argtypes = [C.c_int, HOOKPROC, wt.HINSTANCE, wt.DWORD]
user32.SetWindowsHookExW.restype = wt.HHOOK
user32.CallNextHookEx.argtypes = [wt.HHOOK, C.c_int, wt.WPARAM, wt.LPARAM]
user32.CallNextHookEx.restype = C.c_ssize_t
user32.UnhookWindowsHookEx.argtypes = [wt.HHOOK]
kernel32.GetModuleHandleW.restype = wt.HMODULE
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]


def log(text: str) -> None:
    line = f"{time.strftime('%H:%M:%S')}.{int(time.time() * 1000) % 1000:03d} {text}"
    with LOG.open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def device_name(handle) -> str:
    size = wt.UINT(0)
    user32.GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, None, C.byref(size))
    if size.value <= 1:
        return "?"
    buf = C.create_unicode_buffer(size.value)
    user32.GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, buf, C.byref(size))
    name = buf.value
    return name[-90:] if len(name) > 90 else name


def wndproc(hwnd, msg, wparam, lparam):
    if msg == WM_INPUT:
        size = wt.UINT(0)
        user32.GetRawInputData(lparam, RID_INPUT, None, C.byref(size),
                               C.sizeof(RAWINPUTHEADER))
        if size.value:
            buf = C.create_string_buffer(size.value)
            got = user32.GetRawInputData(lparam, RID_INPUT, buf, C.byref(size),
                                         C.sizeof(RAWINPUTHEADER))
            if got == size.value:
                hdr = C.cast(buf, C.POINTER(RAWINPUTHEADER)).contents
                name = device_name(hdr.hDevice)
                if hdr.dwType == RIM_TYPEKEYBOARD:
                    kb = C.cast(C.addressof(buf) + C.sizeof(RAWINPUTHEADER),
                                C.POINTER(RAWKEYBOARD)).contents
                    if kb.VKey or kb.MakeCode:
                        log(f"RAW-KBD vk=0x{kb.VKey:02X}({SPECIAL.get(kb.VKey, '')}) "
                            f"scan=0x{kb.MakeCode:02X} msg=0x{kb.Message:04X} dev={name}")
                elif hdr.dwType == RIM_TYPEHID:
                    hid = C.cast(C.addressof(buf) + C.sizeof(RAWINPUTHEADER),
                                 C.POINTER(RAWHID)).contents
                    n = min(hid.dwSizeHid * hid.dwCount, 128)
                    data = bytes(hid.bRawData[:n]).hex()
                    log(f"RAW-HID size={hid.dwSizeHid} count={hid.dwCount} "
                        f"data={data} dev={name}")
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


def hook(ncode, wparam, lparam):
    if ncode == 0:
        kb = C.cast(lparam, C.POINTER(KBDLLHOOKSTRUCT)).contents
        vk = int(kb.vkCode)
        mark = "INJ " if kb.flags & LLKHF_INJECTED else ""
        name = SPECIAL.get(vk, "")
        if wparam in (WM_KEYDOWN, WM_SYSKEYDOWN):
            log(f"HOOK down {mark}vk=0x{vk:02X}({name}) scan=0x{kb.scanCode:02X}")
        else:
            log(f"HOOK up   {mark}vk=0x{vk:02X}({name}) scan=0x{kb.scanCode:02X}")
    return user32.CallNextHookEx(None, ncode, wparam, lparam)


def main() -> None:
    LOG.write_text("", encoding="utf-8")
    wc = WNDCLASSW()
    proc = WNDPROC(wndproc)
    wc.lpfnWndProc = proc
    wc.hInstance = kernel32.GetModuleHandleW(None)
    wc.lpszClassName = "AgentRMCMediaProbe"
    user32.RegisterClassW(C.byref(wc))
    hwnd = user32.CreateWindowExW(0, "AgentRMCMediaProbe", "", 0, 0, 0, 0, 0,
                                  None, None, wc.hInstance, None)
    if not hwnd:
        log(f"CreateWindowExW failed err={C.get_last_error()}")
        return

    devs = (RAWINPUTDEVICE * 2)()
    devs[0] = RAWINPUTDEVICE(0x01, 0x06, RIDEV_INPUTSINK, hwnd)   # 键盘
    devs[1] = RAWINPUTDEVICE(0x0C, 0x01, RIDEV_INPUTSINK, hwnd)   # 消费类
    if not user32.RegisterRawInputDevices(devs, 2, C.sizeof(RAWINPUTDEVICE)):
        log(f"RegisterRawInputDevices failed err={C.get_last_error()}")

    hproc = HOOKPROC(hook)
    hook_handle = user32.SetWindowsHookExW(WH_KEYBOARD_LL, hproc, None, 0)
    log(f"start duration={DURATION}s hook={bool(hook_handle)}")

    msg = wt.MSG()
    end = time.time() + DURATION
    while time.time() < end:
        while user32.PeekMessageW(C.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(C.byref(msg))
            user32.DispatchMessageW(C.byref(msg))
        time.sleep(0.005)

    if hook_handle:
        user32.UnhookWindowsHookEx(hook_handle)
    log("done")


if __name__ == "__main__":
    main()
