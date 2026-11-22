"""逐卡图标判定：用 UIA 在「应用」页签里找每个卡片名字文本（.aname），
取其 BoundingRectangle，在 PrintWindow 全窗截图上对应位置裁 44x44 图标块
（名字正上方），统计彩色像素。REAL = 真实图标，其余 = 线条占位。

用法: python rc_probe/apps_icon_cards.py
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


APP_NAMES = ["浏览器", "Chrome", "Edge", "Qoder", "Qoder CN",
             "WorkBuddy AI", "WorkBuddy", "DeepSeek", "Codex", "ZCode",
             "剪映", "Photoshop", "Premiere Pro", "Illustrator",
             "OpenSquilla"]


def main():
    hwnd = aps.find_main_hwnd()
    if hwnd is None:
        print("NO_MAIN_WINDOW")
        sys.exit(1)
    user32.ShowWindow(hwnd, 5)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.4)
    uiac, uia = aps.get_uia()
    root = uia.ElementFromHandle(hwnd)
    walker = uia.RawViewWalker

    boxes = {}

    def visit(node, depth):
        if depth > 60 or node is None:
            return
        try:
            name = node.CurrentName
        except Exception:
            name = None
        if name in APP_NAMES and name not in boxes:
            r = node.CurrentBoundingRectangle
            if r.right > r.left:
                boxes[name] = (r.left, r.top, r.right, r.bottom)
        child = walker.GetFirstChildElement(node)
        while child:
            visit(child, depth + 1)
            child = walker.GetNextSiblingElement(child)

    visit(root, 0)
    img = grab(hwnd)
    if img is None:
        print("PRINTWINDOW_FAILED")
        sys.exit(1)
    base = img.convert("RGB")
    d = ImageDraw.Draw(base)
    print(f"found {len(boxes)}/{len(APP_NAMES)} name boxes")
    real = 0
    for nm, (l, t, r, b) in boxes.items():
        cx = (l + r) // 2
        # 图标在名字上方：上移一个图标块（约 44px）+ 间距
        iy = t - 48
        tile = img.crop((cx - 22, iy - 22, cx + 22, iy + 22)).convert("RGB")
        px = list(tile.getdata())
        colored = sum(1 for (rr, gg, bb) in px
                      if max(rr, gg, bb) > 90 and (max(rr, gg, bb) - min(rr, gg, bb)) > 45)
        verdict = "REAL" if colored > 260 else "placeholder"
        if colored > 90:
            real += 1
        color = "lime" if colored > 260 else ("orange" if colored > 90 else "red")
        d.rectangle([cx - 22, iy - 22, cx + 22, iy + 22], outline=color, width=2)
        print(f"{nm:>14}: colored={colored:4d} {verdict}")
    base.save(OUT / "apps_icon_cards.png")
    print(f"saved rc_probe/apps_icon_cards.png  (realish={real})")


if __name__ == "__main__":
    main()
