"""逐卡图标采样：按名字框定位卡片，在名字正上方 62..106px 处取 44px 图标块，
统计彩色像素判定真实图标。用法: python rc_probe/apps_icon_probe.py"""
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
    hwnd = aps.find_main_hwnd()
    if hwnd is None:
        print("NO_MAIN_WINDOW")
        sys.exit(1)
    user32.ShowWindow(hwnd, 9)
    user32.ShowWindow(hwnd, 5)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)
    uiac, uia = aps.get_uia()
    root = uia.ElementFromHandle(hwnd)
    walker = uia.RawViewWalker

    # 点「应用」页签
    stack = [root]
    tab = None
    while stack and tab is None:
        n = stack.pop()
        try:
            nm = n.CurrentName
        except Exception:
            nm = None
        if nm == "应用":
            tab = n
        c = walker.GetFirstChildElement(n)
        while c:
            stack.append(c)
            c = walker.GetNextSiblingElement(c)
    if tab is not None:
        b = tab.CurrentBoundingRectangle
        user32.SetCursorPos((b.left + b.right) // 2, (b.top + b.bottom) // 2)
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(1.2)

    # 收集名字框（屏幕绝对坐标，物理像素）
    boxes = {}

    def visit(node, depth):
        if depth > 60 or node is None:
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

    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    win = (r.left, r.top)
    print(f"win_origin={win} boxes={len(boxes)}/15")

    img = grab(hwnd)
    if img is None:
        print("PRINTWINDOW_FAILED")
        sys.exit(1)
    base = img.convert("RGB")
    d = ImageDraw.Draw(base)

    def colored_count(tile):
        px = list(tile.getdata())
        return sum(1 for (r_, g_, b_) in px
                   if max(r_, g_, b_) > 90 and (max(r_, g_, b_) - min(r_, g_, b_)) > 45)

    real = 0
    for nm, (l, t, rr, b) in sorted(boxes.items(), key=lambda kv: (kv[1][1], kv[1][0])):
        cx = (l + rr) // 2 - win[0]
        top = t - win[1]
        y0 = top - 106
        y1 = top - 62
        x0 = cx - 22
        x1 = cx + 22
        tile = base.crop((x0, y0, x1, y1))
        c = colored_count(tile)
        bright = tile.convert("L").getextrema()
        verdict = "REAL" if c > 260 else ("mono" if bright[1] > 170 and c < 60 else "placeholder")
        if c > 260:
            real += 1
        color = "lime" if c > 260 else ("deepskyblue" if bright[1] > 170 else "red")
        d.rectangle([x0, y0, x1, y1], outline=color, width=2)
        print(f"{nm:>13}: colored={c:4d} lumMax={bright[1]:3d} {verdict}")
    base.save(OUT / "apps_icon_probe.png")
    print(f"saved rc_probe/apps_icon_probe.png REAL={real}/{len(boxes)}")


if __name__ == "__main__":
    main()
