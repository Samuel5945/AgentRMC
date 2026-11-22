"""应用页图标逐卡判定：从全窗截图里按列切出每个 44px 图标块，
统计彩色像素占比，输出哪张卡是真实图标、哪张还是线条占位。
用法: python rc_probe/apps_icon_grid.py apps_audit_full.png
"""
import pathlib
import sys

from PIL import Image

IMG = sys.argv[1] if len(sys.argv) > 1 else "rc_probe/apps_audit_full.png"
OUT = pathlib.Path("rc_probe")

im = Image.open(IMG).convert("RGB")
w, h = im.size
# 应用页布局：卡片网格大约从 y=200 开始，列宽约 (w*2/3)/4，图标块 44px 居中。
# 不做精确几何：先裁整页右半区（列表列），逐行找图标块（44x44 灰底上的彩色团）。
left = w // 3
region = im.crop((left, 170, w, h))
rw, rh = region.size
step = 108  # 近似：卡片列间距 + 边距
results = []
for y in range(12, rh - 60, step):
    row = []
    for x in range(8, rw - 56, step):
        tile = region.crop((x, y, x + 44, y + 44))
        px = list(tile.getdata())
        colored = sum(1 for (r, g, b) in px
                      if max(r, g, b) > 90 and (max(r, g, b) - min(r, g, b)) > 45)
        row.append("REAL" if colored > 260 else ("low" if colored > 90 else "placeholder"))
    results.append(row)

real = sum(r.count("REAL") for r in results)
ph = sum(r.count("placeholder") for r in results)
low = sum(r.count("low") for r in results)
print(f"grid={len(results)}x{len(results[0]) if results else 0} REAL={real} low={low} placeholder={ph}")
for r in results:
    print(" ".join(f"{c:>5}" for c in r))

# 保存调试图：给每个 REAL 块画绿框
dbg = im.copy()
from PIL import ImageDraw
d = ImageDraw.Draw(dbg)
for i, r in enumerate(results):
    for j, c in enumerate(r):
        x = left + 8 + j * step
        y = 170 + 12 + i * step
        color = "lime" if c == "REAL" else ("orange" if c == "low" else "red")
        d.rectangle([x, y, x + 44, y + 44], outline=color, width=2)
dbg.save(OUT / "apps_icon_grid.png")
print("saved", OUT / "apps_icon_grid.png")
