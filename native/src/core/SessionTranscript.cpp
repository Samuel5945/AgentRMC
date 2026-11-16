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
// 沙箱、OpenSquilla 任务目录、VirtualDisplay 同理。拿这种目录名当项目名，会把
// 同一批单任务拆成一串「按日期」的分组——目录页统一收进「未分类任务」。
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
        QStringLiteral("/virtualdisplay"),
    };
    for (const QString &dir : generatedDirs) {
        if (p.contains(dir)) {
            return true;
        }
    }
    if (p.endsWith(QStringLiteral("/workbuddy")) ||
        p.endsWith(QStringLiteral("/workbuddy ai"))) {
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

QString capText(const QString &text) {
    if (text.size() <= kMaxTextChars) {
        return text;
    }
    return text.left(kMaxTextChars) + QStringLiteral("\n…（内容过长，已截断）");
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

// 全量解析缓存：库文件 mtime+size 变了才重读。翻页与 2.5s 轮询都吃缓存。
struct FullCache {
    QString stamp;
    QVector<QJsonObject> all;  // 正序
    QString title;
    int realTotal = 0;  // 实际可渲染条数（SQLite 封顶 500 内、滤掉注入后）
};
QHash<QString, FullCache> g_cache;

QString fileStamp(const QString &path) {
    const QFileInfo fi(path);
    return QString::number(fi.lastModified().toMSecsSinceEpoch()) + QLatin1Char(':') +
           QString::number(fi.size());
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
    out.total = full.realTotal;
    const int end = qMax(0, full.all.size() - offset);
    const int start = qMax(0, end - limit);
    for (int i = start; i < end; ++i) {
        out.messages.append(full.all.at(i));
    }
    out.hasMore = start > 0;
    return out;
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
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        QString role, text, at, id;
        if (!SessionTranscript::messageOf(line, &role, &text, &at, &id)) {
            continue;
        }
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        const QString sid = obj.value(QStringLiteral("sessionId")).toString();
        if (!sid.isEmpty() && sid != sessionId) {
            continue;
        }
        full.all.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("role"), role},
            {QStringLiteral("text"), capText(text)},
            {QStringLiteral("at"), at},
        });
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
    const QString stamp = fileStamp(dbPath);
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
    const QString stamp = fileStamp(dbPath);
    auto it = g_cache.constFind(key);
    if (it != g_cache.constEnd() && it->stamp == stamp) {
        return slicePage(*it, limit, offset);
    }
    ReadOnlyDb db(dbPath);
    if (!db.ok()) {
        out.supported = true;
        out.error = QStringLiteral("OpenSquilla 会话库打不开");
        return out;
    }
    FullCache full;
    full.stamp = stamp;
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
        }
    }
    for (int i = rev.size() - 1; i >= 0; --i) {
        full.all.append(rev.at(i));
    }
    full.realTotal = qMin(total, kSqliteCap);
    cachePut(key, full);
    return slicePage(full, limit, offset);
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
    for (const QFileInfo &fi : files) {
        if (out.size() >= maxSessions || out.size() >= scanCap) {
            break;
        }
        const QString id = fi.completeBaseName();
        if (id.isEmpty()) {
            continue;
        }
        const SessionMeta meta = jsonlMeta(fi.absoluteFilePath());
        SessionTranscript::SessionInfo info;
        info.id = id;
        info.title = shortTitleOf(meta.title, id);
        info.project = meta.project;
        info.updatedAt =
            fi.lastModified().toUTC().toString(Qt::ISODateWithMs);
        info.archived = archivedProject(info.project);
        out.append(info);
    }
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
            info.archived = archivedProject(info.project);
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
            "SELECT s.session_key, "
            "COALESCE(NULLIF(s.display_name,''),NULLIF(s.derived_title,''),"
            "NULLIF(s.subject,''),NULLIF(s.label,''),''), "
            "COALESCE(p.path,''), COALESCE(p.removed_at,0), s.updated_at "
            "FROM sessions s LEFT JOIN project_workspaces p "
            "ON p.workspace_id=s.workspace_id "
            "ORDER BY s.updated_at DESC LIMIT ") +
               QString::number(maxSessions))) {
        while (q.next()) {
            const QString key = q.value(0).toString();
            SessionTranscript::SessionInfo info;
            info.id = key;
            QString title = q.value(1).toString();
            if (title.isEmpty()) {
                // agent:main:webchat:ct2q066i → 会话 ct2q066i（别把 agent:ma 当标题）。
                title = QStringLiteral("会话 ") + key.section(QLatin1Char(':'), -1);
            }
            info.title = shortTitleOf(title, key);
            info.project = q.value(2).toString();
            info.updatedAt = isoFromMs(q.value(4).toLongLong());
            info.archived =
                q.value(3).toLongLong() > 0 || archivedProject(info.project);
            out.append(info);
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
    const QList<QByteArray> lines = plain.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        const QString type = obj.value(QStringLiteral("type")).toString();
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
        if (full.title.size() > 60) {
            full.title = full.title.left(60) + QStringLiteral("…");
        }
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