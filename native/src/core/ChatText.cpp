#include "core/ChatText.h"

#include <QRegularExpression>

namespace ChatText {

bool looksLikeToolCall(const QString &text) {
    static const QRegularExpression kMarker(
        QStringLiteral("<\\s*(tool|function|parameter)|</\\s*(tool|function|parameter)|"
                       "\"tool_calls\"|\"function_call\"|\\bmcp[A-Za-z0-9_]*\\s*\\(|"
                       "getTodoList|\\bgrep\\s*\\("),
        QRegularExpression::CaseInsensitiveOption);
    return kMarker.match(text).hasMatch();
}

QString cleanReply(const QString &text) {
    QString out = text;
    out.remove(QRegularExpression(QStringLiteral("```[\\s\\S]*?```")));
    out.remove(QLatin1Char('`'));
    out.remove(QStringLiteral("**"));
    out.remove(QStringLiteral("__"));
    out.remove(QRegularExpression(
        QStringLiteral("(?m)^\\s{0,3}#{1,6}\\s*")));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*[-*+]\\s+")));
    out.remove(QRegularExpression(
        QStringLiteral("(?m)^\\s*\\d+[.)]\\s+")));
    out.replace(QLatin1Char('*'), QLatin1Char(' '));
    out.replace(QLatin1Char('_'), QLatin1Char(' '));

    QString kept;
    kept.reserve(out.size());
    for (int i = 0; i < out.size(); ++i) {
        const QChar ch = out.at(i);
        const uint u = ch.unicode();
        if (ch.isHighSurrogate() && i + 1 < out.size()) {
            const uint lo = out.at(i + 1).unicode();
            const uint cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            if (cp >= 0x1F000 && cp <= 0x1FAFF) {  // emoji / pictographs
                ++i;
                continue;
            }
        }
        // Dingbats / misc symbols / variation selectors：朗读没意义。
        if ((u >= 0x2600 && u <= 0x27BF) || u == 0xFE0F || u == 0xFE0E) {
            continue;
        }
        kept.append(ch);
    }
    kept.replace(QRegularExpression(QStringLiteral("[ \\t]+")),
                 QStringLiteral(" "));
    kept.replace(QRegularExpression(QStringLiteral("\\s*\\n\\s*")),
                 QStringLiteral(" "));
    kept = kept.trimmed();
    if (kept.isEmpty()) {
        return out.trimmed();
    }
    // 再长也别让 TTS 念一分钟：优先切在句号/问号/感叹号。
    constexpr int kMaxChars = 120;
    if (kept.size() > kMaxChars) {
        const QString head = kept.left(kMaxChars);
        const int cut = qMax(head.lastIndexOf(QChar(0x3002)),
                             qMax(head.lastIndexOf(QChar(0xFF01)),
                                  head.lastIndexOf(QChar(0xFF1F))));
        kept = cut > 20 ? head.left(cut + 1) : head + QStringLiteral("…");
    }
    return kept;
}

}  // namespace ChatText
