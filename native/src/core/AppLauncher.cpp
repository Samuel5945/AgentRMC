#include "AppLauncher.h"

#include "PinyinInitials.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>

#include <cstdio>

#include <algorithm>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace {

// 说话时的填充词，归一化后直接抹掉：「帮我打开剪映吧」= 「打开剪映」。
const QStringList kFillers = {
    QStringLiteral("帮我"), QStringLiteral("帮忙"),
    QStringLiteral("麻烦"), QStringLiteral("给我"),
    QStringLiteral("请"),   QStringLiteral("一下"),
    QStringLiteral("现在"), QStringLiteral("马上"),
    QStringLiteral("立刻"), QStringLiteral("直接"),
    QStringLiteral("我要"), QStringLiteral("我想"),
    QStringLiteral("谢谢"), QStringLiteral("吧"),
    QStringLiteral("呢"),   QStringLiteral("啊"),
    QStringLiteral("呀"),   QStringLiteral("哦"),
};

// 从长到短：先「切换到」再「切到」。
const QStringList kVerbs = {
    QStringLiteral("切换到"), QStringLiteral("打开"),
    QStringLiteral("启动"),   QStringLiteral("运行"),
    QStringLiteral("开启"),   QStringLiteral("回到"),
    QStringLiteral("调出"),   QStringLiteral("切到"),
    QStringLiteral("开"),
};

QMutex g_cacheMutex;
QHash<QString, QString> g_shortcuts;
bool g_shortcutsBuilt = false;

QStringList startMenuRoots() {
    QStringList roots;
    const QString programData = qEnvironmentVariable("ProgramData");
    if (!programData.isEmpty()) {
        roots << programData +
                     QStringLiteral("/Microsoft/Windows/Start Menu/Programs");
    }
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        roots << appData +
                     QStringLiteral("/Microsoft/Windows/Start Menu/Programs");
    }
    return roots;
}

bool looksLikeUninstall(const QString &flatName) {
    return flatName.contains(QStringLiteral("卸载")) ||
           flatName.startsWith(QStringLiteral("uninstall"));
}

// 调用方持有 g_cacheMutex。
void buildShortcutCache() {
    g_shortcuts.clear();
    for (const QString &root : startMenuRoots()) {
        QDirIterator it(root, {QStringLiteral("*.lnk")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString flat =
                AppLauncher::normalize(QFileInfo(path).completeBaseName());
            if (flat.isEmpty() || looksLikeUninstall(flat)) {
                continue;
            }
            // 后一轮扫的是用户目录，同名时盖掉全体用户的那个。
            g_shortcuts.insert(flat, path);
            // 「PotPlayer 64 bit」「Photoshop 2026」这类带尾巴的名字，把
            // 去尾后的名字也挂上（不覆盖已有键），精确说「打开 potplayer」
            // 和模糊容错都能对上。
            QString alt = flat;
            alt.remove(QRegularExpression(QStringLiteral("(32|64)bit$")));
            alt.remove(QRegularExpression(QStringLiteral("\\d+$")));
            if (alt.size() >= 3 && alt != flat && !g_shortcuts.contains(alt)) {
                g_shortcuts.insert(alt, path);
            }
        }
    }
    g_shortcutsBuilt = true;
}

QString shortcutPath(const QString &flatName) {
    if (flatName.isEmpty()) {
        return {};
    }
    QMutexLocker locker(&g_cacheMutex);
    if (!g_shortcutsBuilt) {
        buildShortcutCache();
    }
    return g_shortcuts.value(flatName);
}

QString firstQuotedPath(const QString &command) {
    const int start = command.indexOf(QLatin1Char('"'));
    if (start < 0) {
        return command.section(QLatin1Char(' '), 0, 0);
    }
    const int end = command.indexOf(QLatin1Char('"'), start + 1);
    if (end < 0) {
        return {};
    }
    return command.mid(start + 1, end - start - 1);
}

// 注册表里的 http 关联 -> 默认浏览器真正的 exe；拿不到就退回「开一个网页」。
QString defaultBrowserExe() {
    const QSettings choice(
        QStringLiteral(
            "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice"),
        QSettings::NativeFormat);
    const QString progId = choice.value(QStringLiteral("ProgId")).toString();
    if (progId.isEmpty()) {
        return {};
    }
    const QSettings command(QStringLiteral("HKEY_CLASSES_ROOT\\%1\\shell\\open\\command")
                                .arg(progId),
                            QSettings::NativeFormat);
    // Qt 把注册表的「默认值」键名映射成 "."。
    QString exe = firstQuotedPath(command.value(QStringLiteral(".")).toString());
    if (exe.contains(QStringLiteral("%"))) {
        exe = QDir::cleanPath(exe.replace(QStringLiteral("%1"),
                                          qEnvironmentVariable("SystemRoot")));
    }
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        return {};
    }
    if (exe.contains(QStringLiteral("explorer.exe"), Qt::CaseInsensitive)) {
        return {};
    }
    return exe;
}

