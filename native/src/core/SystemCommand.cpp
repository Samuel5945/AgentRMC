#include "SystemCommand.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

namespace {

// 说话时的赘词，抹掉后不影响判定：「帮我把音量调大一点」=「音量调大」。
// 只放不会改变语义的词——「不要」「别」这类否定词绝不能进这张表。
const QStringList kFillers = {
    QStringLiteral("帮我"), QStringLiteral("帮忙"), QStringLiteral("麻烦"),
    QStringLiteral("请"),   QStringLiteral("给我"), QStringLiteral("我要"),
    QStringLiteral("我想"), QStringLiteral("现在"), QStringLiteral("马上"),
    QStringLiteral("立刻"), QStringLiteral("把"),   QStringLiteral("一下"),
    QStringLiteral("吧"),   QStringLiteral("呢"),   QStringLiteral("啊"),
    QStringLiteral("呀"),   QStringLiteral("哦"),
};

const QStringList kVolumeNouns = {
    QStringLiteral("音量"), QStringLiteral("声音"),
    QStringLiteral("声响"), QStringLiteral("响度"),
    QStringLiteral("大声"), QStringLiteral("小声"),
};

const QStringList kBrightnessNouns = {
    QStringLiteral("屏幕亮度"), QStringLiteral("显示器亮度"),
    QStringLiteral("亮度"),       QStringLiteral("屏幕"),
    QStringLiteral("显示器"),
};

// 否定点名要调系统的话（「别调音量」「这个音量不要动」）一律不命中。
const QStringList kNegations = {
    QStringLiteral("别"),   QStringLiteral("不要"), QStringLiteral("不用"),
    QStringLiteral("不想"), QStringLiteral("不必"), QStringLiteral("无需"),
    QStringLiteral("停止"),
};

const QStringList kQuestions = {
    QStringLiteral("吗"),   QStringLiteral("什么"), QStringLiteral("是不是"),
    QStringLiteral("会不会"), QStringLiteral("为什么"), QStringLiteral("怎么"),
    QStringLiteral("如何"), QStringLiteral("能不能"),
};

const QStringList kUpWords = {
    QStringLiteral("大"), QStringLiteral("高"), QStringLiteral("升"),
    QStringLiteral("加"), QStringLiteral("增"), QStringLiteral("提"),
};

const QStringList kDownWords = {
    QStringLiteral("小"), QStringLiteral("低"), QStringLiteral("降"),
    QStringLiteral("减"),
};

// 「亮度变暗了」是在描述现象，不是命令：光有方向词不够，得有「一点/一些」
// 这种幅度尾缀，或者一个使役动词。
const QStringList kStepVerbs = {
    QStringLiteral("调"), QStringLiteral("设"), QStringLiteral("改"),
    QStringLiteral("弄"), QStringLiteral("来"), QStringLiteral("加"),
    QStringLiteral("减"), QStringLiteral("增"), QStringLiteral("降"),
    QStringLiteral("提"), QStringLiteral("升"),
};

const QStringList kStepTails = {
    QStringLiteral("一点"), QStringLiteral("一些"), QStringLiteral("点"),
    QStringLiteral("些"),
};

// 问「音量多少」是查询，不是调整。
const QStringList kQueryWords = {
    QStringLiteral("多少"), QStringLiteral("多大"), QStringLiteral("几"),
};

// 亮度按屏调：点名所有屏 / 主屏的词。
const QStringList kScopeAllWords = {
    QStringLiteral("所有"), QStringLiteral("全部"),
    QStringLiteral("每块"), QStringLiteral("都"),
};

const QStringList kScopePrimaryWords = {
    QStringLiteral("主屏"), QStringLiteral("主显示"),
    QStringLiteral("主monitor"),
};

const QStringList kVolumeMaxWords = {
    QStringLiteral("最大"), QStringLiteral("最高"),
};

const QStringList kVolumeMinWords = {
    QStringLiteral("最小"), QStringLiteral("最低"),
};

// 亮度独有的方向词（「调亮一点」「暗一些」）。
const QStringList kBrightnessUpWords = {
    QStringLiteral("亮"),
};

const QStringList kBrightnessDownWords = {
    QStringLiteral("暗"),
};

const QStringList kBrightnessMaxWords = {
    QStringLiteral("最亮"), QStringLiteral("最高"), QStringLiteral("最大"),
};

const QStringList kBrightnessMinWords = {
    QStringLiteral("最暗"), QStringLiteral("最低"), QStringLiteral("最小"),
};

QString flatText(const QString &text) {
    QString flat;
    flat.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isSpace() || ch.isPunct()) {
            continue;
        }
        flat.append(ch.toLower());
    }
    for (const QString &filler : kFillers) {
        flat.replace(filler, QString());
    }
    return flat;
}

