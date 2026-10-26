"""按窗口标题截图，用于验收界面。

用法: python rc_probe/shot.py [标题] [输出文件]
"""
import ctypes
import sys
from ctypes import wintypes as wt

from PIL import ImageGrab

title = sys.argv[1] if len(sys.argv) > 1 else "AgentRMC"
out = sys.argv[2] if len(sys.argv) > 2 else "rc_probe/qt_shot.png"

u = ctypes.WinDLL("user32")
h = u.FindWindowW(None, title)
if not h:
    print("窗口未找到")
    sys.exit(1)
u.ShowWindow(h, 9)
u.SetForegroundWindow(h)

rect = wt.RECT()
u.GetWindowRect(h, ctypes.byref(rect))
box = (rect.left, rect.top, rect.right, rect.bottom)
print(f"rect={box} size={box[2] - box[0]}x{box[3] - box[1]}")
img = ImageGrab.grab(bbox=box, all_screens=True)
img.save(out)
print(f"saved {out} {img.size}")
