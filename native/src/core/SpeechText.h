#pragma once

#include <QString>

// 念出口（TTS）专用净化：把提示文本里不适合朗读的部分转成口语。
// 路径/盘符 → 「D盘 foo」，URL 去掉，带标点的名字/装饰标点/表情去掉，
// 连字符转空格。气泡仍显示原文，这里只处理「念」的那一份。
// 净化后若无实义内容返回空串，调用方据此跳过 TTS。
namespace SpeechText {

QString friendly(const QString &text);

}  // namespace SpeechText