bool hasAny(const QString &text, const QStringList &words) {
    for (const QString &word : words) {
        if (text.contains(word)) {
            return true;
        }
    }
    return false;
}

int chinesePercent(const QString &text) {
    static const QHash<QChar, int> digits = {
        {QChar(0x96F6), 0}, {QChar(0x4E00), 1}, {QChar(0x4E8C), 2},
        {QChar(0x4E24), 2}, {QChar(0x4E09), 3}, {QChar(0x56DB), 4},
        {QChar(0x4E94), 5}, {QChar(0x516D), 6}, {QChar(0x4E03), 7},
        {QChar(0x516B), 8}, {QChar(0x4E5D), 9},
    };
    const QChar hundred(0x767E);  // 百
    const QChar ten(0x5341);      // 十
    if (text == QStringLiteral("一百") || text == QStringLiteral("百")) {
        return 100;
    }
    // 只收 0~100，所以除了「一百」以外带「百」的说法（一百零五等）一律不认，
    // 免得把 105 当成 5 调下去。
    if (text.contains(hundred)) {
        return -1;
    }
    int total = 0;
    int pending = 0;
    bool saw = false;
    for (const QChar ch : text) {
        if (digits.contains(ch)) {
            pending = digits.value(ch);
            saw = true;
            continue;
        }
        if (ch == ten) {
            total += (pending == 0 ? 1 : pending) * 10;
            pending = 0;
            saw = true;
            continue;
        }
        return -1;
    }
    if (!saw) {
        return -1;
    }
    total += pending;
    return total >= 0 && total <= 100 ? total : -1;
}

// 明确的百分比说法：「百分之五十」或阿拉伯数字。这两形态无歧义，一定是绝对值。
int explicitPercentIn(const QString &text) {
    const QString marker = QStringLiteral("百分之");
    const int at = text.indexOf(marker);
    if (at >= 0) {
        const int value =
            SystemCommandText::percentValue(text.mid(at + marker.size()));
        if (value >= 0) {
            return value;
        }
    }
    static const QRegularExpression digits(QStringLiteral("(\\d{1,3})"));
    const auto match = digits.match(text);
    if (match.hasMatch()) {
        const int value = match.captured(1).toInt();
        return value >= 0 && value <= 100 ? value : -1;
    }
    return -1;
}

// 只留数字字符再解中文数字：「减二十」→「二十」→ 20。
int numeralIn(const QString &text) {
    static const QString allowed = QStringLiteral("0123456789零一二两三四五六七八九十百");
    QString digits;
    for (const QChar ch : text) {
        if (allowed.contains(ch)) {
            digits.append(ch);
        }
    }
    return digits.isEmpty() ? -1 : SystemCommandText::percentValue(digits);
}

SystemCommand make(const QString &kind, const QString &op, int value = 0) {
    SystemCommand command;
    command.matched = true;
    command.kind = kind;
    command.op = op;
    command.value = value;
    return command;
}

struct ExactEntry {
    QString spoken;
    SystemCommand command;
};

