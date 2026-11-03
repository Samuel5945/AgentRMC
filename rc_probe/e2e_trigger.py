"""Fire the product's global voice hotkey (Ctrl+Alt+V) once to start a
recording session; silence auto-stop (700ms) should finish it and hit the
stub ASR. If no stub request shows up in 10s, tap once more to toggle-finish.
Never taps a third time: a spare tap would start a SECOND session.
"""
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import send_key, user32  # noqa: E402

LOG = pathlib.Path(r"D:\githubrepos\Agentrmc\rc_probe\e2e_asr_stub.log")


def posts():
    if not LOG.exists():
        return 0
    return sum(1 for line in LOG.read_text(encoding="utf-8").splitlines()
               if " POST " in line)


def tap():
    for vk in (0x11, 0x12, 0x56):
        send_key(vk, False)
        time.sleep(0.02)
    for vk in (0x56, 0x12, 0x11):
        send_key(vk, True)
        time.sleep(0.02)


down = [k for k, vk in (("L", 0x01), ("R", 0x02), ("M", 0x04))
        if user32.GetAsyncKeyState(vk) & 0x8000]
if down:
    print("USER_INPUT_ACTIVE=%s ABORT" % down)
    sys.exit(1)

base = posts()
print("BASE_POSTS=%d" % base)
tap()
print("TAP1_SENT")
deadline = time.time() + 10
while time.time() < deadline and posts() == base:
    time.sleep(0.3)
if posts() == base:
    print("NO_STUB_REQUEST_AFTER_10S -> TAP2 (toggle finish)")
    tap()
    deadline = time.time() + 6
    while time.time() < deadline and posts() == base:
        time.sleep(0.3)
print("POSTS_NOW=%d (base=%d)" % (posts(), base))
print(LOG.read_text(encoding="utf-8") if LOG.exists() else "<no log>")
