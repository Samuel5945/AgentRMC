#include "core/SessionTranscript.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QVector>

#include <algorithm>

#include <zstd.h>

#include "core/DshSessions.h"
#include "core/ZcodeDispatch.h"

// 宿主自动生成的临时工作区：单任务会话本来没有真项目，WorkBuddy 会按
// 「2026-11-12-21-07-51」这种时间戳建目录，Qoder Documents 工作区、远程控制
// 沙箱、OpenSquilla 任务目录、VirtualDisplay 同理；ZCode 桌面端的无项目任务
// 也都落在 ~/.zcode/workspace/ 下（draft-<uuid> / <时间戳>-<uuid> / default）。
// 拿这种目录名当项目名，会把同一批单任务拆成一串「按日期」的分组——目录页
// 统一收进「未分类任务」（用户 2026-11-18：无工作区的任务折叠到未分类任务里）。
bool SessionTranscript::generatedWorkspace(const QString &project) {
    const QString trimmed = project.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const QString p = QDir::fromNativeSeparators(trimmed).toLower();
    const QStringList generatedDirs{
        QStringLiteral("/workbuddy/"),
        QStringLiteral("/workbuddy ai/"),
        QStringLiteral("/documents/qoder/"),
        QStringLiteral("/appdata/roaming/com.qoder.app.stable/remote-control/"),
        QStringLiteral("/.qwenworkcn/workspace/"),
        QStringLiteral("/.opensquilla/tasks/"),
        QStringLiteral("/.zcode/workspace/"),
        QStringLiteral("/virtualdisplay"),
    };
    for (const QString &dir : generatedDirs) {
        if (p.contains(dir)) {
            return true;
        }
    }
    if (p.endsWith(QStringLiteral("/workbuddy")) ||
        p.endsWith(QStringLiteral("/workbuddy ai")) ||
        p.endsWith(QStringLiteral("/.zcode/workspace"))) {
        return true;
    }
    // 目录名本身就是时间戳：2026-11-12-21-07-51 / 2026-11-12 / 20261112-210751。
    static const QRegularExpression stamp(QStringLiteral(
        "^\\d{4}-?\\d{2}-?\\d{2}([-_ .]?\\d{2}[-_.:]?\\d{2}([-_.:]?\\d{2})?)?$"));
    return stamp.match(QFileInfo(trimmed).fileName()).hasMatch();
}

namespace {

constexpr int kMaxTextChars = 8000;
constexpr int kMaxLimit = 100;
constexpr int kSqliteCap = 500;  // SQLite 宿主全量缓存封顶
constexpr int kCacheEntries = 8;

QString isoFromMs(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString(Qt::ISODateWithMs);
}

// 时间戳两种记法都认：epoch 毫秒/秒（WorkBuddy/ZCode/OpenSquilla）或 ISO 串（Qoder）。
QString normalizeTs(const QJsonValue &v) {
    if (v.isDouble()) {
        qint64 n = static_cast<qint64>(v.toDouble());
        if (n > 100000000000LL) {
            return isoFromMs(n);
        }
        if (n > 1000000000LL) {
            return isoFromMs(n * 1000);
        }
        return QString();
    }
    const QString s = v.toString();
    if (s.isEmpty()) {
        return QString();
    }
    const QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (dt.isValid()) {
        return dt.toUTC().toString(Qt::ISODateWithMs);
    }
    const QDateTime dt2 = QDateTime::fromString(s, Qt::ISODate);
    return dt2.isValid() ? dt2.toUTC().toString(Qt::ISODateWithMs) : QString();
}

// 宿主正文里成串空行很常见：OpenSquilla 每段之间固定四五个，长轮里能到十几行
// （实测一条 3KB 正文里有两段连着 18 个 \n）。聊天页气泡是 pre-wrap 原样渲染，
// 一串空行就是半屏多空白（用户 2026-11-20 报「OpenSquilla 会话在 RMC 里大片
// 空白」的根因）。显示前压成最多一个空行；只压空行、不动行首缩进（代码块、
// 缩进列表要保住）。
QString foldBlankLines(const QString &text) {
    QString out;
    out.reserve(text.size());
    bool pendingBlank = false;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        QString line = raw;
        while (!line.isEmpty() &&
               (line.endsWith(QLatin1Char(' ')) || line.endsWith(QLatin1Char('\t')))) {
            line.chop(1);
        }
        if (line.trimmed().isEmpty()) {
            pendingBlank = !out.isEmpty();
            continue;
        }
        if (!out.isEmpty()) {
            out += pendingBlank ? QStringLiteral("\n\n") : QStringLiteral("\n");
        }
        pendingBlank = false;
        out += line;
    }
    return out;
}

QString capText(const QString &text) {
    QString folded = foldBlankLines(text);
    if (folded.size() <= kMaxTextChars) {
        return folded;
    }
    return folded.left(kMaxTextChars) + QStringLiteral("\n…（内容过长，已截断）");
}

QString contentText(const QJsonValue &content) {
    if (content.isString()) {
        return content.toString();
    }
    if (content.isArray()) {
        QString joined;
        const QJsonArray parts = content.toArray();
        for (const QJsonValue &part : parts) {
            const QJsonObject o = part.toObject();
            const QString pt = o.value(QStringLiteral("type")).toString();
            if (pt == QStringLiteral("text") ||
                pt == QStringLiteral("input_text") ||
                pt == QStringLiteral("output_text")) {
                if (!joined.isEmpty()) {
                    joined += QLatin1Char('\n');
                }
                joined += o.value(QStringLiteral("text")).toString();
            }
        }
        return joined;
    }
    return QString();
}

QString jsonlHomeFor(const QString &agentExe) {
    if (agentExe.compare(QStringLiteral("WorkBuddy.exe"), Qt::CaseInsensitive) == 0) {
        return QDir::homePath() + QStringLiteral("/.workbuddy");
    }
    if (agentExe.compare(QStringLiteral("WorkBuddyAI.exe"), Qt::CaseInsensitive) == 0) {
        return QDir::homePath() + QStringLiteral("/.workbuddy-ai");
    }
    if (agentExe.compare(QStringLiteral("Qoder.exe"), Qt::CaseInsensitive) == 0) {
        return QDir::homePath() + QStringLiteral("/.qoder");
    }
    if (agentExe.compare(QStringLiteral("Qoder CN.exe"), Qt::CaseInsensitive) == 0) {
        return QDir::homePath() + QStringLiteral("/.qoder-cn");
    }
    return QString();
}

QString findJsonl(const QString &home, const QString &sessionId) {
    QDir projects(home + QStringLiteral("/projects"));
    const QStringList slugs = projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &slug : slugs) {
        const QString file = projects.filePath(
            slug + QLatin1Char('/') + sessionId + QStringLiteral(".jsonl"));
        if (QFileInfo::exists(file)) {
            return file;
        }
    }
    return QString();
}

// 全量解析缓存：库文件 mtime+size 变了才重读（SQLite 宿主连 -wal 一起算，见
// sqliteStamp）。翻页与 2.5s 轮询都吃缓存。
struct FullCache {
    QString stamp;
    QVector<QJsonObject> all;  // 正序
    QString title;
    QString project;  // cwd：无工作区任务靠它定位产物目录
    QString model;    // 会话当前模型（能读到就给）
    int realTotal = 0;  // 实际可渲染条数（SQLite 封顶 500 内、滤掉注入后）
};
QHash<QString, FullCache> g_cache;

QString fileStamp(const QString &path) {
    const QFileInfo fi(path);
    return QString::number(fi.lastModified().toMSecsSinceEpoch()) + QLatin1Char(':') +
           QString::number(fi.size());
}

// SQLite 宿主（ZCode / OpenSquilla）的库默认是 WAL 模式：新提交先落 <db>-wal，
// 主库文件只在 checkpoint 时才被动到——而 checkpoint 什么时候来，写入方说了算
// （本机实测过 16 分钟不 checkpoint 的会话库）。所以只按主库 mtime+size 打 stamp
// 会让缓存永远不失效：对话页停在上一份快照，点「刷新」也还是同一份缓存
// （2026-11-17 用户报「OpenSquilla 当前会话停止同步，刷新也没用」）。
// 把 -wal 一起算进 stamp，WAL 一长缓存就失效。-shm 只反映锁，变化太勤，不算。
QString sqliteStamp(const QString &dbPath) {
    QString s = fileStamp(dbPath);
    const QString wal = dbPath + QStringLiteral("-wal");
    if (QFileInfo::exists(wal)) {
        s += QLatin1Char('|') + fileStamp(wal);
    }
    return s;
}

void cachePut(const QString &key, const FullCache &entry) {
    if (g_cache.size() >= kCacheEntries && !g_cache.contains(key)) {
        g_cache.erase(g_cache.begin());
    }
    g_cache.insert(key, entry);
}

// 从全量正序数组里切「从末尾数 offset 条、取 limit 条」的一页。
SessionTranscript::Result slicePage(const FullCache &full, int limit, int offset) {
    SessionTranscript::Result out;
    out.supported = true;
    out.title = full.title;
    out.project = full.project;
    out.model = full.model;
    out.total = full.realTotal;
    const int end = qMax(0, full.all.size() - offset);
    const int start = qMax(0, end - limit);
    for (int i = start; i < end; ++i) {
        out.messages.append(full.all.at(i));
    }
    out.hasMore = start > 0;
    return out;
}

// WorkBuddy/Qoder 的 present_files 工具结果：Agent 把产物「摆到」用户面前时，
// 路径只在这条结果里（正文常常不再写一遍），于是聊天页既没有下载卡片、产物面板
// 也只会按白名单另扫一遍。抽出来挂到下一条 assistant 消息上（2026-11-19 用户
// 反馈：构建器新产出的凝蓝_OS8.hwt 没有卡片）。
QStringList presentedFilesOf(const QByteArray &line) {
    if (!line.contains("present_files")) {
        return {};
    }
    const QJsonObject obj = QJsonDocument::fromJson(line).object();
    const QJsonArray out = obj.value(QStringLiteral("output")).toArray();
    for (const QJsonValue &v : out) {
        QString text = v.toObject().value(QStringLiteral("text")).toString();
        if (text.isEmpty()) {
            text = v.toString();
        }
        if (!text.contains(QStringLiteral("\"files\""))) {
            continue;
        }
        const QJsonArray files = QJsonDocument::fromJson(text.toUtf8())
                                     .object()
                                     .value(QStringLiteral("files"))
                                     .toArray();
        QStringList list;
        for (const QJsonValue &fv : files) {
            const QString p = fv.toString().trimmed();
            if (!p.isEmpty() && !list.contains(p)) {
                list.append(p);
            }
        }
        if (!list.isEmpty()) {
            return list;
        }
    }
    return {};
}

