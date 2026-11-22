"""WebView2 渲染审计：PrintWindow 抓窗 → 扫描卡片图标区的非背景像素团块，
定位图标真实位置并统计彩色数，输出每卡图标块的实际包围盒。
用法: python rc_probe/apps_icon_scan.py
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


def main():
    hwnd = aps.find_main_hwnd()
    user32.ShowWindow(hwnd, 9)
    user32.ShowWindow(hwnd, 5)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)

    # 点「应用」页签（确保当前页）
    uiac, uia = aps.get_uia()
    root = uia.ElementFromHandle(hwnd)
    walker = uia.RawViewWalker
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

    img = grab(hwnd)
    w, h = img.size
    # 卡片列表区域：x>~330（左语音面板外）；y>~150（标题/提示/分隔线外）
    card_zone = img.convert("RGB").crop((330, 150, w, h))
    cw, ch = card_zone.size
    # 图标 = 深色文字/背景上的亮色团块。卡片背景 w6≈rgba(255,255,255,8/255)，
    # 页面深底。找「非近黑」像素：sum>90 即候选图标（白 logo 也算亮）。
    import numpy as np
    a = np.asarray(card_zone, dtype=np.int16)
    lum = a.sum(axis=2)
    mask = (lum > 140) & (lum < 720)  # 排除极暗背景与纯白文字？文字也亮…
    # 更稳：找列向投影峰：图标列（44 宽）在每列都有 >20 个亮像素；
    # 文字只有几行。先画 mask 可视化。
    m_img = (mask * 255).astype("uint8")
    Image.fromarray(m_img).save(OUT / "apps_icon_mask.png")

    # 列投影
    colsum = mask.sum(axis=0)
    rowsum = mask.sum(axis=1)
    cols = [i for i in range(cw) if colsum[i] > 28]
    rows = [i for i in range(ch) if rowsum[i] > 28]

    def runs(xs):
        out = []
        if not xs:
            return out
        s = xs[0]
        p = xs[0]
        for v in xs[1:]:
            if v - p > 3:
                out.append((s, p))
                s = v
            p = v
        out.append((s, p))
        return out

    col_bands = [r for r in runs(cols) if r[1] - r[0] > 18]
    row_bands = [r for r in runs(rows) if r[1] - r[0] > 18]
    print("col_bands(图标列):", col_bands[:8])
    print("row_bands(亮行):", row_bands[:12])

    dbg = img.convert("RGB").copy()
    d = ImageDraw.Draw(dbg)
    for (x0, x1) in col_bands:
        for (y0, y1) in row_bands[:6]:
            d.rectangle([330 + x0, 150 + y0, 330 + x1, 150 + y1],
                        outline=(255, 255, 0), width=2)
    dbg.save(OUT / "apps_icon_scan.png")
    print("saved rc_probe/apps_icon_scan.png")


if __name__ == "__main__":
    main()
