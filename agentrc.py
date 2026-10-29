"""AgentRMC - 把 BLE 遥控器（极米/小米系 Realtek 方案）改造成电脑 Agent 的输入终端。

工作方式
--------
1. 用 Raw Input 按设备路径过滤，只接收遥控器的按键，不干扰真实键盘鼠标。
2. 语音键被按住时会以 ~31ms 间隔连发同一个键值（默认 F5），据此判定"长按"：
   - 第一次 keydown 先挂起，250ms 内等到第二次 → 开始语音
   - 等不到 → 单击，什么都不做（原生 F5 已经由 Windows 发给前台窗口了）
3. 语音有两种接法：
   - hold   —— 按住目标程序的"按住说话"键，松手放开（老式 PTT）
   - toggle —— 按下发一次快捷键、松手再发一次（Qoder 的语音是"再按一次停止"的开关）
4. 松手后录音送本地 whisper 转写，结果写入剪贴板并粘贴到当前焦点窗口。
5. 其余按键按 profiles.json 的映射转成系统动作，支持长按连发、点击/长按双动作。

用法
----
    python agentrc.py              正常运行
    python agentrc.py --list-mic   列出麦克风
    python agentrc.py --list-keys  打印收到的所有按键（调试）
    python agentrc.py --test-mic   录 5 秒并转写，验证语音链路
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path

try:
    import winsound
except ImportError:  # 非 Windows
    winsound = None

def _app_dir() -> Path:
    """打包成 exe 后，配置和模型放在 exe 同目录，而不是临时解包目录。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


HERE = _app_dir()
CONFIG_PATH = HERE / "config.json"
COMMANDS_PATH = HERE / "commands.json"

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# --------------------------------------------------------------------------
# 配置
# --------------------------------------------------------------------------
DEFAULT_CONFIG = {
    "device_mark": "01000d_pid&3838",
    "voice_key": {
        "vkey": 0x74,          # F5
        "confirm_window_ms": 250,
        "min_repeats": 2,
        "silence_stop_ms": 700,
        "min_record_ms": 300,
    },
    "keys": {
        "38": "up", "40": "down", "37": "left", "39": "right",
        "13": "confirm", "27": "back", "93": "menu", "36": "home",
    },
    "actions": {
        "up":      {"type": "key", "vk": 0x26},
        "down":    {"type": "key", "vk": 0x28},
        "left":    {"type": "key", "vk": 0x25},
        "right":   {"type": "key", "vk": 0x27},
        "confirm": {"type": "key", "vk": 0x0D},
        "back":    {"type": "key", "vk": 0x1B},
        "menu":    {"type": "key", "vk": 0x5D},
        "home":    {"type": "key", "vk": 0x5B},
    },
    "voice": {
        "input_device": None,
        "sample_rate": 16000,
        "asr_model": "small",
        "asr_language": "zh",
        "proxy": "http://127.0.0.1:7897",
        "paste": True,
        "auto_enter": False,
    },
    "ui": {
        "auto_listen": True,
        "start_minimized": True,
    },
}


def load_config() -> dict:
    if CONFIG_PATH.exists():
        try:
            user_cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except Exception as e:  # noqa: BLE001
            print(f"[!] config.json 解析失败，用默认值: {e}")
            return dict(DEFAULT_CONFIG)
        merged = dict(DEFAULT_CONFIG)
        for k, v in user_cfg.items():
            if isinstance(v, dict) and isinstance(merged.get(k), dict):
                merged[k] = {**merged[k], **v}
            else:
                merged[k] = v
        return merged
    CONFIG_PATH.write_text(json.dumps(DEFAULT_CONFIG, ensure_ascii=False, indent=2),
                           encoding="utf-8")
    print(f"[i] 已生成默认配置 {CONFIG_PATH}")
    return dict(DEFAULT_CONFIG)


CFG = load_config()

# --------------------------------------------------------------------------
# 语音指令规则：识别出的文本先过一遍本地规则，命中就直接执行，否则交给 Agent
# --------------------------------------------------------------------------
DEFAULT_COMMANDS = {
    "_comment": "match 是关键词列表，命中任一即触发。action.type 支持 run / key / volume / paste / enter",
    "rules": [
        {"match": ["打开浏览器", "开浏览器", "上网"],
         "action": {"type": "run", "cmd": "start msedge"}},
        {"match": ["打开记事本", "记事本"],
         "action": {"type": "run", "cmd": "notepad"}},
        {"match": ["打开计算器", "计算器"],
         "action": {"type": "run", "cmd": "calc"}},
        {"match": ["打开资源管理器", "我的电脑", "文件管理"],
         "action": {"type": "run", "cmd": "explorer"}},
        {"match": ["截图", "截屏"],
         "action": {"type": "key", "vk": 0x53, "mods": ["win", "shift"]}},
        {"match": ["静音", "取消静音"],
         "action": {"type": "key", "vk": 0xAD}},
        {"match": ["音量加", "大声点", "音量调高"],
         "action": {"type": "key", "vk": 0xAF, "repeat": 3}},
        {"match": ["音量减", "小声点", "音量调低"],
         "action": {"type": "key", "vk": 0xAE, "repeat": 3}},
        {"match": ["播放", "暂停", "继续播放"],
         "action": {"type": "key", "vk": 0xB3}},
        {"match": ["锁屏", "锁定电脑"],
         "action": {"type": "run", "cmd": "rundll32.exe user32.dll,LockWorkStation"}},
    ],
    "fallback": "paste",
}


def load_commands() -> list:
    if not COMMANDS_PATH.exists():
        COMMANDS_PATH.write_text(json.dumps(DEFAULT_COMMANDS, ensure_ascii=False, indent=2),
                                 encoding="utf-8")
        print(f"[i] 已生成默认指令表 {COMMANDS_PATH}")
        return DEFAULT_COMMANDS["rules"]
    try:
        data = json.loads(COMMANDS_PATH.read_text(encoding="utf-8"))
        return data.get("rules", [])
    except Exception as e:  # noqa: BLE001
        print(f"[!] commands.json 解析失败，忽略本地指令: {e}")
        return []


MOD_VK = {"win": 0x5B, "ctrl": 0x11, "alt": 0x12, "shift": 0x10}
FILLERS = ["帮我", "请", "麻烦", "能不能", "可以", "一下", "一个", "个", "吧", "呢"]


def normalize(text: str) -> str:
    """去掉口语填充词，"截个图" -> "截图"。"""
    out = text
    for f in FILLERS:
        out = out.replace(f, "")
    return out


def press_combo(mods: list, vk: int, repeat: int = 1) -> None:
    for _ in range(max(1, repeat)):
        for m in mods:
            mv = MOD_VK.get(m.lower())
            if mv:
                user32.keybd_event(mv, 0, 0, 0)
        user32.keybd_event(vk, 0, 0, 0)
        time.sleep(0.02)
        user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)
        for m in reversed(mods):
            mv = MOD_VK.get(m.lower())
            if mv:
                user32.keybd_event(mv, 0, KEYEVENTF_KEYUP, 0)
        time.sleep(0.05)


# ---- SendInput：比 keybd_event 更贴近真实按键（带扫描码，Chromium/Electron 才认） ----
KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_EXTENDEDKEY = 0x0001
INPUT_KEYBOARD = 1
MAPVK_VK_TO_VSC = 0
EXTENDED_VKS = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                0x2D, 0x2E, 0x5B, 0x5C, 0x5D, 0x6F, 0x90, 0x91,
                0xA3, 0xA5, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB3}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _INPUTunion(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _INPUTunion)]


user32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
user32.SendInput.restype = wt.UINT
user32.MapVirtualKeyW.argtypes = [wt.UINT, wt.UINT]
user32.MapVirtualKeyW.restype = wt.UINT


def _send_key(vk: int, up: bool = False, extra: int = 0) -> None:
    scan = user32.MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 0xFF
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0)
    if vk in EXTENDED_VKS:
        flags |= KEYEVENTF_EXTENDEDKEY
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.u.ki = KEYBDINPUT(wVk=vk, wScan=scan, dwFlags=flags, time=0, dwExtraInfo=extra)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def hold_combo(mods: list, vk: int) -> None:
    """按住不放：修饰键和主键都只发 keydown，等 release_combo 收尾。

    用 SendInput + 扫描码，Electron/Chromium 这类应用才认（keybd_event 只给 vk
    时，Chromium 可能拿不到正确的 code，表现为"按了没反应"）。
    """
    for m in mods:
        mv = MOD_VK.get(m.lower())
        if mv:
            _send_key(mv)
            time.sleep(0.01)
    _send_key(vk)


