"""语音面板配色对账：自绘的底必须和 QSS 子控件同一个调色板。

经典主题浅色模式下，面板底/麦克风/Toast 是 C++ 自绘的，曾经写死深色，
而标题栏、转写框走 QSS 令牌已经翻成浅色 —— 症状就是黑底压深字，谁都读不出。
这个脚本按窗口标题抓「AgentRMC 语音」，量三件事：
  1) 面板底和标题栏是不是同一个明暗调（标题栏由 QSS 上色，是调色板的地面真相）
  2) 状态行文字相对面板底的对比度（WCAG 比值）
  3) 麦克风圆盘底 / 转写框是不是跟着卡片色走

用法: python rc_probe/voice_light_check.py [期望 light|dark|auto]
期望填 auto（默认）就按标题栏实际色调判，深浅色两种模式都能跑。
"""
import ctypes
import statistics
import sys
from ctypes import wintypes as wt

from PIL import ImageGrab

TITLE = "AgentRMC 语音"
LOGICAL_W = 380  # VoiceWindow::setFixedSize(380, 560)

u = ctypes.WinDLL("user32")


def rgb_lum(px):
    """相对亮度（sRGB 线性化后加权），0=纯黑 1=纯白。"""
    def lin(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4

    r, g, b = px[0], px[1], px[2]
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def contrast(a, b):
    la, lb = rgb_lum(a), rgb_lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def main():
    # 先声明 DPI 感知，否则 GetWindowRect 拿到的是虚拟化坐标，采样点全偏。
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception as exc:  # noqa: BLE001
        print(f"WARN SetProcessDpiAwareness failed: {exc}")

    expect = sys.argv[1] if len(sys.argv) > 1 else "auto"
    h = u.FindWindowW(None, TITLE)
    if not h:
        print(f"VERDICT: SKIP 窗口未找到（{TITLE}）——先打开语音面板")
        return 2
    u.ShowWindow(h, 9)  # SW_RESTORE
    u.SetForegroundWindow(h)

    rect = wt.RECT()
    if not u.GetWindowRect(h, ctypes.byref(rect)):
        print("VERDICT: SKIP GetWindowRect 失败")
        return 2
    box = (rect.left, rect.top, rect.right, rect.bottom)
    w, hpx = box[2] - box[0], box[3] - box[1]
    if w < LOGICAL_W or hpx < 500:
        print(f"VERDICT: SKIP 窗口尺寸异常 rect={box} {w}x{hpx}")
        return 2
    scale = w / LOGICAL_W
    print(f"info rect={box} physical={w}x{hpx} scale={scale:.2f}")

    img = ImageGrab.grab(bbox=box, all_screens=True)

    def sample(lx, ly, half=2):
        """逻辑坐标 → 物理像素中位数（避开单点噪声）。"""
        px, py = int(lx * scale), int(ly * scale)
        got = [
            img.getpixel((px + dx, py + dy))
            for dx in range(-half, half + 1)
            for dy in range(-half, half + 1)
        ]
        return tuple(statistics.median(c[i] for c in got) for i in range(3))

    # 标题栏（QSS 的 @card.bg@）：调色板的地面真相，用它推期望。
    header = sample(LOGICAL_W / 2, 22)
    light_mode = rgb_lum(header) > 0.5 if expect == "auto" else expect == "light"
    print(f"reading header={header} lum={rgb_lum(header):.3f} "
          f"expect={'light' if light_mode else 'dark'}")

    # 面板底：正文区左留白（x=10 在 20px 内容边距外，只有背景）。
    body_pts = [(10, 120), (10, 250), (10, 430), (LOGICAL_W - 10, 250)]
    body = [sample(*p) for p in body_pts]
    body_lums = [rgb_lum(c) for c in body]
    spread = max(body_lums) - min(body_lums)
    if spread > 0.06:
        # 四个空白点颜色不一致 ⇒ 大概率被别的窗盖住了，不下结论。
        print(f"VERDICT: INCONCLUSIVE 面板底采样不一致 spread={spread:.3f} "
              f"pts={list(zip(body_pts, body))}（窗口可能被遮挡，把它挪到最前再跑）")
        return 2
    body_bg = body[len(body) // 2]
    body_lum = rgb_lum(body_bg)

    checks = []
    checks.append((
        (rgb_lum(body_bg) > 0.5) == light_mode,
        f"面板底与标题栏同调 body={body_bg} lum={body_lum:.3f} "
        f"header_lum={rgb_lum(header):.3f}",
    ))

    # 状态行「点击开始 · 再点结束」：取带内与面板底亮度差最大的像素当文字，
    # 这样深色模式（浅字压深底）和浅色模式（深字压浅底）用同一个判据。
    band_y0, band_y1 = int(184 * scale), int(210 * scale)
    band_x0, band_x1 = int(24 * scale), int((LOGICAL_W - 24) * scale)
    ink = max((img.getpixel((x, y)) for y in range(band_y0, band_y1)
               for x in range(band_x0, band_x1)),
              key=lambda p: abs(rgb_lum(p) - body_lum))
    ratio = contrast(ink, body_bg)
    checks.append((ratio >= 4.5, f"状态行文字对比度={ratio:.1f}:1 "
                                 f"text={ink} bg={body_bg}"))

    def modal(lx0, lx1, ly0, ly1):
        """区域内出现最多的颜色：圆盘/卡片是被填色的底，文字只占少数像素。
        取最亮/最暗都会被文字骗到（旧版圆盘是深底+浅字，最亮=字）。"""
        counts = {}
        for y in range(int(ly0 * scale), int(ly1 * scale)):
            for x in range(int(lx0 * scale), int(lx1 * scale)):
                key = tuple((c // 8) * 8 for c in img.getpixel((x, y))[:3])
                counts[key] = counts.get(key, 0) + 1
        return max(counts, key=counts.get)

    # 麦克风圆盘底（自绘，应当等于卡片色）：圆盘中心 (190,116) 半径 44。
    disc = modal(LOGICAL_W / 2 - 40, LOGICAL_W / 2 + 40, 76, 156)
    checks.append(((rgb_lum(disc) > 0.5) == light_mode,
                   f"麦克风圆盘={disc} lum={rgb_lum(disc):.3f}"))

    # 转写框（QSS @card.bg@）：和圆盘应当同调，两者不一致说明有一处没跟调色板。
    card = modal(30, LOGICAL_W - 30, 240, 295)
    checks.append(((rgb_lum(card) > 0.5) == (rgb_lum(disc) > 0.5),
                   f"卡片同调：圆盘={disc} 转写框={card}"))

    passed = 0
    for ok, what in checks:
        print(f"{'PASS' if ok else 'FAIL'} {what}")
        passed += 1 if ok else 0
    total = len(checks)
    print(f"VERDICT: {'PASS' if passed == total else 'FAIL'} "
          f"({passed}/{total}) voice-panel palette follows theme")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
