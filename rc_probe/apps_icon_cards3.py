"""逐卡图标判定 v3：UIA 坐标是「物理 2x、相对屏幕原点」——
除以缩放因子得到截图像素坐标。用法: python rc_probe/apps_icon_cards3.py
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

    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    win = (r.left, r.top, r.right, r.bottom)

    # 1) 先点「应用」页签（可能当前不在这一页）
    stack = [root]
    tab = None
    while stack:
        n = stack.pop()
        try:
            nm = n.CurrentName
        except Exception:
            nm = None
        if nm == "应用" and tab is None:
            tab = n
        c = walker.GetFirstChildElement(n)
        while c:
            stack.append(c)
            c = walker.GetNextSiblingElement(c)
    if tab is not None:
        tb = tab.CurrentBoundingRectangle
        x = (tb.left + tb.right) // 2
        y = (tb.top + tb.bottom) // 2
        user32.SetCursorPos(x, y)
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(1.2)

    # 2) 收集名字框（UIA 物理屏幕坐标）
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

    # 3) 缩放因子：UIA 坐标是物理像素但被系统按 2x 上报
    #    （窗口物理 2120 宽 vs UIA 窗口 ~4240）→ 直接除 2 对齐截图。
    wr = root.CurrentBoundingRectangle
    scale = (win[2] - win[0]) / float(wr.right - wr.left)
    print(f"win={win} uiaWinW={wr.right - wr.left} scale={scale:.3f}")

    img = grab(hwnd)
    base = img.convert("RGB")
    d = ImageDraw.Draw(base)
    print(f"found {len(boxes)}/{len(APP_NAMES)}; img={img.size}")
    real = 0
    for nm, (l, t, rr, b) in sorted(boxes.items(), key=lambda kv: (kv[1][1], kv[1][0])):
        cx = ((l + rr) // 2 - win[0]) * scale
        top = (t - win[1]) * scale
        half = 22 * scale  # 图标 44px（截图与 UIA 同为物理像素）
        iy = top - 5 * scale - half  # 名字上方：卡片内边距 8 + 5px 间隙，图标中心
        x0, y0 = int(cx - half), int(iy - half)
        x1, y1 = int(cx + half), int(iy + half)
        tile = img.convert("RGB").crop((x0, y0, x1, y1))
        px = list(tile.getdata())
        colored = sum(1 for (r_, g_, b_) in px
                      if max(r_, g_, b_) > 90 and (max(r_, g_, b_) - min(r_, g_, b_)) > 45)
        verdict = "REAL" if colored > 260 else ("low" if colored > 90 else "placeholder")
        if colored > 260:
            real += 1
        color = "lime" if colored > 260 else ("orange" if colored > 90 else "red")
        d.rectangle([x0, y0, x1, y1], outline=color, width=3)
        print(f"{nm:>14}: colored={colored:4d} box=({x0},{y0},{x1},{y1}) {verdict}")
    base.save(OUT / "apps_icon_cards3.png")
    print(f"saved rc_probe/apps_icon_cards3.png  (REAL={real}/{len(boxes)})")


if __name__ == "__main__":
    main()