def release_combo(mods: list, vk: int) -> None:
    _send_key(vk, up=True)
    for m in reversed(mods):
        mv = MOD_VK.get(m.lower())
        if mv:
            _send_key(mv, up=True)


def tap_combo(mods: list, vk: int, hold_ms: int = 18) -> None:
    """完整按一下组合键（按下 + 松开）。

    开关式语音（Qoder）需要的是两次独立的 keydown：按下开始、再按一次停止。
    """
    for m in mods:
        mv = MOD_VK.get(m.lower())
        if mv:
            _send_key(mv)
            time.sleep(0.008)
    _send_key(vk)
    time.sleep(max(0.01, hold_ms / 1000))
    _send_key(vk, up=True)
    for m in reversed(mods):
        mv = MOD_VK.get(m.lower())
        if mv:
            _send_key(mv, up=True)


# --------------------------------------------------------------------------
# 前台窗口识别 + 场景 Profile
# --------------------------------------------------------------------------
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
MOUSEEVENTF_WHEEL = 0x0800

user32.GetForegroundWindow.restype = wt.HWND
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowTextLengthW.argtypes = [wt.HWND]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR,
                                                ctypes.POINTER(wt.DWORD)]


def foreground_info() -> tuple[str, str]:
    """返回 (进程文件名, 窗口标题)。"""
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return "", ""
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    exe = ""
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if h:
        buf = ctypes.create_unicode_buffer(512)
        size = wt.DWORD(512)
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            exe = os.path.basename(buf.value)
        kernel32.CloseHandle(h)
    title = ""
    n = user32.GetWindowTextLengthW(hwnd)
    if n > 0:
        tbuf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, tbuf, n + 1)
        title = tbuf.value
    return exe, title


def scroll(delta: int) -> None:
    """滚轮，正数向上。一格 120。"""
    user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, delta, 0)


# --------------------------------------------------------------------------
# 显示器亮度（DDC/CI，主显示器）与鼠标相对移动
# --------------------------------------------------------------------------
MOUSEEVENTF_MOVE = 0x0001

user32.MonitorFromPoint.restype = wt.HMONITOR
user32.MonitorFromPoint.argtypes = [wt.POINT, wt.DWORD]


class _PHYSICAL_MONITOR(ctypes.Structure):
    _fields_ = [("hPhysicalMonitor", wt.HANDLE),
                ("szPhysicalMonitorDescription", wt.WCHAR * 128)]


try:
    _dxva2 = ctypes.WinDLL("dxva2")
    _dxva2.GetPhysicalMonitorsFromHMONITOR.argtypes = [
        wt.HMONITOR, wt.DWORD, ctypes.POINTER(_PHYSICAL_MONITOR)]
    _dxva2.GetPhysicalMonitorsFromHMONITOR.restype = wt.BOOL
    _dxva2.GetNumberOfPhysicalMonitorsFromHMONITOR.argtypes = [
        wt.HMONITOR, ctypes.POINTER(wt.DWORD)]
    _dxva2.GetNumberOfPhysicalMonitorsFromHMONITOR.restype = wt.BOOL
    _dxva2.GetVCPFeatureAndVCPFeatureReply.argtypes = [
        wt.HANDLE, wt.BYTE, ctypes.POINTER(wt.DWORD),
        ctypes.POINTER(wt.DWORD), ctypes.POINTER(wt.DWORD)]
    _dxva2.GetVCPFeatureAndVCPFeatureReply.restype = wt.BOOL
    _dxva2.SetVCPFeature.argtypes = [wt.HANDLE, wt.BYTE, wt.DWORD]
    _dxva2.SetVCPFeature.restype = wt.BOOL
except OSError:  # 没有 dxva2 时亮度动作静默失效
    _dxva2 = None

_brightness_monitor = None


def _primary_monitor() -> int | None:
    global _brightness_monitor
    if _brightness_monitor:
        return _brightness_monitor
    if _dxva2 is None:
        return None
    hmon = user32.MonitorFromPoint(wt.POINT(0, 0), 1)  # PRIMARY
    if not hmon:
        return None
    n = wt.DWORD()
    if not _dxva2.GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, ctypes.byref(n)) or not n.value:
        return None
    arr = (_PHYSICAL_MONITOR * n.value)()
    if not _dxva2.GetPhysicalMonitorsFromHMONITOR(hmon, n.value, arr):
        return None
    _brightness_monitor = arr[0].hPhysicalMonitor
    return _brightness_monitor


def brightness(delta: int) -> bool:
    """主显示器亮度 ±delta（0~100），不支持 DDC/CI 时返回 False。"""
    h = _primary_monitor()
    if not h:
        return False
    for _ in range(2):  # DDC/CI 偶发超时，重试一次
        cur, mx, typ = wt.DWORD(), wt.DWORD(), wt.DWORD()
        if _dxva2.GetVCPFeatureAndVCPFeatureReply(
                h, 0x10, ctypes.byref(typ), ctypes.byref(cur), ctypes.byref(mx)):
            top = mx.value or 100
            val = max(0, min(top, cur.value + delta))
            return bool(_dxva2.SetVCPFeature(h, 0x10, val))
        time.sleep(0.02)
    return False


def mouse_move(dx: int, dy: int) -> None:
    user32.mouse_event(MOUSEEVENTF_MOVE, dx, dy, 0, 0)


def mouse_click() -> None:
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.02)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


# --------------------------------------------------------------------------
# 鼠标模式的全局低层键盘钩子：吞掉原生方向键/Enter，并直接驱动鼠标。
# 只拦方向键与 Enter；返回/菜单放行，退出仍由 Raw Input 处理。
# --------------------------------------------------------------------------
WH_KEYBOARD_LL = 13
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_SYSKEYDOWN, WM_SYSKEYUP = 0x0104, 0x0105
LLKHF_INJECTED = 0x10
CURSOR_KEYS = {0x25: (-8, 0), 0x26: (0, -8), 0x27: (8, 0), 0x28: (0, 8)}


class _KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", wt.DWORD), ("scanCode", wt.DWORD),
                ("flags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


_HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, wt.WPARAM, wt.LPARAM)
_cursor_hook = None
_cursor_proc = None
_cursor_last = 0.0

user32.SetWindowsHookExW.restype = wt.HHOOK
user32.SetWindowsHookExW.argtypes = [ctypes.c_int, _HOOKPROC, wt.HINSTANCE, wt.DWORD]
user32.UnhookWindowsHookEx.argtypes = [wt.HHOOK]
user32.CallNextHookEx.argtypes = [wt.HHOOK, ctypes.c_int, wt.WPARAM, wt.LPARAM]
user32.CallNextHookEx.restype = ctypes.c_ssize_t


def _cursor_hook_cb(nCode, wParam, lParam):
    global _cursor_last
    if nCode == 0:
        kb = ctypes.cast(lParam, ctypes.POINTER(_KBDLLHOOKSTRUCT)).contents
        if not (kb.flags & LLKHF_INJECTED):
            vk = int(kb.vkCode)
            if vk in CURSOR_KEYS or vk == 0x0D:
                if wParam in (WM_KEYDOWN, WM_SYSKEYDOWN):
                    _cursor_last = time.time()
                    if vk == 0x0D:
                        mouse_click()
                    else:
                        mouse_move(*CURSOR_KEYS[vk])
                return 1
    return user32.CallNextHookEx(None, nCode, wParam, lParam)


def cursor_hook_start() -> bool:
    global _cursor_hook, _cursor_proc
    if _cursor_hook:
        return True
    _cursor_proc = _HOOKPROC(_cursor_hook_cb)
    _cursor_hook = user32.SetWindowsHookExW(WH_KEYBOARD_LL, _cursor_proc, None, 0)
    return bool(_cursor_hook)


