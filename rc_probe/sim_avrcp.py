"""模拟蓝牙耳机 AVRCP：注入一帧带 INJ 标记的 0xB3 keydown，不发 keyup。

用法: python rc_probe/sim_avrcp.py [次数] [间隔秒]
"""
import ctypes
import sys
import time
from ctypes import wintypes as wt

u = ctypes.WinDLL("user32", use_last_error=True)

INPUT_KEYBOARD = 1
KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_SCANCODE = 0x0008
VK_MEDIA_PLAY_PAUSE = 0xB3


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class UNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", UNION)]


u.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
u.SendInput.restype = wt.UINT
u.MapVirtualKeyW.argtypes = [wt.UINT, wt.UINT]
u.MapVirtualKeyW.restype = wt.UINT

count = int(sys.argv[1]) if len(sys.argv) > 1 else 1
gap = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
scan = u.MapVirtualKeyW(VK_MEDIA_PLAY_PAUSE, 0)

for i in range(count):
    event = INPUT()
    event.type = INPUT_KEYBOARD
    event.u.ki = KEYBDINPUT(VK_MEDIA_PLAY_PAUSE, scan,
                            KEYEVENTF_EXTENDEDKEY | KEYEVENTF_SCANCODE, 0, 0)
    sent = u.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT))
    print(f"{i + 1}/{count} SendInput={sent} scan=0x{scan:02X}", flush=True)
    if i + 1 < count:
        time.sleep(gap)