// 一句话说得出口，parse() 就认得；spokenList() 也照这一张表念「你能说什么」。
// 用有序表而不是 QHash：清单要有稳定次序，同义说法归到同一个动作下面。
const QVector<ExactEntry> &exactTable() {
    static const QVector<ExactEntry> table = {
        {QStringLiteral("播放"), make("media", "play")},
        {QStringLiteral("继续播放"), make("media", "play")},
        {QStringLiteral("暂停"), make("media", "pause")},
        {QStringLiteral("暂停播放"), make("media", "pause")},
        {QStringLiteral("播放暂停"), make("media", "toggle")},
        {QStringLiteral("下一首"), make("media", "next")},
        {QStringLiteral("下一曲"), make("media", "next")},
        {QStringLiteral("下一首歌"), make("media", "next")},
        {QStringLiteral("上一首"), make("media", "prev")},
        {QStringLiteral("上一曲"), make("media", "prev")},
        {QStringLiteral("上一首歌"), make("media", "prev")},
        {QStringLiteral("关屏幕"), make("screen_off", "off")},
        {QStringLiteral("关闭屏幕"), make("screen_off", "off")},
        {QStringLiteral("关屏"), make("screen_off", "off")},
        {QStringLiteral("熄屏"), make("screen_off", "off")},
        {QStringLiteral("关闭显示器"), make("screen_off", "off")},
        {QStringLiteral("锁屏"), make("lock", "lock")},
        {QStringLiteral("锁定屏幕"), make("lock", "lock")},
        {QStringLiteral("锁定电脑"), make("lock", "lock")},
        {QStringLiteral("睡眠"), make("sleep", "suspend")},
        {QStringLiteral("休眠"), make("hibernate", "hibernate")},
        {QStringLiteral("电脑睡眠"), make("sleep", "suspend")},
        {QStringLiteral("关机"), make("shutdown", "off")},
        {QStringLiteral("关闭电脑"), make("shutdown", "off")},
        {QStringLiteral("电脑关机"), make("shutdown", "off")},
        {QStringLiteral("重启"), make("restart", "off")},
        {QStringLiteral("重启电脑"), make("restart", "off")},
        {QStringLiteral("取消关机"), make("shutdown_cancel", "cancel")},
        {QStringLiteral("取消重启"), make("shutdown_cancel", "cancel")},
        {QStringLiteral("静音"), make("volume", "mute")},
        {QStringLiteral("取消静音"), make("volume", "unmute")},
        {QStringLiteral("解除静音"), make("volume", "unmute")},
        {QStringLiteral("关闭静音"), make("volume", "unmute")},
    };
    return table;
}

// 表里同一种说法有好几种讲法，清单只报第一种（第一种最常用）。
QString firstSpoken(const QString &kind, const QString &op = QString()) {
    for (const ExactEntry &entry : exactTable()) {
        if (entry.command.kind == kind &&
            (op.isEmpty() || entry.command.op == op)) {
            return entry.spoken;
        }
    }
    return QString();
}

SystemCommand parseExact(const QString &flat) {
    for (const ExactEntry &entry : exactTable()) {
        if (entry.spoken == flat) {
            return entry.command;
        }
    }
    return {};
}

// 音量与亮度共用同一套「值 / 最值 / 方向」判定，只有词表不同。
SystemCommand parseLevel(const QString &flat, const QStringList &nouns,
                         const QString &kind, const QStringList &upExtra,
                         const QStringList &downExtra,
                         const QStringList &maxWords,
                         const QStringList &minWords,
                         const QHash<QString, int> &nounDirection = {}) {
    QString noun;
    int at = -1;
    for (const QString &candidate : nouns) {
        const int found = flat.indexOf(candidate);
        if (found >= 0 && (at < 0 || found < at)) {
            noun = candidate;
            at = found;
        }
    }
    if (at < 0) {
        return {};
    }
    const QString before = flat.left(at);
    const QString after = flat.mid(at + noun.size());
    // 否定词出现在域名词前面 = 这句话在拒绝调整，不是命令。
    if (hasAny(before, kNegations) || hasAny(after, kQuestions)) {
        return {};
    }
    SystemCommand command;
    command.kind = kind;
    command.matched = true;
    if (kind == QStringLiteral("brightness")) {
        if (hasAny(before, kScopeAllWords)) {
            command.screen = ScreenScope::All;
        } else if (hasAny(before, kScopePrimaryWords)) {
            command.screen = ScreenScope::Primary;
        }
    }
    if (hasAny(after, kQueryWords)) {
        command.op = QStringLiteral("query");
        return command;
    }
    if (hasAny(after, maxWords)) {
        command.op = QStringLiteral("set");
        command.value = 100;
        return command;
    }
    if (hasAny(after, minWords)) {
        command.op = QStringLiteral("set");
        command.value = 0;
        return command;
    }
    const int percent = explicitPercentIn(after);
    if (percent >= 0) {
        command.op = QStringLiteral("set");
        command.value = percent;
        return command;
    }
    QStringList upWords = kUpWords;
    upWords += upExtra;
    QStringList downWords = kDownWords;
    downWords += downExtra;
    const bool saysUp = hasAny(after, upWords);
    const bool saysDown = hasAny(after, downWords);
    // 「小声一点」的方向藏在名词里，后半句只有幅度，得从名词补出来。
    const int implied = nounDirection.value(noun, 0);
    const bool goesUp = saysUp || (!saysDown && implied > 0);
    const bool goesDown = saysDown || (!saysUp && implied < 0);
    if (goesUp == goesDown) {
        // 没有方向词时，纯中文数字（「音量五十」）才是绝对值。
        const int spoken = implied == 0 ? numeralIn(after) : -1;
        if (spoken >= 0) {
            command.op = QStringLiteral("set");
            command.value = spoken;
            return command;
        }
        return {};  // 没有方向，或者「又高又低」这种自相矛盾的话
    }
    if (!hasAny(after, kStepTails) && !hasAny(after, kStepVerbs)) {
        return {};
    }
    // 「减二十」里的中文数字是幅度，不是目标值；「大一点」这种带尾缀的
    // 一律按小步长走，不去抠里面的「一」。
    int magnitude = hasAny(after, kStepTails) ? 5 : 10;
    if (!hasAny(after, kStepTails)) {
        const int spoken = numeralIn(after);
        if (spoken > 0) {
            magnitude = spoken;
        }
    }
    command.op = QStringLiteral("step");
    command.value = magnitude * (goesUp ? 1 : -1);
    return command;
}

}  // namespace