def cursor_hook_stop() -> None:
    global _cursor_hook
    if _cursor_hook:
        user32.UnhookWindowsHookEx(_cursor_hook)
        _cursor_hook = None


# --------------------------------------------------------------------------
# 点击前台窗口里的相对位置（把焦点送回 Qoder 聊天框）
# --------------------------------------------------------------------------
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.GetDpiForWindow.argtypes = [wt.HWND]
user32.GetDpiForWindow.restype = wt.UINT


def click_foreground(rel_x: float = 0.68, from_bottom: float = 70) -> bool:
    """在前台窗口里点一下，用于把焦点放回聊天输入框。

    rel_x       —— 客户区宽度比例（0~1），默认偏右，避开左侧编辑器
    from_bottom —— 距客户区底部的逻辑像素，按窗口 DPI 换算成物理像素
    """
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return False
    rect = wt.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
        return False
    w = rect.right - rect.left
    h = rect.bottom - rect.top
    if w < 40 or h < 40:
        return False
    origin = wt.POINT(0, 0)
    if not user32.ClientToScreen(hwnd, ctypes.byref(origin)):
        return False
    try:
        dpi = user32.GetDpiForWindow(hwnd) or 96
    except Exception:  # noqa: BLE001
        dpi = 96
    offset = int(round(max(1.0, from_bottom) * dpi / 96.0))
    x = origin.x + int(w * max(0.0, min(1.0, rel_x)))
    y = origin.y + h - max(1, offset)
    # 不把光标挪回原位：如果你的任务栏在屏幕边缘且自动隐藏，挪回去会把任务栏蹭出来。
    # 光标留在聊天输入区正好是下一次说话的位置。
    user32.SetCursorPos(x, y)
    time.sleep(0.012)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.012)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.012)
    return True


def voice_focus_click(profile: dict | None = None) -> tuple[float, float] | None:
    """profile 里 voice.focus_click = [rel_x, from_bottom]；没有就返回 None。"""
    v = (profile or {}).get("voice") or {}
    fc = v.get("focus_click")
    if not fc:
        return None
    try:
        return float(fc[0]), float(fc[1])
    except (TypeError, ValueError, IndexError):
        return None


user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]


def taskbar_state() -> str:
    """任务栏窗口的可见标志和矩形，用来排查"按语音键弹任务栏"。"""
    h = user32.FindWindowW("Shell_TrayWnd", None)
    if not h:
        return ""
    r = wt.RECT()
    if not user32.GetWindowRect(h, ctypes.byref(r)):
        return ""
    return (f"vis={int(bool(user32.IsWindowVisible(h)))}"
            f":{r.left},{r.top},{r.right},{r.bottom}")


PROFILES_PATH = HERE / "profiles.json"

DEFAULT_PROFILES = {
    "_comment": "按前台进程名/窗口标题匹配，命中第一个即用；都不中则用 default",
    "profiles": [
        {
            "name": "browser",
            "match": {"process": ["chrome.exe", "msedge.exe", "firefox.exe", "brave.exe"]},
            "keys": {"38": "scroll_up", "40": "scroll_down", "37": "back", "39": "forward",
                     "13": "enter", "27": "esc", "36": "new_tab", "93": "menu"},
            "actions": {
                "scroll_up": {"type": "scroll", "delta": 360},
                "scroll_down": {"type": "scroll", "delta": -360},
                "back": {"type": "key", "vk": 0x25, "mods": ["alt"]},
                "forward": {"type": "key", "vk": 0x27, "mods": ["alt"]},
                "enter": {"type": "key", "vk": 0x0D},
                "esc": {"type": "key", "vk": 0x1B},
                "new_tab": {"type": "key", "vk": 0x54, "mods": ["ctrl"]},
                "menu": {"type": "key", "vk": 0x5D}
            }
        },
        {
            "name": "editor",
            "match": {"process": ["code.exe", "cursor.exe", "windsurf.exe", "devenv.exe"]},
            "keys": {"38": "prev_editor", "40": "next_editor", "37": "prev_tab",
                     "39": "next_tab", "13": "enter", "27": "esc", "36": "palette",
                     "93": "menu"},
            "actions": {
                "prev_editor": {"type": "key", "vk": 0x21, "mods": ["ctrl"]},
                "next_editor": {"type": "key", "vk": 0x22, "mods": ["ctrl"]},
                "prev_tab": {"type": "key", "vk": 0x09, "mods": ["ctrl", "shift"]},
                "next_tab": {"type": "key", "vk": 0x09, "mods": ["ctrl"]},
                "enter": {"type": "key", "vk": 0x0D},
                "esc": {"type": "key", "vk": 0x1B},
                "palette": {"type": "key", "vk": 0x50, "mods": ["ctrl", "shift"]},
                "menu": {"type": "key", "vk": 0x5D}
            }
        },
        {
            "name": "terminal",
            "match": {"process": ["windowsterminal.exe", "cmd.exe", "powershell.exe",
                                  "pwsh.exe", "conhost.exe"]},
            "keys": {"38": "hist_prev", "40": "hist_next", "37": "word_left",
                     "39": "word_right", "13": "enter", "27": "esc", "36": "clear"},
            "actions": {
                "hist_prev": {"type": "key", "vk": 0x26},
                "hist_next": {"type": "key", "vk": 0x28},
                "word_left": {"type": "key", "vk": 0x25, "mods": ["ctrl"]},
                "word_right": {"type": "key", "vk": 0x27, "mods": ["ctrl"]},
                "enter": {"type": "key", "vk": 0x0D},
                "esc": {"type": "key", "vk": 0x1B},
                "clear": {"type": "key", "vk": 0x4C, "mods": ["ctrl"]}
            }
        },
        {
            "name": "qoder",
            "match": {"process": ["qoder.exe", "qoder cn.exe"]},
            "keys": {"38": "scroll_up", "40": "scroll_down", "37": "cursor_left",
                     "39": "cursor_right", "13": "send", "27": "back",
                     "93": "newline", "36": "chat_top", "122": "fullscreen"},
            "nav": {"exit_key": 36,
                    "keys": {"38": "nav_top", "40": "nav_bottom",
                             "37": "nav_prev", "39": "nav_next"}},
            "actions": {
                "scroll_up": {"type": "scroll", "delta": 300,
                              "hold_repeat": 100, "hold_accel": True},
                "scroll_down": {"type": "scroll", "delta": -300,
                                "hold_repeat": 100, "hold_accel": True},
                "cursor_left": {"type": "none", "note": "原生 ← 移动光标"},
                "cursor_right": {"type": "none", "note": "原生 → 移动光标"},
                "send": {"type": "none", "note": "原生 Enter 发送"},
                "back": {"type": "none", "note": "原生 Esc 停止/返回"},
                "newline": {"type": "key", "vk": 0x0D, "mods": ["shift"]},
                "fullscreen": {"type": "none", "note": "原生 F11 直通"},
                "nav_top": {"type": "key", "vk": 0x21, "label": "上翻一页"},
                "nav_bottom": {"type": "key", "vk": 0x22, "label": "下翻一页"},
                "nav_prev": {"type": "key", "vk": 0x09, "mods": ["shift"],
                             "label": "上一个控件"},
                "nav_next": {"type": "key", "vk": 0x09, "label": "下一个控件"},
                "nav_exit": {"type": "nav_exit", "label": "退出导航"},
                "chat_top": {
                    "type": "scroll", "delta": 9000, "repeat": 3, "label": "滚到顶"
                },
            },
            "voice": {"hotkey": {"mods": ["ctrl", "shift"], "vk": 0x56,
                                 "trigger": "toggle"}}
        },
        {
            "name": "agent",
            "extends": "qoder",
            "match": {"process": ["workbuddyai.exe", "workbuddy.exe"],
                      "title": ["workbuddy"]},
            "voice": {"hotkey": {"mods": ["ctrl"], "vk": 0x44,
                                 "trigger": "toggle"}},
        },
        {
            "name": "default",
            "match": {},
            "keys": {"38": "up", "40": "down", "37": "left", "39": "right",
                     "13": "confirm", "27": "back", "93": "menu", "36": "home"},
            "actions": {
                "up": {"type": "key", "vk": 0x26},
                "down": {"type": "key", "vk": 0x28},
                "left": {"type": "key", "vk": 0x25},
                "right": {"type": "key", "vk": 0x27},
                "confirm": {"type": "key", "vk": 0x0D},
                "back": {"type": "key", "vk": 0x1B},
                "menu": {"type": "key", "vk": 0x5D},
                "home": {"type": "none", "note": "原生 Home（不再注入 Win）"}
            }
        }
    ]
}