// ---- 工具调用 → 一行「系统信息」 ----
// Agent 闷头跑工具时，聊天页只停在上一条回复上，看起来「什么都没发生」。
// 把**耗时长**的调用落成一行灰色小字（执行命令 35.3s · cd …），既有交代又不会
// 淹没对话。阈值依据本机会话实测：工具调用耗时中位数 0.28s、p90 5.7s，
// 2s 以下逐条刷出来反而更乱（2026-11-19 用户提「很长时间什么都不显示」）。
constexpr qint64 kSysCallMinMs = 2000;
// 每轮耗时（OpenSquilla 只有轮级时间戳，没有单次工具耗时）：10s 以下不值得占一行。
constexpr qint64 kSysTurnMinMs = 10000;

// 工具调用配对用的临时记录（jsonl / DSH 两个读取器共用）。
struct ToolCallInfo {
    QString name;
    QString brief;
    QString at;
    double ts = 0;
};

QString humanDuration(qint64 ms) {
    if (ms < 1000) {
        return QStringLiteral("%1ms").arg(ms);
    }
    if (ms < 60000) {
        return QStringLiteral("%1s").arg(static_cast<double>(ms) / 1000.0, 0, 'f', 1);
    }
    const qint64 s = ms / 1000;
    return QStringLiteral("%1m%2s").arg(s / 60).arg(s % 60);
}

// 工具名 → 中文动词；认不出来的原样显示（别硬造）。
// 三个宿主的命名各不相同：WorkBuddy/Qoder 是 Bash/Read/Edit，DSH 是
// pwsh/read/write/edit，OpenSquilla 是 exec_command/read_file/edit_file。
QString toolVerb(const QString &name) {
    static const QHash<QString, QString> kVerbs{
        {QStringLiteral("Bash"), QStringLiteral("执行命令")},
        {QStringLiteral("Read"), QStringLiteral("读文件")},
        {QStringLiteral("Write"), QStringLiteral("写文件")},
        {QStringLiteral("Edit"), QStringLiteral("改文件")},
        {QStringLiteral("Grep"), QStringLiteral("搜索")},
        {QStringLiteral("Glob"), QStringLiteral("找文件")},
        {QStringLiteral("ViewImage"), QStringLiteral("看图")},
        {QStringLiteral("TaskCreate"), QStringLiteral("建任务")},
        {QStringLiteral("TaskUpdate"), QStringLiteral("更新任务")},
        {QStringLiteral("TaskOutput"), QStringLiteral("等任务")},
        {QStringLiteral("AskUserQuestion"), QStringLiteral("问你")},
        {QStringLiteral("present_files"), QStringLiteral("产出文件")},
        {QStringLiteral("WebFetch"), QStringLiteral("抓网页")},
        {QStringLiteral("WebSearch"), QStringLiteral("搜网页")},
        // DSH
        {QStringLiteral("pwsh"), QStringLiteral("执行命令")},
        {QStringLiteral("read"), QStringLiteral("读文件")},
        {QStringLiteral("write"), QStringLiteral("写文件")},
        {QStringLiteral("edit"), QStringLiteral("改文件")},
        {QStringLiteral("read_image"), QStringLiteral("看图")},
        {QStringLiteral("grep"), QStringLiteral("搜索")},
        {QStringLiteral("glob"), QStringLiteral("找文件")},
        {QStringLiteral("skill"), QStringLiteral("加载技能")},
        {QStringLiteral("agent_browser"), QStringLiteral("浏览器")},
        {QStringLiteral("ask_user_question"), QStringLiteral("问你")},
        {QStringLiteral("load_workspace_dependencies"), QStringLiteral("装依赖")},
        // OpenSquilla
        {QStringLiteral("exec_command"), QStringLiteral("执行命令")},
        {QStringLiteral("read_file"), QStringLiteral("读文件")},
        {QStringLiteral("write_file"), QStringLiteral("写文件")},
        {QStringLiteral("edit_file"), QStringLiteral("改文件")},
        {QStringLiteral("grep_search"), QStringLiteral("搜索")},
        {QStringLiteral("list_dir"), QStringLiteral("列目录")},
        {QStringLiteral("execute_code"), QStringLiteral("跑代码")},
        {QStringLiteral("process"), QStringLiteral("等进程")},
        {QStringLiteral("browser"), QStringLiteral("浏览器")},
        {QStringLiteral("image"), QStringLiteral("看图")},
        {QStringLiteral("request_user_input"), QStringLiteral("问你")},
        {QStringLiteral("cron"), QStringLiteral("定时任务")},
    };
    return kVerbs.value(name, name);
}

QString clipLine(QString s, int max) {
    s = s.simplified();
    if (s.size() > max) {
        s = s.left(max) + QStringLiteral("…");
    }
    return s;
}

// 工具参数两种记法都吃：WorkBuddy 存成「JSON 字符串」，Qoder 可能直接是对象。
QJsonObject callArgs(const QJsonValue &v) {
    if (v.isObject()) {
        return v.toObject();
    }
    if (v.isString()) {
        return QJsonDocument::fromJson(v.toString().toUtf8()).object();
    }
    return QJsonObject();
}

// 参数里挑一样最能说明「在干什么」的：命令首行、文件名、搜索词…
QString toolCallBrief(const QString &name, const QJsonObject &args) {
    if (args.isEmpty()) {
        return QString();
    }
    auto str = [&args](const char *key) {
        return args.value(QLatin1String(key)).toString();
    };
    if (name == QLatin1String("Bash") || name == QLatin1String("pwsh") ||
        name == QLatin1String("exec_command")) {
        // DSH 的命令工具还带一句人写的 description，比命令本身更好读。
        const QString desc = str("description");
        if (!desc.isEmpty()) {
            return clipLine(desc, 60);
        }
        QString cmd = str("command").section(QLatin1Char('\n'), 0, 0).trimmed();
        // 「cd "工作目录" && 真命令」里的 cd 段对「在干什么」没信息量，去掉；
        // 开头是带引号的绝对路径 exe（"C:/Program Files/Python312/python.exe"）
        // 也只留程序名。
        static const QRegularExpression cdRe(QStringLiteral(
            "^cd\\s+(?:\"[^\"]*\"|'[^']*'|[^&|]+?)\\s*(?:&&|;)\\s*"));
        static const QRegularExpression exeRe(QStringLiteral(
            "^\"[^\"]*[\\\\/]([^\"/\\\\]+)\"\\s*"));
        cmd.remove(cdRe);
        cmd.replace(exeRe, QStringLiteral("\\1 "));
        return clipLine(cmd, 60);
    }
    if (name == QLatin1String("Read") || name == QLatin1String("Write") ||
        name == QLatin1String("Edit") || name == QLatin1String("read") ||
        name == QLatin1String("write") || name == QLatin1String("edit") ||
        name == QLatin1String("read_file") || name == QLatin1String("write_file") ||
        name == QLatin1String("edit_file")) {
        QString p = str("file_path");
        if (p.isEmpty()) {
            p = str("path");
        }
        return p.isEmpty() ? QString() : QFileInfo(p).fileName();
    }
    if (name == QLatin1String("ViewImage") || name == QLatin1String("read_image") ||
        name == QLatin1String("image")) {
        QString p = str("image_path");
        if (p.isEmpty()) {
            p = str("path");
        }
        return p.isEmpty() ? QString() : QFileInfo(p).fileName();
    }
    if (name == QLatin1String("Grep") || name == QLatin1String("Glob") ||
        name == QLatin1String("grep") || name == QLatin1String("glob") ||
        name == QLatin1String("grep_search")) {
        QString pat = str("pattern");
        if (pat.isEmpty()) {
            pat = str("query");
        }
        return clipLine(pat, 40);
    }
    if (name == QLatin1String("list_dir")) {
        return QFileInfo(str("path")).fileName();
    }
    if (name == QLatin1String("skill")) {
        return clipLine(str("name"), 40);
    }
    if (name == QLatin1String("present_files")) {
        const int n = args.value(QStringLiteral("files")).toArray().size();
        return n > 0 ? QStringLiteral("%1 个文件").arg(n) : QString();
    }
    if (name == QLatin1String("TaskCreate")) {
        return clipLine(str("subject"), 40);
    }
    if (name == QLatin1String("TaskUpdate")) {
        return clipLine(str("taskId"), 40);
    }
    if (name == QLatin1String("TaskOutput")) {
        return clipLine(str("task_id"), 40);
    }
    if (name == QLatin1String("AskUserQuestion") ||
        name == QLatin1String("ask_user_question") ||
        name == QLatin1String("request_user_input")) {
        const QJsonArray qs = args.value(QStringLiteral("questions")).toArray();
        if (!qs.isEmpty()) {
            return clipLine(qs.at(0).toObject()
                                .value(QStringLiteral("question"))
                                .toString(),
                            40);
        }
        QString q = str("question");
        if (q.isEmpty()) {
            q = str("prompt");
        }
        return clipLine(q, 40);
    }
    return QString();
}

QString toolCallLine(const QString &name, const QString &brief, qint64 ms) {
    QString line = toolVerb(name);
    if (ms > 0) {
        line += QLatin1Char(' ') + humanDuration(ms);
    }
    if (!brief.isEmpty()) {
        line += QStringLiteral(" · ") + brief;
    }
    return line;
}

// 一行系统信息（role=system）：聊天页渲染成灰色小字，不是对话气泡。
QJsonObject sysRow(const QString &text, const QString &at) {
    return QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                       {QStringLiteral("text"), text},
                       {QStringLiteral("at"), at}};
}

