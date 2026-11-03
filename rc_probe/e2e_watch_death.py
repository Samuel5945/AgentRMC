"""Restart-isolation monitor: is AgentRMCNative dying on the voice hotkey?
Phase 1: watch 15s untouched. Phase 2: one Ctrl+Alt+V tap, watch 8s.
Prints alive/recording/state transitions; never taps twice.
"""
import sys
import time
import json
import pathlib
import subprocess
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import send_key  # noqa: E402

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def pid():
    res = subprocess.run(["tasklist", "/FI", "IMAGENAME eq AgentRMCNative.exe",
                          "/FO", "CSV", "/NH"], capture_output=True, text=True,
                         encoding="mbcs", errors="ignore")
    for line in res.stdout.splitlines():
        parts = line.strip().split('","')
        if len(parts) >= 2 and parts[0].strip('"').lower() == "agentrmcnative.exe":
            return int(parts[1].strip('"'))
    return None


def state():
    try:
        with OPENER.open("http://127.0.0.1:45915/state", timeout=1.5) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:
        return None


p0 = pid()
print("START_PID=%s" % p0)
t0 = time.time()
while time.time() - t0 < 15:
    s = state()
    print("t=%4.1f alive=%s state=%s" % (
        time.time() - t0, pid() is not None,
        None if s is None else {k: s.get(k) for k in
                                ("listening", "recording", "transcribing")}))
    time.sleep(2.5)
if pid() is None:
    print("DIED_UNTOUCHED")
    sys.exit(2)
for vk in (0x11, 0x12, 0x56):
    send_key(vk, False)
    time.sleep(0.02)
for vk in (0x56, 0x12, 0x11):
    send_key(vk, True)
    time.sleep(0.02)
print("TAP_SENT")
t0 = time.time()
while time.time() - t0 < 8:
    s = state()
    print("t=%4.1f alive=%s state=%s" % (
        time.time() - t0, pid() is not None,
        None if s is None else {k: s.get(k) for k in
                                ("listening", "recording", "transcribing")}))
    time.sleep(0.4)
print("END_PID=%s" % pid())
