"""WebView2 原型验收：列出 AgentRMCNative 进程的可见窗口，并把主窗截下来。

为什么不用 --shot：那条路走 QWidget::grab()，WebView2 是独立子 HWND，
grab 抓不到它的画面（截出来是一块白）。这里用两种真办法各来一张：
  1) PrintWindow + PW_RENDERFULLCONTENT —— 不受遮挡/跨屏影响，Chromium 子窗能进
  2) 屏幕区域抓取 —— 万一 PrintWindow 拿不到合成层，用这张兜底
另外把窗口标题/几何打出来，确认左 Qt 右 WebView 的分栏到底有没有出来。

用法: python rc_probe/web_shell_shot.py [输出前缀]
"""
import ctypes
import ctypes.wintypes as wt
import pathlib
import sys

ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
from PIL import Image, ImageGrab  # noqa: E402

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

TAG = sys.argv[1] if len(sys.argv) > 1 else "web_shell"
OUT = pathlib.Path("rc_probe")

targets = set()
pid = wt.DWORD()


def pids_of_image(exe):
    """按进程名收集 PID（TH32CS_SNAPPROCESS）。"""
    class PE32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                    ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                    ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG),
                    ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_wchar * 260)]

    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    entry = PE32()
    entry.dwSize = ctypes.sizeof(PE32)
    out = set()
    if kernel32.Process32FirstW(snap, ctypes.byref(entry)):
        while True:
            if entry.szExeFile.lower() == exe.lower():
                out.add(entry.th32ProcessID)
            if not kernel32.Process32NextW(snap, ctypes.byref(entry)):
                break
    kernel32.CloseHandle(snap)
    return out


kernel32 = ctypes.windll.kernel32
targets = pids_of_image("AgentRMCNative.exe")
if not targets:
    print("没有 AgentRMCNative.exe 在跑")
    sys.exit(1)

wins = []
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def cb(h, _):
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if pid.value in targets and user32.IsWindowVisible(h):
        n = user32.GetWindowTextLengthW(h)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, buf, n + 1)
        r = wt.RECT()
        user32.GetWindowRect(h, ctypes.byref(r))
        style = user32.GetWindowLongW(h, -16)  # GWL_STYLE
        wins.append((h, buf.value, (r.left, r.top, r.right, r.bottom), style))
    return True


user32.EnumWindows(CB(cb), 0)
for h, title, box, style in wins:
    print(f"hwnd={h:#x} title={title!r} rect={box} "
          f"size={box[2]-box[0]}x{box[3]-box[1]} style={style:#x}")

# 主窗就是标题 'AgentRMC Native' 那个（无边框自定义顶栏，别按 WS_CAPTION 找；
# 桌宠是 168x168 的置顶小窗，标题 'AgentRMCNative'）。
def is_main(item):
    _h, title, _box, _style = item
    return title == "AgentRMC Native"


mains = [w for w in wins if is_main(w)]
if not mains:
    print("没找到主窗（可能还在托盘里）")
    sys.exit(1)
h, title, box, _ = max(mains, key=lambda w: (w[2][2]-w[2][0]) * (w[2][3]-w[2][1]))
print(f"主窗 {title!r} {box}")

user32.ShowWindow(h, 5)   # SW_SHOW
user32.SetForegroundWindow(h)
import time
time.sleep(0.4)

r = wt.RECT()
user32.GetWindowRect(h, ctypes.byref(r))
box = (r.left, r.top, r.right, r.bottom)

# --- 1) PrintWindow ---
w, hh = box[2] - box[0], box[3] - box[1]
bmp = gdi32.CreateCompatibleBitmap(user32.GetDC(0), w, hh)
dc = gdi32.CreateCompatibleDC(user32.GetDC(0))
gdi32.SelectObject(dc, bmp)
ok = user32.PrintWindow(h, dc, 2)  # PW_RENDERFULLCONTENT
print("PrintWindow:", ok)
if ok:
    class BI(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                    ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bmi = BI()
    bmi.biSize = ctypes.sizeof(BI)
    bmi.biWidth, bmi.biHeight = w, -hh
    bmi.biPlanes, bmi.biBitCount, bmi.biCompression = 1, 32, 0
    buf = ctypes.create_string_buffer(w * hh * 4)
    if gdi32.GetDIBits(dc, bmp, 0, hh, buf, ctypes.byref(bmi), 0):
        img = Image.frombuffer("RGBA", (w, hh), buf, "raw", "BGRA", 0, 1)
        p = OUT / f"{TAG}_printwindow.png"
        img.convert("RGB").save(p)
        print("saved", p, img.size)
    gdi32.DeleteDC(dc)
    gdi32.DeleteObject(bmp)

# --- 2) 屏幕抓取 ---
try:
    shot = ImageGrab.grab(bbox=box, all_screens=True)
    p = OUT / f"{TAG}_screen.png"
    shot.save(p)
    print("saved", p, shot.size)
except Exception as e:  # noqa: BLE001
    print("屏幕抓取失败:", e)
