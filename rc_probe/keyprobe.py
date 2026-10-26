"""遥控器按键全通道探测：键盘 / 消费类(音量) / 系统控制 / HID 原始报告。

为什么单独写一个：
  现有 rawlisten / learn_keys 只注册了 `usUsagePage=0x01, usUsage=0x06`（键盘集合），
  而音量键、静音键在 HID 里通常属于 **Consumer Control（0x0C/0x01）**，走的是另一条
  顶层集合。只注册键盘集合的话，按音量键什么都收不到 —— 看起来就像"这个键没映射"。

  本脚本同时注册三路 Raw Input，并把 HID 类型的原始报告也打出来，用来判断：
    · 音量键到底有没有报文到达用户态
    · 它是以键盘 vkey 形式来的，还是 Consumer usage 形式来的

用法:
    python rc_probe/keyprobe.py [秒数]
"""
from __future__ import annotations

import collections
import ctypes
import ctypes.wintypes as wt
import sys
import time
from pathlib import Path

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

kernel32.GetModuleHandleW.restype = wt.HMODULE
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE,
                                   wt.LPVOID]
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = wt.LPARAM
user32.GetRawInputData.argtypes = [wt.LPARAM, wt.UINT, ctypes.c_void_p,
                                   ctypes.POINTER(wt.UINT), wt.UINT]
user32.GetRawInputData.restype = wt.UINT
user32.GetRawInputDeviceInfoW.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p,
                                          ctypes.POINTER(wt.UINT)]
user32.GetRawInputDeviceInfoW.restype = wt.UINT
user32.RegisterRawInputDevices.argtypes = [ctypes.c_void_p, wt.UINT, wt.UINT]
user32.RegisterRawInputDevices.restype = wt.BOOL
user32.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]
user32.PeekMessageW.restype = wt.BOOL
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.restype = wt.LPARAM

WM_INPUT = 0x00FF
RID_INPUT = 0x10000003
RIDI_DEVICENAME = 0x20000007
RIDEV_INPUTSINK = 0x00000100
HWND_MESSAGE = wt.HWND(-3)
TARGET_MARK = "01000d_pid&3838"
ALL_DEVICES = False   # --all 时不过滤设备，看是不是别处冒出来了

RIM_TYPES = {0: "MOUSE", 1: "KEYBOARD", 2: "HID"}


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE), ("wParam", wt.WPARAM)]


class RAWKEYBOARD(ctypes.Structure):
    _fields_ = [("MakeCode", wt.USHORT), ("Flags", wt.USHORT),
                ("Reserved", wt.USHORT), ("VKey", wt.USHORT),
                ("Message", wt.DWORD), ("ExtraInformation", wt.DWORD)]


class RAWHID(ctypes.Structure):
    _fields_ = [("dwSizeHid", wt.DWORD), ("dwCount", wt.DWORD),
                ("bRawData", ctypes.c_ubyte * 1)]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", ctypes.c_void_p),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


WNDPROC = ctypes.WINFUNCTYPE(wt.LPARAM, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
_WNDPROC_REF = None

start = time.time()
seen: collections.Counter = collections.Counter()
total = 0


def dev_name(h) -> str:  # noqa: ANN001
    size = wt.UINT(0)
    user32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, None, ctypes.byref(size))
    if not size.value:
        return ""
    buf = ctypes.create_unicode_buffer(size.value + 1)
    user32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, buf, ctypes.byref(size))
    return buf.value


def wndproc(hwnd, msg, wparam, lparam):  # noqa: ANN001
    global total
    if msg != WM_INPUT:
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    size = wt.UINT(0)
    user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(size),
                           ctypes.sizeof(RAWINPUTHEADER))
    if not size.value:
        return 0
    buf = ctypes.create_string_buffer(size.value)
    if user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(size),
                              ctypes.sizeof(RAWINPUTHEADER)) == 0xFFFFFFFF:
        return 0

    hdr = ctypes.cast(buf, ctypes.POINTER(RAWINPUTHEADER)).contents
    name = dev_name(hdr.hDevice).lower()
    if not ALL_DEVICES and TARGET_MARK not in name:
        return 0

    off = ctypes.sizeof(RAWINPUTHEADER)
    body = bytes(buf[off:size.value])
    total += 1
    t = round(time.time() - start, 2)

    if hdr.dwType == 1 and len(body) >= ctypes.sizeof(RAWKEYBOARD):
        kb = RAWKEYBOARD.from_buffer_copy(body)
        key = ("KB", int(kb.VKey), int(kb.Message))
        seen[key] += 1
        if seen[key] <= 3:
            kind = "keydown" if kb.Message in (0x0100, 0x0104) else "keyup"
            who = "" if not ALL_DEVICES else f" @{name[-14:]}"
            print(f"  [{t:6.2f}] KEYBOARD vk=0x{kb.VKey:02X} ({kb.VKey}) "
                  f"scan=0x{kb.MakeCode:02X} {kind}{who}  [第 {seen[key]} 次]", flush=True)
    elif hdr.dwType == 2 and len(body) >= 8:
        hs, cnt = int.from_bytes(body[0:4], "little"), int.from_bytes(body[4:8], "little")
        data = body[8:8 + hs * cnt]
        key = ("HID", hs, data.hex(" "))
        seen[key] += 1
        if seen[key] <= 3:
            print(f"  [{t:6.2f}] HID  size={hs} count={cnt}  {data.hex(' ')}  "
                  f"[第 {seen[key]} 次]", flush=True)
    else:
        key = ("OTHER", hdr.dwType, body.hex(" ")[:40])
        seen[key] += 1
        if seen[key] <= 3:
            print(f"  [{t:6.2f}] {RIM_TYPES.get(hdr.dwType, hdr.dwType)} "
                  f"{body.hex(' ')[:60]}", flush=True)
    return 0


