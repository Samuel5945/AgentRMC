#pragma once

#include <QFontMetrics>
#include <QRect>
#include <QString>
#include <QtGlobal>

// 语音转写框的折行与量高。
//
// style.qss 里 QLabel#voiceTranscript 的 padding(10)/border(1) 是写死的逻辑
// 像素，而 font-size 会被「字号」设置等比放大（ThemeManager::renderQss）——
// 所以「这个框放得下几行」只能按当前字体现量。旧版把高度写死成 32/48，在
// 120% 字号档两行就要 66，第二行被裁得只剩半截（用户 2026-11-05 真机）。
namespace transcript {

// 上下左右各一层的箱体厚度（padding + border），与 style.qss 同步。
constexpr int kChromePx = (10 + 1) * 2;
// 最多显示几行；再长的话折成省略号，全文走悬浮提示和点击复制。
constexpr int kMaxLines = 3;

// 整段话按 width 折行后实际占多高。与 QLabel 的 wordWrap 同一套断行引擎。
inline int textHeight(const QString &text, const QFontMetrics &fm, int width) {
    if (width <= 0) {
        return fm.lineSpacing();
    }
    return fm.boundingRect(QRect(0, 0, width, 100000),
                           Qt::AlignLeft | Qt::TextWordWrap, text)
        .height();
}

// 折到 maxLines 行以内：放得下就原样返回；放不下就二分找「加上省略号仍然放得
// 下」的最长前缀。宁可少几个字，也不让谁只露出半个字。
inline QString elideToLines(const QString &text, const QFontMetrics &fm,
                            int width, int maxLines = kMaxLines) {
    const int budget = fm.lineSpacing() * maxLines;
    if (width <= 0 || textHeight(text, fm, width) <= budget) {
        return text;
    }
    const QString ellipsis = QStringLiteral("…");
    int lo = 0;
    int hi = text.length();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (textHeight(text.left(mid) + ellipsis, fm, width) <= budget) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return text.left(lo) + ellipsis;
}

// 箱体高度 = 文字实际占的高（不足一行按一行算）+ 上下箱体厚度。
// 调用方传进来的必须是 elideToLines() 的结果，否则这里给的高度照样裁字；
// chrome 用实测到的箱体厚度（QSS 边距不缩放，但几何量出来更准）。
inline int boxHeight(const QString &shown, const QFontMetrics &fm, int width,
                     int chrome = kChromePx) {
    return qMax(fm.lineSpacing(), textHeight(shown, fm, width)) + chrome;
}

}  // namespace transcript
