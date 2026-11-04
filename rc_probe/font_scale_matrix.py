"""Font-scale acceptance: set ui.font_scale, relaunch, shoot both forms.
Usage: python font_scale_matrix.py <tag> <scale-percent> [theme] [scheme]
Keeps everything else in config as-is; caller restores the user's config.
"""
import json
import pathlib
import subprocess
import sys
import time

TAG, SCALE = sys.argv[1], sys.argv[2]
THEME = sys.argv[3] if len(sys.argv) > 3 else None
SCHEME = sys.argv[4] if len(sys.argv) > 4 else None
CFG = pathlib.Path(r"D:\apps\AgentRMC\config.json")
EXE = r"D:\apps\AgentRMC\AgentRMCNative.exe"

subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
               capture_output=True)
time.sleep(0.8)
cfg = json.loads(CFG.read_text(encoding="utf-8"))
cfg["ui"]["font_scale"] = SCALE
if THEME is not None:
    cfg["ui"]["theme"] = THEME
if SCHEME is not None:
    cfg["ui"]["color_scheme"] = SCHEME
cfg["ui"]["voice_mode"] = False
cfg["ui"]["left_panel_voice"] = True
CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8")
for i in range(2):
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "(Invoke-CimMethod -ClassName Win32_Process -MethodName "
                    "Create -Arguments @{ CommandLine = '\"%s\"' }).ProcessId"
                    % EXE], capture_output=True)
    time.sleep(4 if i == 0 else 3)
subprocess.run([sys.executable,
                str(pathlib.Path(__file__).parent / "ux_shots.py"), TAG])
print("FONTSCALE_%s_DONE scale=%s" % (TAG, SCALE))
