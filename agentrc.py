"""AgentRMC - 把 BLE 遥控器（极米/小米系 Realtek 方案）改造成电脑 Agent 的输入终端。

工作方式
--------
1. 用 Raw Input 按设备路径过滤，只接收遥控器的按键，不干扰真实键盘鼠标。
2. 语音键被按住时会以 ~31ms 间隔连发同一个键值（默认 F5），据此判定"长按"：
   - 第一次 keydown 先挂起，250ms 内等到第二次 → 确认是遥控器，开始录音
   - 等不到 → 判定为真实键盘单击，把这次按键补发出去
3. 松手后录音送本地 whisper 转写，结果写入剪贴板并粘贴到当前焦点窗口。
4. 其余按键按 config.json 里的映射，转成系统动作（默认模拟方向键/回车/ESC）。

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
                "scroll_up": {"type": "scroll", "delta": -360},
                "scroll_down": {"type": "scroll", "delta": 360},
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
            "name": "agent",
            "match": {"process": ["workbuddyai.exe"], "title": ["workbuddy"]},
            "keys": {"38": "scroll_up", "40": "scroll_down", "13": "send",
                     "27": "esc", "36": "top", "93": "newline"},
            "actions": {
                "scroll_up": {"type": "scroll", "delta": -360},
                "scroll_down": {"type": "scroll", "delta": 360},
                "send": {"type": "key", "vk": 0x0D},
                "esc": {"type": "key", "vk": 0x1B},
                "top": {"type": "key", "vk": 0x24, "mods": ["ctrl"]},
                "newline": {"type": "key", "vk": 0x0D, "mods": ["shift"]}
            }
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
                "home": {"type": "key", "vk": 0x5B}
            }
        }
    ]
}


def load_profiles() -> list:
    if not PROFILES_PATH.exists():
        PROFILES_PATH.write_text(json.dumps(DEFAULT_PROFILES, ensure_ascii=False, indent=2),
                                 encoding="utf-8")
        print(f"[i] 已生成默认场景表 {PROFILES_PATH}")
        return DEFAULT_PROFILES["profiles"]
    try:
        data = json.loads(PROFILES_PATH.read_text(encoding="utf-8"))
        return data.get("profiles", [])
    except Exception as e:  # noqa: BLE001
        print(f"[!] profiles.json 解析失败，用内置默认: {e}")
        return DEFAULT_PROFILES["profiles"]


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

    def current(self) -> dict:
        now = time.time()
        if now - self._ts < self.CACHE_TTL:
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


def start_listener() -> wt.HWND:
    global _WNDPROC_REF
    wc = WNDCLASSW()
    _WNDPROC_REF = WNDPROC(_wndproc)
    wc.lpfnWndProc = _WNDPROC_REF
    wc.lpszClassName = "AgentRMCWnd"
    wc.hInstance = kernel32.GetModuleHandleW(None)
    atom = user32.RegisterClassW(ctypes.byref(wc))
    if not atom:
        raise RuntimeError(f"RegisterClassW 失败 err={ctypes.get_last_error()}")
    hwnd = user32.CreateWindowExW(0, "AgentRMCWnd", "AgentRMC", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, None, wc.hInstance, None)
    if not hwnd:
        raise RuntimeError(f"CreateWindowExW 失败 err={ctypes.get_last_error()}")
    rid = RAWINPUTDEVICE(0x01, 0x06, RIDEV_INPUTSINK, hwnd)
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)):
        raise RuntimeError(f"RegisterRawInputDevices 失败 err={ctypes.get_last_error()}")
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

        self.state = "idle"          # idle | pending | recording
        self.pending_since = 0.0
        self.record_started = 0.0
        self.last_voice_event = 0.0
        self.repeat_count = 0
        self.busy_transcribing = False

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
                if self.recorder.start():
                    beep(1180, 70)
                    print("● 录音中...")
                else:
                    self.state = "idle"
        elif self.state == "recording":
            self.last_voice_event = now

    def on_voice_up(self, now: float) -> None:
        if self.state == "pending":
            # 单次按下 → 不是遥控器语音键，补发
            self.state = "idle"
            print("· 单次 F5，已转发")
            tap_key(self.voice_vk)
        elif self.state == "recording":
            self.last_voice_event = now

    def tick(self, now: float) -> None:
        if self.state == "pending" and (now - self.pending_since) * 1000 > self.confirm_ms:
            # 等不到第二次连发，认定是真实键盘
            self.state = "idle"
            tap_key(self.voice_vk)

        if self.state == "recording":
            idle_ms = (now - self.last_voice_event) * 1000
            dur_ms = (now - self.record_started) * 1000
            if idle_ms > self.silence_ms:
                self.finish_recording(dur_ms)

    def finish_recording(self, dur_ms: float) -> None:
        self.state = "idle"
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

    # ---- 普通按键 ----
    def on_key(self, vkey: int) -> None:
        prof = self.profiles.current()
        name = prof.get("keys", {}).get(str(vkey))
        if not name:
            return
        action = prof.get("actions", {}).get(name)
        if not action:
            return
        kind = action.get("type")
        pname = prof.get("name", "?")
        if kind == "key":
            press_combo(action.get("mods", []), int(action["vk"]), int(action.get("repeat", 1)))
            print(f"· [{pname}] {name} -> vk 0x{int(action['vk']):02X}")
        elif kind == "scroll":
            scroll(int(action["delta"]))
            print(f"· [{pname}] {name} -> scroll {action['delta']}")
        elif kind == "run":
            subprocess.Popen(action["cmd"], shell=True)
            print(f"· [{pname}] {name} -> run {action['cmd']}")


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

    hwnd = start_listener()
    app = App()

    print("=" * 62)
    print("  AgentRMC 已启动")
    print(f"  设备过滤: {CFG['device_mark']}")
    print(f"  语音键: vkey={app.voice_vk} (0x{app.voice_vk:02X})，长按触发录音")
    print(f"  ASR: {CFG['voice']['asr_model']} / {CFG['voice']['asr_language']}"
          f"{'' if app.asr.available() else '  [未安装 faster-whisper]'}")
    print(f"  场景: {len(app.profiles.profiles)} 个 profile，当前 {app.profiles.describe()}")
    print(f"  指令: {len(app.rules.rules)} 条")
    print("  Ctrl+C 退出")
    print("=" * 62)

    if not args.list_keys and app.asr.available():
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
                    app.on_key(ev["vkey"])
            if not args.list_keys:
                app.tick(now)
            time.sleep(0.005)
    except KeyboardInterrupt:
        print("\n退出")
        user32.DestroyWindow(hwnd)


if __name__ == "__main__":
    main()
