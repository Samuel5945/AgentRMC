# 一次性迁移：把 native/resources/style.qss 里的颜色字面量替换成 @token@ 占位符。
# classic 主题的 palette = 迁移前的原值，ThemeManager 内嵌同一份默认值，保证零回归。
# 再跑一次无害：替换后文件里已没有颜色字面量，脚本会因「字面量集合为空」直接退出。
import json
import re
import sys
from pathlib import Path

QSS = Path(__file__).resolve().parents[2] / "native" / "resources" / "style.qss"

# 语义命名的十六进制色（hex -> token）
HEX_NAMES = {
    "#0b0e14": "bg",
    "#0f141d": "dialog.bg",
    "#121722": "card.bg",
    "#161d2a": "raised",
    "#05070c": "guide.image.bg",
    "#e6edf3": "text",
    "#c9d1d9": "text.dim",
    "#dbe4ee": "text.soft",
    "#b6c2cf": "text.body",
    "#8b949e": "muted",
    "#5b6472": "muted.2",
    "#46505e": "faint",
    "#3d4653": "faint.2",
    "#6b7684": "ghost",
    "#4a5568": "line",
    "#4f8cff": "accent",
    "#7c5cff": "accent.2",
    "#6299ff": "accent.hover",
    "#8d70ff": "accent2.hover",
    "#9ec1ff": "accent.text",
    "#7dabff": "accent.link",
    "#d29922": "warn",
    "#e5484d": "danger",
    "#ffffff": "on.accent",
}

text = QSS.read_text(encoding="utf-8")

# color: white; 是主按钮的文字色，特殊处理成 on.accent
text, n_white = re.subn(r"color:\s*white;", "color: @on.accent@;", text)

literals = set(re.findall(r"#[0-9a-fA-F]{6}\b", text)) | set(re.findall(r"rgba\([^)]*\)", text))

palette = {"on.accent": "white"}
missing = []
for lit in sorted(literals):
    if lit.startswith("#"):
        token = HEX_NAMES.get(lit.lower())
    else:
        m = re.match(r"rgba\(\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\)", lit)
        if not m:
            missing.append(lit)
            continue
        r, g, b, a = (int(x) for x in m.groups())
        if (r, g, b) == (255, 255, 255):
            token = f"w{a}"          # 白色透明度阶梯（表面/描边）
        elif (r, g, b) == (79, 140, 255):
            token = f"a{a}"          # 主题蓝透明度阶梯（高亮/选中）
        elif (r, g, b) == (15, 20, 29) and a == 242:
            token = "bubble.bg"      # 桌宠气泡底
        else:
            token = None
        if token:
            palette[token] = lit
    if token is None:
        missing.append(lit)
        continue
    palette[token] = palette.get(token, lit)
    text = text.replace(lit, f"@{token}@")

if missing:
    print("UNMAPPED literals:", missing, file=sys.stderr)
    sys.exit(1)

QSS.write_text(text, encoding="utf-8")
Path(__file__).with_name("classic_palette.json").write_text(
    json.dumps(palette, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"tokenized: white×{n_white}, {len(literals)} literals, {len(palette)} tokens")
for k in sorted(palette):
    print(f"  {k} = {palette[k]}")