# 变焦/对焦键（vkey 122 = F11）：所有 profile 统一补一条，按下即切全屏
for _prof in DEFAULT_PROFILES["profiles"]:
    _prof.setdefault("keys", {})["122"] = "fullscreen"
    _prof.setdefault("actions", {})["fullscreen"] = {"type": "none",
                                                    "note": "原生 F11 直通"}


def resolve_profiles(profiles: list) -> list:
    """解析 extends：子 profile 继承父 profile 的 keys/actions/voice，再覆盖。

    顺序仍然是优先级，extends 只负责合并字段，不改变匹配顺序。
    """
    by_name = {p.get("name"): p for p in profiles if p.get("name")}
    resolved: dict = {}

    def resolve(p: dict, depth: int = 0) -> dict:
        name = p.get("name")
        if name in resolved:
            return resolved[name]
        base: dict = {}
        parent = by_name.get(p.get("extends")) if depth < 4 else None
        if parent is not None and parent is not p:
            base = resolve(parent, depth + 1)
        merged = {**base, **p}
        merged["keys"] = {**(base.get("keys") or {}), **(p.get("keys") or {})}
        merged["actions"] = {**(base.get("actions") or {}), **(p.get("actions") or {})}
        if base.get("voice") and p.get("voice"):
            merged["voice"] = {**base["voice"], **p["voice"]}
        if name:
            resolved[name] = merged
        return merged

    return [resolve(p) for p in profiles]


def load_profiles() -> list:
    if not PROFILES_PATH.exists():
        PROFILES_PATH.write_text(json.dumps(DEFAULT_PROFILES, ensure_ascii=False, indent=2),
                                 encoding="utf-8")
        print(f"[i] 已生成默认场景表 {PROFILES_PATH}")
        return resolve_profiles(DEFAULT_PROFILES["profiles"])
    try:
        data = json.loads(PROFILES_PATH.read_text(encoding="utf-8"))
        return resolve_profiles(data.get("profiles", []))
    except Exception as e:  # noqa: BLE001
        print(f"[!] profiles.json 解析失败，用内置默认: {e}")
        return resolve_profiles(DEFAULT_PROFILES["profiles"])


class ProfileManager:
    CACHE_TTL = 0.3

    def __init__(self, profiles: list):
        self.profiles = profiles
        self.default = next((p for p in profiles if p.get("name") == "default"),
                            profiles[-1] if profiles else {"keys": {}, "actions": {}})
        self._exe = ""
        self._title = ""
        self._ts = 0.0
        self._prof = self.default

    def current(self, force: bool = False) -> dict:
        now = time.time()
        if not force and now - self._ts < self.CACHE_TTL:
            return self._prof
        self._exe, self._title = foreground_info()
        for p in self.profiles:
            m = p.get("match") or {}
            procs = [x.lower() for x in m.get("process", [])]
            if procs and self._exe.lower() in procs:
                self._prof, self._ts = p, now
                return p
            titles = [x.lower() for x in m.get("title", [])]
            if titles and any(t in self._title.lower() for t in titles):
                self._prof, self._ts = p, now
                return p
        self._prof, self._ts = self.default, now
        return self.default

    def describe(self) -> str:
        p = self.current()
        return f"{p.get('name', '?')} ({self._exe or 'unknown'})"


class RuleEngine:
    def __init__(self, rules: list):
        self.rules = rules

    def match(self, text: str) -> dict | None:
        if not text:
            return None
        for candidate in (text, normalize(text)):
            for r in self.rules:
                for kw in r.get("match", []):
                    if kw and kw in candidate:
                        return r
        return None

    def dispatch(self, text: str) -> bool:
        """命中规则则执行并返回 True。"""
        rule = self.match(text)
        if rule is None:
            return False
        action = rule.get("action", {})
        kind = action.get("type")
        desc = "/".join(rule.get("match", [])[:2])
        print(f"⚡ 命中指令 [{desc}] -> {kind}")
        try:
            if kind == "run":
                subprocess.Popen(action["cmd"], shell=True)
            elif kind == "key":
                press_combo(action.get("mods", []), int(action["vk"]),
                            int(action.get("repeat", 1)))
            elif kind == "paste":
                paste_text(text, bool(action.get("enter", False)))
            elif kind == "enter":
                paste_text(text, True)
            else:
                print(f"[!] 未知动作类型 {kind}")
                return False
        except Exception as e:  # noqa: BLE001
            print(f"[!] 执行失败: {e}")
            return False
        return True

# --------------------------------------------------------------------------
# 语音键的接法：按住（hold）或开关（toggle）
# --------------------------------------------------------------------------
DEFAULT_HOTKEY = {"vk": 0x56, "mods": ["ctrl", "shift"], "trigger": "hold"}


def voice_hotkey(profile: dict | None = None) -> tuple[list, int, str]:
    """返回 (mods, vk, trigger)。

    profile 里的 voice 可以覆盖全局配置，例如 Qoder 的语音是"再按一次停止"：
        "voice": {"trigger": "toggle"}
    """
    base = {**DEFAULT_HOTKEY, **(CFG["voice"].get("hotkey") or {})}
    over = (profile or {}).get("voice") or {}
    hk = {**base, **(over.get("hotkey") or {})}
    trigger = str(over.get("trigger", hk.get("trigger", "hold"))).lower()
    if trigger not in ("hold", "toggle"):
        trigger = "hold"
    return [str(m).lower() for m in hk.get("mods", [])], int(hk.get("vk", 0x56)), trigger


# --------------------------------------------------------------------------
# 动作描述（按键映射页 / 事件流共用）
# --------------------------------------------------------------------------
VK_LABEL = {
    0x08: "BACKSPACE", 0x09: "TAB", 0x0D: "ENTER", 0x1B: "ESC", 0x20: "SPACE",
    0x21: "PGUP", 0x22: "PGDN", 0x23: "END", 0x24: "HOME",
    0x25: "LEFT", 0x26: "UP", 0x27: "RIGHT", 0x28: "DOWN", 0x2E: "DELETE",
    0x5B: "WIN", 0x5D: "APPS", 0xAD: "MUTE", 0xAE: "VOL-", 0xAF: "VOL+",
    0xB3: "PLAY",
}


def describe_action(act: dict, hold_info: bool = True) -> str:
    """把动作描述成人话，给按键映射页和事件流用。"""
    if not act:
        return "无动作"
    kind = act.get("type")
    label = str(act.get("label") or "").strip()
    if label:
        text = label
    elif kind == "key":
        mods = "+".join(str(m).upper() for m in act.get("mods", []) or [])
        vk = int(act.get("vk", 0))
        text = f"{mods}+" if mods else ""
        text += VK_LABEL.get(vk, f"VK 0x{vk:02X}")
        rep = int(act.get("repeat", 1) or 1)
        if rep > 1:
            text += f" ×{rep}"
    elif kind == "scroll":
        d = int(act.get("delta", 0))
        text = f"滚轮 {'↑' if d > 0 else '↓'} {abs(d)}"
        rep = int(act.get("repeat", 1) or 1)
        if rep > 1:
            text += f" ×{rep}"
    elif kind == "run":
        cmd = str(act.get("cmd", ""))
        text = f"启动 {cmd}" if cmd else "启动"
    elif kind == "paste":
        text = "粘贴" + ("并回车" if act.get("enter") else "")
    elif kind == "enter":
        text = "粘贴并回车"
    elif kind == "none":
        text = str(act.get("note", "")).strip() or "直通（不拦截）"
    else:
        text = str(kind)

    if hold_info:
        hold = act.get("hold_action")
        if hold:
            text += " · 长按 " + describe_action(hold, hold_info=False)
        elif act.get("hold_repeat"):
            text += " · 按住连滚" if kind == "scroll" else " · 按住连发"
    return text