// <task-notification> 的状态/摘要 → 一行系统信息（不是用户气泡，也别丢）。
QString taskNoteLine(const QString &raw) {
    if (!raw.contains(QStringLiteral("<task-notification"))) {
        return QString();
    }
    static const QRegularExpression statusRe(
        QStringLiteral("<status>([^<]*)</status>"));
    static const QRegularExpression summaryRe(
        QStringLiteral("<summary>([\\s\\S]*?)</summary>"));
    static const QRegularExpression prefixRe(
        QStringLiteral("^Background command\\s*"));
    const QString status = statusRe.match(raw).captured(1).trimmed();
    QString head = QStringLiteral("后台任务完成");
    if (status.contains(QStringLiteral("fail"), Qt::CaseInsensitive)) {
        head = QStringLiteral("后台任务失败");
    } else if (status.contains(QStringLiteral("cancel"), Qt::CaseInsensitive)) {
        head = QStringLiteral("后台任务已取消");
    }
    QString summary = summaryRe.match(raw).captured(1);
    summary.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    summary.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    summary.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    summary.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    summary.remove(prefixRe);
    summary = clipLine(summary, 50);
    if (summary.startsWith(QLatin1Char('"'))) {
        summary = summary.mid(1);
    }
    return summary.isEmpty() ? head : head + QStringLiteral(" · ") + summary;
}

// 宿主往会话里塞的「注入消息」冒充 user 角色：Qoder 的技能注入是
// "Base directory for this skill: <path>" + 整篇 SKILL.md，不过滤的话聊天页
// 会冒一个巨大的「我」气泡，把技能文档当用户发言（2026-11-20 用户截图）。
// Claude Code 风格的命令回显、上下文续接同理。
bool isInjectedUserText(const QString &text) {
    const QString s = text.trimmed();
    static const QStringList kPrefixes = {
        QStringLiteral("Base directory for this skill"),
        QStringLiteral("<command-name>"),
        QStringLiteral("<local-command-stdout>"),
        QStringLiteral("<local-command-stderr>"),
        QStringLiteral("<user_info>"),
        QStringLiteral("Caveat: The messages below were generated"),
        QStringLiteral("This session is being continued")};
    for (const QString &prefix : kPrefixes) {
        if (s.startsWith(prefix, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

SessionTranscript::Result readJsonl(const QString &home, const QString &sessionId,
                                    int limit, int offset) {
    SessionTranscript::Result out;
    const QString path = findJsonl(home, sessionId);
    if (path.isEmpty()) {
        out.supported = true;
        out.error = QStringLiteral("会话记录不在本机库里（可能已清理）");
        return out;
    }
    const QString key = QStringLiteral("jsonl|") + path;
    const QString stamp = fileStamp(path);
    auto it = g_cache.constFind(key);
    if (it != g_cache.constEnd() && it->stamp == stamp) {
        return slicePage(*it, limit, offset);
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        out.supported = true;
        out.error = QStringLiteral("会话记录打不开");
        return out;
    }
    FullCache full;
    full.stamp = stamp;
    QStringList pendingFiles;   // present_files 摆出来的产物，等下一句 assistant
    int lastAsst = -1;
    // 工具调用 → 结果配对（算耗时），见文件头 kSysCallMinMs。
    QHash<QString, ToolCallInfo> calls;
    QStringList callOrder;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        if (obj.isEmpty()) {
            continue;
        }
        const QString sid = obj.value(QStringLiteral("sessionId")).toString();
        if (!sid.isEmpty() && sid != sessionId) {
            continue;
        }
        const QString rowType = obj.value(QStringLiteral("type")).toString();
        const QString rowAt =
            normalizeTs(obj.value(QStringLiteral("timestamp")));
        if (rowType == QLatin1String("function_call")) {
            const QString callId = obj.value(QStringLiteral("callId")).toString();
            const QString name = obj.value(QStringLiteral("name")).toString();
            if (!callId.isEmpty() && !name.isEmpty()) {
                ToolCallInfo pc;
                pc.name = name;
                pc.at = rowAt;
                pc.ts = obj.value(QStringLiteral("timestamp")).toDouble();
                pc.brief = toolCallBrief(
                    name, callArgs(obj.value(QStringLiteral("arguments"))));
                calls.insert(callId, pc);
                callOrder.append(callId);
            }
            continue;
        }
        if (rowType == QLatin1String("function_call_result")) {
            // present_files 的结果顺带把产物挂出来（正文不一定再写路径）。
            for (const QString &p : presentedFilesOf(line)) {
                if (!pendingFiles.contains(p)) {
                    pendingFiles.append(p);
                }
            }
            const QString callId = obj.value(QStringLiteral("callId")).toString();
            auto hit = calls.find(callId);
            if (hit != calls.end()) {
                const double end = obj.value(QStringLiteral("timestamp")).toDouble();
                const qint64 ms = (hit->ts > 0 && end > hit->ts)
                                      ? static_cast<qint64>(end - hit->ts)
                                      : 0;
                if (ms >= kSysCallMinMs) {
                    full.all.append(QJsonObject{
                        {QStringLiteral("role"), QStringLiteral("system")},
                        {QStringLiteral("text"),
                         toolCallLine(hit->name, hit->brief, ms)},
                        {QStringLiteral("at"), rowAt}});
                }
                calls.erase(hit);
            }
            continue;
        }
        if (rowType == QLatin1String("message")) {
            // 后台任务完成通知（宿主写成 user 角色的 XML）→ 一行系统信息。
            const QJsonObject msg = obj.value(QStringLiteral("message")).toObject();
            QJsonValue content = msg.value(QStringLiteral("content"));
            if (content.isUndefined() || content.isNull()) {
                content = obj.value(QStringLiteral("content"));
            }
            const QString note = taskNoteLine(contentText(content));
            if (!note.isEmpty()) {
                full.all.append(QJsonObject{
                    {QStringLiteral("role"), QStringLiteral("system")},
                    {QStringLiteral("text"), note},
                    {QStringLiteral("at"), rowAt}});
                continue;
            }
        }
        QString role, text, at, id;
        if (!SessionTranscript::messageOf(line, &role, &text, &at, &id)) {
            continue;
        }
        if (role == QLatin1String("user") && isInjectedUserText(text)) {
            continue;  // 技能注入 / 命令回显：不是用户真的说的话
        }
        // 当前模型记在 assistant 行的 providerData 里（WorkBuddy/Qoder 都有，
        // 显示名在 requestModelName）。取最后一条有值的就是"现在在用的"。
        // WorkBuddy 的 jsonl 没有标题字段：不补的话 chatStateFor 只能兜底
        // 用待办标题（=Agent 名），待办会话页顶栏就看不到会话名
        // （2026-11-19 用户截图：大标题是「WorkBuddy」）。DSH 分支同款：
        // 取首条非空 user 消息。
        if (full.project.isEmpty()) {
            const QString cwd = obj.value(QStringLiteral("cwd")).toString();
            if (!cwd.isEmpty()) {
                full.project = cwd;
            }
        }
        if (full.title.isEmpty() && role == QLatin1String("user") &&
            !text.trimmed().isEmpty()) {
            full.title = text.trimmed().left(30);  // 顶栏展示宽度有限
        }
        if (role == QLatin1String("assistant")) {
            const QJsonObject pd =
                obj.value(QStringLiteral("providerData")).toObject();
            QString m = pd.value(QStringLiteral("requestModelName")).toString();
            if (m.isEmpty()) {
                m = pd.value(QStringLiteral("model")).toString();
            }
            if (m.isEmpty()) {
                m = obj.value(QStringLiteral("message"))
                        .toObject()
                        .value(QStringLiteral("model"))
                        .toString();
            }
            if (!m.isEmpty()) {
                full.model = m;
            }
        }
        QJsonObject row{
            {QStringLiteral("id"), id},
            {QStringLiteral("role"), role},
            {QStringLiteral("text"), capText(text)},
            {QStringLiteral("at"), at},
        };
        if (role == QLatin1String("assistant") && !pendingFiles.isEmpty()) {
            row.insert(QStringLiteral("files"),
                       QJsonArray::fromStringList(pendingFiles));
            pendingFiles.clear();
        }
        full.all.append(row);
        if (role == QLatin1String("assistant")) {
            lastAsst = full.all.size() - 1;
        }
    }
    // 会话最后一条就是 present_files（后面没有正文了）：挂到最近一条 assistant 上，
    // 别让产物在页面上凭空消失。
    if (!pendingFiles.isEmpty() && lastAsst >= 0) {
        QJsonObject row = full.all.at(lastAsst);
        QJsonArray files = row.value(QStringLiteral("files")).toArray();
        for (const QString &p : pendingFiles) {
            files.append(p);
        }
        row.insert(QStringLiteral("files"), files);
        full.all.replace(lastAsst, row);
    }
    // 还在跑的调用（会话最后一条就是它，没有结果行）：也要露一行，否则页面
    // 停在上一条回复上，看起来像卡住。
    // 但**只认「晚于最后一条已渲染消息」的调用**：历史残留（分页截断导致
    // result 在另一页、或格式没配对上）以前会永远以「进行中」挂在会话末尾
    // ——用户 2026-11-20 报「最下面一直有两个进行中，明明早完成了」。
    qint64 lastMsgMs = 0;
    for (const QJsonObject &row : full.all) {
        // 只跟真实的对话消息比：system 行（工具摘要/备注）不算「最后一条消息」，
        // 否则刚跑完的那条调用会被自己旁边那行摘要误判成历史残留。
        if (row.value(QStringLiteral("role")).toString() ==
            QLatin1String("system")) {
            continue;
        }
        const QDateTime dt =
            QDateTime::fromString(row.value(QStringLiteral("at")).toString(),
                                  Qt::ISODate);
        if (dt.isValid()) {
            lastMsgMs = qMax(lastMsgMs, dt.toMSecsSinceEpoch());
        }
    }
    for (const QString &callId : callOrder) {
        const auto hit = calls.constFind(callId);
        if (hit == calls.constEnd()) {
            continue;
        }
        const QDateTime callAt =
            QDateTime::fromString(hit->at, Qt::ISODate);
        if (lastMsgMs > 0 && callAt.isValid() &&
            callAt.toMSecsSinceEpoch() < lastMsgMs) {
            continue;  // 早于最后一条消息：已完成的历史调用，不再挂「进行中」
        }
        QString text = QStringLiteral("进行中 · ") + toolVerb(hit->name);
        if (!hit->brief.isEmpty()) {
            text += QStringLiteral(" · ") + hit->brief;
        }
        full.all.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("text"), text},
            {QStringLiteral("at"), hit->at}});
    }
    full.realTotal = full.all.size();
    cachePut(key, full);
    return slicePage(full, limit, offset);
}

