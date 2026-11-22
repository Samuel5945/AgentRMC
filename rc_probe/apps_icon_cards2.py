"""逐卡图标判定 v2：修 DPI 映射（截图是物理像素，UIA 是逻辑像素）+
图标块按卡宽自适应。用法: python rc_probe/apps_icon_cards2.py
"""
import ctypes
import ctypes.wintypes as wt
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import apps_page_shot as aps  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
OUT = pathlib.Path("rc_probe")

APP_NAMES = ["浏览器", "Chrome", "Edge", "Qoder", "Qoder CN",
             "WorkBuddy AI", "WorkBuddy", "DeepSeek", "Codex", "ZCode",
             "剪映", "Photoshop", "Premiere Pro", "Illustrator",
             "OpenSquilla"]


def grab(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    bmp = gdi32.CreateCompatibleBitmap(user32.GetDC(0), w, h)
    dc = gdi32.CreateCompatibleDC(user32.GetDC(0))
    gdi32.SelectObject(dc, bmp)
    img = None
    if user32.PrintWindow(hwnd, dc, 2):
        class BI(ctypes.Structure):
            _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                        ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                        ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                        ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                        ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                        ("biClrImportant", wt.DWORD)]

        bmi = BI()
        bmi.biSize = ctypes.sizeof(BI)
        bmi.biWidth, bmi.biHeight = w, -h
        bmi.biPlanes, bmi.biBitCount, bmi.biCompression = 1, 32, 0
        buf = ctypes.create_string_buffer(w * h * 4)
        if gdi32.GetDIBits(dc, bmp, 0, h, buf, ctypes.byref(bmi), 0):
            img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1)
    gdi32.DeleteDC(dc)
    gdi32.DeleteObject(bmp)
    return img


def main():
    # DPI 感知（脚本进程默认 system-aware，这里强制 per-monitor 对齐 Qt）
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass
    hwnd = aps.find_main_hwnd()
    if hwnd is None:
        print("NO_MAIN_WINDOW")
        sys.exit(1)
    user32.ShowWindow(hwnd, 9)   # SW_RESTORE
    user32.ShowWindow(hwnd, 5)   # SW_SHOW
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)
    uiac, uia = aps.get_uia()
    root = uia.ElementFromHandle(hwnd)
    walker = uia.RawViewWalker

    # UIA 返回的逻辑坐标 → 截图物理坐标的缩放：先取窗口矩形的两套读数。
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    win_phys = (r.left, r.top, r.right, r.bottom)
    win_w_phys = r.right - r.left

    boxes = {}

    def visit(node, depth):
        if depth > 60 or node is None or len(boxes) == len(APP_NAMES):
            return
        try:
            name = node.CurrentName
        except Exception:
            name = None
        if name in APP_NAMES and name not in boxes:
            rr = node.CurrentBoundingRectangle
            if rr.right > rr.left:
                boxes[name] = (rr.left, rr.top, rr.right, rr.bottom)
        child = walker.GetFirstChildElement(node)
        while child:
            visit(child, depth + 1)
            child = walker.GetNextSiblingElement(child)

    visit(root, 0)

    # 缩放因子：窗口内容区物理宽 / UIA 逻辑宽（UIA 元素与窗口同原点差）
    # 用 Chrome 名字框宽 50 逻辑 → 实际名字约 50 逻辑，物理 50*dpi。
    # 直接按主窗物理宽 / (逻辑 bbox 最大 x 差) 不精确；改为 Qt 主窗
    # 在 UIA 的窗口矩形 vs GetWindowRect 的物理矩形。
    try:
        wr = root.CurrentBoundingRectangle
        logic_w = wr.right - wr.left
        scale = win_w_phys / float(logic_w) if logic_w else 1.0
    except Exception:
        scale = 1.0
    print(f"window phys={win_phys} scale={scale:.3f}")

    img = grab(hwnd)
    if img is None:
        print("PRINTWINDOW_FAILED")
        sys.exit(1)
    base = img.convert("RGB")
    d = ImageDraw.Draw(base)
    print(f"found {len(boxes)}/{len(APP_NAMES)} name boxes; img={img.size}")
    real = 0
    for nm, (l, t, rr, b) in sorted(boxes.items(), key=lambda kv: (kv[1][1], kv[1][0])):
        # 逻辑 → 物理（相对主窗客户区）：先减去窗口原点，再乘缩放
        px_l = (l - win_phys[0]) * scale
        px_t = (t - win_phys[1]) * scale
        cx = (px_l + (rr - win_phys[0]) * scale) / 2
        ty = px_t
        # 图标块 44 逻辑 → 44*scale 物理；在名字上方 ~12 逻辑 + 44 图标
        half = 22 * scale
        iy = ty - 16 * scale - half  # 图标中心
        tile = img.convert("RGB").crop(
            (int(cx - half), int(iy - half), int(cx + half), int(iy + half)))
        px = list(tile.getdata())
        colored = sum(1 for (r_, g_, b_) in px
                      if max(r_, g_, b_) > 90 and (max(r_, g_, b_) - min(r_, g_, b_)) > 45)
        verdict = "REAL" if colored > 260 else ("low" if colored > 90 else "placeholder")
        if colored > 90:
            real += 1
        color = "lime" if colored > 260 else ("orange" if colored > 90 else "red")
        d.rectangle([int(cx - half), int(iy - half), int(cx + half), int(iy + half)],
                    outline=color, width=2)
        print(f"{nm:>14}: colored={colored:4d} {verdict}")
    base.save(OUT / "apps_icon_cards2.png")
    print(f"saved rc_probe/apps_icon_cards2.png  (realish={real}/{len(APP_NAMES)})")


if __name__ == "__main__":
    main()
