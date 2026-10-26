"""遥控器按键学习工具：逐个提示按键，自动记录 vkey/扫描码，生成 keymap.json。

用法: python rc_probe/learn_keys.py
      （或双击 tools/learn_keys.bat，不需要管理员权限）

原理: 用 Raw Input 按设备路径过滤，只接收 VID_000D/PID_3838 这个遥控器的事件，
      不会和真实键盘互相干扰。
"""
import ctypes
import ctypes.wintypes as wt
import json
import queue
import sys
import time
from pathlib import Path

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_INPUT = 0x00FF
WM_DESTROY = 0x0002
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIM_TYPEKEYBOARD = 1
RIDI_DEVICENAME = 0x20000007
HWND_MESSAGE = wt.HWND(-3)
TARGET_MARK = "01000d_pid&3838"

OUT = Path(__file__).resolve().parent / "keymap.json"

# (key_id, 提示文字, 是否长按, 等待秒数)
STEPS = [
    ("voice",    "语音键（按住 3 秒，随便说句话，然后松手）", True,  9.0),
    ("vol_up",   "音量 +",        False, 7.0),
    ("vol_down", "音量 -",        False, 7.0),
    ("power",    "电源键",        False, 7.0),
    ("mute",     "静音键",        False, 7.0),
    ("source",   "信号源键",      False, 7.0),
]


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE), ("wParam", wt.WPARAM)]


class RAWKEYBOARD(ctypes.Structure):
    _fields_ = [("MakeCode", wt.USHORT), ("Flags", wt.USHORT),
                ("Reserved", wt.USHORT), ("VKey", wt.USHORT),
                ("Message", wt.UINT), ("ExtraInformation", wt.ULONG)]


WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

events: "queue.Queue[dict]" = queue.Queue()
_devname_cache: dict = {}

user32.GetRawInputData.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p,
                                   ctypes.POINTER(wt.UINT), wt.UINT]
user32.GetRawInputData.restype = wt.UINT
user32.GetRawInputDeviceInfoW.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p,
                                          ctypes.POINTER(wt.UINT)]
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t


def device_name(h) -> str:
    key = h or 0
    if key in _devname_cache:
        return _devname_cache[key]
    size = wt.UINT(0)
    user32.GetRawInputDeviceInfoW(wt.HANDLE(key), RIDI_DEVICENAME, None, ctypes.byref(size))
    name = ""
    if size.value > 1:
        buf = ctypes.create_unicode_buffer(size.value)
        if user32.GetRawInputDeviceInfoW(wt.HANDLE(key), RIDI_DEVICENAME, buf,
                                         ctypes.byref(size)) > 0:
            name = buf.value
    _devname_cache[key] = name
    return name


def wndproc(hwnd, msg, wparam, lparam):
    if msg == WM_INPUT:
        size = wt.UINT(0)
        user32.GetRawInputData(wt.HANDLE(lparam), RID_INPUT, None, ctypes.byref(size),
                               ctypes.sizeof(RAWINPUTHEADER))
        if size.value:
            buf = ctypes.create_string_buffer(size.value)
            got = user32.GetRawInputData(wt.HANDLE(lparam), RID_INPUT, buf,
                                         ctypes.byref(size),
                                         ctypes.sizeof(RAWINPUTHEADER))
            if got == size.value:
                hdr = ctypes.cast(buf, ctypes.POINTER(RAWINPUTHEADER)).contents
                if TARGET_MARK in device_name(hdr.hDevice).lower():
                    if hdr.dwType == RIM_TYPEKEYBOARD:
                        kb = ctypes.cast(ctypes.addressof(buf) + ctypes.sizeof(RAWINPUTHEADER),
                                         ctypes.POINTER(RAWKEYBOARD)).contents
                        events.put({"t": time.time(), "vkey": kb.VKey,
                                    "scan": kb.MakeCode, "flags": kb.Flags,
                                    "down": kb.Message in (0x0100, 0x0104)})
    elif msg == WM_DESTROY:
        user32.PostQuitMessage(0)
        return 0
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


wc = wt.WNDCLASSW()
wc.lpfnWndProc = WNDPROC(wndproc)
wc.lpszClassName = "RCLearnWnd"
wc.hInstance = kernel32.GetModuleHandleW(None)
user32.RegisterClassW(wc)
hwnd = user32.CreateWindowExW(0, "RCLearnWnd", "RCLearn", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, None, wc.hInstance, None)

rid = RAWINPUTDEVICE(0x01, 0x06, RIDEV_INPUTSINK, hwnd)
if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)):
    print("RegisterRawInputDevices failed", ctypes.get_last_error())
    sys.exit(1)


def pump() -> None:
    """处理所有挂起的 Raw Input 消息。"""
    msg = wt.MSG()
    while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))


def wait_step(label: str, hold: bool, timeout: float) -> dict | None:
    """等一个键，返回统计结果。"""
    t0 = time.time()
    first = None
    downs = 0
    ups = 0
    while time.time() - t0 < timeout:
        pump()
        while not events.empty():
            ev = events.get()
            if first is None:
                if not ev["down"]:
                    continue
                first = ev
                downs = 1
                t_first = ev["t"]
            else:
                if ev["vkey"] != first["vkey"]:
                    continue
                if ev["down"]:
                    downs += 1
                else:
                    ups += 1
        if first is not None and not hold:
            # 短按键：收到 up 就结束
            if ups > 0:
                break
        if first is not None and hold:
            # 长按键：等到松手后再多等 1.2s 收尾
            if ups > 0 and time.time() - t_first > 0.5:
                break
    if first is None:
        return None
    # 清空残留
    while not events.empty():
        events.get()
    return {"vkey": first["vkey"], "scan": first["scan"],
            "down_count": downs, "up_count": ups,
            "duration_ms": round((time.time() - t_first) * 1000)}


print("=" * 62)
print("  遥控器按键学习")
print("  只接收 VID_000D/PID_3838 这个遥控器的按键，不影响真实键盘")
print("  每一步按提示按键；8 秒内没按就自动跳过")
print("=" * 62)
print()

result = {}
for key_id, label, hold, timeout in STEPS:
    print(f">>> 请按【{label}】")
    info = wait_step(label, hold, timeout)
    if info is None:
        print("    (跳过)\n")
        continue
    result[key_id] = info
    tag = f"连发 {info['down_count']} 次" if info["down_count"] > 1 else "单次"
    print(f"    OK  vkey={info['vkey']} (0x{info['vkey']:02X})  "
          f"scan=0x{info['scan']:02X}  {tag}  {info['duration_ms']}ms\n")

OUT.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print("=" * 62)
print(f"已写入 {OUT}")
print(json.dumps(result, ensure_ascii=False, indent=2))
