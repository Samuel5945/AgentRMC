#pragma once

// 由 native/tools/gen_pinyin.py 生成（数据源 pypinyin），勿手改。
// 汉字 → 无声调拼音，供语音模糊匹配把「剪易」对上「剪映」这类听岔。

#include <QString>

namespace PinyinInitials {

// 没有的字返回空串；zh/ch/sh 已折叠成 z/c/s。
QString of(char32_t code);

}  // namespace PinyinInitials