// 识别引擎常把英文名带个尾巴（"Qoder" → "coder c"）。别名是纯英文时，
// 允许再跟最多两个 ASCII 字母数字；中文尾巴不算（那是接着说的话）。
bool aliasMatches(const QString &want, const QString &alias) {
    if (want == alias) {
        return true;
    }
    if (alias.size() < 4 || !want.startsWith(alias)) {
        return false;
    }
    const QString tail = want.mid(alias.size());
    if (tail.size() > 2) {
        return false;
    }
    for (const QChar ch : tail) {
        if (!ch.isLetterOrNumber() || ch.unicode() >= 128) {
            return false;
        }
    }
    return true;
}

int levenshtein(const QString &a, const QString &b) {
    if (a.isEmpty()) {
        return int(b.size());
    }
    if (b.isEmpty()) {
        return int(a.size());
    }
    std::vector<int> prev(b.size() + 1);
    std::vector<int> cur(b.size() + 1);
    for (int j = 0; j <= int(b.size()); ++j) {
        prev[j] = j;
    }
    for (int i = 1; i <= int(a.size()); ++i) {
        cur[0] = i;
        for (int j = 1; j <= int(b.size()); ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[int(b.size())];
}

// 1.0 = 完全一样，越短的分母让「错一点」的长名字掉分更快。
double similarity(const QString &a, const QString &b) {
    const int longest = int(std::max(a.size(), b.size()));
    if (longest == 0) {
        return 0.0;
    }
    return 1.0 - levenshtein(a, b) / double(longest);
}

namespace {

constexpr const char *kVowels = "aeiouy";

bool isVowel(QChar ch) {
    return ch.unicode() < 128 && (ch == 'a' || ch == 'e' || ch == 'i' ||
                                  ch == 'o' || ch == 'u' || ch == 'y');
}

// 把一个字折进骨架，连续重复的只留一个（buddy 的 dd 不该卡分）。
void pushBone(QString *out, QChar ch) {
    if (!out->isEmpty() && out->back() == ch) {
        return;
    }
    out->append(ch);
}

}  // namespace

// 声母骨架：汉字取拼音首字母（剪映→jy、深度求索→sdqs），ASCII 走辅音骨架
// （去元音，y 按半元音；deepseek→dpsk）。这是第一层容错：
// 「WOKBER」「WRK八点」「剪易」都在这层直接对上。
QString initialBones(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.unicode() < 128) {
            if (ch.isLetterOrNumber() && !isVowel(ch)) {
                pushBone(&out, ch);
            }
            continue;
        }
        const QString pinyin = PinyinInitials::of(ch.unicode());
        if (!pinyin.isEmpty()) {
            pushBone(&out, pinyin.front());
        }
    }
    return out;
}

// 全拼骨架：汉字取完整无声调拼音（剪映→jianying），ASCII 仍走辅音骨架。
// 比声母骨架更挑剔，负责「简面 jianmian ≈ 剪映 jianying」这种声母对不上
// 但读音接近的走样。
QString pinyinBones(const QString &text) {
    QString out;
    out.reserve(text.size() * 2);
    for (const QChar ch : text) {
        if (ch.unicode() < 128) {
            if (ch.isLetterOrNumber() && !isVowel(ch)) {
                pushBone(&out, ch);
            }
            continue;
        }
        const QString pinyin = PinyinInitials::of(ch.unicode());
        for (const QChar p : pinyin) {
            pushBone(&out, p);
        }
    }
    return out;
}

// 全拼骨架的分数罚量：全拼路径按 (相似度 - 罚量) 计入，等效门槛抬高到
// 0.65，压住「链机 lianji ≈ 浏览器 liulanqi」这类只是读音有点像的词。
constexpr double kPinyinPenalty = 0.05;

