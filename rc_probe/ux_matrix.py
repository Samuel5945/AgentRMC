"""Switch theme/scheme with the app stopped, relaunch (WMI-free: Start-Process
would die with the exec tree, so use WMI via powershell), wake main, shoot both
forms. Usage: python ux_matrix.py <tag> <theme> <scheme>
"""
import json
import pathlib
import subprocess
import sys
import time

TAG, THEME, SCHEME = sys.argv[1], sys.argv[2], sys.argv[3]
CFG = pathlib.Path(r"D:\apps\AgentRMC\config.json")
EXE = r"D:\apps\AgentRMC\AgentRMCNative.exe"

subprocess.run(["taskkill", "/IM", "AgentRMCNative.exe", "/F"],
               capture_output=True)
time.sleep(0.8)
cfg = json.loads(CFG.read_text(encoding="utf-8"))
cfg["ui"]["theme"] = THEME
cfg["ui"]["color_scheme"] = SCHEME
CFG.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8")
for i in range(2):
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "(Invoke-CimMethod -ClassName Win32_Process -MethodName "
                    "Create -Arguments @{ CommandLine = '\"%s\"' }).ProcessId"
                    % EXE], capture_output=True)
    time.sleep(4 if i == 0 else 3)
subprocess.run([sys.executable,
                str(pathlib.Path(__file__).parent / "ux_shots.py"), TAG])
print("MATRIX_%s_DONE theme=%s scheme=%s" % (TAG, THEME, SCHEME))
