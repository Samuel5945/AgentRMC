"""顶栏主题矩阵截图（classic/xiaoai-whale × light/dark）。

跑的是 build\\Release\\config.json + build\\Release\\AgentRMCNative.exe（没装机的
那份），所以不动用户装的 config.json；跑完自动还原原值。

用法: python rc_probe/ux_topbar_matrix.py [exe目录]
"""
import ctypes
import json
import pathlib
import subprocess
import sys
import time
from ctypes import wintypes as wt

HERE = pathlib.Path(__file__).parent
ROOT = sys.argv[1] if len(sys.argv) > 1 else r"D:\githubrepos\Agentrmc\native\build\Release"
EXE = str(pathlib.Path(ROOT) / "AgentRMCNative.exe")
CFG = pathlib.Path(ROOT) / "config.json"

ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
u = ctypes.WinDLL("user32")
JOB = ctypes.WinDLL("kernel32")


def launch():
    # 用 explorer 当父进程：Start-Process 起的 GUI 会随工具会话回收。
    subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"], capture_output=True)
    time.sleep(1.0)
    subprocess.run(["explorer.exe", EXE], capture_output=True)
    time.sleep(9)


def place():
    h = u.FindWindowW(None, "AgentRMC Native")
    if not h:
        print("  ! 主窗没起来")
        return False
    u.ShowWindow(h, 9)
    u.MoveWindow(h, 34, 24, 2800, 1520, True)  # 1400x760 @200%
    time.sleep(1.2)
    return True


def shoot(tag):
    subprocess.run([sys.executable, str(HERE / "ux_topbar.py"), tag], check=False)
    return HERE / "out" / f"topbar_{tag}_zoom.png"


def main():
    orig = CFG.read_text(encoding="utf-8")
    cfg = json.loads(orig)
    combos = [("classic-light", "classic", "light"),
              ("classic-dark", "classic", "dark"),
              ("whale-light", "xiaoai-whale", "light"),
              ("whale-dark", "xiaoai-whale", "dark")]
    try:
        for tag, theme, scheme in combos:
            cfg["ui"]["theme"] = theme
            cfg["ui"]["color_scheme"] = scheme
            cfg["ui"]["start_minimized"] = False
            CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=4),
                           encoding="utf-8")
            print(f"== {tag} theme={theme} scheme={scheme}")
            launch()
            if place():
                shoot(f"tb_{tag}")
    finally:
        CFG.write_text(orig, encoding="utf-8")
        print("config restored")
    print("MATRIX_DONE")


main()
