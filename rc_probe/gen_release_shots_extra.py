"""补两张：使用引导页（WebView2 真实窗口）与桌宠 + 语音小窗实拍。

用法: python rc_probe/gen_release_shots_extra.py [版本号]
版本号缺省读 plugins/agentrmc-bridge/VERSION，输出到 rc_probe/shots<数字>。
"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import ctypes.wintypes as wt
import json
import pathlib
import shutil
import subprocess
import sys
import time
from ctypes import byref

import comtypes.client
from PIL import Image, ImageGrab

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = (sys.argv[1].strip() if len(sys.argv) > 1 else
           (ROOT / "plugins" / "agentrmc-bridge" / "VERSION")
           .read_text(encoding="utf-8").strip())
CFG = pathlib.Path(r"D:\apps\AgentRMC\config.json")
EXE = r"D:\apps\AgentRMC\AgentRMCNative.exe"
OUT = pathlib.Path(__file__).resolve().parent / ("shots" + VERSION.replace(".", ""))
OUT.mkdir(exist_ok=True)
CFG_BAK = CFG.with_suffix(".json.shot_bak")

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)


def set_cfg(**kw):
    d = json.loads(CFG.read_text(encoding="utf-8"))
    d["ui"].update(kw)
    CFG.write_text(json.dumps(d, ensure_ascii=False, indent=4), encoding="utf-8")


def restart(wait_for="AgentRMC Native"):
    subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
                   capture_output=True)
    time.sleep(1.0)
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Start-Process '%s'" % EXE], capture_output=True)
    for _ in range(40):
        time.sleep(0.5)
        h = user32.FindWindowW(None, wait_for)
        if h:
            return h
    raise RuntimeError("window not found: " + wait_for)


def rect(h):
    r = wt.RECT()
    user32.GetWindowRect(h, byref(r))
    return r.left, r.top, r.right, r.bottom


def click_name(hwnd, name):
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if (el.CurrentName or "") == name:
            r = el.GetCurrentPropertyValue(30001)
            try:
                l, t, w, h = r.left, r.top, r.right - r.left, r.bottom - r.top
            except AttributeError:
                l, t, w, h = r
            pt = wt.POINT()
            user32.GetCursorPos(byref(pt))
            old = (pt.x, pt.y)
            user32.SetCursorPos(int(l + w / 2), int(t + h / 2))
            time.sleep(0.1)
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.05)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            user32.SetCursorPos(*old)
            time.sleep(1.2)
            return True
    return False


def print_window(hwnd, tag, width=1600):
    l, t, r, b = rect(hwnd)
    w, h = r - l, b - t
    bmp = gdi32.CreateCompatibleBitmap(user32.GetDC(0), w, h)
    dc = gdi32.CreateCompatibleDC(user32.GetDC(0))
    gdi32.SelectObject(dc, bmp)
    ok = user32.PrintWindow(hwnd, dc, 2)

    class BI(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                    ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bmi = BI()
    bmi.biSize = ctypes.sizeof(BI)
    bmi.biWidth, bmi.biHeight = w, -h
    bmi.biPlanes, bmi.biBitCount, bmi.biCompression = 1, 32, 0
    buf = ctypes.create_string_buffer(w * h * 4)
    got = gdi32.GetDIBits(dc, bmp, 0, h, buf, byref(bmi), 0)
    gdi32.DeleteDC(dc)
    gdi32.DeleteObject(bmp)
    if not ok or not got:
        print("FAIL", tag, ok, got)
        return
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    img = img.resize((width, round(h * width / w)), Image.LANCZOS)
    p = OUT / (tag + ".jpg")
    img.save(p, quality=88)
    print("saved", p.name, img.size)


def main():
    try:
        # --- 使用引导页 ---
        set_cfg(theme="classic", color_scheme="light", voice_mode=False,
                left_panel_voice=True)
        h = restart()
        print("guide click:", click_name(h, "使用引导"))
        print_window(h, "guide_cl_light")

        # --- 桌宠 + 语音小窗：真实桌面实拍 ---
        set_cfg(theme="classic", color_scheme="dark", voice_mode=True,
                left_panel_voice=True)
        h = restart("AgentRMC 语音")
        time.sleep(2.5)
        panel = user32.FindWindowW(None, "AgentRMC 语音")
        pet = user32.FindWindowW(None, "AgentRMCNative")
        print("panel", bool(panel), rect(panel) if panel else None)
        print("pet", bool(pet), rect(pet) if pet else None)
        if panel and pet:
            pl, pt, pr, pb = rect(panel)
            ql, qt, qr, qb = rect(pet)
            m = 56
            box = (min(pl, ql) - m, min(pt, qt) - m,
                   max(pr, qr) + m, max(pb, qb) + m)
            img = ImageGrab.grab(bbox=box, all_screens=True)
            p = OUT / "pet_panel.jpg"
            img.save(p, quality=90)
            print("saved", p.name, img.size)
    finally:
        print("restoring config")
        shutil.copy2(CFG_BAK, CFG)
        restart()


if __name__ == "__main__":
    main()