// 汉字口述对纯英文别名时声母骨架容易撞车（「这个应用」的 zgy 蹭 zcode），
// 这条路径要求骨架够长；中文应用名走拼音路径和同文字的别名兜底。
bool wantHasHanChar(const QString &text) {
    for (const QChar ch : text) {
        if (ch.unicode() >= 0x2E80) {
            return true;
        }
    }
    return false;
}

bool aliasIsAscii(const QString &text) {
    for (const QChar ch : text) {
        if (ch.unicode() >= 128) {
            return false;
        }
    }
    return true;
}

// 首字符必须对得上（原样或两种骨架任选其一）：不放行的话
// 「打开 vscode」会蹭到 zcode。全拼骨架要更挑剔（门槛抬高 kPinyinPenalty），
// 不然「打开链机 lianji」会蹭到浏览器 liulanqi。
double aliasScore(const QString &want, const QString &flat) {
    if (want.isEmpty() || flat.isEmpty()) {
        return 0.0;
    }
    double score = 0.0;
    // 原样比对只用于两侧都是纯 ASCII 的场合：汉字和字母逐字比会把
    // 「coder设置」硬凑成 "codercn"（设→c、置→n 各只扣 1 分）。
    if (want.front() == flat.front() && aliasIsAscii(want) && aliasIsAscii(flat)) {
        score = similarity(want, flat);
    }
    const QString wantInit = initialBones(want);
    const QString aliasInit = initialBones(flat);
    const bool shortHanVsAscii = wantHasHanChar(want) && aliasIsAscii(flat) &&
                                 wantInit.size() < 4;
    if (!shortHanVsAscii && !wantInit.isEmpty() && !aliasInit.isEmpty() &&
        wantInit.front() == aliasInit.front()) {
        score = std::max(score, similarity(wantInit, aliasInit));
    }
    const QString wantPinyin = pinyinBones(want);
    const QString aliasPinyin = pinyinBones(flat);
    if (!wantPinyin.isEmpty() && !aliasPinyin.isEmpty() &&
        wantPinyin.front() == aliasPinyin.front()) {
        score = std::max(score, similarity(wantPinyin, aliasPinyin) -
                                    kPinyinPenalty);
    }
    return score;
}

// 模糊档的门槛：分数要够高，还要明显甩开第二名（不同应用之间）。
// 1 - 2/5 这类值在 double 里是 0.59999...，比较必须带容差。
constexpr double kFuzzyMinScore = 0.6;
constexpr double kFuzzyMargin = 0.1;
constexpr double kScoreEpsilon = 1e-9;

QString shortcutDetail(const QString &path) {
    return QStringLiteral("开始菜单 · %1.lnk")
        .arg(QFileInfo(path).completeBaseName());
}

QString shellErrorText(int code) {
    wchar_t *buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    if (FormatMessageW(flags, nullptr, DWORD(code), 0,
                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr) > 0 &&
        buffer != nullptr) {
        const QString text = QString::fromWCharArray(buffer).trimmed();
        LocalFree(buffer);
        if (!text.isEmpty()) {
            return text;
        }
    }
    return QStringLiteral("启动失败（错误码 %1）").arg(code);
}

bool launchTarget(const AppTarget &target, QString *error) {
    if (!target.found) {
        if (error != nullptr) {
            *error = QStringLiteral("没有可用的启动目标");
        }
        return false;
    }
    if (target.kind == QStringLiteral("aumid")) {
        return QProcess::startDetached(
            QStringLiteral("explorer.exe"),
            {QStringLiteral("shell:AppsFolder\\") + target.target});
    }
    const QString path = target.kind == QStringLiteral("url")
                             ? target.target
                             : QDir::toNativeSeparators(target.target);
    const std::wstring wide = path.toStdWString();
    const HINSTANCE handle = ShellExecuteW(nullptr, L"open", wide.c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
    const INT_PTR code = reinterpret_cast<INT_PTR>(handle);
    if (code > 32) {
        return true;
    }
    if (error != nullptr) {
        *error = shellErrorText(int(code));
    }
    return false;
}

}  // namespace

