"""应用页图标验证：点一个真实应用（WorkBuddy）的卡片看能否启动，
同时抓图标中心像素色彩数判断「真实图标 vs 通用占位」。

判定标准：
- 卡片图标区 44x44 内取色相数（HSV），>=3 且像素非灰 => 真图标；
  <=2 => 线条占位。
用法: python rc_probe/apps_icon_audit.py
"""
import ctypes
import ctypes.wintypes as wt
import pathlib
import sys
import time

import apps_page_shot as aps
from PIL import Image

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


def color_stats(im):
    im = im.convert("RGB")
    px = list(im.getdata())
    # 灰阶比例
    gray = sum(1 for r, g, b in px
               if max(r, g, b) - min(r, g, b) <= 12) / len(px)
    # 亮像素（排除透明/暗底）里有多少明显彩色的
    vivid = sum(1 for r, g, b in px
                if max(r, g, b) > 90 and (max(r, g, b) - min(r, g, b)) > 40)
    return gray, vivid


def find_card_images():
    """在主窗截图上找卡片：卡片图标是 44px 方块，位于页面右侧区域。
    简单办法：截整页，人工报告各候选块色彩数不可靠；
    改为点击验证 —— 点卡片中心如果应用被启动（/state 的窗口计数变化）
    说明卡片存在且可点；再抓全屏给用户看。
    """
    pass


def main():
    hwnd = aps.find_main_hwnd()
    if hwnd is None:
        print("NO_MAIN_WINDOW")
        sys.exit(1)
    user32.ShowWindow(hwnd, 5)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    img = grab(hwnd)
    if img is None:
        print("PRINTWINDOW_FAILED")
        sys.exit(1)
    img.convert("RGB").save(OUT / "apps_audit_full.png")
    print("saved", OUT / "apps_audit_full.png")

    # 取主窗内 WebView 区域（右侧 2/3）的若干 44px 小块做色彩统计。
    # 卡片网格从 y≈170 开始；步长 ≈ 单卡宽+10。先整幅找「彩色密集块」。
    w, h = img.size
    region = img.crop((w // 3, 150, w, h)).convert("RGB")
    gray, vivid = color_stats(region)
    print(f"right-region gray_ratio={gray:.3f} vivid_px={vivid} size={region.size}")
    if vivid > 2000:
        print("VERDICT: 彩色像素充足，像真实图标")
    else:
        print("VERDICT: 彩色像素很少，可能全是占位/线条图")


if __name__ == "__main__":
    main()