# --------------------------------------------------------------------------
# 按键路由：CLI 与 GUI 共用
# --------------------------------------------------------------------------
# --------------------------------------------------------------------------
# 临时模式：长按返回 → 音量/亮度，长按菜单 → 鼠标；单击任一个退出
# --------------------------------------------------------------------------
MODE_IDLE_S = 12.0
TAP_WINDOW = 0.30
TAP_TARGETS = {"27": "media", "36": "nav", "93": "cursor"}
MODE_TRIGGERS = {"27": "media", "93": "cursor"}
MODE_LABELS = {"media": "音量/亮度模式", "cursor": "鼠标模式"}
MODE_KEYS = {
    "media": {"38": "volume_up", "40": "volume_down",
              "37": "brightness_down", "39": "brightness_up"},
    "cursor": {},
}
MODE_ACTIONS = {
    "volume_up": {"type": "key", "vk": 0xAF, "hold_repeat": 120, "label": "音量 +"},
    "volume_down": {"type": "key", "vk": 0xAE, "hold_repeat": 120, "label": "音量 -"},
    "brightness_up": {"type": "brightness", "delta": 10, "hold_repeat": 160,
                      "label": "亮度 +"},
    "brightness_down": {"type": "brightness", "delta": -10, "hold_repeat": 160,
                        "label": "亮度 -"},
    "mouse_up": {"type": "mouse", "dx": 0, "dy": -8, "hold_repeat": 45,
                 "hold_accel": True, "label": "鼠标 ↑"},
    "mouse_down": {"type": "mouse", "dx": 0, "dy": 8, "hold_repeat": 45,
                   "hold_accel": True, "label": "鼠标 ↓"},
    "mouse_left": {"type": "mouse", "dx": -8, "dy": 0, "hold_repeat": 45,
                   "hold_accel": True, "label": "鼠标 ←"},
    "mouse_right": {"type": "mouse", "dx": 8, "dy": 0, "hold_repeat": 45,
                    "hold_accel": True, "label": "鼠标 →"},
    "mouse_click": {"type": "click", "label": "鼠标左键"},
}


class KeyRouter:
    """按键 → Profile → 动作；带长按连发和"点击 / 长按"双动作。

    - hold_repeat: 毫秒，按住时每隔这么久重复一次（可加速）
    - hold_action + hold_defer: 按住超过 hold_ms 才执行长按动作，
      否则松手时执行普通动作（单击不延迟触发）

    事件流只在松手时记一条：单击是动作本身，长按带时长和连发次数。
    """

    MAX_HOLD_S = 8.0

    def __init__(self, profiles: ProfileManager, emit=None):
        self.profiles = profiles
        self.emit = emit
        self._active: dict[int, dict] = {}
        self._held: dict[int, dict] = {}
        self._deferred: dict[int, dict] = {}
        self.nav_mode = False
        self.mode: str | None = None
        self.mode_last = 0.0
        self._tap_pending: dict[int, dict] = {}

    def reset(self) -> None:
        """停止监听 / 重载配置时清掉按住的键，否则重启后会一直连发。"""
        self._active.clear()
        self._held.clear()
        self._deferred.clear()
        self.nav_mode = False
        self.mode = None
        self._tap_pending.clear()
        cursor_hook_stop()

    def _exit_mode(self) -> None:
        if not self.mode:
            return
        label = MODE_LABELS.get(self.mode, "模式")
        self.mode = None
        cursor_hook_stop()
        beep(560, 70)
        if self.emit:
            self.emit("status", text=f"{label} 关")

    def key_down(self, vkey: int, now: float) -> None:
        if vkey in self._active:
            return  # 按住期间设备连发的 keydown 不重复触发
        prof = self.profiles.current()
        pname = prof.get("name", "?")
        actions = prof.get("actions", {}) or {}
        nav = prof.get("nav") or {}

        if self.mode:
            if str(vkey) in MODE_TRIGGERS:
                self._exit_mode()
                return
            if self.mode == "cursor" and (vkey in CURSOR_KEYS or vkey == 0x0D):
                return  # 由低层钩子处理并吞掉，Raw Input 不应再响应
            name = ((prof.get("modes", {}).get(self.mode)
                     or MODE_KEYS.get(self.mode, {})).get(str(vkey)))
            action = actions.get(name) or MODE_ACTIONS.get(name or "")
            if name and action:
                self.mode_last = now
                self._begin(vkey, now, pname, name, action)
                return

        if self.nav_mode:
            name = (nav.get("keys") or {}).get(str(vkey))
            if str(vkey) == str(nav.get("exit_key", "")):
                name = "nav_exit"
            if name:
                action = actions.get(name)
                if action:
                    self._begin(vkey, now, pname, name, action)
                    return

        name = prof.get("keys", {}).get(str(vkey))
        action = actions.get(name) if name else None
        if action is None and str(vkey) in MODE_TRIGGERS:
            # 未映射的返回/菜单键也能双击进模式
            action = {"type": "none", "note": "直通（不拦截）"}
            name = f"mode_{MODE_TRIGGERS[str(vkey)]}"
        if not name or not action:
            return
        # 返回/主页/菜单统一双击进模式；第一击延迟 0.3s 执行，等第二击。
        if str(vkey) in TAP_TARGETS:
            p = self._tap_pending.get(vkey)
            if p and now - p["last"] <= 0.45:
                del self._tap_pending[vkey]
                self._active.pop(vkey, None)
                target = TAP_TARGETS[str(vkey)]
                if target == "nav":
                    self._dispatch(pname, "nav_toggle",
                                   {"type": "nav_toggle", "label": "导航模式"})
                else:
                    self._dispatch(pname, f"mode_{target}",
                                   {"type": "mode", "mode": target,
                                    "label": MODE_LABELS[target]})
                return
            st = {"started": now, "action": action, "name": name, "profile": pname,
                  "count": 0, "fired": None, "hold_action": None, "deferred": False}
            self._active[vkey] = st
            self._tap_pending[vkey] = {"due": now + TAP_WINDOW, "last": now, "st": st}
            return
        self._begin(vkey, now, pname, name, action)

    def _begin(self, vkey: int, now: float, pname: str, name: str,
               action: dict) -> None:
        st = {"started": now, "action": action, "name": name, "profile": pname,
              "count": 0, "fired": None, "hold_action": action.get("hold_action"),
              "deferred": False}
        self._active[vkey] = st
        hold_action = st["hold_action"]

        if hold_action and action.get("hold_defer"):
            st["deferred"] = True
            due = now + float(action.get("hold_ms", 500)) / 1000
            self._deferred[vkey] = {"due": due, "st": st}
            return

        self._dispatch(pname, name, action)
        st["fired"] = "base"
        st["count"] = 1
        hold_ms = float(action.get("hold_ms", 600)) / 1000
        if hold_action:
            self._held[vkey] = {"due": now + hold_ms, "once": True, "st": st}
            return
        repeat_ms = float(action.get("hold_repeat", 0) or 0)
        if repeat_ms > 0:
            interval = repeat_ms / 1000
            self._held[vkey] = {"due": now + interval, "interval": interval,
                                "accel": bool(action.get("hold_accel")), "st": st}

    def key_up(self, vkey: int, now: float) -> None:
        if vkey in self._tap_pending:
            # 菜单键第一击：等双击窗口结束再决定
            self._active.pop(vkey, None)
            self._held.pop(vkey, None)
            self._deferred.pop(vkey, None)
            return
        st = self._active.pop(vkey, None)
        self._held.pop(vkey, None)
        d = self._deferred.pop(vkey, None)
        if st is None:
            return
        if d is not None:
            # 还没到长按阈值：松手时才执行单击动作
            self._dispatch(st["profile"], st["name"], st["action"])
            st["fired"] = "base"
            st["count"] += 1
        self._emit_press(st, max(0.0, (now - st["started"]) * 1000))

    def tick(self, now: float) -> None:
        if self.mode:
            if not self.mode_last:
                self.mode_last = now
            last = max(self.mode_last, _cursor_last if self.mode == "cursor" else 0.0)
            if now - last > MODE_IDLE_S:
                self._exit_mode()
        for vk, p in list(self._tap_pending.items()):
            if now >= p["due"]:
                del self._tap_pending[vk]
                st = p["st"]
                self._dispatch(st["profile"], st["name"], st["action"])
                st["fired"] = "base"
                st["count"] = 1
                self._emit_press(st, 0.0)  # 双击窗口的等待不算长按
        for vk, d in list(self._deferred.items()):
            if now >= d["due"]:
                del self._deferred[vk]
                st = d["st"]
                self._dispatch(st["profile"], st["name"], st["hold_action"])
                st["fired"] = "hold"
                st["count"] += 1

        for vk, h in list(self._held.items()):
            st = h["st"]
            if now - st["started"] > self.MAX_HOLD_S:
                del self._held[vk]
                continue
            if now < h["due"]:
                continue
            if h.get("once"):
                del self._held[vk]
                self._dispatch(st["profile"], st["name"], st["hold_action"])
                st["fired"] = "hold"
                st["count"] += 1
                continue
            n = 0
            while now >= h["due"] and n < 3:
                self._dispatch(st["profile"], st["name"], st["action"])
                st["count"] += 1
                n += 1
                if h["accel"]:
                    h["interval"] = max(0.045, h["interval"] * 0.86)
                h["due"] += h["interval"]

        # keyup 丢了的兜底：8 秒后收尾，别让状态卡死
        for vk, st in list(self._active.items()):
            if now - st["started"] > self.MAX_HOLD_S:
                self._active.pop(vk, None)
                self._held.pop(vk, None)
                self._deferred.pop(vk, None)
                self._emit_press(st, self.MAX_HOLD_S * 1000)

    def _emit_press(self, st: dict, dur_ms: float) -> None:
        if self.emit is None:
            return
        if st.get("fired") == "hold":
            detail = "长按 · " + describe_action(st["hold_action"], hold_info=False)
        else:
            detail = describe_action(st["action"], hold_info=False)
            n = int(st.get("count", 1) or 1)
            if n > 1:
                detail = f"长按 {dur_ms / 1000:.1f}s · {detail} ×{n}"
            elif dur_ms >= 250 and not st.get("deferred"):
                detail = f"长按 {dur_ms / 1000:.1f}s · {detail}"
        self.emit("key", name=st["name"], profile=st["profile"], detail=detail)

    def _dispatch(self, profile: str, name: str, action: dict) -> None:
        kind = action.get("type")
        try:
            if kind == "key":
                press_combo(action.get("mods", []), int(action["vk"]),
                            int(action.get("repeat", 1)))
            elif kind == "scroll":
                reps = max(1, int(action.get("repeat", 1) or 1))
                for i in range(reps):
                    scroll(int(action["delta"]))
                    if i + 1 < reps:
                        time.sleep(0.02)
            elif kind == "run":
                subprocess.Popen(action["cmd"], shell=True)
            elif kind == "nav_toggle":
                self.nav_mode = not self.nav_mode
                beep(900 if self.nav_mode else 560, 70)
                if self.emit:
                    self.emit("status",
                              text="导航模式 " + ("开" if self.nav_mode else "关"))
            elif kind == "nav_exit":
                self.nav_mode = False
                if self.emit:
                    self.emit("status", text="导航模式 关")
            elif kind == "mode":
                m = str(action.get("mode") or "")
                if m in MODE_KEYS:
                    if self.mode == m:
                        self._exit_mode()
                    else:
                        self.mode = m
                        self.mode_last = 0.0
                        self.nav_mode = False
                        if m == "cursor":
                            global _cursor_last
                            _cursor_last = 0.0
                        if m == "cursor" and not cursor_hook_start() and self.emit:
                            self.emit("error", text="鼠标模式钩子安装失败")
                        beep(900, 70)
                        if self.emit:
                            self.emit("status",
                                      text=f"{MODE_LABELS[m]} 开（单击返回/菜单退出）")
            elif kind == "brightness":
                if not brightness(int(action.get("delta", 10))) and self.emit:
                    self.emit("error", text="亮度调节失败：显示器不支持 DDC/CI")
            elif kind == "mouse":
                mouse_move(int(action.get("dx", 0)), int(action.get("dy", 0)))
            elif kind == "click":
                mouse_click()
            elif kind == "none":
                pass
            else:
                return
        except Exception as e:  # noqa: BLE001
            if self.emit:
                self.emit("error", text=f"动作执行失败: {e}")