namespace SystemCommandText {

int percentValue(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return -1;
    }
    bool numeric = true;
    for (const QChar ch : trimmed) {
        if (!ch.isDigit()) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        const int value = trimmed.toInt();
        return value >= 0 && value <= 100 ? value : -1;
    }
    return chinesePercent(trimmed);
}

bool needsConfirm(const QString &kind) {
    return kind == QStringLiteral("shutdown") ||
           kind == QStringLiteral("restart") ||
           kind == QStringLiteral("sleep") ||
           kind == QStringLiteral("hibernate");
}

QString category(const QString &kind) {
    if (kind == QStringLiteral("volume")) {
        return QStringLiteral("volume");
    }
    if (kind == QStringLiteral("brightness")) {
        return QStringLiteral("brightness");
    }
    if (kind == QStringLiteral("media")) {
        return QStringLiteral("media");
    }
    return QStringLiteral("power");
}

QString label(const SystemCommand &command) {
    const QString kind = command.kind;
    if (kind == QStringLiteral("volume")) {
        if (command.op == QStringLiteral("mute")) {
            return QStringLiteral("静音");
        }
        if (command.op == QStringLiteral("unmute")) {
            return QStringLiteral("取消静音");
        }
        if (command.op == QStringLiteral("step")) {
            return command.value >= 0 ? QStringLiteral("音量 +")
                                      : QStringLiteral("音量 -");
        }
        return QStringLiteral("音量");
    }
    if (kind == QStringLiteral("brightness")) {
        if (command.op == QStringLiteral("step")) {
            return command.value >= 0 ? QStringLiteral("亮度 +")
                                      : QStringLiteral("亮度 -");
        }
        return QStringLiteral("亮度");
    }
    if (kind == QStringLiteral("media")) {
        static const QHash<QString, QString> names = {
            {QStringLiteral("play"), QStringLiteral("播放")},
            {QStringLiteral("pause"), QStringLiteral("暂停")},
            {QStringLiteral("toggle"), QStringLiteral("播放/暂停")},
            {QStringLiteral("next"), QStringLiteral("下一首")},
            {QStringLiteral("prev"), QStringLiteral("上一首")},
        };
        return names.value(command.op, QStringLiteral("媒体"));
    }
    static const QHash<QString, QString> names = {
        {QStringLiteral("screen_off"), QStringLiteral("关屏幕")},
        {QStringLiteral("lock"), QStringLiteral("锁屏")},
        {QStringLiteral("sleep"), QStringLiteral("睡眠")},
        {QStringLiteral("hibernate"), QStringLiteral("休眠")},
        {QStringLiteral("shutdown"), QStringLiteral("关机")},
        {QStringLiteral("restart"), QStringLiteral("重启")},
        {QStringLiteral("shutdown_cancel"), QStringLiteral("取消关机")},
    };
    return names.value(kind, kind);
}

QString confirmPhrase(const QString &kind) {
    SystemCommand command;
    command.kind = kind;
    return QStringLiteral("确认") + label(command);
}

