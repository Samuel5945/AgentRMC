"""Screenshot the main window as-is (no clicks). Usage: python ux_main_shot.py [tag]"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import sys
import time
import pathlib
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

from PIL import ImageGrab

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32  # noqa: E402

TAG = sys.argv[1] if len(sys.argv) > 1 else "main"
pids = set(pids_for("AgentRMCNative.exe"))
out = []
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def cb(h, l):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, byref(pid))
    if pid.value in pids and user32.IsWindowVisible(h):
        n = user32.GetWindowTextLengthW(h)
        b = create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, b, n + 1)
        out.append((h, b.value))
    return True


user32.EnumWindows(CB(cb), 0)
main = [h for h, t in out if t == "AgentRMC Native"]
if not main:
    print("NO_MAIN")
    sys.exit(1)
user32.SetForegroundWindow(main[0])
time.sleep(0.5)
r = wt.RECT()
user32.GetWindowRect(main[0], byref(r))
ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).save(
    r"D:\githubrepos\Agentrmc\rc_probe\ux_%s.png" % TAG)
print("DONE")