const QVector<AppEntry> &AppLauncher::catalog() {
    static const QVector<AppEntry> table = {
        {QStringLiteral("browser"), QStringLiteral("浏览器"),
         {QStringLiteral("浏览器"), QStringLiteral("默认浏览器"),
          QStringLiteral("网页"), QStringLiteral("browser")},
         {}, {}},
        {QStringLiteral("chrome"), QStringLiteral("Chrome"),
         {QStringLiteral("chrome"), QStringLiteral("谷歌浏览器"),
          QStringLiteral("谷歌")},
         {QStringLiteral("Google Chrome")}, {}},
        {QStringLiteral("edge"), QStringLiteral("Edge"),
         {QStringLiteral("edge"), QStringLiteral("微软浏览器")},
         {QStringLiteral("Microsoft Edge")}, {}},
        {QStringLiteral("qoder"), QStringLiteral("Qoder"),
         {QStringLiteral("qoder"), QStringLiteral("coder")},
         {QStringLiteral("Qoder")}, QStringLiteral("com.qoder.app")},
        {QStringLiteral("qodercn"), QStringLiteral("Qoder CN"),
         {QStringLiteral("qoder cn"), QStringLiteral("qodercn"),
          QStringLiteral("coder cn"), QStringLiteral("codercn"),
          QStringLiteral("qoder 中国版"), QStringLiteral("qoder 中文版")},
         {QStringLiteral("Qoder CN")}, QStringLiteral("com.qodercn.app")},
        {QStringLiteral("workbuddy"), QStringLiteral("WorkBuddy"),
         {QStringLiteral("workbuddy"), QStringLiteral("work buddy"),
          QStringLiteral("工作伙伴")},
         {QStringLiteral("WorkBuddy")}, QStringLiteral("WorkBuddy.WorkBuddy")},
        {QStringLiteral("deepseek"), QStringLiteral("DeepSeek"),
         {QStringLiteral("deepseek"), QStringLiteral("deep seek"),
          QStringLiteral("深度求索"), QStringLiteral("深度搜索")},
         {QStringLiteral("DeepSeek Harness")},
         QStringLiteral("com.deepseek.harness")},
        {QStringLiteral("codex"), QStringLiteral("Codex"),
         {QStringLiteral("codex"), QStringLiteral("chatgpt"),
          QStringLiteral("chat gpt"), QStringLiteral("gpt"),
          QStringLiteral("openai")},
         {}, QStringLiteral("OpenAI.Codex_2p2nqsd0c76g0!App")},
        {QStringLiteral("zcode"), QStringLiteral("ZCode"),
         {QStringLiteral("zcode"), QStringLiteral("z code")},
         {QStringLiteral("ZCode")}, QStringLiteral("dev.zcode.app")},
        {QStringLiteral("jianying"), QStringLiteral("剪映"),
         {QStringLiteral("剪映"), QStringLiteral("剪映专业版"),
          QStringLiteral("jianying"), QStringLiteral("jianyingpro")},
         {QStringLiteral("剪映专业版")},
         QStringLiteral("Bytedance.JianyingPro")},
    };
    return table;
}

QStringList AppLauncher::appIds() {
    QStringList ids;
    for (const AppEntry &entry : catalog()) {
        ids << entry.id;
    }
    return ids;
}

const AppEntry *AppLauncher::findApp(const QString &id) {
    for (const AppEntry &entry : catalog()) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

QString AppLauncher::nameOf(const QString &id) {
    const AppEntry *entry = findApp(id);
    return entry != nullptr ? entry->name : id;
}

AppTarget AppLauncher::resolve(const AppEntry &entry) {
    AppTarget target;
    if (entry.id == QStringLiteral("browser")) {
        const QString exe = defaultBrowserExe();
        target.found = true;
        if (!exe.isEmpty()) {
            target.kind = QStringLiteral("exe");
            target.target = exe;
            target.detail = QStringLiteral("系统默认浏览器 · ") +
                            QFileInfo(exe).fileName();
        } else {
            target.kind = QStringLiteral("url");
            target.target = QStringLiteral("https://www.bing.com");
            target.detail = QStringLiteral("系统默认浏览器");
        }
        return target;
    }
    for (const QString &name : entry.shortcuts) {
        const QString path = shortcutPath(normalize(name));
        if (!path.isEmpty()) {
            target.found = true;
            target.kind = QStringLiteral("shortcut");
            target.target = path;
            target.detail = shortcutDetail(path);
            return target;
        }
    }
    if (!entry.aumid.isEmpty()) {
        target.found = true;
        target.kind = QStringLiteral("aumid");
        target.target = entry.aumid;
        target.detail = QStringLiteral("Windows 应用");
    }
    return target;
}

AppTarget AppLauncher::resolve(const QString &id) {
    const AppEntry *entry = findApp(id);
    return entry != nullptr ? resolve(*entry) : AppTarget{};
}

bool AppLauncher::launch(const QString &id, QString *error) {
    const AppEntry *entry = findApp(id);
    if (entry == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("不认识的应用：") + id;
        }
        return false;
    }
    const AppTarget target = resolve(*entry);
    if (!target.found) {
        if (error != nullptr) {
            *error = QStringLiteral("这台电脑上没装 ") + entry->name;
        }
        return false;
    }
    return launchTarget(target, error);
}

