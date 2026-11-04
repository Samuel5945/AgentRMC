#include "core/SpeechText.h"

#include "core/ChatText.h"

#include <QRegularExpression>

namespace {

// 念出来是噪音的文件扩展名：路径化简后把 basename 的扩展名去掉。
const QRegularExpression &extRe() {
    static const QRegularExpression re(
        QStringLiteral("\\.(?:cpp|cc|cxx|h|hpp|js|mjs|cjs|ts|tsx|jsx|json|md|exe|dll|"
                       "png|jpe?g|gif|webp|svg|py|bat|ps1|sh|txt|log|ya?ml|xml|html?|css|"
                       "zip|7z|nsi|wav|mp3|flac|onnx|lib|obj|cmake|ini|cfg|toml|lock|pdf|"
                       "db|sqlite)(?![A-Za-z0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

QString baseName(const QString &path) {
    const int slash = qMax(path.lastIndexOf(QLatin1Char('\\')),
                           path.lastIndexOf(QLatin1Char('/')));
    QString base = slash >= 0 ? path.mid(slash + 1) : path;
    base.remove(extRe());
    return base.trimmed();
}

// 逐个匹配路径 token 化简。keepDrive=true 时保留 Windows 盘符念成「X盘」，
// 否则只念最后一段文件名（相对/POSIX 路径）。
QString reducePaths(const QString &in, const QRegularExpression &re,
                    bool keepDrive) {
    QString out;
    int last = 0;
    auto it = re.globalMatch(in);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append(in.mid(last, m.capturedStart() - last));
        const QString rest = keepDrive ? m.captured(2) : m.captured(0);
        const QString base = baseName(rest);
        if (keepDrive) {
            out.append(m.captured(1).toUpper() + QStringLiteral("盘"));
            if (!base.isEmpty()) {
                out.append(QLatin1Char(' ') + base);
            }
        } else {
            out.append(base.isEmpty() ? m.captured(0) : base);
        }
        last = m.capturedEnd();
    }
    out.append(in.mid(last));
    return out;
}

}  // namespace

namespace SpeechText {

QString friendly(const QString &text) {
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    QString out = text;

    // 1) URL 整条去掉（念出来全是噪音）。
    out.remove(QRegularExpression(
        QStringLiteral("(?:https?://|www\\.)[^\\s，。！？；、）)】」》]+")));

    // 2) Windows 盘符路径 → 「X盘 [文件名]」：D:\repo\foo.cpp → 「D盘 foo」。
    static const QRegularExpression winPath(QStringLiteral(
        "([A-Za-z]):[\\\\/]([^\\s，。！？；、\"'（）()【】「」《》<>]*)"));
    out = reducePaths(out, winPath, true);

    // 3) 相对 / POSIX 路径 → 只念最后一段文件名：native/src/foo.cpp → 「foo」。
    static const QRegularExpression posixPath(QStringLiteral(
        "[\\\\/]?(?:[A-Za-z0-9_.\\-]+[\\\\/])+[A-Za-z0-9_.\\-]+"));
    out = reducePaths(out, posixPath, false);

    // 4) 残留的裸盘符 C: → 「C盘」（时间 12:30 是数字，不会命中）。
    out.replace(QRegularExpression(QStringLiteral("([A-Za-z]):")),
                QStringLiteral("\\1盘"));

    // 5) 散落的文件扩展名去掉：foo.cpp → 「foo」。
    out.remove(extRe());

    // 6) 标识符里的连字符转空格，让 TTS 按词念：handoff-focus → 「handoff focus」。
    out.replace(QRegularExpression(QStringLiteral("([A-Za-z0-9])-(?=[A-Za-z0-9])")),
                QStringLiteral("\\1 "));

    // 7) 装饰标点 / 括号 / 引号 / 箭头转空格（保留括号里的正文），
    //    表情和 markdown 由 cleanReply 收尾。保留 。！？，、；： 做停顿。
    static const QRegularExpression decor(QStringLiteral(
        "[「」《》【】〖〗（）()\\\\[\\]{}〈〉<>“”\"'`~|•·→←—…]"));
    out.replace(decor, QStringLiteral(" "));

    // 8) 收尾：去 markdown/表情、压空白、限长（复用对话清理同一套）。
    QString kept = ChatText::cleanReply(out);
    // 只剩标点/空白时视为无实义内容，让调用方跳过 TTS。
    if (!kept.contains(QRegularExpression(QStringLiteral("[\\p{L}\\p{N}]")))) {
        return QString();
    }
    return kept;
}

}  // namespace SpeechText
