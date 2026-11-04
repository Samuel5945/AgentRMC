"""Shoot the pet speech bubble for one theme/scheme combo.

Usage: python bubble_shots.py <tag> <theme> <scheme>
Kills the installed app, swaps config (restored afterwards), relaunches,
POSTs /speak with TTS muted, grabs the pet+bubble window region.
"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import json
import pathlib
import subprocess
import sys
import time
import urllib.request
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

from PIL import ImageGrab

TAG, THEME, SCHEME = sys.argv[1], sys.argv[2], sys.argv[3]
CFG = pathlib.Path(r"D:\apps\AgentRMC\config.json")
EXE = r"D:\apps\AgentRMC\AgentRMCNative.exe"
user32 = ctypes.windll.user32

original = CFG.read_text(encoding="utf-8")
subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
               capture_output=True)
time.sleep(0.8)
cfg = json.loads(original)
cfg["ui"]["theme"] = THEME
cfg["ui"]["color_scheme"] = SCHEME
cfg["ui"]["pet_enabled"] = True
cfg["ui"]["start_minimized"] = True
cfg["voice"]["tts"]["enabled"] = False  # 只截气泡，不念测试文案
CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8")
subprocess.run(["powershell", "-NoProfile", "-Command",
                "(Invoke-CimMethod -ClassName Win32_Process -MethodName "
                "Create -Arguments @{ CommandLine = '\"%s\"' }).ProcessId"
                % EXE], capture_output=True)
time.sleep(4)

req = urllib.request.Request(
    "http://127.0.0.1:45915/speak",
    data=json.dumps({"message": "米饭要趁热吃。这是一段稍长的话，检查气泡换行和文字边距。"}).encode(),
    headers={"Content-Type": "application/json"})
urllib.request.urlopen(req, timeout=5)
time.sleep(1.0)

kernel32 = ctypes.windll.kernel32
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def image_name(pid):
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return ""
    buf = create_unicode_buffer(1024)
    size = wt.DWORD(1024)
    ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, byref(size))
    kernel32.CloseHandle(h)
    return buf.value if ok else ""


rects = []


def cb(h, l):
    if not user32.IsWindowVisible(h):
        return True
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, byref(pid))
    if image_name(pid.value).lower().endswith("agentrmcnative.exe"):
        r = wt.RECT()
        user32.GetWindowRect(h, byref(r))
        w, hh = r.right - r.left, r.bottom - r.top
        if 0 < w < 700 and 0 < hh < 700:  # pet / bubble / card, not main
            rects.append((r.left, r.top, r.right, r.bottom))
    return True


CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
user32.EnumWindows(CB(cb), 0)
if not rects:
    print("NO_PET_WINDOWS")
    sys.exit(1)
l = min(r[0] for r in rects) - 12
t = min(r[1] for r in rects) - 12
r = max(r[2] for r in rects) + 12
b = max(r[3] for r in rects) + 12
out = pathlib.Path(__file__).parent / ("bubble_%s.png" % TAG)
ImageGrab.grab(bbox=(l, t, r, b), all_screens=True).save(out)
print("SHOT", out, "rects", rects)

subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
               capture_output=True)
time.sleep(0.5)
CFG.write_text(original, encoding="utf-8")
print("BUBBLE_%s_DONE theme=%s scheme=%s" % (TAG, THEME, SCHEME))