# --------------------------------------------------------------------------
# 单实例：两个进程同时监听会把每个按键注入两次
# --------------------------------------------------------------------------
ERROR_ALREADY_EXISTS = 183
_mutex_handle = None


def acquire_single_instance(name: str = "Local\\AgentRMC.SingleInstance") -> bool:
    global _mutex_handle
    if _mutex_handle:
        return True
    kernel32.CreateMutexW.restype = wt.HANDLE
    kernel32.CreateMutexW.argtypes = [wt.LPVOID, wt.BOOL, wt.LPCWSTR]
    kernel32.CloseHandle.argtypes = [wt.HANDLE]
    h = kernel32.CreateMutexW(None, False, name)
    if not h:
        return True  # 拿不到互斥量时放行，不挡用户
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        kernel32.CloseHandle(h)
        return False
    _mutex_handle = h
    return True


# --------------------------------------------------------------------------
# Raw Input 监听
# --------------------------------------------------------------------------
WM_INPUT = 0x00FF
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIM_TYPEKEYBOARD = 1
RIDI_DEVICENAME = 0x20000007
HWND_MESSAGE = wt.HWND(-3)
WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP = 0x0100, 0x0101, 0x0104, 0x0105


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE), ("wParam", wt.WPARAM)]


class RAWKEYBOARD(ctypes.Structure):
    _fields_ = [("MakeCode", wt.USHORT), ("Flags", wt.USHORT), ("Reserved", wt.USHORT),
                ("VKey", wt.USHORT), ("Message", wt.UINT),
                ("ExtraInformation", wt.ULONG)]


WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


# 64 位下必须声明 restype/argtypes，否则句柄会被截断成 32 位
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
user32.GetRawInputData.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p,
                                   ctypes.POINTER(wt.UINT), wt.UINT]
user32.GetRawInputData.restype = wt.UINT
user32.GetRawInputDeviceInfoW.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p,
                                          ctypes.POINTER(wt.UINT)]
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t

key_events: deque = deque()
_devname_cache: dict = {}


def _device_name(h) -> str:
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


def _wndproc(hwnd, msg, wparam, lparam):
    if msg == WM_INPUT:
        size = wt.UINT(0)
        user32.GetRawInputData(wt.HANDLE(lparam), RID_INPUT, None, ctypes.byref(size),
                               ctypes.sizeof(RAWINPUTHEADER))
        if size.value:
            buf = ctypes.create_string_buffer(size.value)
            got = user32.GetRawInputData(wt.HANDLE(lparam), RID_INPUT, buf,
                                         ctypes.byref(size), ctypes.sizeof(RAWINPUTHEADER))
            if got == size.value:
                hdr = ctypes.cast(buf, ctypes.POINTER(RAWINPUTHEADER)).contents
                if CFG["device_mark"] in _device_name(hdr.hDevice).lower():
                    if hdr.dwType == RIM_TYPEKEYBOARD:
                        kb = ctypes.cast(ctypes.addressof(buf) + ctypes.sizeof(RAWINPUTHEADER),
                                         ctypes.POINTER(RAWKEYBOARD)).contents
                        key_events.append({
                            "t": time.time(), "vkey": kb.VKey, "scan": kb.MakeCode,
                            "down": kb.Message in (WM_KEYDOWN, WM_SYSKEYDOWN),
                        })
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


# 必须保持引用：局部变量被 GC 回收后，Windows 回调会指向已释放内存导致 access violation
_WNDPROC_REF = None
_listener_hwnd = None