// SQLite 两表（ZCode message+part / OpenSquilla transcript_entries）共用连接管理。
class ReadOnlyDb {
public:
    ReadOnlyDb(const QString &path) : conn_(QStringLiteral("armc_transcript_") +
                                            QUuid::createUuid().toString(QUuid::Id128)) {
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(path);
        db_.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        ok_ = db_.open();
    }
    ~ReadOnlyDb() {
        if (ok_) {
            db_.close();
        }
        QSqlDatabase::removeDatabase(conn_);
    }
    bool ok() const { return ok_; }
    QSqlDatabase &db() { return db_; }

private:
    QString conn_;
    QSqlDatabase db_;
    bool ok_ = false;
};

// ZCode 把环境快照/模型说明/agent 与 skills 清单/AGENTS 与 gitStatus 上下文/
// token 余量/各种 nudge（含 task-notification）全记成独立 user message，但带
// 结构化标记（synthetic / visibility=model-only / metadata.runtimeMessage），
// 真用户输入没有这些标记（本机全库 1939 条 user 行交叉验证 0 漏判）。不过滤
// 的话聊天页全是「我」的注入气泡（用户 2026-11-15 反馈）。
bool isZcodeInjectedUser(const QJsonObject &data) {
    if (data.value(QStringLiteral("synthetic")).toBool(false)) {
        return true;
    }
    if (data.value(QStringLiteral("visibility")).toString() ==
        QStringLiteral("model-only")) {
        return true;
    }
    const QJsonObject meta = data.value(QStringLiteral("metadata")).toObject();
    if (meta.contains(QStringLiteral("runtimeMessage")) ||
        meta.value(QStringLiteral("visibility")).toString() ==
            QStringLiteral("model-only")) {
        return true;
    }
    return false;
}

SessionTranscript::Result readZcode(const QString &root, const QString &sessionId,
                                    int limit, int offset) {
    SessionTranscript::Result out;
    const QString dbPath = root + QStringLiteral("/cli/db/db.sqlite");
    if (!QFileInfo::exists(dbPath)) {
        out.supported = true;
        out.error = QStringLiteral("ZCode 会话库不在本机");
        return out;
    }
    const QString key = QStringLiteral("zcode|") + dbPath + QLatin1Char('|') + sessionId;
    const QString stamp = sqliteStamp(dbPath);
    auto it = g_cache.constFind(key);
    if (it != g_cache.constEnd() && it->stamp == stamp) {
        return slicePage(*it, limit, offset);
    }
    ReadOnlyDb db(dbPath);
    if (!db.ok()) {
        out.supported = true;
        out.error = QStringLiteral("ZCode 会话库打不开");
        return out;
    }
    FullCache full;
    full.stamp = stamp;
    QSqlQuery titleQ(db.db());
    titleQ.prepare(QStringLiteral("SELECT title FROM session WHERE id=?"));
    titleQ.addBindValue(sessionId);
    if (titleQ.exec() && titleQ.next()) {
        full.title = titleQ.value(0).toString();
    }
    titleQ.finish();
    // 当前模型在 message.data.model（{providerID, modelID, variant}）里，取最新
    // 一条带 model 的即可——ZCode 每条 assistant 都会记。
    QSqlQuery modelQ(db.db());
    modelQ.prepare(QStringLiteral(
        "SELECT data FROM message WHERE session_id=? AND data LIKE '%\"model\"%' "
        "ORDER BY sequence DESC LIMIT 1"));
    modelQ.addBindValue(sessionId);
    if (modelQ.exec() && modelQ.next()) {
        const QJsonValue mv = QJsonDocument::fromJson(modelQ.value(0).toByteArray())
                                  .object()
                                  .value(QStringLiteral("model"));
        if (mv.isObject()) {
            full.model = mv.toObject().value(QStringLiteral("modelID")).toString();
        } else if (mv.isString()) {
            full.model = mv.toString();
        }
    }
    modelQ.finish();
    QSqlQuery countQ(db.db());
    countQ.prepare(QStringLiteral("SELECT COUNT(*) FROM message WHERE session_id=?"));
    countQ.addBindValue(sessionId);
    int total = 0;
    if (countQ.exec() && countQ.next()) {
        total = countQ.value(0).toInt();
    }
    // 只缓存最近 kSqliteCap 条（含纯工具行），正文在 part 表按 sequence 拼。
    const int fetch = qMin(total, kSqliteCap);
    QSqlQuery mq(db.db());
    mq.prepare(QStringLiteral(
        "SELECT id, data, time_created FROM message "
        "WHERE session_id=? ORDER BY sequence DESC LIMIT ?"));
    mq.addBindValue(sessionId);
    mq.addBindValue(fetch);
    struct Msg {
        QString id;
        QString role;
        QString at;
        bool injected = false;  // harness 注入的 user 行（不进聊天页）
    };
    QVector<Msg> msgs;
    if (mq.exec()) {
        while (mq.next()) {
            const QJsonObject data =
                QJsonDocument::fromJson(mq.value(1).toByteArray()).object();
            Msg m;
            m.id = mq.value(0).toString();
            m.role = data.value(QStringLiteral("role")).toString();
            m.at = isoFromMs(mq.value(2).toLongLong());
            m.injected = isZcodeInjectedUser(data);
            msgs.append(m);
        }
    }
    std::reverse(msgs.begin(), msgs.end());
    QHash<QString, QString> textByMsg;
    if (!msgs.isEmpty()) {
        QString sql = QStringLiteral(
            "SELECT message_id, data FROM part "
            "WHERE session_id=? AND message_id IN (");
        for (int i = 0; i < msgs.size(); ++i) {
            sql += (i ? QStringLiteral(",?") : QStringLiteral("?"));
        }
        sql += QStringLiteral(") ORDER BY sequence");
        QSqlQuery pq(db.db());
        pq.prepare(sql);
        pq.addBindValue(sessionId);
        for (const Msg &m : msgs) {
            pq.addBindValue(m.id);
        }
        if (pq.exec()) {
            while (pq.next()) {
                const QJsonObject data =
                    QJsonDocument::fromJson(pq.value(1).toByteArray()).object();
                if (data.value(QStringLiteral("type")).toString() !=
                    QStringLiteral("text")) {
                    continue;
                }
                const QString mid = pq.value(0).toString();
                QString &acc = textByMsg[mid];
                if (!acc.isEmpty()) {
                    acc += QLatin1Char('\n');
                }
                acc += data.value(QStringLiteral("text")).toString();
            }
        }
    }
    for (const Msg &m : msgs) {
        if (m.role != QStringLiteral("user") &&
            m.role != QStringLiteral("assistant")) {
            continue;
        }
        // 系统注入的 user 行不进聊天页（见 isZcodeInjectedUser）。
        if (m.role == QStringLiteral("user") && m.injected) {
            continue;
        }
        const QString text = textByMsg.value(m.id).trimmed();
        if (text.isEmpty()) {
            continue;
        }
        full.all.append(QJsonObject{
            {QStringLiteral("id"), m.id},
            {QStringLiteral("role"), m.role},
            {QStringLiteral("text"), capText(text)},
            {QStringLiteral("at"), m.at},
        });
    }
    // total 口径与 jsonl/DSH 对齐：滤掉注入后实际可渲染条数（fetch 已封顶）。
    full.realTotal = full.all.size();
    cachePut(key, full);
    return slicePage(full, limit, offset);
}

// 下面三个 helper 定义在会话目录段，但 readOsq（聊天页标题）也要用，先声明。
QString shortTitleOf(const QString &text, const QString &sessionId);
bool isGenericOsqTitle(const QString &text);
QString normalizeOsqUserText(const QString &raw);

// 正在跑的那一轮（agent_tasks.finished_at 为空）：现查现算，**不进全量缓存**。
// 长轮十几分钟才落一条 transcript，页面上就靠这个「本轮已 N 分」证明还活着；
// 缓存住的话秒数会停在打开页面那一刻（用户 2026-11-20 报的卡住不动）。
SessionTranscript::Result attachRunningOsq(SessionTranscript::Result out,
                                           const QString &stateRoot,
                                           const QString &sessionKey) {
    ReadOnlyDb db(stateRoot + QStringLiteral("/sessions.db"));
    if (!db.ok()) {
        return out;
    }
    QSqlQuery q(db.db());
    q.prepare(QStringLiteral(
        "SELECT started_at FROM agent_tasks WHERE session_key=? "
        "AND finished_at IS NULL ORDER BY started_at DESC LIMIT 1"));
    q.addBindValue(sessionKey);
    if (q.exec() && q.next()) {
        const qint64 started = q.value(0).toLongLong();
        if (started > 0) {
            out.runningSinceMs = started;
            out.runningText =
                QStringLiteral("本轮已 ") +
                humanDuration(QDateTime::currentMSecsSinceEpoch() - started);
        }
    }
    q.finish();
    return out;
}

