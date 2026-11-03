"""Verify the voice key / mic waveform actually animates while recording.
Clicks 试一句 on the main window, grabs the voice-key region at ~10fps;
then opens the standalone panel, clicks its mic, grabs the mic region.
Usage: python ux_wave.py [tag]
"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import json
import subprocess
import sys
import time
import pathlib
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
from PIL import ImageGrab, ImageChops

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32  # noqa: E402

TAG = sys.argv[1] if len(sys.argv) > 1 else "wave"
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
pids = set(pids_for("AgentRMCNative.exe"))


def windows():
    out = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            b = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, b, n + 1)
            out.append((h, b.value))
        return True
    user32.EnumWindows(CB(cb), 0)
    return out


def click(el):
    r = el.GetCurrentPropertyValue(30001)
    l, t, w, h = (tuple(r) if isinstance(r, (tuple, list))
                  else (r.left, r.top, r.right - r.left, r.bottom - r.top))
    pt = wt.POINT()
    user32.GetCursorPos(byref(pt))
    old = (pt.x, pt.y)
    user32.SetCursorPos(int(l + w / 2), int(t + h / 2))
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    user32.SetCursorPos(*old)
    time.sleep(0.4)


def find(hwnd, name):
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if (el.CurrentName or "") == name:
            return el
    return None


def rect_of(el):
    r = el.GetCurrentPropertyValue(30001)
    if isinstance(r, (tuple, list)):
        l, t, w, h = r
        return (l, t, l + w, t + h)
    return (r.left, r.top, r.right, r.bottom)


def burst(bbox, n=12):
    frames = []
    for i in range(n):
        frames.append(ImageGrab.grab(bbox=bbox, all_screens=True))
        time.sleep(0.1)
    changed = sum(1 for a, b in zip(frames, frames[1:])
                  if ImageChops.difference(a, b).getbbox() is not None)
    return frames, changed


wins = windows()
main = [h for h, t in wins if t == "AgentRMC Native"]
if not main:
    # 上轮留在独立语音页：点「完整界面」把主窗叫回来。
    for h, t in wins:
        if t == "AgentRMC 语音":
            back = find(h, "完整界面")
            if back is not None:
                click(back)
                time.sleep(0.8)
    wins = windows()
    main = [h for h, t in wins if t == "AgentRMC Native"]
if not main:
    print("NO_MAIN")
    sys.exit(1)
hwnd = main[0]
user32.SetForegroundWindow(hwnd)
time.sleep(0.5)

key = find(hwnd, "语音键")
if key is None:
    print("NO_VOICE_KEY")
    sys.exit(1)
kr = rect_of(key)
click(key)  # 真实用户路径：语音键开始录音；静音自停前抢帧
time.sleep(0.3)
frames, changed = burst(kr)
click(key)  # 收尾停止
frames[0].save(r"D:\githubrepos\Agentrmc\rc_probe\ux_%s_key0.png" % TAG)
frames[6].save(r"D:\githubrepos\Agentrmc\rc_probe\ux_%s_key1.png" % TAG)
print("KEY_FRAMES_CHANGED=%d/11" % changed)

# 独立页入口收进托盘/桌宠后，走启动路径：voice_mode=True 直接开面板。
subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
               capture_output=True)
time.sleep(0.8)
cfg_path = pathlib.Path(r"D:\apps\AgentRMC\config.json")
cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
cfg["ui"]["voice_mode"] = True
cfg_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=4),
                    encoding="utf-8")
subprocess.run(["powershell", "-NoProfile", "-Command",
                "(Invoke-CimMethod -ClassName Win32_Process -MethodName "
                "Create -Arguments @{ CommandLine = '\"%s\"' }).ProcessId"
                % r"D:\apps\AgentRMC\AgentRMCNative.exe"],
               capture_output=True)
time.sleep(4)
pids = set(pids_for("AgentRMCNative.exe"))
panels = [h for h, t in windows() if t == "AgentRMC 语音"]
if panels:
    ph = panels[0]
    user32.SetForegroundWindow(ph)
    mic = find(ph, "语音键")
    if mic is not None:
        mr = rect_of(mic)
        click(mic)  # start recording
        time.sleep(0.3)
        frames, changed = burst(mr)
        frames[0].save(r"D:\githubrepos\Agentrmc\rc_probe\ux_%s_mic0.png" % TAG)
        frames[6].save(r"D:\githubrepos\Agentrmc\rc_probe\ux_%s_mic1.png" % TAG)
        print("MIC_FRAMES_CHANGED=%d/11" % changed)
        click(mic)  # stop
    else:
        print("NO_MIC")
else:
    print("NO_PANEL")
print("DONE")
