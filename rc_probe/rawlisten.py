"""遥控器按键探测：Raw Input 监听，只记录 VID_000D/PID_3838 这个 BLE 遥控器发出的键。
用法: python rawlisten.py <日志路径> [秒数]
"""
import ctypes
import ctypes.wintypes as wt
import json
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_INPUT = 0x00FF
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIM_TYPEKEYBOARD = 1
RIDI_DEVICENAME = 0x20000007
HWND_MESSAGE = wt.HWND(-3)
TARGET_MARK = "01000d_pid&3838"

VK_NAMES = {
    0x00: "(无)", 0x08: "Backspace", 0x09: "Tab", 0x0D: "Enter", 0x1B: "Esc",
    0x20: "Space", 0x21: "PageUp", 0x22: "PageDown", 0x23: "End", 0x24: "Home",
    0x25: "Left", 0x26: "Up", 0x27: "Right", 0x28: "Down", 0x2D: "Insert",
    0x2E: "Delete", 0x5B: "LWin", 0x5C: "RWin", 0x5D: "Apps",
    0x70: "F1", 0x71: "F2", 0x72: "F3", 0x73: "F4", 0x74: "F5", 0x75: "F6",
    0x76: "F7", 0x77: "F8", 0x78: "F9", 0x79: "F10", 0x7A: "F11", 0x7B: "F12",
    0xA0: "LShift", 0xA1: "RShift", 0xA2: "LCtrl", 0xA3: "RCtrl",
    0xA4: "LAlt", 0xA5: "RAlt", 0xAD: "Mute", 0xAE: "VolDown", 0xAF: "VolUp",
    0xB0: "NextTrack", 0xB1: "PrevTrack", 0xB2: "StopMedia", 0xB3: "PlayPause",
    0xA6: "BrowserBack", 0xA7: "BrowserForward", 0xA8: "BrowserRefresh",
    0xAC: "BrowserHome", 0xB4: "Mail", 0xB5: "MediaSelect", 0xB6: "App1", 0xB7: "App2",
}


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


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


# --- 关键：64 位下必须声明 restype/argtypes，否则句柄被截断 ---
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
user32.GetRawInputData.restype = wt.UINT
user32.GetRawInputData.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p,
                                   ctypes.POINTER(wt.UINT), wt.UINT]
user32.GetRawInputDeviceInfoW.restype = wt.UINT
user32.GetRawInputDeviceInfoW.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p,
                                          ctypes.POINTER(wt.UINT)]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.PeekMessageW.restype = wt.BOOL
user32.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]
user32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.restype = ctypes.c_ssize_t
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]

_devname_cache = {}


def device_name(h):
    key = int(h) if h else 0
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


out_path = sys.argv[1] if len(sys.argv) > 1 else "events.jsonl"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
out = open(out_path, "a", encoding="utf-8")
start = time.time()
seen_any = False


def wndproc(hwnd, msg, wparam, lparam):
    global seen_any
    if msg == WM_INPUT:
        size = wt.UINT(0)
        user32.GetRawInputData(wt.HANDLE(lparam), RID_INPUT, None, ctypes.byref(size),
                               ctypes.sizeof(RAWINPUTHEADER))
        if size.value:
            buf = ctypes.create_string_buffer(size.value)
            n = user32.GetRawInputData(wt.HANDLE(lparam), RID_INPUT, buf, ctypes.byref(size),
                                       ctypes.sizeof(RAWINPUTHEADER))
            if n == size.value:
                hdr = ctypes.cast(buf, ctypes.POINTER(RAWINPUTHEADER)).contents
                dev = device_name(hdr.hDevice)
                if TARGET_MARK in dev.lower() and hdr.dwType == RIM_TYPEKEYBOARD:
                    kb = ctypes.cast(ctypes.byref(buf, ctypes.sizeof(RAWINPUTHEADER)),
                                     ctypes.POINTER(RAWKEYBOARD)).contents
                    vk = kb.VKey
                    rec = {
                        "t": round(time.time() - start, 3),
                        "vkey": vk,
                        "name": VK_NAMES.get(vk, f"VK_0x{vk:02X}"),
                        "makecode": kb.MakeCode,
                        "flags": kb.Flags,
                        "msg": "down" if kb.Message == 0x0100 else (
                            "up" if kb.Message == 0x0101 else hex(kb.Message)),
                    }
                    line = json.dumps(rec, ensure_ascii=False)
                    print(line, flush=True)
                    out.write(line + "\n")
                    out.flush()
                    seen_any = True
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


wc = WNDCLASSW()
wc.lpfnWndProc = WNDPROC(wndproc)
wc.lpszClassName = "RCProbeWnd"
wc.hInstance = kernel32.GetModuleHandleW(None)
atom = user32.RegisterClassW(ctypes.byref(wc))
if not atom:
    print("RegisterClassW failed", ctypes.get_last_error(), flush=True)
    sys.exit(1)

hwnd = user32.CreateWindowExW(0, "RCProbeWnd", "RCProbe", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, None, wc.hInstance, None)
if not hwnd:
    print("CreateWindowExW failed", ctypes.get_last_error(), flush=True)
    sys.exit(1)

rid = RAWINPUTDEVICE(0x01, 0x06, RIDEV_INPUTSINK, hwnd)
if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)):
    print("RegisterRawInputDevices failed", ctypes.get_last_error(), flush=True)
    sys.exit(1)

print(f"LISTENING {duration}s -> {out_path}", flush=True)

msg = wt.MSG()
deadline = time.time() + duration
while time.time() < deadline:
    if user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        if msg.message == 0x0012:
            break
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))
    else:
        time.sleep(0.01)

print(f"DONE captured={seen_any}", flush=True)
out.close()
