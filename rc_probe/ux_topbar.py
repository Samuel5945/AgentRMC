"""顶栏验收截图：整窗一张 + 顶栏右排放大一张（图标化改动用）。

系统缩放 200%（DPI 192）：不声明 per-monitor awareness 的话 GetWindowRect 给逻辑
坐标、ImageGrab 抓物理像素，框会偏（踩过：截出来是窗口左上角一块背景）。

用法: python rc_probe/ux_topbar.py <tag> [窗口标题]
产物: rc_probe/out/topbar_<tag>.png        整窗（物理像素，2x）
      rc_probe/out/topbar_<tag>_1x.png     整窗缩回 1x，看设计尺寸
      rc_probe/out/topbar_<tag>_strip.png  顶栏 1x
      rc_probe/out/topbar_<tag>_zoom.png   顶栏右排 2x（物理像素原样）
"""
import ctypes
import pathlib
import sys
import time
from ctypes import wintypes as wt

from PIL import Image, ImageGrab

try:  # 必须在任何 GetWindowRect 之前
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
except Exception as exc:  # pragma: no cover
    print("SetProcessDpiAwareness failed:", exc)

TAG = sys.argv[1] if len(sys.argv) > 1 else "shot"
# 主窗标题是 "AgentRMC Native"（同名进程还有桌宠/浮窗几个 Qt 窗，不能按进程找）。
TITLE = sys.argv[2] if len(sys.argv) > 2 else "AgentRMC Native"
OUT = pathlib.Path(__file__).parent / "out"
OUT.mkdir(exist_ok=True)

u = ctypes.WinDLL("user32")
h = u.FindWindowW(None, TITLE)
if not h:
    print("窗口未找到:", TITLE)
    sys.exit(1)
u.ShowWindow(h, 9)
u.SetForegroundWindow(h)
time.sleep(0.6)

rect = wt.RECT()
u.GetWindowRect(h, ctypes.byref(rect))
box = (rect.left, rect.top, rect.right, rect.bottom)
img = ImageGrab.grab(bbox=box, all_screens=True)
scale = round(u.GetDpiForSystem() / 96)
full = OUT / f"topbar_{TAG}.png"
img.save(full)

one = img.resize((img.width // scale, img.height // scale), Image.LANCZOS)
one.save(OUT / f"topbar_{TAG}_1x.png")
strip = one.crop((0, 0, one.width, min(56, one.height)))
strip.save(OUT / f"topbar_{TAG}_strip.png")
# 物理像素的顶栏右排 = 2x 视图（DPI 200% 下正好是设计尺寸的两倍）
phys_strip = img.crop((0, 0, img.width, min(56 * scale, img.height)))
zoom = phys_strip.crop((max(0, phys_strip.width - 620 * scale), 0,
                        phys_strip.width, phys_strip.height))
zoom.save(OUT / f"topbar_{TAG}_zoom.png")
print(f"rect={box} size={img.size} scale={scale}")
print(f"-> rc_probe/out/topbar_{TAG}.png / _1x.png / _strip.png / _zoom.png")
