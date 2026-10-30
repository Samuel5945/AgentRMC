"""两个低层钩子在不同线程安装，看谁先收到注入键（不吞真实键盘）。"""
import ctypes as C, ctypes.wintypes as W, sys, threading, time
sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent.parent))
import agentrc as core
user32 = core.user32
WH_KEYBOARD_LL = 13
HOOKPROC = C.WINFUNCTYPE(C.c_ssize_t, C.c_int, W.WPARAM, W.LPARAM)
log = []
lock = threading.Lock()
ready = threading.Event()
threads = []

class KBDLLHOOKSTRUCT(C.Structure):
    _fields_ = [("vkCode", W.DWORD), ("scanCode", W.DWORD), ("flags", W.DWORD),
                ("time", W.DWORD), ("dwExtraInfo", C.c_size_t)]

def make_cb(tag):
    def cb(n, w, l):
        if n == 0:
            kb = C.cast(l, C.POINTER(KBDLLHOOKSTRUCT)).contents
            if kb.vkCode == 0x7C:
                with lock: log.append((tag, time.perf_counter()))
                return 1
        return user32.CallNextHookEx(None, n, w, l)
    return HOOKPROC(cb)

procs = []
def start_hook(tag):
    proc = make_cb(tag); procs.append(proc)
    h = user32.SetWindowsHookExW(WH_KEYBOARD_LL, proc, None, 0)
    print(tag, 'installed', bool(h), flush=True)
    msg = W.MSG()
    while user32.GetMessageW(C.byref(msg), None, 0, 0):
        user32.TranslateMessage(C.byref(msg)); user32.DispatchMessageW(C.byref(msg))

for tag in ('A', 'B'):
    t = threading.Thread(target=start_hook, args=(tag,), daemon=True)
    t.start(); threads.append(t); time.sleep(0.15)

core._send_key(0x7C); core._send_key(0x7C, up=True)
time.sleep(0.2)
print('log:', log)
if log: print('FIRST =', log[0][0])
print('ORDER =', 'B-first (last-installed wins)' if log and log[0][0] == 'B' else 'A-first (first-installed wins)')
