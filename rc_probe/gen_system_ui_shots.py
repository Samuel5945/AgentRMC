"""给「语音系统控制」的新 UI 出验收图：语音设置对话框 + 应用页底栏，
跑 classic×light / classic×dark / 鲸鱼娘 三档，跑完把 build 的 config.json 还原。
全程走 exe 的 --shot/--shot-settings 离屏渲染，不动正在跑的实例、不抢端口。
"""
import json
import pathlib
import subprocess

REL = pathlib.Path(r"D:\githubrepos\Agentrmc\native\build\Release")
CFG = REL / "config.json"
EXE = str(REL / "AgentRMCNative.exe")
OUT = pathlib.Path(r"D:\githubrepos\Agentrmc\rc_probe")

SCENES = [
    ("cl_light", "classic", "light"),
    ("cl_dark", "classic", "dark"),
    ("whale", "xiaoai-whale", "dark"),
]

original = CFG.read_text(encoding="utf-8")


def shoot(name, args):
    path = str(OUT / name)
    target = pathlib.Path(path)
    if target.exists():
        target.unlink()
    # 这些 flag 都按「取下一个参数」读值，所以路径必须紧跟在第一个 flag 后面。
    argv = [EXE, args[0], path] + args[1:]
    subprocess.run(argv, capture_output=True)
    print("%-32s %s" % (name, "OK" if target.exists() else "MISSING"))


try:
    for tag, theme, scheme in SCENES:
        cfg = json.loads(original)
        cfg["ui"]["theme"] = theme
        cfg["ui"]["color_scheme"] = scheme
        cfg["ui"]["voice_mode"] = False
        cfg["ui"]["left_panel_voice"] = False
        CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8")
        # 语音设置对话框：新的四个分类开关在这里
        shoot("ux_sys_%s_settings.png" % tag, ["--shot-settings"])
        # 应用页底栏：新的「语音系统控制」总开关在这里（应用是第 6 个页签）
        shoot("ux_sys_%s_app.png" % tag, ["--shot", "--tab", "5"])
finally:
    CFG.write_text(original, encoding="utf-8")
    print("config restored")
