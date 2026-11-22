"""像素级调试：在截图上标出 Chrome 卡片的文字行、列网格、名字框位置，
确认审计采样框为什么没落到图标上。"""
from PIL import Image, ImageDraw

im = Image.open("rc_probe/apps_icon_cards3.png").convert("RGB")
d = ImageDraw.Draw(im)
# 网格：每 44 步画细线（x 340..920, y 180..700）
for x in range(340, 941, 44):
    d.line([(x, 170), (x, 700)], fill=(255, 0, 255), width=1)
for y in range(160, 701, 44):
    d.line([(340, y), (920, y)], fill=(255, 0, 255), width=1)
# 标出几个关键已知坐标（来自 v3 审计输出）
d.rectangle([359, 223, 403, 267], outline=(0, 255, 0), width=2)   # 浏览器 REAL
d.rectangle([359, 339, 403, 383], outline=(255, 0, 0), width=2)   # Chrome placeholder
# Chrome 名字框（UIA）：win=(1055,1286) 原点 → 截图坐标
# 名字框 (1807,1863,1857,1880) 屏幕物理 → 窗口相对：减 win 原点
# 注意 v3 截图原点 = win[0..1]
# 名字在截图里应约在 x 1807-1055=752? 不对，窗口在第二块屏上……
# 直接在输出上画名字文本行（y 应 = 名字 top - win top）
d.line([(300, 1863-1286), (950, 1863-1286)], fill=(255, 255, 0), width=1)
d.text((5, 5), f"winTop={1863-1286}", fill=(255, 255, 0))
im.save("rc_probe/pixel_debug.png")
print("saved rc_probe/pixel_debug.png", im.size)
