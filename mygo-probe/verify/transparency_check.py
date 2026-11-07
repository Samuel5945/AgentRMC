# transparency_check.py — 透明度铁证验证：探针身后垫纯红板，三段采样。
#
# 判据（窗角 24x24，避开立绘和卡片）：
#   pre  = 探针未开、红板在   → 应为红 (255,0,0)，否则背景板没铺好
#   mid  = 探针开着           → 红 = 真透明；白 = 白底实锤
#   post = 探针关了           → 应与 pre 相同（背景板稳定）
# 同时 mid 采卡片区域亮像素，确认立绘/卡片内容还在渲染。
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = (sys.argv[1] if len(sys.argv) > 1
       else os.path.abspath(os.path.join(HERE, "..", "mygo-probe.exe")))
TAG = os.path.splitext(os.path.basename(EXE))[0]
SHOT = os.path.join(HERE, "shot.ps1")
SHOTS_DIR = os.path.join(os.environ["TEMP"], "mygoprobe-shots")
TITLE = "MyGo Probe"
WM_CLOSE = 0x0010

u = ctypes.windll.user32
gdi = ctypes.windll.gdi32
u.SetProcessDPIAware()


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


def wait_window(timeout=12):
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = u.FindWindowW(None, TITLE)
        if h:
            return h
        time.sleep(0.25)
    raise SystemExit("FAIL: window not found")


def rect_of(h):
    r = RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def close_probe(h):
    u.PostMessageW(h, WM_CLOSE, 0, 0)
    for _ in range(20):
        if not u.FindWindowW(None, TITLE):
            return True
        time.sleep(0.25)
    return False


def grab(x, y, w, hgt):
    """截屏区域，返回 (bmp_path, md5, 中心像素RGB, 亮像素数)。"""
    os.makedirs(SHOTS_DIR, exist_ok=True)
    hdc = u.GetDC(0)
    mem = gdi.CreateCompatibleDC(hdc)
    bmp = gdi.CreateCompatibleBitmap(hdc, w, hgt)
    gdi.SelectObject(mem, bmp)
    gdi.BitBlt(mem, 0, 0, w, hgt, hdc, int(x), int(y), 0x00CC0020)
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -hgt
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * hgt * 4)
    gdi.GetDIBits(mem, bmp, 0, hgt, buf, ctypes.byref(bi), 0)
    px = buf.raw
    cx, cy = w // 2, hgt // 2
    i = (cy * w + cx) * 4  # BGRA
    center_rgb = (px[i + 2], px[i + 1], px[i])
    bright = sum(1 for j in range(0, len(px), 4)
                 if px[j] + px[j + 1] + px[j + 2] > 600)
    md5 = hashlib.md5(px).hexdigest()
    fh = struct_pack_bmp(54 + len(px))
    path = os.path.join(SHOTS_DIR, "t_%d_%d_%dx%d.bmp" % (x, y, w, hgt))
    with open(path, "wb") as f:
        f.write(fh + bytes(bi) + px)
    gdi.DeleteObject(bmp)
    gdi.DeleteDC(mem)
    u.ReleaseDC(0, hdc)
    return path, md5, center_rgb, bright


def struct_pack_bmp(size):
    import struct
    return struct.pack("<2sIHHI", b"BM", size, 0, 0, 54)


def is_white(rgb):
    return all(v >= 245 for v in rgb)


def is_red(rgb):
    return rgb[0] >= 240 and rgb[1] <= 40 and rgb[2] <= 40


def main():
    # 1) 标定：探针放 (400,400)，读实际物理 rect
    cal = subprocess.Popen([EXE, "-x", "400", "-y", "400"])
    h = wait_window()
    r = rect_of(h)
    close_probe(h)
    cal.wait(timeout=10)
    time.sleep(0.5)

    # 2) 红板：盖住探针 rect 外扩 120 物理像素
    bx, by = r[0] - 120, r[1] - 120
    bw, bh = (r[2] - r[0]) + 240, (r[3] - r[1]) + 240
    red = subprocess.Popen([
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", os.path.join(HERE, "redboard.ps1"),
        "-X", str(bx), "-Y", str(by), "-W", str(bw), "-H", str(bh), "-Ms", "45000",
    ])
    time.sleep(2.0)

    # 3) 三段采样
    _, pre_md5, pre_rgb, _ = grab(r[0] + 4, r[1] + 4, 24, 24)

    probe = subprocess.Popen([EXE, "-x", "400", "-y", "400"])
    h2 = wait_window()
    r2 = rect_of(h2)
    time.sleep(0.8)
    _, mid_md5, mid_rgb, card_bright = grab(r2[0] + 4, r2[1] + 4, 24, 24)
    _, _, _, card_bright2 = grab(r2[0] + 16, r2[3] - 44, (r2[2] - r2[0]) - 32, 36)

    subprocess.run([
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", SHOT, "-Path",
        os.path.join(SHOTS_DIR, "shot3_%s.png" % TAG),
    ], capture_output=True)

    close_probe(h2)
    probe.wait(timeout=10)
    time.sleep(0.6)
    _, post_md5, post_rgb, _ = grab(r2[0] + 4, r2[1] + 4, 24, 24)

    red.terminate()  # 关红板（它也有 45s 自关兜底）

    if not is_red(pre_rgb) or not is_red(post_rgb) or pre_md5 != post_md5:
        verdict = "INCONCLUSIVE（背景板不稳: pre=%s post=%s）" % (pre_rgb, post_rgb)
    elif is_red(mid_rgb):
        verdict = "PASS 真透明（窗角=红板色）"
    elif is_white(mid_rgb):
        verdict = "FAIL 白底实锤（窗角=白）"
    else:
        verdict = "PARTIAL 窗角=%s（既非红也非白，看截图）" % (mid_rgb,)

    print(json.dumps({
        "probe_rect_physical": list(r2),
        "pre_rgb": pre_rgb, "mid_rgb": mid_rgb, "post_rgb": post_rgb,
        "pre_md5": pre_md5, "mid_md5": mid_md5, "post_md5": post_md5,
        "card_bright_px": card_bright, "card_bright_px_mid": card_bright2,
        "verdict": verdict,
    }, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
