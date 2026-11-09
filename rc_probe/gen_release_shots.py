"""发布截图：真实窗口 + WebView2 PrintWindow，逐主题截主窗页签。

用法: python rc_probe/gen_release_shots.py [版本号]
版本号缺省读 plugins/agentrmc-bridge/VERSION，输出到 rc_probe/shots<数字>。

演示数据只在这一轮有效：先备份 config.json 与 work-items.json，写入 6 条
多 Agent 演示待办（其余活动卡标 done），全部截完还原并重启应用。
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
import uuid
from ctypes import byref, create_unicode_buffer

import comtypes.client
from PIL import Image

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = (sys.argv[1].strip() if len(sys.argv) > 1 else
           (ROOT / "plugins" / "agentrmc-bridge" / "VERSION")
           .read_text(encoding="utf-8").strip())
CFG = pathlib.Path(r"D:\apps\AgentRMC\config.json")
EXE = r"D:\apps\AgentRMC\AgentRMCNative.exe"
STORE = pathlib.Path(r"C:\Users\PC\AppData\Roaming\LIG\AgentRMCNative\work-items.json")
OUT = pathlib.Path(__file__).resolve().parent / ("shots" + VERSION.replace(".", ""))
OUT.mkdir(exist_ok=True)

CFG_BAK = CFG.with_suffix(".json.shot_bak")
STORE_BAK = STORE.with_suffix(".json.shot_bak")


def backup_state():
    # 每次开跑都重新备份，别复用上一轮可能已拍脏的备份
    shutil.copy2(CFG, CFG_BAK)
    shutil.copy2(STORE, STORE_BAK)

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)

DEMO = [
    # event, title, message, session, project, minutes_ago
    ("permission", "Qoder", "Qoder：要不要把这一批改动一起提交并推送？",
     "sess_demo_qoder_a", "lig-site", 2),
    ("question", "Qoder CN", "Qoder CN：方案 A（原位替换）和方案 B（新增入口）你更想要哪个？",
     "sess_demo_qodercn_b", "tuwen-editor", 3),
    ("stop", "ZCode", "ZCode：远程任务中控台的发布物料已全部就绪，等你验收。",
     "sess_demo_zcode_c", "AgentRMC", 4),
    ("stop", "WorkBuddy", "WorkBuddy：本周设计周报已生成，等你验收。",
     "sess_demo_wb_d", "weekly-report", 6),
    ("progress", "DeepSeek Harness", "DeepSeek Harness：正在跑全量回归测试（42 / 96）。",
     "sess_demo_dsh_e", "AgentRMC", 8),
    ("progress", "OpenSquilla", "OpenSquilla：正在整理本周竞品数据，预计 10 分钟。",
     "sess_demo_os_f", "research", 10),
]

STAGE = {"permission": "needs-me", "question": "needs-me",
         "stop": "review", "progress": "working"}
EXE_OF = {"Qoder": "Qoder.exe", "Qoder CN": "Qoder CN.exe",
          "ZCode": "ZCode.exe", "WorkBuddy": "WorkBuddy.exe",
          "DeepSeek Harness": "DeepSeek Harness.exe",
          "OpenSquilla": "OpenSquilla.exe"}


def write_demo_store():
    data = json.loads(STORE.read_text(encoding="utf-8"))
    items = data if isinstance(data, list) else data.get("items", [])
    now = time.time()
    for it in items:
        if not it.get("done"):
            it["done"] = True
            it["stage"] = "done"
    for event, title, message, sess, project, mins in DEMO:
        ts = time.strftime("%Y-%m-%dT%H:%M:%S.000Z",
                           time.gmtime(now - mins * 60))
        items.append({
            "id": str(uuid.uuid4()),
            "kind": event,
            "stage": STAGE[event],
            "title": title,
            "message": message,
            "project": project,
            "session_id": sess,
            "agent_exe": EXE_OF[title],
            "raise": EXE_OF[title],
            "source": "hook",
            "created_at": ts,
            "updated_at": ts,
            "attention_since": ts,
            "dedupe_key": "%s:%s" % (sess, event),
            "done": False,
        })
    STORE.write_text(json.dumps(data, ensure_ascii=False, indent=1),
                     encoding="utf-8")


def set_cfg(theme, scheme):
    d = json.loads(CFG.read_text(encoding="utf-8"))
    d["ui"]["theme"] = theme
    d["ui"]["color_scheme"] = scheme
    d["ui"]["voice_mode"] = False
    d["ui"]["left_panel_voice"] = True
    CFG.write_text(json.dumps(d, ensure_ascii=False, indent=4), encoding="utf-8")


def restart():
    subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
                   capture_output=True)
    time.sleep(1.0)
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Start-Process '%s'" % EXE], capture_output=True)
    for _ in range(40):
        time.sleep(0.5)
        h = user32.FindWindowW(None, "AgentRMC Native")
        if h:
            return h
    raise RuntimeError("main window never appeared")


def click_name(hwnd, name):
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if (el.CurrentName or "") == name:
            r = el.GetCurrentPropertyValue(30001)
            try:
                l, t, w, h = (r.left, r.top, r.right - r.left, r.bottom - r.top)
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
            time.sleep(1.0)
            return True
    return False


def grab(hwnd, tag, width=1600):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, byref(r))
    w, h = r.right - r.left, r.bottom - r.top
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
        print("FAIL PrintWindow", tag, ok, got)
        return
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    img = img.resize((width, round(h * width / w)), Image.LANCZOS)
    p = OUT / (tag + ".jpg")
    img.save(p, quality=88)
    print("saved", p.name, img.size)


SCENES = [
    ("classic", "dark", "待我处理", "inbox_cl_dark"),
    ("classic", "light", "待我处理", "inbox_cl_light"),
    ("xiaoai-whale", "light", "待我处理", "inbox_whale_light"),
    ("xiaoai-whale", "dark", "待我处理", "inbox_whale_dark"),
    ("classic", "dark", "系统", "system_cl_dark"),
    ("classic", "dark", "应用", "apps_cl_dark"),
    ("classic", "light", "按键映射", "keymap_cl_light"),
    ("classic", "dark", "事件流", "events_cl_dark"),
]

def main():
    print("version:", VERSION, "->", OUT)
    backup_state()
    write_demo_store()
    try:
        current = None
        for theme, scheme, tab, tag in SCENES:
            if (theme, scheme) != current:
                set_cfg(theme, scheme)
                hwnd = restart()
                current = (theme, scheme)
                time.sleep(1.2)
            found = click_name(hwnd, tab)
            if not found:
                print("TAB NOT FOUND:", tab)
            grab(hwnd, tag)
    finally:
        print("restoring config + store")
        shutil.copy2(CFG_BAK, CFG)
        shutil.copy2(STORE_BAK, STORE)
        restart()
    print("DONE")


if __name__ == "__main__":
    main()