SessionTranscript::Result readOsq(const QString &stateRoot, const QString &sessionKey,
                                  int limit, int offset) {
    SessionTranscript::Result out;
    const QString dbPath = stateRoot + QStringLiteral("/sessions.db");
    if (!QFileInfo::exists(dbPath)) {
        out.supported = true;
        out.error = QStringLiteral("OpenSquilla 会话库不在本机");
        return out;
    }
    const QString key = QStringLiteral("osq|") + dbPath + QLatin1Char('|') + sessionKey;
    const QString stamp = sqliteStamp(dbPath);
    auto it = g_cache.constFind(key);
    if (it != g_cache.constEnd() && it->stamp == stamp) {
        return attachRunningOsq(slicePage(*it, limit, offset), stateRoot, sessionKey);
    }
    ReadOnlyDb db(dbPath);
    if (!db.ok()) {
        out.supported = true;
        out.error = QStringLiteral("OpenSquilla 会话库打不开");
        return out;
    }
    FullCache full;
    full.stamp = stamp;
    // 会话标题：聊天页头部原来一直是通道占位名（「WebChat」），和派发页同源
    // 一起修（2026-11-19）。取不到就当没有，退首条用户正文。
    QSqlQuery titleQ(db.db());
    titleQ.prepare(QStringLiteral(
        "SELECT display_name, derived_title, "
        "COALESCE(NULLIF(subject,''),NULLIF(label,''),'') "
        "FROM sessions WHERE session_key=?"));
    titleQ.addBindValue(sessionKey);
    if (titleQ.exec() && titleQ.next()) {
        for (int col = 0; col < 3; ++col) {
            const QString cand = titleQ.value(col).toString();
            if (!cand.isEmpty() && !isGenericOsqTitle(cand)) {
                full.title = shortTitleOf(cand, sessionKey.section(QLatin1Char(':'), -1));
                break;
            }
        }
    }
    titleQ.finish();
    // 会话当前模型（胶囊「会话当前 · xxx」用）：model_override 是桥/页面切过的值，
    // 优先读它；没切过就读网关记账的 model。
    QSqlQuery mq(db.db());
    mq.prepare(QStringLiteral(
        "SELECT COALESCE(NULLIF(model_override,''),NULLIF(model,'')) "
        "FROM sessions WHERE session_key=?"));
    mq.addBindValue(sessionKey);
    if (mq.exec() && mq.next()) {
        full.model = mq.value(0).toString();
    }
    mq.finish();
    if (full.title.isEmpty()) {
        QSqlQuery fu(db.db());
        fu.prepare(QStringLiteral(
            "SELECT content FROM transcript_entries "
            "WHERE session_key=? AND role='user' ORDER BY id LIMIT 1"));
        fu.addBindValue(sessionKey);
        if (fu.exec() && fu.next()) {
            const QString t = normalizeOsqUserText(fu.value(0).toString());
            if (!t.isEmpty()) {
                full.title = shortTitleOf(t, sessionKey.section(QLatin1Char(':'), -1));
            }
        }
        fu.finish();
    }
    QSqlQuery countQ(db.db());
    countQ.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM transcript_entries WHERE session_key=?"));
    countQ.addBindValue(sessionKey);
    int total = 0;
    if (countQ.exec() && countQ.next()) {
        total = countQ.value(0).toInt();
    }
    QSqlQuery q(db.db());
    q.prepare(QStringLiteral(
        "SELECT id, role, content, created_at FROM transcript_entries "
        "WHERE session_key=? ORDER BY id DESC LIMIT ?"));
    q.addBindValue(sessionKey);
    q.addBindValue(qMin(total, kSqliteCap));
    QVector<QJsonObject> rev;
    QVector<qint64> revMs;   // 与 rev 对齐：合并「本轮」行时按时间插
    if (q.exec()) {
        while (q.next()) {
            const QString role = q.value(1).toString();
            if (role != QStringLiteral("user") &&
                role != QStringLiteral("assistant")) {
                continue;
            }
            const QString text = q.value(2).toString().trimmed();
            if (text.isEmpty()) {
                continue;
            }
            rev.append(QJsonObject{
                {QStringLiteral("id"), q.value(0).toString()},
                {QStringLiteral("role"), role},
                {QStringLiteral("text"), capText(text)},
                {QStringLiteral("at"), normalizeTs(QJsonValue::fromVariant(q.value(3)))},
            });
            revMs.append(q.value(3).toLongLong());
        }
    }
    // 每轮耗时（agent_tasks）：OpenSquilla 的 tool_calls 不带耗时，但每一轮的
    // 开始/结束都有记录——闷头跑几分钟时，页面至少能看到「本轮 3m25s · 完成」。
    struct TurnRow {
        qint64 at = 0;
        QString text;
    };
    QVector<TurnRow> turns;
    QSqlQuery tq(db.db());
    tq.prepare(QStringLiteral(
        "SELECT started_at, finished_at, status, COALESCE(error_message,'') "
        "FROM agent_tasks WHERE session_key=? AND finished_at IS NOT NULL "
        "ORDER BY finished_at"));
    tq.addBindValue(sessionKey);
    if (tq.exec()) {
        while (tq.next()) {
            const qint64 started = tq.value(0).toLongLong();
            const qint64 finished = tq.value(1).toLongLong();
            if (started <= 0 || finished <= started ||
                finished - started < kSysTurnMinMs) {
                continue;
            }
            const QString status = tq.value(2).toString();
            QString text = QStringLiteral("本轮 ") + humanDuration(finished - started);
            if (status == QStringLiteral("succeeded")) {
                text += QStringLiteral(" · 完成");
            } else if (status == QStringLiteral("failed")) {
                text += QStringLiteral(" · 失败");
            } else if (status == QStringLiteral("cancelled")) {
                text += QStringLiteral(" · 已取消");
            } else if (status == QStringLiteral("abandoned")) {
                text += QStringLiteral(" · 中断");
            }
            const QString err = clipLine(tq.value(3).toString(), 40);
            if (!err.isEmpty()) {
                text += QStringLiteral("：") + err;
            }
            turns.append({finished, text});
        }
    }
    tq.finish();
    int ti = 0;
    for (int i = rev.size() - 1; i >= 0; --i) {
        while (ti < turns.size() && turns.at(ti).at <= revMs.at(i)) {
            full.all.append(sysRow(turns.at(ti).text, isoFromMs(turns.at(ti).at)));
            ++ti;
        }
        full.all.append(rev.at(i));
    }
    while (ti < turns.size()) {
        full.all.append(sysRow(turns.at(ti).text, isoFromMs(turns.at(ti).at)));
        ++ti;
    }
    // 正在跑的那一轮**不往 messages 里塞**：那股「已 N 分」是随时间变的，
    // 塞进全量缓存就会永远停在打开页面那一刻（用户 2026-11-20 报「进行中 ·
    // 本轮已 13m41s 卡住不动」）。改成每次 read 现查（见 attachRunningOsq），
    // 由聊天页底部的实时行渲染。
    full.realTotal = qMin(total, kSqliteCap);
    cachePut(key, full);
    return attachRunningOsq(slicePage(full, limit, offset), stateRoot, sessionKey);
}

QString osqStateRoot() {
    const QString appdata = QString::fromLocal8Bit(qgetenv("APPDATA"));
    if (appdata.isEmpty()) {
        return QString();
    }
    QDir base(appdata + QStringLiteral("/@opensquilla"));
    const QStringList profiles = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &profile : profiles) {
        const QString state = base.filePath(
            profile + QStringLiteral("/opensquilla/state"));
        if (QFileInfo::exists(state + QStringLiteral("/sessions.db"))) {
            return state;
        }
    }
    return QString();
}

// ---------- 会话目录：手机派发页选会话用，只读元数据 ----------

struct SessionMeta {
    QString stamp;
    QString title;
    QString project;
};
QHash<QString, SessionMeta> g_sessionMeta;

// 项目是否已经不适合算“当前项目”：只认真正不可用的——路径已不存在。
// 宿主自动生成的临时工作区（见 generatedWorkspace）只决定「归到哪个组」，
// 不自动算归档：目录还在就说明会话仍可用，不该显示成可删的已归档项
// （用户 2026-11-16 定：Documents\Qoder 里的真实工作被误判成已归档）。
bool archivedProject(const QString &project) {
    if (project.trimmed().isEmpty()) {
        return false;  // 未分类另算
    }
    return !QFileInfo::exists(project);
}

// OpenSquilla 的 display_name 常是宿主写死的通道通名：webchat 会话一律叫
// 「WebChat」，派发页整屏同名卡根本分不出哪条是哪条（用户 2026-11-19 报）。
// 通名不算标题——放行 derived_title / 首条用户正文。
bool isGenericOsqTitle(const QString &text) {
    static const QSet<QString> kGeneric = {
        QStringLiteral("webchat"), QStringLiteral("clisession"),
        QStringLiteral("chat"), QStringLiteral("session"),
        QStringLiteral("conversation")};
    QString s = text.simplified().toLower();
    s.remove(QLatin1Char(' ')).remove(QLatin1Char('-')).remove(QLatin1Char('_'));
    return kGeneric.contains(s);
}

// 首条用户正文当标题用前的归一：正文可能是 {"text":"…"} 的 JSON，也可能带
// 「[2026-11-19T14:20+08:00 Thu Asia/Shanghai]」这种运行时时间头（实测都有）。
QString normalizeOsqUserText(const QString &raw) {
    QString s = raw.trimmed();
    if (s.startsWith(QLatin1Char('{'))) {
        const QString t =
            QJsonDocument::fromJson(raw.toUtf8()).object().value(QStringLiteral("text")).toString();
        if (!t.trimmed().isEmpty()) {
            s = t;
        }
    }
    static const QRegularExpression runtimeHeader(
        QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2}[^\\]]*\\]\\s*"));
    s.remove(runtimeHeader);
    return s.simplified();
}

QString shortTitleOf(const QString &text, const QString &sessionId) {
    QString t = text.simplified();
    if (t.size() > 60) {
        t = t.left(60) + QStringLiteral("…");
    }
    if (t.isEmpty()) {
        t = QStringLiteral("会话 ") + sessionId.left(8);
    }
    return t;
}

