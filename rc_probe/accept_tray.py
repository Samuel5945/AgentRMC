"""打包版端到端验收：关窗只隐藏且监听不停 + 启动不再闪窗。

跑法：python rc_probe/accept_tray.py
会临时把 release 配置的 start_minimized 改成 false 来拿到一个可见窗口，
测完恢复原值并正常启动一次。
"""
import ctypes
import hashlib
import json
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
u = ctypes.windll.user32
ROOT = Path(__file__).resolve().parent.parent
PKG = ROOT / "release" / "AgentRMC"
EXE = PKG / "AgentRMC.exe"
CFG = PKG / "config.json"
LOG = PKG / "rc_probe" / "gui_events.log"
WM_CLOSE = 0x0010
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def onscreen(pid):
    """返回该进程所有"可见且在某个显示器范围内"的顶层窗口。"""
    hits = []

    def cb(h, _l):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != pid or not u.IsWindowVisible(h):
            return True
        r = wintypes.RECT()
        u.GetWindowRect(h, ctypes.byref(r))
        w, ht = r.right - r.left, r.bottom - r.top
        # 显示器范围：本机三屏，正常主窗口一定落在 x∈[-2560,2906], y∈[-55,2983]
        if -2560 <= r.left <= 2906 and -55 <= r.top <= 2983 and w > 300:
            t = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(h, t, 256)
            hits.append((h, t.value, (r.left, r.top, w, ht)))
        return True

    u.EnumWindows(CB(cb), 0)
    return hits


def pid_of():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq AgentRMC.exe",
                          "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.strip('"').split('","')
        if len(parts) > 1 and parts[0] == "AgentRMC.exe" and parts[1].isdigit():
            return int(parts[1])
    return None


def kill(pid):
    if pid:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        time.sleep(1.5)


def launch():
    subprocess.Popen([str(EXE)], cwd=str(PKG))
    time.sleep(6)
    return pid_of()


orig = json.loads(CFG.read_text(encoding="utf-8"))
shutil.copy2(CFG, ROOT / "rc_probe" / "accept_cfg_backup.json")
print("原配置 ui =", orig["ui"])
log_base = LOG.read_text(encoding="utf-8").count("\n") if LOG.exists() else 0

# ---- 阶段 1：可见窗口 + 关窗只隐藏 ----
cfg = json.loads(json.dumps(orig))
cfg["ui"]["start_minimized"] = False
CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
kill(pid_of())
pid = launch()
wins = onscreen(pid) if pid else []
print(f"[1] 启动后进程 {pid}，屏幕内窗口 {len(wins)} 个：{wins}")
assert pid and wins, "阶段1 没拿到可见主窗口"
hwnd = wins[0][0]
u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
time.sleep(2.5)
alive = pid_of()
after = onscreen(alive) if alive else []
body = LOG.read_text(encoding="utf-8") if LOG.exists() else ""
new = body.split("\n")[log_base:]
stopped = any("监听已停止" in x for x in new)
print(f"[1] WM_CLOSE 后：进程存活={bool(alive)} 屏幕内窗口={len(after)} "
      f"日志出现『监听已停止』={stopped}")
assert alive and not after and not stopped, "关窗应当只隐藏、监听不停"
print("[1] PASS 关窗=收进托盘继续监听")

# ---- 阶段 2：恢复 start_minimized，确认启动不闪窗 ----
kill(alive)
CFG.write_text(json.dumps(orig, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
assert (hashlib.sha256(CFG.read_bytes()).hexdigest()
        == hashlib.sha256((ROOT / "rc_probe" / "accept_cfg_backup.json").read_bytes()).hexdigest())
log_base = len(body.split("\n")) - 1
pid = launch()
t0 = time.monotonic()
flashes = []
while time.monotonic() - t0 < 5.0:
    for h, t, rect in onscreen(pid):
        if (t, rect) not in [f[1:] for f in flashes]:
            flashes.append((time.monotonic() - t0, t, rect))
    time.sleep(0.1)
body2 = LOG.read_text(encoding="utf-8")
new2 = [x for x in body2.split("\n")[log_base:] if x.strip()]
print(f"[2] 藏托盘启动 5s：屏幕内窗口 {len(flashes)} 个 {flashes}")
print(f"[2] 新日志 {len(new2)} 条：{[json.loads(x).get('text', json.loads(x)['kind']) for x in new2[:3]]}")
assert not flashes, "启动仍然闪窗"
assert any("监听已启动" in x for x in new2), "启动后没开始监听"
print("[2] PASS 启动不闪窗，且后台照常监听")
print("\n全部通过；当前进程", pid_of())
