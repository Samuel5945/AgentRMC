"""Raw Input 原始报告尺寸嗅探。

为什么用 Raw Input 而不是 hidapi / CreateFile：
  Raw Input 走的是内核把报告投递给窗口的路径，**不需要 FILE_READ_DATA**，
  所以哪怕 1812 的 HID 接口 DACL 不给用户读权限，WM_INPUT 里照样能拿到
  原始报告内容。之前 hidapi / CreateFileW 全 err=5，但这条路没被堵死。

判断逻辑：
  键盘集合的原始报告固定是 RAWKEYBOARD = 16 字节。如果按住语音键时出现
  20/64/256 字节之类的报告，说明音频（或分片音频）确实藏在 HID 报告里，
  只是走的是厂商自定义格式。

用法: python rc_probe/rawsize.py [秒数]
"""
from __future__ import annotations

import collections
import ctypes
import ctypes.wintypes as wt
import json
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
user32.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]
user32.PeekMessageW.restype = wt.BOOL
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.restype = wt.LPARAM

WM_INPUT = 0x00FF
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIDI_DEVICENAME = 0x20000007
HWND_MESSAGE = wt.HWND(-3)
TARGET_MARK = "01000d_pid&3838"

OUT = Path(__file__).resolve().parent / "raw_sizes.jsonl"


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE), ("wParam", wt.WPARAM)]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", ctypes.c_void_p),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


WNDPROC = ctypes.WINFUNCTYPE(wt.LPARAM, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

STANDARD = 16          # RAWKEYBOARD 固定 16 字节
stats: collections.Counter = collections.Counter()
big: list[dict] = []
keys = 0
log = open(OUT, "w", encoding="utf-8")
start = time.time()
_WNDPROC_REF = None


def device_name(h) -> str:  # noqa: ANN001
    size = wt.UINT(0)
    user32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, None, ctypes.byref(size))
    if not size.value:
        return ""
    buf = ctypes.create_unicode_buffer(size.value + 1)
    user32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, buf, ctypes.byref(size))
    return buf.value


def wndproc(hwnd, msg, wparam, lparam):  # noqa: ANN001
    global keys
    if msg != WM_INPUT:
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    size = wt.UINT(0)
    user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(size),
                           ctypes.sizeof(RAWINPUTHEADER))
    if size.value == 0:
        return 0
    buf = ctypes.create_string_buffer(size.value)
    got = user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(size),
                                 ctypes.sizeof(RAWINPUTHEADER))
    if got == 0xFFFFFFFF:
        return 0

    hdr = ctypes.cast(buf, ctypes.POINTER(RAWINPUTHEADER)).contents
    name = device_name(hdr.hDevice)
    if TARGET_MARK not in name.lower():
        return 0

    payload = bytes(buf[ctypes.sizeof(RAWINPUTHEADER):size.value])
    stats[len(payload)] += 1
    rec = {"t": round(time.time() - start, 3), "size": len(payload),
           "hex": payload[:48].hex(" ")}
    log.write(json.dumps(rec) + "\n")
    log.flush()
    if len(payload) != STANDARD:
        big.append(rec)
        print(f"    [{rec['t']:7.3f}] 非标准报告 size={len(payload)} "
              f"{payload[:32].hex(' ')}", flush=True)
    else:
        keys += 1
        vk = int.from_bytes(payload[6:8], 'little') if len(payload) >= 8 else 0
        msg = int.from_bytes(payload[8:12], 'little') if len(payload) >= 12 else 0
        tag = '按下' if msg in (0x0100, 0x0104) else '松开'
        if keys <= 8 or keys % 25 == 0:
            print(f"    [{rec['t']:7.3f}] 按键#{keys} vk=0x{vk:02X} {tag}", flush=True)
    return 0


def main() -> None:
    global _WNDPROC_REF
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

    _WNDPROC_REF = WNDPROC(wndproc)
    wc = WNDCLASSW()
    wc.lpfnWndProc = ctypes.cast(_WNDPROC_REF, ctypes.c_void_p)
    wc.lpszClassName = "AgentRMCRawSize"
    wc.hInstance = kernel32.GetModuleHandleW(None)
    if not user32.RegisterClassW(ctypes.byref(wc)):
        print(f"RegisterClassW 失败 err={ctypes.get_last_error()}")
        return
    hwnd = user32.CreateWindowExW(0, "AgentRMCRawSize", "AgentRMCRawSize", 0,
                                  0, 0, 0, 0, HWND_MESSAGE, None, wc.hInstance, None)
    if not hwnd:
        print(f"CreateWindowExW 失败 err={ctypes.get_last_error()}")
        return
    rid = RAWINPUTDEVICE(0x01, 0x06, RIDEV_INPUTSINK, hwnd)
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)):
        print(f"RegisterRawInputDevices 失败 err={ctypes.get_last_error()}")
        return

    print("=" * 56)
    print(f"  现在开始，{secs:.0f} 秒窗口")
    print("  请按住遥控器语音键、说一句话、按住 3 秒再松手")
    print("  松手后隔 1 秒再按一次（第二次用来确认可复现）")
    print("=" * 56, flush=True)
    deadline = time.time() + secs
    next_tick = time.time() + 5
    msg = wt.MSG()
    while time.time() < deadline:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        now = time.time()
        if now >= next_tick:
            next_tick = now + 5
            left = deadline - now
            print(f"    …已跑 {secs - left:3.0f}s / 剩 {left:3.0f}s   已收 "
                  f"{sum(stats.values())} 个报告", flush=True)
        time.sleep(0.003)
    print("  窗口结束", flush=True)

    print("\n===== 报告尺寸分布 =====")
    if not stats:
        print("  一个报告都没收到（遥控器没按键？或不在线）")
    for size, n in sorted(stats.items()):
        mark = "  <- 非标准，可能是音频分片" if size != STANDARD else ""
        print(f"  {size:4d} 字节 × {n}{mark}")
    print(f"按键报告(16B): {keys}  非标准报告: {len(big)}")
    print(f"明细: {OUT}")
    try:
        input("按回车关闭…")
    except (EOFError, OSError):
        pass


if __name__ == "__main__":
    main()