// jsonl 的题名可能在前面几行（ai-title / 首条 user），项目在 cwd。
// 只读首屏，按 mtime+size 缓存；这是目录页 2.5s 轮询不卡的关键。
SessionMeta jsonlMeta(const QString &path) {
    const QString stamp = fileStamp(path);
    auto hit = g_sessionMeta.constFind(path);
    if (hit != g_sessionMeta.constEnd() && hit->stamp == stamp) {
        return *hit;
    }
    SessionMeta meta;
    meta.stamp = stamp;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return meta;
    }
    qint64 bytes = 0;
    int lines = 0;
    QString firstUser;
    while (!f.atEnd() && lines < 500 && bytes < 256 * 1024) {
        const QByteArray line = f.readLine();
        bytes += line.size();
        ++lines;
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        if (obj.isEmpty()) {
            continue;
        }
        if (meta.project.isEmpty()) {
            meta.project = obj.value(QStringLiteral("cwd")).toString();
            if (meta.project.isEmpty()) {
                const QJsonArray dirs =
                    obj.value(QStringLiteral("directories")).toArray();
                if (!dirs.isEmpty()) {
                    meta.project = dirs.at(0).toString();
                }
            }
        }
        if (meta.title.isEmpty()) {
            const QString aiTitle =
                obj.value(QStringLiteral("aiTitle")).toString();
            if (!aiTitle.isEmpty()) {
                meta.title = aiTitle;
            }
        }
        if (firstUser.isEmpty()) {
            const QString type = obj.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("user") ||
                type == QStringLiteral("message")) {
                QString role, text, at;
                if (SessionTranscript::messageOf(line, &role, &text, &at) &&
                    role == QStringLiteral("user") && !text.trimmed().isEmpty()) {
                    firstUser = text;
                }
            }
        }
        // ai-title 一般在前几行；Qoder 没有 ai-title，拿到首条 user 就够了。
        if (!meta.title.isEmpty() && !meta.project.isEmpty()) {
            break;
        }
    }
    if (meta.title.isEmpty()) {
        meta.title = firstUser;
    }
    if (g_sessionMeta.size() >= 256) {
        g_sessionMeta.clear();
    }
    g_sessionMeta.insert(path, meta);
    return meta;
}

// jsonl 只是正文；哪些会话还算活着，只有宿主自己的库知道。WorkBuddy / WorkBuddy AI
// 把状态写在 <home>/workbuddy.db 的 sessions 表里：deleted_at 非空＝宿主侧删过，
// status='archived'＝宿主侧归档。jsonl 不跟着删（实测早删掉的会话文件还躺在磁盘上），
// 所以只按「项目目录是否存在」判归档会把一堆早就结束的测试会话算成活跃
// （用户 2026-11-17 拿客户端分组截图对账：客户端只剩 2 条，派发页列了 8 条）。
QSet<QString> workbuddyArchivedIds(const QString &home) {
    QSet<QString> out;
    const QString dbPath = home + QStringLiteral("/workbuddy.db");
    if (!QFileInfo::exists(dbPath)) {
        return out;  // Qoder 等其它 jsonl 宿主没有这张表
    }
    ReadOnlyDb db(dbPath);
    if (!db.ok()) {
        return out;
    }
    QSqlQuery q(db.db());
    // 老库/测试库可能缺这两列：查询失败就当没有归档信息，退回目录判断。
    if (q.exec(QStringLiteral("SELECT id FROM sessions "
                              "WHERE deleted_at IS NOT NULL "
                              "OR LOWER(status) = 'archived'"))) {
        while (q.next()) {
            out.insert(q.value(0).toString());
        }
    }
    q.finish();
    return out;
}

QVector<SessionTranscript::SessionInfo> listJsonlSessions(const QString &home,
                                                          int maxSessions) {
    QVector<SessionTranscript::SessionInfo> out;
    QVector<QFileInfo> files;
    QDirIterator it(home + QStringLiteral("/projects"),
                    QStringList{QStringLiteral("*.jsonl")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (fi.size() > 2) {
            files.append(fi);
        }
    }
    std::sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return a.lastModified() > b.lastModified();
    });
    const int scanCap = qMax(60, maxSessions * 6);
    // 宿主侧已删除/已归档的会话（只有 WorkBuddy 系有这张表，其余宿主是空集）。
    const QSet<QString> hostArchived = workbuddyArchivedIds(home);
    for (const QFileInfo &fi : files) {
        if (out.size() >= maxSessions || out.size() >= scanCap) {
            break;
        }
        const QString id = fi.completeBaseName();
        if (id.isEmpty()) {
            continue;
        }
        // 子代理会话（agent-*.jsonl）不是用户会话：宿主客户端不列它，派发也没有意义。
        // 它不在宿主 sessions 表里，所以上面的归档标记也查不到，只能靠目录判断——
        // 混在活跃组里就是一条「27 天前」的陌生会话（用户 2026-11-17 报的正是这个）。
        if (id.startsWith(QStringLiteral("agent-"))) {
            continue;
        }
        const SessionMeta meta = jsonlMeta(fi.absoluteFilePath());
        SessionTranscript::SessionInfo info;
        info.id = id;
        info.title = shortTitleOf(meta.title, id);
        info.project = meta.project;
        info.updatedAt =
            fi.lastModified().toUTC().toString(Qt::ISODateWithMs);
        info.archived = archivedProject(info.project) || hostArchived.contains(id);
        out.append(info);
    }
    return out;
}

// ZCode 桌面端把会话当「任务」管，归档/删除记在自己的任务索引里：
// <home>/v2/tasks-index.sqlite 的 tasks 表（archived / deleted 列）。cli 库
// session.time_archived 只是建了列，当前版本从不写（实测 243 条全 0）——只看它，
// 桌面端归档掉的上百条会话永远算活跃，继续混在「正在进行的项目」的组里
// （用户 2026-11-18 报的正是这个）。task_id 与 cli session.id 同一个命名空间，
// 索引里对不上的行（纯桌面任务）不在 cli 列表里，自然不会误标。
QSet<QString> zcodeDesktopArchivedIds(const QString &root) {
    QSet<QString> out;
    const QString dbPath = root + QStringLiteral("/v2/tasks-index.sqlite");
    if (!QFileInfo::exists(dbPath)) {
        return out;  // 没装桌面端 / 还没有任务索引
    }
    ReadOnlyDb db(dbPath);
    if (!db.ok()) {
        return out;
    }
    QSqlQuery q(db.db());
    // 更老的索引可能没有 tasks 表或这两列：查询失败就当没有归档信息，退回目录判断。
    if (q.exec(QStringLiteral(
            "SELECT task_id FROM tasks WHERE archived=1 OR deleted=1"))) {
        while (q.next()) {
            out.insert(q.value(0).toString());
        }
    }
    q.finish();
    return out;
}

QVector<SessionTranscript::SessionInfo> listZcodeSessions(const QString &root,
                                                           int maxSessions) {
    QVector<SessionTranscript::SessionInfo> out;
    const QString dbPath = root + QStringLiteral("/cli/db/db.sqlite");
    if (!QFileInfo::exists(dbPath)) {
        return out;
    }
    ReadOnlyDb db(dbPath);
    if (!db.ok()) {
        return out;
    }
    // 旧版/测试库不一定有 directory/time_updated/归档列：按实际列拼查询，
    // 缺列就当没这项元数据，不能因为目录页把整个宿主漏掉。
    QSet<QString> cols;
    QSqlQuery pragma(db.db());
    if (pragma.exec(QStringLiteral("PRAGMA table_info(session)"))) {
        while (pragma.next()) {
            cols.insert(pragma.value(1).toString());
        }
    }
    pragma.finish();
    const bool hasDir = cols.contains(QStringLiteral("directory"));
    const bool hasUpdated = cols.contains(QStringLiteral("time_updated"));
    QString sql = QStringLiteral("SELECT id,title");
    sql += hasDir ? QStringLiteral(",directory") : QStringLiteral(",''");
    sql += hasUpdated ? QStringLiteral(",time_updated")
                      : QStringLiteral(",0");
    sql += QStringLiteral(" FROM session WHERE id NOT LIKE 'sess_subagent_%'");
    if (cols.contains(QStringLiteral("time_deleted"))) {
        sql += QStringLiteral(" AND COALESCE(time_deleted,0)=0");
    }
    if (cols.contains(QStringLiteral("time_archived"))) {
        sql += QStringLiteral(" AND COALESCE(time_archived,0)=0");
    }
    if (hasUpdated) {
        sql += QStringLiteral(" ORDER BY time_updated DESC");
    }
    sql += QStringLiteral(" LIMIT ") + QString::number(maxSessions);
    QSqlQuery q(db.db());
    // 桌面端的归档/删除标记在 v2 任务索引里（见 zcodeDesktopArchivedIds）。
    const QSet<QString> desktopArchived = zcodeDesktopArchivedIds(root);
    // 归档/删除/子 Agent 的内部会话不进“派发”列表，避免用户选到不可用的树。
    if (q.exec(sql)) {
        while (q.next()) {
            const QString id = q.value(0).toString();
            SessionTranscript::SessionInfo info;
            info.id = id;
            info.title = shortTitleOf(q.value(1).toString(), id);
            info.project = q.value(2).toString();
            const qint64 updated = q.value(3).toLongLong();
            if (updated > 0) {
                info.updatedAt = isoFromMs(updated);
            }
            info.archived = archivedProject(info.project) ||
                            desktopArchived.contains(id);
            out.append(info);
        }
    }
    q.finish();
    return out;
}