def start_listener() -> wt.HWND:
    global _WNDPROC_REF, _listener_hwnd
    if _listener_hwnd:
        return _listener_hwnd
    wc = WNDCLASSW()
    _WNDPROC_REF = WNDPROC(_wndproc)
    wc.lpfnWndProc = _WNDPROC_REF
    wc.lpszClassName = "AgentRMCWnd"
    wc.hInstance = kernel32.GetModuleHandleW(None)
    atom = user32.RegisterClassW(ctypes.byref(wc))
    if not atom and ctypes.get_last_error() != 1410:   # 1410 = 类已注册
        raise RuntimeError(f"RegisterClassW 失败 err={ctypes.get_last_error()}")
    hwnd = user32.CreateWindowExW(0, "AgentRMCWnd", "AgentRMC", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, None, wc.hInstance, None)
    if not hwnd:
        raise RuntimeError(f"CreateWindowExW 失败 err={ctypes.get_last_error()}")
    rid = RAWINPUTDEVICE(0x01, 0x06, RIDEV_INPUTSINK, hwnd)
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)):
        raise RuntimeError(f"RegisterRawInputDevices 失败 err={ctypes.get_last_error()}")
    _listener_hwnd = hwnd
    return hwnd


def pump() -> None:
    msg = wt.MSG()
    while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))


# --------------------------------------------------------------------------
# 输入注入
# --------------------------------------------------------------------------
KEYEVENTF_KEYUP = 0x0002
CF_UNICODETEXT = 13
GMEM_MOVEABLE = 0x0002

user32.keybd_event.argtypes = [wt.BYTE, wt.BYTE, wt.DWORD, ctypes.c_size_t]
user32.OpenClipboard.argtypes = [wt.HWND]
user32.SetClipboardData.argtypes = [wt.UINT, wt.HANDLE]
user32.GetClipboardData.argtypes = [wt.UINT]
user32.GetClipboardData.restype = wt.HANDLE
kernel32.GlobalAlloc.restype = wt.HANDLE
kernel32.GlobalLock.argtypes = [wt.HANDLE]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = [wt.HANDLE]
kernel32.GlobalSize.argtypes = [wt.HANDLE]
kernel32.GlobalSize.restype = ctypes.c_size_t


def beep(freq: int = 1000, ms: int = 80) -> None:
    """非阻塞提示音。"""
    if winsound is None:
        return

    def _b():
        try:
            winsound.Beep(freq, ms)
        except Exception:  # noqa: BLE001
            pass

    threading.Thread(target=_b, daemon=True).start()


def tap_key(vk: int) -> None:
    user32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.012)
    user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)


def set_clipboard(text: str) -> bool:
    for _ in range(6):
        if user32.OpenClipboard(None):
            break
        time.sleep(0.03)
    else:
        return False
    try:
        user32.EmptyClipboard()
        data = text.encode("utf-16-le") + b"\x00\x00"
        h = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
        if not h:
            return False
        p = kernel32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        kernel32.GlobalUnlock(h)
        user32.SetClipboardData(CF_UNICODETEXT, h)
        return True
    finally:
        user32.CloseClipboard()


def paste_text(text: str, auto_enter: bool = False) -> None:
    if not text:
        return
    if not set_clipboard(text):
        print("[!] 剪贴板写入失败")
        return
    time.sleep(0.05)
    VK_CONTROL, VK_V, VK_RETURN = 0x11, 0x56, 0x0D
    user32.keybd_event(VK_CONTROL, 0, 0, 0)
    time.sleep(0.01)
    user32.keybd_event(VK_V, 0, 0, 0)
    time.sleep(0.02)
    user32.keybd_event(VK_V, 0, KEYEVENTF_KEYUP, 0)
    user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)
    if auto_enter:
        time.sleep(0.08)
        tap_key(VK_RETURN)


# --------------------------------------------------------------------------
# 录音
# --------------------------------------------------------------------------
class Recorder:
    def __init__(self, cfg: dict):
        self.rate = int(cfg.get("sample_rate", 16000))
        self.device = cfg.get("input_device")
        self._stream = None
        self._frames: list = []
        self._lock = threading.Lock()
        self.level = 0.0

    def _callback(self, indata, frames, time_info, status):  # noqa: ANN001
        if status:
            pass
        with self._lock:
            self._frames.append(indata.copy())
            try:
                import numpy as np
                self.level = float(np.sqrt(np.mean(indata * indata)))
            except Exception:  # noqa: BLE001
                self.level = 0.0

    def start(self) -> bool:
        import sounddevice as sd
        with self._lock:
            self._frames = []
        try:
            self._stream = sd.InputStream(samplerate=self.rate, channels=1, dtype="float32",
                                          device=self.device, callback=self._callback,
                                          blocksize=1024)
            self._stream.start()
            return True
        except Exception as e:  # noqa: BLE001
            print(f"[!] 录音启动失败: {e}")
            self._stream = None
            return False

    def stop(self):  # noqa: ANN201
        import numpy as np
        if self._stream is not None:
            self._stream.stop()
            self._stream.close()
            self._stream = None
        with self._lock:
            frames = list(self._frames)
            self._frames = []
        if not frames:
            return None
        return np.concatenate(frames, axis=0).flatten()


# --------------------------------------------------------------------------
# 语音识别
# --------------------------------------------------------------------------
class Transcriber:
    def __init__(self, model_size: str, language: str):
        self.model_size = model_size
        self.language = language
        self._model = None
        self._failed = False

    def available(self) -> bool:
        try:
            import faster_whisper  # noqa: F401
            return True
        except ImportError:
            return False

    def load(self):
        if self._model is not None or self._failed:
            return self._model
        os.environ.setdefault("HF_HUB_DISABLE_XET", "1")

        # 优先用本地已下载的模型目录，避免联网
        local_dir = HERE / "models" / f"faster-whisper-{self.model_size}"
        if (local_dir / "model.bin").exists():
            try:
                from faster_whisper import WhisperModel
                print(f"[i] 加载本地模型 {local_dir.name} ...")
                self._model = WhisperModel(str(local_dir), device="cpu", compute_type="int8")
                print("[i] 模型就绪")
                return self._model
            except Exception as e:  # noqa: BLE001
                print(f"[!] 本地模型加载失败: {e}")

        proxy = CFG["voice"].get("proxy")
        attempts = []
        if proxy:
            attempts.append(("代理 " + proxy, proxy))
        attempts.append(("直连", None))
        for label, p in attempts:
            for k in ("HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY"):
                if p:
                    os.environ[k] = p
                else:
                    os.environ.pop(k, None)
            try:
                from faster_whisper import WhisperModel
                print(f"[i] 加载 whisper {self.model_size}（{label}，首次会下载模型）...")
                self._model = WhisperModel(self.model_size, device="cpu", compute_type="int8")
                print("[i] 模型就绪")
                return self._model
            except Exception as e:  # noqa: BLE001
                print(f"[!] {label} 失败: {str(e).splitlines()[0][:120]}")
        print("[!] whisper 加载失败。可用 tools\\download_model.bat 手动下载模型")
        self._failed = True
        return None

    def transcribe(self, audio) -> str:  # noqa: ANN001
        model = self.load()
        if model is None:
            return ""
        segments, _info = model.transcribe(audio, language=self.language,
                                           beam_size=1, vad_filter=True)
        parts = []
        for s in segments:
            # whisper 对静音/噪声容易产生幻觉，用置信度过滤掉
            if s.no_speech_prob > 0.6 and s.avg_logprob < -0.6:
                continue
            parts.append(s.text)
        return "".join(parts).strip()