QString spokenList() {
    // 逐条从解析词表生成：以后加个说法只改 exactTable()/词表常量，
    // 这里跟着变——桌宠答给用户的清单不会和真听得懂的词分叉。
    QStringList media;
    QStringList seenOps;
    QStringList power;
    QStringList danger;
    QStringList seenKinds;
    for (const ExactEntry &entry : exactTable()) {
        const QString &kind = entry.command.kind;
        if (kind == QStringLiteral("volume")) {
            continue;
        }
        if (kind == QStringLiteral("media")) {
            if (!seenOps.contains(entry.command.op)) {
                seenOps.append(entry.command.op);
                media.append(entry.spoken);
            }
            continue;
        }
        if (!seenKinds.contains(kind)) {
            seenKinds.append(kind);
            power.append(entry.spoken);
            if (needsConfirm(kind)) {
                danger.append(entry.spoken);
            }
        }
    }
    const QString mutePair = firstSpoken(QStringLiteral("volume"),
                                         QStringLiteral("mute")) +
                             QStringLiteral("/") +
                             firstSpoken(QStringLiteral("volume"),
                                         QStringLiteral("unmute"));
    QStringList lines;
    lines << QString::fromUtf8(
                "音量：可说 %1；升一档用 %2，降一档用 %3，也能直接给数值"
                "（音量 30 / 百分之三十）；顶格=%4，到底=%5；还有 %6。"
                "问「音量多少」是查当前值。")
                 .arg(kVolumeNouns.join(QStringLiteral("/")),
                      kUpWords.join(QStringLiteral("/")),
                      kDownWords.join(QStringLiteral("/")),
                      kVolumeMaxWords.join(QStringLiteral("/")),
                      kVolumeMinWords.join(QStringLiteral("/")), mutePair);
    lines << QString::fromUtf8(
                "亮度：可说 %1；方向词同音量，另有 %2/%3；顶格=%4，到底=%5；"
                "加 %6 调所有屏、加 %7 只调主屏。问「亮度多少」是查当前值。")
                 .arg(kBrightnessNouns.join(QStringLiteral("/")),
                      kBrightnessUpWords.join(QStringLiteral("/")),
                      kBrightnessDownWords.join(QStringLiteral("/")),
                      kBrightnessMaxWords.join(QStringLiteral("/")),
                      kBrightnessMinWords.join(QStringLiteral("/")),
                      kScopeAllWords.join(QStringLiteral("/")),
                      kScopePrimaryWords.join(QStringLiteral("/")));
    lines << QString::fromUtf8("媒体：%1（同义说法都认）。")
                 .arg(media.join(QStringLiteral("、")));
    lines << QString::fromUtf8("屏幕与电源：%1。")
                 .arg(power.join(QStringLiteral("、")));
    if (!danger.isEmpty()) {
        lines << QString::fromUtf8(
                    "其中 %1 属危险动作：先说一句，要用户再说「确认XX」"
                    "（如「确认关机」）才真执行。")
                     .arg(danger.join(QStringLiteral("/")));
    }
    return lines.join(QStringLiteral("\n"));
}

SystemCommand parse(const QString &text) {
    const QString flat = flatText(text);
    if (flat.isEmpty()) {
        return {};
    }
    // 「确认关机」这类口令也解析出来，交调用方比对当前待确认的那一条；
    // 解析层不认识 pending 状态，保持纯函数。
    const QString confirmPrefix = QStringLiteral("确认");
    if (flat.startsWith(confirmPrefix)) {
        SystemCommand inner = parseExact(flat.mid(confirmPrefix.size()));
        if (inner.matched && needsConfirm(inner.kind)) {
            inner.confirmed = true;
            return inner;
        }
        return {};
    }
    const SystemCommand exact = parseExact(flat);
    if (exact.matched) {
        return exact;
    }
    const SystemCommand volume = parseLevel(
        flat, kVolumeNouns, QStringLiteral("volume"), {}, {},
        kVolumeMaxWords, kVolumeMinWords,
        {{QStringLiteral("大声"), 1}, {QStringLiteral("小声"), -1}});
    if (volume.matched) {
        return volume;
    }
    return parseLevel(flat, kBrightnessNouns, QStringLiteral("brightness"),
                      kBrightnessUpWords, kBrightnessDownWords,
                      kBrightnessMaxWords, kBrightnessMinWords);
}

}  // namespace SystemCommandText