QVector<SessionTranscript::SessionInfo> listOsqSessions(const QString &stateRoot,
                                                         int maxSessions) {
    QVector<SessionTranscript::SessionInfo> out;
    const QString dbPath = stateRoot + QStringLiteral("/sessions.db");
    if (!QFileInfo::exists(dbPath)) {
        return out;
    }
    ReadOnlyDb db(dbPath);
    if (!db.ok()) {
        return out;
    }
    QSqlQuery q(db.db());
    // 项目以宿主自己的工作区表为准（execution_workspace 只是任务沙箱目录，
    // 拿来分组会把每个任务当项目）。removed_at 是 OpenSquilla 的归档标记。
    if (q.exec(QStringLiteral(
            "SELECT s.session_key, s.display_name, s.derived_title, "
            "COALESCE(NULLIF(s.subject,''),NULLIF(s.label,''),''), "
            "COALESCE(p.path,''), COALESCE(p.removed_at,0), s.updated_at "
            "FROM sessions s LEFT JOIN project_workspaces p "
            "ON p.workspace_id=s.workspace_id "
            "ORDER BY s.updated_at DESC LIMIT ") +
               QString::number(maxSessions))) {
        // 没解出标题的行先记下，整批退查首条用户正文——200 条相关子查询
        // 在没索引的表上是逐条全扫，一次 IN 批量查只扫一遍。
        QVector<int> fallbackIdx;
        QStringList fallbackKeys;
        while (q.next()) {
            const QString key = q.value(0).toString();
            SessionTranscript::SessionInfo info;
            info.id = key;
            QString title;
            for (int col = 1; col <= 3; ++col) {
                const QString cand = q.value(col).toString();
                // 「WebChat」这类通道通名不算标题（2026-11-19 派发页整屏同名）。
                if (!cand.isEmpty() && !isGenericOsqTitle(cand)) {
                    title = cand;
                    break;
                }
            }
            info.title = title;
            if (title.isEmpty()) {
                fallbackIdx.append(out.size());
                fallbackKeys.append(key);
            }
            info.project = q.value(4).toString();
            info.updatedAt = isoFromMs(q.value(6).toLongLong());
            info.archived =
                q.value(5).toLongLong() > 0 || archivedProject(info.project);
            out.append(info);
        }
        if (!fallbackKeys.isEmpty()) {
            QStringList marks;
            for (int i = 0; i < fallbackKeys.size(); ++i) {
                marks << QStringLiteral("?");
            }
            QSqlQuery f(db.db());
            f.prepare(QStringLiteral(
                "SELECT session_key, content FROM transcript_entries "
                "WHERE role='user' AND session_key IN (") +
                marks.join(QLatin1Char(',')) + QStringLiteral(") ORDER BY id"));
            for (const QString &k : fallbackKeys) {
                f.addBindValue(k);
            }
            QHash<QString, QString> firstUser;
            if (f.exec()) {
                while (f.next()) {
                    const QString k = f.value(0).toString();
                    const QString c = f.value(1).toString();
                    // 按 id 升序，第一条非空就是它。
                    if (!c.trimmed().isEmpty() && !firstUser.contains(k)) {
                        firstUser.insert(k, c);
                    }
                }
            }
            f.finish();
            for (int i = 0; i < fallbackIdx.size(); ++i) {
                QString t = normalizeOsqUserText(firstUser.value(fallbackKeys.at(i)));
                if (t.isEmpty()) {
                    // agent:main:webchat:ct2q066i → 会话 ct2q066i（别把 agent:ma 当标题）。
                    t = QStringLiteral("会话 ") +
                        fallbackKeys.at(i).section(QLatin1Char(':'), -1);
                }
                out[fallbackIdx.at(i)].title = t;
            }
        }
        for (SessionTranscript::SessionInfo &info : out) {
            info.title = shortTitleOf(info.title, info.id);
        }
    }
    q.finish();
    return out;
}

QVector<SessionTranscript::SessionInfo> listDshSessions(const QString &root,
                                                         int maxSessions) {
    QVector<SessionTranscript::SessionInfo> out;
    const QString dir = root + QStringLiteral(
                                   "/storages/session_projcache/sessions");
    QDir d(dir);
    if (!d.exists()) {
        return out;
    }
    const QFileInfoList files =
        d.entryInfoList(QStringList{QStringLiteral("session-*.json")},
                        QDir::Files, QDir::Time);
    for (const QFileInfo &fi : files) {
        if (out.size() >= maxSessions) {
            break;
        }
        const QString id = fi.completeBaseName();
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject rec =
            QJsonDocument::fromJson(f.readAll()).object()
                .value(QStringLiteral("record"))
                .toObject();
        const QJsonObject rows = rec.value(QStringLiteral("rows")).toObject();
        SessionTranscript::SessionInfo info;
        info.id = id;
        info.title = shortTitleOf(
            rows.value(QStringLiteral("title"))
                .toObject()
                .value(QStringLiteral("val"))
                .toString(),
            id);
        info.project = rows.value(QStringLiteral("workingDirectory"))
                           .toObject()
                           .value(QStringLiteral("val"))
                           .toString();
        if (info.project.isEmpty()) {
            info.project = rec.value(QStringLiteral("identity"))
                               .toObject()
                               .value(QStringLiteral("cwd"))
                               .toString();
        }
        info.updatedAt =
            fi.lastModified().toUTC().toString(Qt::ISODateWithMs);
        info.archived = archivedProject(info.project);
        out.append(info);
    }
    return out;
}

}  // namespace

QVector<SessionTranscript::SessionInfo> SessionTranscript::listSessions(
    const QString &agentExe, int maxSessions, const QString &rootOverride) {
    if (maxSessions <= 0) {
        return {};
    }
    const QString jsonlHome = jsonlHomeFor(agentExe);
    if (!jsonlHome.isEmpty()) {
        return listJsonlSessions(
            rootOverride.isEmpty() ? jsonlHome : rootOverride, maxSessions);
    }
    if (agentExe.compare(QStringLiteral("ZCode.exe"), Qt::CaseInsensitive) == 0) {
        return listZcodeSessions(
            rootOverride.isEmpty() ? ZcodeDispatch::zcodeHome() : rootOverride,
            maxSessions);
    }
    if (agentExe.compare(QStringLiteral("OpenSquilla.exe"),
                         Qt::CaseInsensitive) == 0) {
        const QString root = rootOverride.isEmpty() ? osqStateRoot() : rootOverride;
        return root.isEmpty() ? QVector<SessionInfo>{}
                              : listOsqSessions(root, maxSessions);
    }
    if (agentExe.compare(QStringLiteral("DeepSeek Harness.exe"),
                         Qt::CaseInsensitive) == 0) {
        return listDshSessions(
            rootOverride.isEmpty() ? DshSessions::home() : rootOverride,
            maxSessions);
    }
    return {};
}

// 宿主把「不是用户说的话」也记成 user 角色：后台任务完成通知、上下文摘要、
// 本地命令回显/输出。这些渲染成「我」的气泡就是把 harness 的内部 XML 摊给
// 用户看（2026-11-19 用户截图：`<task-notification>` 整段出现在「我」的气泡里），
// 也不能拿去当会话标题。真用户文本的 `<user_query>` / `<选中文本>` 不在此列。
bool harnessUserNoise(const QString &text) {
    const QString t = text.trimmed();
    if (!t.startsWith(QLatin1Char('<'))) {
        return false;
    }
    static const QStringList kTags{
        QStringLiteral("task-notification"),
        QStringLiteral("conversation_history_summary"),
        QStringLiteral("cb_summary"),
        QStringLiteral("command-message"),
        QStringLiteral("local-command-caveat"),
        QStringLiteral("local-command-stdout"),
    };
    for (const QString &tag : kTags) {
        if (t.startsWith(QLatin1Char('<') + tag)) {
            return true;
        }
    }
    return false;
}

QString SessionTranscript::unwrap(const QString &raw) {
    static const QRegularExpression reminder(
        QStringLiteral("<system-reminder[\\s\\S]*?</system-reminder>"));
    static const QRegularExpression queryTag(QStringLiteral("</?user_query>"));
    QString text = raw;
    text.remove(reminder);
    text.remove(queryTag);
    return text.trimmed();
}

bool SessionTranscript::messageOf(const QByteArray &line, QString *role,
                                  QString *text, QString *at, QString *id) {
    const QJsonObject obj = QJsonDocument::fromJson(line).object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QJsonObject msg = obj.value(QStringLiteral("message")).toObject();
    QString r = msg.value(QStringLiteral("role")).toString();
    if (r.isEmpty()) {
        r = obj.value(QStringLiteral("role")).toString();
    }
    const bool isUser = type == QStringLiteral("user") ||
                        (type == QStringLiteral("message") &&
                         r == QStringLiteral("user"));
    const bool isAssistant = type == QStringLiteral("assistant") ||
                             (type == QStringLiteral("message") &&
                              r == QStringLiteral("assistant"));
    if (!isUser && !isAssistant) {
        return false;
    }
    if (obj.value(QStringLiteral("isSidechain")).toBool(false)) {
        return false;
    }
    QJsonValue content = msg.value(QStringLiteral("content"));
    if (content.isUndefined() || content.isNull()) {
        content = obj.value(QStringLiteral("content"));
    }
    const QString joined = unwrap(contentText(content));
    if (joined.isEmpty()) {
        return false;
    }
    if (isUser && harnessUserNoise(joined)) {
        return false;  // 后台任务通知/摘要/本地命令回显，不是用户的话
    }
    *role = isUser ? QStringLiteral("user") : QStringLiteral("assistant");
    *text = joined;
    QJsonValue ts = obj.value(QStringLiteral("timestamp"));
    if (ts.isUndefined() || ts.isNull()) {
        ts = msg.value(QStringLiteral("timestamp"));
    }
    *at = normalizeTs(ts);
    if (id != nullptr) {
        QString mid = obj.value(QStringLiteral("id")).toString();
        if (mid.isEmpty()) {
            mid = obj.value(QStringLiteral("uuid")).toString();
        }
        if (mid.isEmpty()) {
            mid = *at + QLatin1Char('|') + *role + QLatin1Char('|') + joined.left(80);
        }
        *id = mid;
    }
    return true;
}

// —— DSH：session.v4.jsonl.zstd（多帧拼接）——
// 宿主把每个会话的事件日志压成 zstd，追加轮次落在后续帧里。ZSTD_decompressStream
// 循环喂入即跨帧（一帧解完 ret=0，输入还有剩就接着解下一帧），等价于探针里
// zstandard 的 read_across_frames=True。只读、带帽，不写宿主库。
constexpr qint64 kMaxPlainBytes = 256 * 1024 * 1024;  // 解压上限，防病态文件吃满内存

