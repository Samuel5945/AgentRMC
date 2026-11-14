#include "core/SessionTranscript.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QVector>

#include <algorithm>

#include <zstd.h>

#include "core/DshSessions.h"
#include "core/ZcodeDispatch.h"

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
    int realTotal = 0;  // jsonl＝真实总数；SQLite＝封顶后的总数
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
    full.realTotal = qMin(total, kSqliteCap);
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

}  // namespace

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