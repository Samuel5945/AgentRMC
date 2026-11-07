# probe_check.py — MyGo 桌宠生死线自动验证（纯 Win32 API，无第三方依赖）
# 子命令: probe | pix | drag | after
import ctypes
import hashlib
import json
import os
import struct
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32
gdi = ctypes.windll.gdi32
u.SetProcessDPIAware()

TITLE = "MyGo Probe"
GA_ROOT = 2
WS_CAPTION = 0x00C00000
WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_LAYERED = 0x00080000
LEFTDOWN, LEFTUP = 0x0002, 0x0004
WM_CLOSE = 0x0010


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class RECT(wintypes.RECT):
    pass


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


u.FindWindowW.restype = wintypes.HWND
u.WindowFromPoint.argtypes = [POINT]
u.WindowFromPoint.restype = wintypes.HWND
u.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
u.GetAncestor.restype = wintypes.HWND

SHOTS = os.path.join(os.environ["TEMP"], "mygoprobe-shots")
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "last_rect.json")


def find_window(timeout=12):
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = u.FindWindowW(None, TITLE)
        if h:
            return h
        time.sleep(0.25)
    raise SystemExit("FAIL: window '%s' not found" % TITLE)


def rect_of(h):
    r = RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def pid_of(h):
    p = wintypes.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(p))
    return p.value


def exstyle(h):
    return u.GetWindowLongW(h, -20)


def hit_root(x, y):
    u.SetCursorPos(int(x), int(y))
    time.sleep(0.25)
    h = u.WindowFromPoint(POINT(int(x), int(y)))
    return u.GetAncestor(h, GA_ROOT)


def hotkey_ctrl_alt_t():
    VK_C, VK_M, VK_T = 0x11, 0x12, 0x54
    for vk, flag in ((VK_C, 0), (VK_M, 0), (VK_T, 0),
                     (VK_T, 2), (VK_M, 2), (VK_C, 2)):
        u.keybd_event(vk, 0, flag, 0)
    time.sleep(0.6)


def grab(x, y, w, hgt):
    """截屏指定区域，返回 (bmp_path, md5, bright_pixel_count)。top-down 32bpp。"""
    os.makedirs(SHOTS, exist_ok=True)
    hdc = u.GetDC(0)
    mem = gdi.CreateCompatibleDC(hdc)
    bmp = gdi.CreateCompatibleBitmap(hdc, w, hgt)
    gdi.SelectObject(mem, bmp)
    gdi.BitBlt(mem, 0, 0, w, hgt, hdc, int(x), int(y), 0x00CC0020)
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -hgt  # top-down
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * hgt * 4)
    gdi.GetDIBits(mem, bmp, 0, hgt, buf, ctypes.byref(bi), 0)
    px = buf.raw
    bright = sum(1 for i in range(0, len(px), 4)
                 if px[i] + px[i + 1] + px[i + 2] > 600)
    md5 = hashlib.md5(px).hexdigest()
    # 存 BMP 供人眼复核（BMP 头 54 字节 + 像素）
    fh = struct.pack("<2sIHHI", b"BM", 54 + len(px), 0, 0, 54)
    path = os.path.join(SHOTS, "grab_%d_%d_%dx%d.bmp" % (x, y, w, hgt))
    with open(path, "wb") as f:
        f.write(fh + bytes(bi) + px)
    gdi.DeleteObject(bmp)
    gdi.DeleteDC(mem)
    u.ReleaseDC(0, hdc)
    return path, md5, bright


