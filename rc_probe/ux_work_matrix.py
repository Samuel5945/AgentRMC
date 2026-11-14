"""待办页「会话标题」验收矩阵：classic×light / classic×dark / xiaoai-whale。

隔离方式：应用停着的时候把真实 work-items.json 与 spool 挪去备份，铺合成卡
（真实会话 id，标题走宿主投影缓存），跑完原样还原 + sha1 断言——Qt 的
AppDataLocation 走 SHGetKnownFolderPath，改 APPDATA 环境变量没用（实测踩过）。
合成场景三条卡：同项目两条（重名→补标题）、无项目一条（行身份退标题）。
应用经 WMI 拉起（活过工具 call）；截图前 AttachThreadInput 抢前台。
用法：python ux_work_matrix.py
"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import ctypes.wintypes as wt
import hashlib
import json
import pathlib
import shutil
import subprocess
import time
from ctypes import byref, create_unicode_buffer
from datetime import datetime, timedelta, timezone

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation
from PIL import ImageGrab

ROOT = pathlib.Path(r"D:\githubrepos\Agentrmc")
CFG = pathlib.Path(r"D:\apps\AgentRMC\config.json")
EXE = r"D:\apps\AgentRMC\AgentRMCNative.exe"
BACKUP = ROOT / "rc_probe" / "_ux_work_backup"
DATA_DIR = pathlib.Path.home() / "AppData/Roaming/LIG/AgentRMCNative"
REAL_STORE = DATA_DIR / "work-items.json"
REAL_SPOOL = DATA_DIR / "pending-events.jsonl"
ROUNDS = [("work_classic_light", "classic", "light"),
          ("work_classic_dark", "classic", "dark"),
          ("work_whale", "xiaoai-whale", "light")]

uia = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32


def sha1(p):
    return hashlib.sha1(p.read_bytes()).hexdigest() if p.exists() else ""


def taskkill():
    subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
                   capture_output=True)
    time.sleep(0.8)


def wmi_launch(command_line):
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "(Invoke-CimMethod -ClassName Win32_Process -MethodName "
                    "Create -Arguments @{ CommandLine = '%s' }).ProcessId"
                    % command_line.replace("'", "''")], capture_output=True)


def seed_store():
    """三条合成卡：同项目重名两条 + 无项目一条；会话 id 都是真实投影。"""
    now = datetime.now(timezone.utc)

    def iso(dt):
        return dt.strftime("%Y-%m-%dT%H:%M:%S.000Z")

    def card(cid, sid, project, stage, kind, message, waited_min=0):
        att = now - timedelta(minutes=waited_min)
        return {"id": cid, "source": "hook",
                "agent_exe": "DeepSeek Harness.exe",
                "session_id": sid, "project": project, "kind": kind,
                "stage": stage, "title": "DeepSeek Harness",
                "message": message, "raise": "DeepSeek Harness.exe",
                "dedupe_key": sid + ":" + kind,
                "created_at": iso(att), "updated_at": iso(att),
                "attention_since": iso(att) if stage != "working" else "",
                "done": False}

    items = [
        card("probe-b", "session-1d3d9c52-d043-457e-a1ac-023ac28d96b4",
             r"D:\githubrepos\Agentrmc", "needs-me", "permission",
             "等你确认工具授权（同项目重名卡二）", waited_min=25),
        card("probe-a", "session-1001ba4c-399e-45c5-9aa8-8262e01894b7",
             r"D:\githubrepos\Agentrmc", "review", "stop",
             "修复完成，等你验收（同项目重名卡一）", waited_min=10),
        card("probe-c", "session-ba443b2d-0a59-495b-8b9f-15accc878cf0",
             "", "working", "progress",
             "项目为空：行身份应退会话标题而不是短 id"),
    ]
    REAL_STORE.write_text(
        json.dumps({"items": items}, ensure_ascii=False), encoding="utf-8")


def windows_for(exe_name):
    pids = set()
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + exe_name,
                          "/FO", "CSV", "/NH"], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    for line in out.stdout.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[1].isdigit():
            pids.add(int(parts[1]))
    wins = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            b = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, b, n + 1)
            wins.append((h, b.value))
        return True
    user32.EnumWindows(CB(cb), 0)
    return wins


def chrome_child(hwnd):
    children = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, l):
        b = create_unicode_buffer(64)
        user32.GetClassNameW(h, b, 64)
        children.append((h, b.value))
        return True
    user32.EnumChildWindows(hwnd, CB(cb), 0)
    return next((h for h, c in children if c == "Chrome_WidgetWin_1"), None)


def force_foreground(hwnd):
    # ALT 抖一下拿前台权限，再 AttachThreadInput 挂到当前前台线程上抢焦点。
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    fg = user32.GetForegroundWindow()
    fg_tid = user32.GetWindowThreadProcessId(fg, None)
    my_tid = kernel32.GetCurrentThreadId()
    attached = bool(user32.AttachThreadInput(my_tid, fg_tid, True))
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    if attached:
        user32.AttachThreadInput(my_tid, fg_tid, False)
    time.sleep(0.7)


def click_name(root, name):
    arr = root.FindAll(4, uia.CreateTrueCondition())  # TreeScope_Descendants
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if (el.CurrentName or "") == name and \
                el.GetCurrentPropertyValue(30003) == 50000:  # Button
            r = el.GetCurrentPropertyValue(30001)
            l, t, w, h = (tuple(r) if isinstance(r, (tuple, list))
                          else (r.left, r.top, r.right - r.left,
                                r.bottom - r.top))
            pt = wt.POINT()
            user32.GetCursorPos(byref(pt))
            old = (pt.x, pt.y)
            user32.SetCursorPos(int(l + w / 2), int(t + h / 2))
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.05)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            user32.SetCursorPos(*old)
            time.sleep(0.8)
            return True
    return False


def shot(hwnd, tag):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, byref(r))
    path = ROOT / "rc_probe" / ("ux_%s.png" % tag)
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                   all_screens=True).save(str(path))
    print("SHOT %s" % path)


def main():
    store_before = sha1(REAL_STORE)
    spool_original = (BACKUP / "spool-original.jsonl")
    spool_extra = (BACKUP / "spool-extra.jsonl")
    orig_cfg = json.loads(CFG.read_text(encoding="utf-8"))
    orig_ui = dict(orig_cfg.get("ui", {}))
    BACKUP.mkdir(parents=True, exist_ok=True)
    # 备份只做一次（轮间再备份会把合成卡当成真实库存下来）。
    taskkill()
    if REAL_STORE.exists():
        shutil.copy2(REAL_STORE, BACKUP / "work-items.json")
    if REAL_SPOOL.exists():
        shutil.move(str(REAL_SPOOL), str(spool_original))
    try:
        for tag, theme, scheme in ROUNDS:
            taskkill()
            # 停应用期间插件补投的 spool 收进额外备份，还原时接回原 spool 尾部。
            if REAL_SPOOL.exists():
                with open(spool_extra, "ab") as out:
                    out.write(REAL_SPOOL.read_bytes())
                REAL_SPOOL.unlink()
            seed_store()
            cfg = json.loads(CFG.read_text(encoding="utf-8"))
            cfg.setdefault("ui", {})
            cfg["ui"]["theme"] = theme
            cfg["ui"]["color_scheme"] = scheme
            cfg["ui"]["voice_mode"] = False
            cfg["ui"]["left_panel_voice"] = True
            CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=4),
                           encoding="utf-8")
            wmi_launch('"%s"' % EXE)
            time.sleep(7)
            mains = [h for h, t in windows_for("AgentRMCNative.exe")
                     if t == "AgentRMC Native"]
            if not mains:
                print("NO_MAIN %s" % tag)
                continue
            hwnd = mains[0]
            force_foreground(hwnd)
            web = chrome_child(hwnd)
            if web:
                root = uia.ElementFromHandle(web)
                print("tab click %s: %s" % (tag, click_name(root, "待我处理")))
            else:
                print("NO_CHROME_CHILD %s" % tag)
            time.sleep(1.0)
            force_foreground(hwnd)
            shot(hwnd, tag)
    finally:
        taskkill()
        # 原样还原真实库；spool = 原件 + 轮间补投，按序接回。
        if (BACKUP / "work-items.json").exists():
            shutil.copy2(BACKUP / "work-items.json", REAL_STORE)
        if spool_original.exists() or spool_extra.exists():
            with open(REAL_SPOOL, "wb") as out:
                if spool_original.exists():
                    out.write(spool_original.read_bytes())
                if spool_extra.exists():
                    out.write(spool_extra.read_bytes())
        cfg = json.loads(CFG.read_text(encoding="utf-8"))
        cfg["ui"] = orig_ui
        CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=4),
                       encoding="utf-8")
        wmi_launch('"%s"' % EXE)
        time.sleep(6)
    print("REAL_STORE_UNTOUCHED %s" % (sha1(REAL_STORE) == store_before))
    restored = REAL_SPOOL.read_bytes() if REAL_SPOOL.exists() else b""
    original = spool_original.read_bytes() if spool_original.exists() else b""
    print("SPOOL_ORIGINAL_PRESERVED %s" % restored.startswith(original))
    state = subprocess.run(["curl.exe", "-s", "--noproxy", "*",
                            "http://127.0.0.1:45915/state"],
                           capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    print("STATE_ALIVE %s" % bool((state.stdout or "").strip().startswith("{")))
    print("WORK_MATRIX_DONE")


if __name__ == "__main__":
    main()