AppVoiceMatch AppLauncher::matchVoice(const QString &text) {
    AppVoiceMatch match;
    QString flat = normalize(text);
    for (const QString &filler : kFillers) {
        flat.replace(filler, QString());
    }
    QString verb;
    for (const QString &candidate : kVerbs) {
        if (flat.startsWith(candidate)) {
            verb = candidate;
            break;
        }
    }
    if (verb.isEmpty()) {
        return match;
    }
    const QString want = flat.mid(verb.size());
    if (want.isEmpty()) {
        return match;
    }

    auto take = [&match](const AppEntry &entry) {
        match.matched = true;
        match.builtin = true;
        match.appId = entry.id;
        match.name = entry.name;
        match.target = resolve(entry);
        match.resolved = match.target.found;
        return match;
    };

    for (const AppEntry &entry : catalog()) {
        for (const QString &alias : entry.aliases) {
            if (normalize(alias) == want) {
                return take(entry);
            }
        }
    }

    // 内置表之外，开始菜单里的应用按名字原样打开（「打开 WorkBuddy AI」）。
    if (want.size() >= 2) {
        const QString path = shortcutPath(want);
        if (!path.isEmpty()) {
            match.matched = true;
            match.resolved = true;
            match.name = QFileInfo(path).completeBaseName();
            match.target = {true, QStringLiteral("shortcut"), path,
                            shortcutDetail(path)};
            return match;
        }
    }

    // 最后才容错：英文名被识别成 "coder c" 这种带尾巴的说法。
    for (const AppEntry &entry : catalog()) {
        for (const QString &alias : entry.aliases) {
            if (aliasMatches(want, normalize(alias))) {
                return take(entry);
            }
        }
    }

    // 全都没对上：识别引擎把名字听岔了——英文整个走样（"WOKBER"、
    // "DIP SCK"、"WRK八点"），中文出别字（「剪映」→「简验 / 剪易 / 剪印」）。
    // 按「原样 / 声母骨架 / 全拼骨架」三种口径，在内置表 + 开始菜单全部
    // 快捷方式里找最接近的；分数要够高还要明显甩开第二名，普通口述才不
    // 会被误当成启动指令。名字的声母骨架只是这句话开头时反而不认——那是
    // 「打开剪映专业版设置」这种说完名字接着说话的口述。
    if (want.size() >= 2) {
        // 「打开 coder 设置 / zcode设置」：话里的英文字母段和某个应用名
        // 一模一样 → 说完名字接着说话的口述，整个模糊档直接放弃。
        QString current;
        bool exactRun = false;
        for (const QChar ch : want + QChar(0x2E80)) {
            if (ch.unicode() < 128 && ch.isLetterOrNumber()) {
                current.append(ch);
                continue;
            }
            if (current.size() >= 2) {
                for (const AppEntry &entry : catalog()) {
                    for (const QString &alias : entry.aliases) {
                        if (normalize(alias) == current) {
                            exactRun = true;
                            break;
                        }
                    }
                    if (exactRun) {
                        break;
                    }
                }
                if (!exactRun) {
                    QMutexLocker locker(&g_cacheMutex);
                    if (!g_shortcutsBuilt) {
                        buildShortcutCache();
                    }
                    exactRun = g_shortcuts.contains(current);
                }
            }
            if (exactRun) {
                break;
            }
            current.clear();
        }
        if (!exactRun) {
            const QString wantBones = initialBones(want);
            const AppEntry *bestEntry = nullptr;
            QString bestLink;
            double bestScore = 0.0;
            double secondScore = 0.0;
            auto offer = [&](double score, const AppEntry *entry,
                             const QString &link) {
                if (score <= 0.0) {
                    return;
                }
                if (qEnvironmentVariableIsSet("AGENTRMC_FUZZY_DEBUG")) {
                    std::fprintf(stderr,
                                 "[fuzzy] %.4f  %s (best %.4f 2nd %.4f)\n",
                                 score,
                                 entry != nullptr
                                     ? entry->id.toUtf8().constData()
                                     : link.toUtf8().constData(),
                                 bestScore, secondScore);
                }
                if (score > bestScore) {
                    secondScore = bestScore;
                    bestScore = score;
                    bestEntry = entry;
                    bestLink = link;
                } else if (score > secondScore) {
                    secondScore = score;
                }
            };
            for (const AppEntry &entry : catalog()) {
                double score = 0.0;
                for (const QString &alias : entry.aliases) {
                    const QString flat = normalize(alias);
                    if (!wantBones.isEmpty()) {
                        const QString aliasBones = initialBones(flat);
                        if (aliasBones.size() < wantBones.size() &&
                            wantBones.startsWith(aliasBones)) {
                            // 名字的声母骨架只是这句话开头（「打开剪映
                            // 专业版设置」）：口述，这个应用不参与模糊。
                            score = 0.0;
                            break;
                        }
                    }
                    score = std::max(score, aliasScore(want, flat));
                }
                offer(score, &entry, QString());
            }
            {
                // 开始菜单里的应用同样容错：「逗包」也能开「豆包」。
                // 名字里含内置别名的（WorkBuddy AI.lnk、DeepSeek Harness…）
                // 是内置应用自己的快捷方式，跳过——不然和内置表打平分，
                // 第二名margin会被吃光。
                QStringList builtinFlats;
                for (const AppEntry &entry : catalog()) {
                    for (const QString &alias : entry.aliases) {
                        builtinFlats.append(normalize(alias));
                    }
                }
                QMutexLocker locker(&g_cacheMutex);
                if (!g_shortcutsBuilt) {
                    buildShortcutCache();
                }
                for (auto it = g_shortcuts.constBegin();
                     it != g_shortcuts.constEnd(); ++it) {
                    const QString &flat = it.key();
                    bool isBuiltin = false;
                    for (const QString &aliasFlat : builtinFlats) {
                        if (flat.contains(aliasFlat)) {
                            isBuiltin = true;
                            break;
                        }
                    }
                    if (isBuiltin) {
                        continue;
                    }
                    if (!wantBones.isEmpty()) {
                        const QString linkBones = initialBones(flat);
                        if (linkBones.size() < wantBones.size() &&
                            wantBones.startsWith(linkBones)) {
                            continue;
                        }
                    }
                    offer(aliasScore(want, flat), nullptr, it.value());
                }
            }
            if (bestScore >= kFuzzyMinScore - kScoreEpsilon &&
                bestScore - secondScore >= kFuzzyMargin - kScoreEpsilon) {
                if (bestEntry != nullptr) {
                    return take(*bestEntry);
                }
                match.matched = true;
                match.resolved = true;
                match.name = QFileInfo(bestLink).completeBaseName();
                match.target = {true, QStringLiteral("shortcut"), bestLink,
                                shortcutDetail(bestLink)};
                return match;
            }
        }
    }
    return match;
}

AppLaunchResult AppLauncher::runVoice(const QString &text) {
    const AppVoiceMatch match = matchVoice(text);
    AppLaunchResult result;
    result.matched = match.matched;
    result.appId = match.appId;
    result.name = match.name;
    if (!match.matched) {
        return result;
    }
    if (!match.resolved) {
        result.error = QStringLiteral("这台电脑上没装 ") + match.name;
        return result;
    }
    result.ok = launchTarget(match.target, &result.error);
    return result;
}

void AppLauncher::refreshCache() {
    QMutexLocker locker(&g_cacheMutex);
    g_shortcutsBuilt = false;
    g_shortcuts.clear();
}

QString AppLauncher::normalize(const QString &text) {
    QString flat;
    flat.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isSpace() || ch.isPunct()) {
            continue;
        }
        flat.append(ch.toLower());
    }
    return flat;
}