def cmd_probe():
    h = find_window()
    r = rect_of(h)
    w, hgt = r[2] - r[0], r[3] - r[1]
    probe_pid = pid_of(h)
    style = u.GetWindowLongW(h, -16)
    ex = exstyle(h)
    cx = (r[0] + r[2]) // 2
    cy = r[1] + hgt // 4  # 立绘区域（DragWindow 手柄）
    base = pid_of(hit_root(cx, cy))

    hotkey_ctrl_alt_t()
    root_on = pid_of(hit_root(cx, cy))

    hotkey_ctrl_alt_t()
    root_off = pid_of(hit_root(cx, cy))

    report = {
        "found": True,
        "pid": probe_pid,
        "rect": list(r),
        "size": [w, hgt],
        "frameless": not (style & WS_CAPTION),
        "topmost": bool(ex & WS_EX_TOPMOST),
        "skip_taskbar_toolwindow": bool(ex & WS_EX_TOOLWINDOW),
        "layered": bool(ex & WS_EX_LAYERED),
        "baseline_hit_is_probe": base == probe_pid,
        "passthrough_hit_differs": root_on != probe_pid,
        "revert_hit_is_probe": root_off == probe_pid,
    }
    with open(STATE, "w") as f:
        json.dump({"rect": list(r)}, f)
    print(json.dumps(report, indent=1))
    ok = (report["frameless"] and report["topmost"] and
          report["baseline_hit_is_probe"] and
          report["passthrough_hit_differs"] and
          report["revert_hit_is_probe"])
    print("PROBE VERDICT:", "PASS" if ok else "FAIL")


def cmd_pix():
    h = find_window()
    r = rect_of(h)
    x0, y0, x1, y1 = r
    w, hgt = x1 - x0, y1 - y0
    # 角落 24x24：窗口矩形内、立绘/卡片之外 → 若真逐像素透明，关窗前后应逐位相同
    corner_path, corner_md5, _ = grab(x0 + 2, y0 + 2, 24, 24)
    # 卡片区域（窗口底部 40px 高、避开圆角）：应渲染出高亮文本
    card_path, card_md5, card_bright = grab(x0 + 16, y1 - 44, w - 32, 36)
    with open(STATE, "r") as f:
        st = json.load(f)
    st["corner"] = {"path": corner_path, "md5": corner_md5,
                    "x": x0 + 2, "y": y0 + 2}
    st["card"] = {"path": card_path, "md5": card_md5, "bright": card_bright}
    with open(STATE, "w") as f:
        json.dump(st, f)
    print(json.dumps({"corner_md5": corner_md5, "card_md5": card_md5,
                      "card_bright_px": card_bright}, indent=1))


def cmd_after():
    with open(STATE, "r") as f:
        st = json.load(f)
    p, _, _ = grab(st["corner"]["x"], st["corner"]["y"], 24, 24)
    # 直接重读像素比对
    with open(st["corner"]["path"], "rb") as f:
        before = f.read()
    with open(p, "rb") as f:
        after = f.read()
    print(json.dumps({
        "corner_identical_after_close": before == after,
        "before_path": st["corner"]["path"],
        "after_path": p,
    }, indent=1))


def cmd_drag():
    h = find_window()
    r0 = rect_of(h)
    x0, y0, x1, y1 = r0
    cx = (x0 + x1) // 2
    cy = y0 + (y1 - y0) // 4  # 立绘（DragWindow 手柄）
    u.SetCursorPos(cx, cy)
    time.sleep(0.3)
    u.mouse_event(LEFTDOWN, 0, 0, 0, 0)
    for i in range(1, 17):
        u.SetCursorPos(cx + i * 12, cy + i * 7)
        time.sleep(0.03)
    time.sleep(0.2)
    u.mouse_event(LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)
    r1 = rect_of(h)
    moved = (r1[0] - x0, r1[1] - y0)
    print(json.dumps({
        "before": [x0, y0], "after": [r1[0], r1[1]],
        "moved": moved, "drag_works": moved != (0, 0),
    }, indent=1))


def cmd_close():
    h = find_window(5)
    if h:
        u.PostMessageW(h, WM_CLOSE, 0, 0)
        time.sleep(1.0)
        if u.FindWindowW(None, TITLE):
            os.system("taskkill /IM mygo-probe.exe /F >nul 2>&1")
            print("closed via taskkill")
        else:
            print("closed via WM_CLOSE")
    else:
        print("already gone")


if __name__ == "__main__":
    {"probe": cmd_probe, "pix": cmd_pix, "drag": cmd_drag,
     "after": cmd_after, "close": cmd_close}[sys.argv[1]]()
