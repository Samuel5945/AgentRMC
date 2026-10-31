"""生成 native/src/core/PinyinInitials.h/.cpp：CJK 基本区每个汉字的拼音。

只在生成时需要 pypinyin（pip install pypinyin），产物直接进仓库，构建不依赖它。

    python native/tools/gen_pinyin.py
"""
from __future__ import annotations

import sys
import unicodedata
from pathlib import Path

from pypinyin import Style, pinyin

ROOT = Path(__file__).resolve().parent.parent
OUT_H = ROOT / "src" / "core" / "PinyinInitials.h"
OUT_CPP = ROOT / "src" / "core" / "PinyinInitials.cpp"

# zh/ch/sh 折叠成 z/c/s：听岔的场景里翘舌不分家，折叠还能省表。
COLLAPSE = {"zh": "z", "ch": "c", "sh": "s"}


def rows() -> list[tuple[int, str]]:
    out = []
    for code in range(0x4E00, 0x9FA6):
        ch = chr(code)
        got = pinyin(ch, style=Style.TONE3, heteronym=False, errors="ignore")
        if not got or not got[0] or not got[0][0]:
            continue
        raw = got[0][0]
        # TONE3 形如 "jian4"；去掉调号数字，规范化到小写字母。
        letters = "".join(c for c in raw if c.isascii() and c.isalpha())
        if not letters:
            continue
        letters = unicodedata.normalize("NFKD", letters)
        letters = "".join(c for c in letters if c.isascii() and c.isalpha())
        if not letters:
            continue
        out.append((code, letters))
    return out


def main() -> None:
    table = rows()
    assert len(table) > 15000, f"表太小，pypinyin 输出异常: {len(table)}"

    header = """#pragma once

// 由 native/tools/gen_pinyin.py 生成（数据源 pypinyin），勿手改。
// 汉字 → 无声调拼音，供语音模糊匹配把「剪易」对上「剪映」这类听岔。

#include <QString>

namespace PinyinInitials {

// 没有的字返回空串；zh/ch/sh 已折叠成 z/c/s。
QString of(char32_t code);

}  // namespace PinyinInitials
"""
    OUT_H.write_text(header, encoding="utf-8")

    lines = [",".join(f"{{0x{code:04X}, \"{py}\"}}" for code, py in table[i:i + 8]) + ","
             for i in range(0, len(table), 8)]
    body = f"""#include "PinyinInitials.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace PinyinInitials {{

namespace {{

struct Entry {{
    char32_t code;
    std::string_view pinyin;
}};

constexpr std::array<Entry, {len(table)}> kTable = {{{{
    {chr(10).join("    " + line for line in lines)}
}}}};

inline const Entry *find(char32_t code) {{
    const Entry *end = kTable.data() + kTable.size();
    const Entry *it = std::lower_bound(
        kTable.data(), end, code,
        [](const Entry &e, char32_t c) {{ return e.code < c; }});
    return it != end && it->code == code ? it : nullptr;
}}

}}  // namespace

QString of(char32_t code) {{
    const Entry *entry = find(code);
    if (entry == nullptr) {{
        return QString();
    }}
    return QString::fromUtf8(entry->pinyin.data(), int(entry->pinyin.size()));
}}

}}  // namespace PinyinInitials
"""
    OUT_CPP.write_text(body, encoding="utf-8")
    print(f"写了 {len(table)} 个字 -> {OUT_CPP.name} / {OUT_H.name}")


if __name__ == "__main__":
    sys.exit(main())