def main() -> None:
    global _WNDPROC_REF
    global ALL_DEVICES
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ALL_DEVICES = "--all" in sys.argv
    secs = float(args[0]) if args else 25.0

    _WNDPROC_REF = WNDPROC(wndproc)
    wc = WNDCLASSW()
    wc.lpfnWndProc = ctypes.cast(_WNDPROC_REF, ctypes.c_void_p)
    wc.lpszClassName = "AgentRMCKeyProbe"
    wc.hInstance = kernel32.GetModuleHandleW(None)
    if not user32.RegisterClassW(ctypes.byref(wc)):
        print(f"RegisterClassW 失败 err={ctypes.get_last_error()}")
        return
    hwnd = user32.CreateWindowExW(0, "AgentRMCKeyProbe", "AgentRMCKeyProbe", 0,
                                  0, 0, 0, 0, HWND_MESSAGE, None, wc.hInstance, None)
    if not hwnd:
        print(f"CreateWindowExW 失败 err={ctypes.get_last_error()}")
        return

    devs = (RAWINPUTDEVICE * 3)()
    # 键盘 / 消费类（音量、静音、播放）/ 系统控制（电源、睡眠）
    devs[0].usUsagePage, devs[0].usUsage = 0x01, 0x06
    devs[1].usUsagePage, devs[1].usUsage = 0x0C, 0x01
    devs[2].usUsagePage, devs[2].usUsage = 0x01, 0x80
    for d in devs:
        d.dwFlags = RIDEV_INPUTSINK
        d.hwndTarget = hwnd
    if not user32.RegisterRawInputDevices(ctypes.byref(devs), 3, ctypes.sizeof(RAWINPUTDEVICE)):
        print(f"RegisterRawInputDevices 失败 err={ctypes.get_last_error()}")
        return

    print("=" * 60)
    scope = "全部设备" if ALL_DEVICES else "仅遥控器"
    print(f"  已注册 3 路 Raw Input：键盘 / 消费类(音量) / 系统控制（{scope}）")
    if secs > 0:
        print(f"  现在请依次按：音量+ → 音量- → 静音（如果有）→ 电源键")
        print(f"  窗口 {secs:.0f} 秒，到点自动结束")
    else:
        print("  现在是「随时按、实时打印」模式，没有窗口期")
        print("  请按：音量+ → 音量- → 静音（如果有）→ 电源键")
        print("  按完在这里按 Ctrl+C 结束")
    print("=" * 60, flush=True)

    deadline = (time.time() + secs) if secs > 0 else float("inf")
    tick = time.time() + 5
    m = wt.MSG()
    try:
        while time.time() < deadline:
            while user32.PeekMessageW(ctypes.byref(m), None, 0, 0, 1):
                user32.TranslateMessage(ctypes.byref(m))
                user32.DispatchMessageW(ctypes.byref(m))
            now = time.time()
            if now >= tick:
                tick = now + 5
                left = f"剩 {deadline - now:3.0f}s" if secs > 0 else f"已跑 {now - start:3.0f}s"
                print(f"  …{left}   已收 {total} 个报告", flush=True)
            time.sleep(0.003)
    except KeyboardInterrupt:
        pass

    print("\n===== 汇总 =====")
    if not seen:
        print("  遥控器没有任何报告到达用户态 —— 这些键被内核吃掉了")
    for key, n in sorted(seen.items(), key=lambda kv: -kv[1]):
        if key[0] == "KB":
            kind = "keydown" if key[2] in (0x0100, 0x0104) else "keyup"
            print(f"  KEYBOARD vk=0x{key[1]:02X} ({key[1]}) {kind} × {n}")
        elif key[0] == "HID":
            print(f"  HID size={key[1]} {key[2]} × {n}")
        else:
            print(f"  {key} × {n}")
    print(f"总计 {total} 个报告")
    try:
        input("按回车关闭…")
    except (EOFError, OSError):
        pass


if __name__ == "__main__":
    main()