QByteArray decompressZstd(const QByteArray &compressed, bool *ok) {
    *ok = false;
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (dctx == nullptr) {
        return {};
    }
    QByteArray out;
    const size_t outCap = ZSTD_DStreamOutSize();
    QByteArray buf(static_cast<int>(outCap), Qt::Uninitialized);
    ZSTD_inBuffer input{compressed.constData(),
                        static_cast<size_t>(compressed.size()), 0};
    bool error = false;
    while (input.pos < input.size) {
        ZSTD_outBuffer output{buf.data(), outCap, 0};
        const size_t before = input.pos;
        const size_t ret = ZSTD_decompressStream(dctx, &output, &input);
        if (ZSTD_isError(ret)) {
            error = true;
            break;
        }
        out.append(buf.constData(), static_cast<int>(output.pos));
        if (out.size() > kMaxPlainBytes) {
            error = true;  // 过大：当损坏处理，不无限吃内存
            break;
        }
        // 无进展保护：坏文件不该让循环空转（正常流每步必 consumed 或 produced）。
        if (input.pos == before && output.pos == 0) {
            break;
        }
    }
    ZSTD_freeDCtx(dctx);
    *ok = !error;
    return out;
}

QString findDshFile(const QString &root, const QString &sessionId) {
    QDir sessions(root + QStringLiteral("/sessions"));
    const QStringList slugs = sessions.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &slug : slugs) {
        const QString file = sessions.filePath(
            slug + QLatin1Char('/') + sessionId +
            QStringLiteral("/session.v4.jsonl.zstd"));
        if (QFileInfo::exists(file)) {
            return file;
        }
    }
    return QString();
}

SessionTranscript::Result readDsh(const QString &root, const QString &sessionId,
                                  int limit, int offset) {
    SessionTranscript::Result out;
    const QString path = findDshFile(root, sessionId);
    if (path.isEmpty()) {
        out.supported = true;
        out.error = QStringLiteral("会话记录不在本机库里（可能已清理）");
        return out;
    }
    const QString key = QStringLiteral("dsh|") + path;
    const QString stamp = fileStamp(path);
    auto it = g_cache.constFind(key);
    if (it != g_cache.constEnd() && it->stamp == stamp) {
        return slicePage(*it, limit, offset);
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        out.supported = true;
        out.error = QStringLiteral("会话记录打不开");
        return out;
    }
    const QByteArray compressed = f.readAll();
    f.close();
    bool ok = false;
    const QByteArray plain = decompressZstd(compressed, &ok);
    if (!ok) {
        out.supported = true;
        out.error = QStringLiteral("会话记录解压失败（文件可能损坏或过大）");
        return out;
    }
    FullCache full;
    full.stamp = stamp;
    QString firstUser;
    // DSH 的 tool/call ↔ tool/result 配对（结果行的 callId 藏在 message 里）。
    QHash<QString, ToolCallInfo> calls;
    QStringList callOrder;
    const QList<QByteArray> lines = plain.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        const QString type = obj.value(QStringLiteral("type")).toString();
        // 工具调用：耗时长的那次落一行系统信息（用户 2026-11-19：DSH 闷头跑
        // 工具时页面什么都不显示）。
        if (type == QStringLiteral("tool/call")) {
            const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
            const QString callId = data.value(QStringLiteral("callId")).toString();
            const QString name = data.value(QStringLiteral("name")).toString();
            if (!callId.isEmpty() && !name.isEmpty()) {
                ToolCallInfo info;
                info.name = name;
                info.at = normalizeTs(obj.value(QStringLiteral("time")));
                info.ts = obj.value(QStringLiteral("time")).toDouble();
                info.brief = toolCallBrief(
                    name, callArgs(data.value(QStringLiteral("arguments"))));
                calls.insert(callId, info);
                callOrder.append(callId);
            }
            continue;
        }
        if (type == QStringLiteral("tool/result")) {
            const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
            const QJsonObject msg = data.value(QStringLiteral("message")).toObject();
            QString callId = msg.value(QStringLiteral("toolCallId")).toString();
            if (callId.isEmpty()) {
                callId = msg.value(QStringLiteral("source"))
                             .toObject()
                             .value(QStringLiteral("callId"))
                             .toString();
            }
            auto hit = calls.find(callId);
            if (hit != calls.end()) {
                const double end = obj.value(QStringLiteral("time")).toDouble();
                const qint64 ms = (hit->ts > 0 && end > hit->ts)
                                      ? static_cast<qint64>(end - hit->ts)
                                      : 0;
                if (ms >= kSysCallMinMs) {
                    full.all.append(sysRow(
                        toolCallLine(hit->name, hit->brief, ms),
                        normalizeTs(obj.value(QStringLiteral("time")))));
                }
                calls.erase(hit);
            }
            continue;
        }
        // 标题取日志里最后一条 session/title（AI 异步生成，与投影缓存同源）。
        if (type == QStringLiteral("session/title")) {
            const QString t = obj.value(QStringLiteral("data"))
                                  .toObject()
                                  .value(QStringLiteral("title"))
                                  .toString();
            if (!t.isEmpty()) {
                full.title = t;
            }
            continue;
        }
        QString role, id;
        QJsonValue content;
        if (type == QStringLiteral("user/message")) {
            const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
            // 只认真正的用户输入（source.kind=="user"）。DSH 把时间上下文、运行时
            // 快照、skill 目录、workspace 指令、工具通知、审批变更等也记成
            // user/message（kind=time-context/runtime-context/skill-catalog/
            // agent-instructions/tool-jobs/user-approval…），那些是系统注入，
            // 不该当成「我」的气泡显示（用户 2026-11-14 反馈多余气泡）。
            const QString kind = data.value(QStringLiteral("source"))
                                     .toObject()
                                     .value(QStringLiteral("kind"))
                                     .toString();
            if (kind != QStringLiteral("user")) {
                continue;
            }
            role = QStringLiteral("user");
            content = data.value(QStringLiteral("content"));
            id = data.value(QStringLiteral("id")).toString();
        } else if (type == QStringLiteral("assistant/message")) {
            const QJsonObject msg = obj.value(QStringLiteral("data"))
                                        .toObject()
                                        .value(QStringLiteral("message"))
                                        .toObject();
            role = QStringLiteral("assistant");
            content = msg.value(QStringLiteral("content"));
            id = msg.value(QStringLiteral("id")).toString();
        } else {
            continue;  // tool/call、step/*、reasoning 等不进聊天页
        }
        // contentText 只取 text/input_text/output_text，天然滤掉 reasoning/tool-call。
        const QString text = SessionTranscript::unwrap(contentText(content));
        if (text.isEmpty()) {
            continue;  // 纯工具调用的 assistant 消息不出气泡
        }
        const QString at = normalizeTs(obj.value(QStringLiteral("time")));
        if (id.isEmpty()) {
            id = at + QLatin1Char('|') + role + QLatin1Char('|') + text.left(80);
        }
        if (role == QStringLiteral("user") && firstUser.isEmpty()) {
            firstUser = text;
        }
        full.all.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("role"), role},
            {QStringLiteral("text"), capText(text)},
            {QStringLiteral("at"), at},
        });
    }
    if (full.title.isEmpty() && !firstUser.isEmpty()) {
        full.title = firstUser.simplified();
        if (full.title.size() > 30) {
            full.title = full.title.left(30) + QStringLiteral("…");
        }
    }
    // 还在跑的工具调用（会话最后一条就是它）：也露一行，别让页面停在上一句上。
    // 同 jsonl 分支：早于最后一条消息的未配对调用属于历史残留，不挂「进行中」。
    qint64 lastMsgMs2 = 0;
    for (const QJsonObject &row : full.all) {
        if (row.value(QStringLiteral("role")).toString() ==
            QLatin1String("system")) {
            continue;  // 同 jsonl 分支：system 行不参与「最后一条消息」
        }
        const QDateTime dt =
            QDateTime::fromString(row.value(QStringLiteral("at")).toString(),
                                  Qt::ISODate);
        if (dt.isValid()) {
            lastMsgMs2 = qMax(lastMsgMs2, dt.toMSecsSinceEpoch());
        }
    }
    for (const QString &callId : callOrder) {
        const auto hit = calls.constFind(callId);
        if (hit == calls.constEnd()) {
            continue;
        }
        const QDateTime callAt = QDateTime::fromString(hit->at, Qt::ISODate);
        if (lastMsgMs2 > 0 && callAt.isValid() &&
            callAt.toMSecsSinceEpoch() < lastMsgMs2) {
            continue;
        }
        QString text = QStringLiteral("进行中 · ") + toolVerb(hit->name);
        if (!hit->brief.isEmpty()) {
            text += QStringLiteral(" · ") + hit->brief;
        }
        full.all.append(sysRow(text, hit->at));
    }
    full.realTotal = full.all.size();
    cachePut(key, full);
    return slicePage(full, limit, offset);
}

SessionTranscript::Result SessionTranscript::read(const QString &agentExe,
                                                  const QString &sessionId,
                                                  int limit, int offsetFromEnd,
                                                  const QString &rootOverride) {
    Result out;
    if (sessionId.trimmed().isEmpty()) {
        out.error = QStringLiteral("待办未关联会话");
        return out;
    }
    limit = qBound(1, limit, kMaxLimit);
    offsetFromEnd = qMax(0, offsetFromEnd);
    const QString jsonlHome = jsonlHomeFor(agentExe);
    if (!jsonlHome.isEmpty()) {
        return readJsonl(rootOverride.isEmpty() ? jsonlHome : rootOverride,
                         sessionId, limit, offsetFromEnd);
    }
    if (agentExe.compare(QStringLiteral("ZCode.exe"), Qt::CaseInsensitive) == 0) {
        return readZcode(rootOverride.isEmpty() ? ZcodeDispatch::zcodeHome()
                                                : rootOverride,
                         sessionId, limit, offsetFromEnd);
    }
    if (agentExe.compare(QStringLiteral("OpenSquilla.exe"), Qt::CaseInsensitive) == 0) {
        const QString root =
            rootOverride.isEmpty() ? osqStateRoot() : rootOverride;
        if (root.isEmpty()) {
            out.supported = true;
            out.error = QStringLiteral("OpenSquilla 会话库不在本机");
            return out;
        }
        return readOsq(root, sessionId, limit, offsetFromEnd);
    }
    if (agentExe.compare(QStringLiteral("DeepSeek Harness.exe"),
                         Qt::CaseInsensitive) == 0) {
        return readDsh(rootOverride.isEmpty() ? DshSessions::home() : rootOverride,
                       sessionId, limit, offsetFromEnd);
    }
    out.error = QStringLiteral("该宿主暂不支持聊天记录查看");
    return out;
}