# --------------------------------------------------------------------------
# 主逻辑
# --------------------------------------------------------------------------
class App:
    def __init__(self):
        self.vk_cfg = CFG["voice_key"]
        self.voice_vk = int(self.vk_cfg["vkey"])
        self.confirm_ms = float(self.vk_cfg["confirm_window_ms"])
        self.silence_ms = float(self.vk_cfg["silence_stop_ms"])
        self.min_record_ms = float(self.vk_cfg["min_record_ms"])

        self.recorder = Recorder(CFG["voice"])
        self.asr = Transcriber(CFG["voice"]["asr_model"], CFG["voice"]["asr_language"])
        self.rules = RuleEngine(load_commands())
        self.profiles = ProfileManager(load_profiles())

        # 语音键有两种用法：
        #   local  —— 自己录音 + 本地 whisper 转写（默认）
        #   hotkey —— 把语音键转给目标程序（Qoder 的 Ctrl+Shift+V 等）
        self.voice_mode = str(CFG["voice"].get("mode", "local")).lower()
        self.active_voice: tuple[list, int, str] = voice_hotkey(self.profiles.current())
        self.holding = False
        self.router = KeyRouter(self.profiles, emit=self._on_router_event)

        self.state = "idle"          # idle | pending | recording
        self.pending_since = 0.0
        self.record_started = 0.0
        self.last_voice_event = 0.0
        self.repeat_count = 0
        self.busy_transcribing = False

    # ---- 普通按键 ----
    def _on_router_event(self, kind: str, **data) -> None:
        if kind == "error":
            print(f"[!] {data.get('text')}")
        elif kind == "key":
            print(f"· [{data.get('profile')}] {data.get('name')} -> {data.get('detail')}")

    # ---- 语音键状态机 ----
    def on_voice_down(self, now: float) -> None:
        if self.state == "idle":
            self.state = "pending"
            self.pending_since = now
            self.repeat_count = 1
        elif self.state == "pending":
            self.repeat_count += 1
            if self.repeat_count >= int(self.vk_cfg["min_repeats"]):
                self.state = "recording"
                self.record_started = now
                self.last_voice_event = now
                if self.voice_mode == "hotkey":
                    prof = self.profiles.current(force=True)
                    mods, vk, trigger = voice_hotkey(prof)
                    self.active_voice = (mods, vk, trigger)
                    fc = voice_focus_click(prof)
                    if fc:
                        ok = click_foreground(*fc)
                        print(f"· 点击聊天框聚焦 {'✓' if ok else '✗'}")
                        if ok:
                            time.sleep(0.02)
                    if trigger == "toggle":
                        tap_combo(mods, vk)      # Qoder：按下先开录，松手再按一次停
                    else:
                        hold_combo(mods, vk)
                    self.holding = True
                    beep(1180, 60)
                    how = "开关" if trigger == "toggle" else "按住"
                    print(f"● 语音快捷键（{how}）{mods}+0x{vk:02X}  tray={taskbar_state()}")
                elif self.recorder.start():
                    beep(1180, 70)
                    print("● 录音中...")
                else:
                    self.state = "idle"
        elif self.state == "recording":
            self.last_voice_event = now

    def on_voice_up(self, now: float) -> None:
        if self.state == "pending":
            # 遥控器短按一下：原生 F5 已经发给前台，这里不再补发（否则刷新两次）
            self.state = "idle"
            print("· 单击语音键")
        elif self.state == "recording":
            self.last_voice_event = now
            if self.voice_mode == "hotkey":
                # 松手就结束，不等静音判定
                self.finish_recording((now - self.record_started) * 1000)

    def tick(self, now: float) -> None:
        if self.state == "pending" and (now - self.pending_since) * 1000 > self.confirm_ms:
            self.state = "idle"

        if self.state == "recording":
            idle_ms = (now - self.last_voice_event) * 1000
            dur_ms = (now - self.record_started) * 1000
            if idle_ms > self.silence_ms:
                self.finish_recording(dur_ms)

        self.router.tick(now)

    def release_holding(self, dur_ms: float = 0) -> None:
        """万一程序停止 / 退出时手还按着，别把目标程序留在录音状态。"""
        if not self.holding:
            return
        mods, vk, trigger = self.active_voice
        if trigger == "toggle":
            # 给目标程序一点时间进入 recording，避免第二次 keydown 被当成"又开始"
            wait = max(0.0, 300.0 - dur_ms) / 1000
            if wait:
                time.sleep(wait)
            tap_combo(mods, vk)
        else:
            release_combo(mods, vk)
        self.holding = False

    def finish_recording(self, dur_ms: float) -> None:
        self.state = "idle"
        if self.voice_mode == "hotkey":
            fg = foreground_info()[0]
            self.release_holding(dur_ms)
            beep(760, 70)
            print(f"● 语音快捷键已结束（前台 {fg or '?'} tray={taskbar_state()}）")
            return
        audio = self.recorder.stop()
        beep(760, 70)
        if audio is None or dur_ms < self.min_record_ms:
            print("· 录音太短，忽略")
            return
        print(f"● 录音结束 {dur_ms/1000:.1f}s，转写中...")
        self.busy_transcribing = True

        def worker():
            try:
                t0 = time.time()
                text = self.asr.transcribe(audio)
                cost = time.time() - t0
                if not text:
                    print("· 没有识别到内容")
                    return
                print(f"✓ ({cost:.1f}s) {text}")
                # 先过本地指令表，命中就执行；否则交给 Agent（粘贴）
                if not self.rules.dispatch(text):
                    if CFG["voice"]["paste"]:
                        paste_text(text, CFG["voice"]["auto_enter"])
            except Exception as e:  # noqa: BLE001
                print(f"[!] 转写失败: {e}")
            finally:
                self.busy_transcribing = False

        threading.Thread(target=worker, daemon=True).start()


def list_mics() -> None:
    import sounddevice as sd
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0:
            print(f"  [{i}] {d['name']}  ({d['max_input_channels']} ch)")


def test_mic(seconds: float = 5.0) -> None:
    import sounddevice as sd
    import numpy as np
    print(f"录 {seconds:.0f} 秒，请说话...")
    rec = sd.rec(int(seconds * 16000), samplerate=16000, channels=1, dtype="float32")
    sd.wait()
    audio = rec.flatten()
    print("录音完成，转写中...")
    asr = Transcriber(CFG["voice"]["asr_model"], CFG["voice"]["asr_language"])
    text = asr.transcribe(audio)
    print(f"结果: {text!r}")
    wav_path = HERE / "rc_probe" / "last_test.wav"
    import wave
    with wave.open(str(wav_path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes((np.clip(audio, -1, 1) * 32767).astype(np.int16).tobytes())
    print(f"音频已存 {wav_path}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list-mic", action="store_true")
    ap.add_argument("--list-keys", action="store_true")
    ap.add_argument("--test-mic", action="store_true")
    ap.add_argument("--seconds", type=float, default=5.0)
    args = ap.parse_args()

    if args.list_mic:
        list_mics()
        return
    if args.test_mic:
        test_mic(args.seconds)
        return
    if not args.list_keys and not acquire_single_instance():
        print("AgentRMC 已经在运行（单实例保护，避免按键注入两次）")
        return

    hwnd = start_listener()
    app = App()
    _mods, _vk, _trigger = voice_hotkey(app.profiles.current())

    print("=" * 62)
    print("  AgentRMC 已启动")
    print(f"  设备过滤: {CFG['device_mark']}")
    print(f"  语音键: vkey={app.voice_vk} (0x{app.voice_vk:02X})，长按触发语音")
    print(f"  语音接法: {_trigger}  {'+'.join(_mods)}+0x{_vk:02X}")
    if app.voice_mode == "hotkey":
        print("  ASR: 不加载（hotkey 模式交给 Agent）")
    else:
        print(f"  ASR: {CFG['voice']['asr_model']} / {CFG['voice']['asr_language']}"
              f"{'' if app.asr.available() else '  [未安装 faster-whisper]'}")
    print(f"  场景: {len(app.profiles.profiles)} 个 profile，当前 {app.profiles.describe()}")
    print(f"  指令: {len(app.rules.rules)} 条")
    print("  Ctrl+C 退出")
    print("=" * 62)

    if not args.list_keys and app.voice_mode != "hotkey" and app.asr.available():
        threading.Thread(target=app.asr.load, daemon=True).start()

    try:
        while True:
            pump()
            now = time.time()
            while key_events:
                ev = key_events.popleft()
                if args.list_keys:
                    print(f"  vkey={ev['vkey']:<3} scan=0x{ev['scan']:02X} "
                          f"{'down' if ev['down'] else 'up':<4}  [{app.profiles.describe()}]")
                    continue
                if ev["vkey"] == app.voice_vk:
                    if ev["down"]:
                        app.on_voice_down(now)
                    else:
                        app.on_voice_up(now)
                elif ev["down"]:
                    app.router.key_down(ev["vkey"], now)
                else:
                    app.router.key_up(ev["vkey"], now)
            if not args.list_keys:
                app.tick(now)
            time.sleep(0.005)
    except KeyboardInterrupt:
        print("\n退出")
        app.release_holding()
        user32.DestroyWindow(hwnd)


if __name__ == "__main__":
    main()
