// SessionTranscript 自测：jsonl 两记法 / 分页游标 / 包装剥离 / ZCode 与
// OpenSquilla SQLite / 缓存失效 / 不支持宿主。全部用临时库，不碰真实会话库。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <cstdio>

#include "core/SessionCleaner.h"
#include "core/SessionTranscript.h"

namespace {

bool ok = true;
int checks = 0;

void check(bool cond, const char *what) {
    ++checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ok = false;
    }
}

void writeLines(const QString &path, const QStringList &lines) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream ts(&f);
    for (const QString &line : lines) {
        ts << line << '\n';
    }
}

void copyBytes(const QString &from, const QString &to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    QFile::copy(from, to);
}

QString wbLine(const QString &id, const QString &role, const QString &text,
               qint64 ts, const QString &sid) {
    QJsonObject obj{
        {QStringLiteral("id"), id},
        {QStringLiteral("timestamp"), ts},
        {QStringLiteral("type"), QStringLiteral("message")},
        {QStringLiteral("role"), role},
        {QStringLiteral("sessionId"), sid},
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{
             {QStringLiteral("type"),
              role == QStringLiteral("user") ? QStringLiteral("input_text")
                                             : QStringLiteral("output_text")},
             {QStringLiteral("text"), text}}}},
    };
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString qoderLine(const QString &uuid, const QString &type, const QString &text,
                  const QString &iso, const QString &sid, bool sidechain) {
    QJsonObject obj{
        {QStringLiteral("uuid"), uuid},
        {QStringLiteral("type"), type},
        {QStringLiteral("timestamp"), iso},
        {QStringLiteral("sessionId"), sid},
        {QStringLiteral("isSidechain"), sidechain},
        {QStringLiteral("message"),
         QJsonObject{
             {QStringLiteral("role"), type},
             {QStringLiteral("content"),
              QJsonArray{QJsonObject{{QStringLiteral("type"),
                                      QStringLiteral("text")},
                                     {QStringLiteral("text"), text}}}}}},
    };
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // 单任务会话的宿主临时工作区（日期目录等）必须认成「未分类任务」，
    // 真实仓库不能误判——否则派发页会把单任务拆成一串按日期的分组。
    {
        const QStringList autoDirs{
            QStringLiteral("C:/Users/x/Documents/WorkBuddy/2026-11-12-21-07-51"),
            QStringLiteral("C:\\Users\\x\\Documents\\WorkBuddy AI\\2026-11-12"),
            QStringLiteral("C:/Users/x/Documents/2026-11-12-21-07-51"),
            QStringLiteral("C:/Users/x/Documents/Qoder/20261112-210751"),
            QStringLiteral(
                "C:/Users/x/AppData/Roaming/com.qoder.app.stable/remote-control/a"),
            QStringLiteral("C:/Users/x/.opensquilla/tasks/t1"),
        };
        for (const QString &dir : autoDirs) {
            check(SessionTranscript::generatedWorkspace(dir),
                  "generated workspace detected");
        }
        const QStringList realDirs{
            QStringLiteral("D:/githubrepos/Agentrmc"),
            QStringLiteral("C:/Users/x/Documents/report-2026"),
            QString(),
        };
        for (const QString &dir : realDirs) {
            check(!SessionTranscript::generatedWorkspace(dir),
                  "real project not treated as generated");
        }
    }
    // --live <session-id>：读真实 ~/.dsh 的 DSH 会话（不碰临时库），打印最新一页，
    // 供装机后对真实数据冒烟；正常无参跑仍是全套临时库自测。
    if (argc >= 3 && QByteArray(argv[1]) == "--live") {
        const QString sid = QString::fromUtf8(argv[2]);
        auto r = SessionTranscript::read(QStringLiteral("DeepSeek Harness.exe"),
                                         sid, 20, 0);
        std::printf("supported=%d total=%d hasMore=%d\n", r.supported, r.total,
                    r.hasMore);
        std::printf("title=%s\n", r.title.toUtf8().constData());
        std::printf("error=%s\n", r.error.toUtf8().constData());
        for (const QJsonValue &m : r.messages) {
            const QJsonObject o = m.toObject();
            std::printf("[%s] %s\n", o.value("role").toString().toUtf8().constData(),
                        o.value("text").toString().left(100).toUtf8().constData());
        }
        return (r.supported && r.total > 0) ? 0 : 1;
    }
    // --catalog <exe>：装机后对真实宿主会话目录冒烟（不写库）。
    if (argc >= 3 && QByteArray(argv[1]) == "--catalog") {
        const QString exe = QString::fromUtf8(argv[2]);
        const int maxSessions =
            argc >= 4 ? QString::fromUtf8(argv[3]).toInt() : 8;
        const auto rows = SessionTranscript::listSessions(exe, maxSessions);
        QSet<QString> projects;
        QSet<QString> archivedProjects;
        int archived = 0;
        for (const auto &row : rows) {
            if (!row.project.isEmpty()) {
                projects.insert(row.project);
                if (row.archived) {
                    archivedProjects.insert(row.project);
                }
            }
            if (row.archived) {
                ++archived;
            }
        }
        std::printf("agent=%s sessions=%d projects=%d archived_sessions=%d "
                    "archived_projects=%d\n",
                    exe.toUtf8().constData(), static_cast<int>(rows.size()),
                    static_cast<int>(projects.size()), archived,
                    static_cast<int>(archivedProjects.size()));
        for (const auto &row : rows) {
            std::printf("%s%s | %s | %s\n",
                        row.archived ? "A " : "  ",
                        row.updatedAt.toUtf8().constData(),
                        row.title.toUtf8().constData(),
                        row.project.toUtf8().constData());
        }
        return rows.isEmpty() ? 1 : 0;
    }
    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::fprintf(stderr, "FAIL temp dir\n");
        return 1;
    }
    const QString sid = QStringLiteral("11111111-2222-4333-8444-555555555555");

    // ---- WorkBuddy 记法 + 包装剥离 + 换行保留 ----
    const QString wbHome = temp.filePath("wb");
    const QString wbFile =
        wbHome + QStringLiteral("/projects/slug-a/") + sid + QStringLiteral(".jsonl");
    const qint64 base = 1794569091773LL;
    writeLines(wbFile, {
        QStringLiteral(R"({"type":"workspace-directories","directories":["D:/x"]})"),
        wbLine("m1", "user",
               QStringLiteral("<system-reminder>secret</system-reminder>\n<user_query>第一句\n第二行</user_query>"),
               base, sid),
        wbLine("m2", "assistant", QStringLiteral("回复一"), base + 1000, sid),
        wbLine("m3", "user", QStringLiteral("第二句"), base + 2000, sid),
        wbLine("m4", "assistant", QStringLiteral("回复二"), base + 3000, sid),
        // 串库杂行：sessionId 不同，必须被过滤
        wbLine("mx", "user", QStringLiteral("别的会话"), base + 4000,
               QStringLiteral("99999999-9999-4999-8999-999999999999")),
    });
    auto r = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"), sid, 50, 0,
                                     wbHome);
    check(r.supported, "wb supported");
    check(r.messages.size() == 4, "wb 4 messages (header+foreign filtered)");
    if (r.messages.size() == 4) {
        const QJsonObject m0 = r.messages.at(0).toObject();
        check(m0.value("role").toString() == QStringLiteral("user"), "wb role user");
        check(m0.value("text").toString() == QStringLiteral("第一句\n第二行"),
              "wb unwrap keeps newline");
        check(m0.value("id").toString() == QStringLiteral("m1"), "wb id");
        check(!m0.value("at").toString().isEmpty(), "wb ts normalized");
        check(r.messages.at(3).toObject().value("text").toString() ==
                  QStringLiteral("回复二"),
              "wb chronological");
    }
    check(r.total == 4 && !r.hasMore, "wb total/hasMore");
    {
        const auto catalog = SessionTranscript::listSessions(
            QStringLiteral("WorkBuddyAI.exe"), 10, wbHome);
        check(catalog.size() == 1, "jsonl session catalog count");
        if (!catalog.isEmpty()) {
            check(catalog.at(0).id == sid, "jsonl catalog id");
            check(catalog.at(0).title.contains(QStringLiteral("第一句")),
                  "jsonl catalog title");
            check(!catalog.at(0).project.isEmpty(),
                  "jsonl catalog project");
            check(catalog.at(0).archived,
                  "jsonl catalog marks missing project archived");
        }
    }

    // ---- 子代理会话（agent-*.jsonl）不进目录 ----
    {
        // 子代理会话不在宿主 sessions 表里，归档标记查不到；列出来就是一条谁都
        // 对不上的陌生会话（用户 2026-11-17 报的那条 27 天前 tuwen-editor）。
        const QString subId = QStringLiteral("agent-a8f3b1");
        writeLines(wbHome + QStringLiteral("/projects/slug-a/") + subId +
                       QStringLiteral(".jsonl"),
                   {QStringLiteral(R"({"type":"message","cwd":"D:/x"})"),
                    wbLine("s1", "user", QStringLiteral("子代理任务"),
                           base + 9000, subId)});
        bool sawSub = false;
        for (const auto &row : SessionTranscript::listSessions(
                 QStringLiteral("WorkBuddy.exe"), 10, wbHome)) {
            if (row.id.startsWith(QStringLiteral("agent-"))) {
                sawSub = true;
            }
        }
        check(!sawSub, "subagent jsonl not listed");
    }

    // ---- 宿主临时目录（日期目录）里会话还在 → 不算已归档 ----
    {
        // WorkBuddy 单任务按时间戳建目录：目录存在就说明会话仍可用，只归
        // 「未分类任务」，不能标成可删的已归档项（用户 2026-11-16 口径）。
        const QString stampDir =
            wbHome + QStringLiteral("/WorkBuddy/2026-11-12-21-07-51");
        QDir().mkpath(stampDir);
        const QString sid2 = QStringLiteral("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
        writeLines(wbHome + QStringLiteral("/projects/slug-b/") + sid2 +
                       QStringLiteral(".jsonl"),
                   {QStringLiteral(R"({"type":"message","cwd":"%1"})").arg(stampDir),
                    wbLine("m1", "user", QStringLiteral("日期目录单任务"),
                           base + 6000, sid2)});
        check(SessionTranscript::generatedWorkspace(stampDir),
              "date dir classified as single task");
        bool found = false;
        for (const auto &row : SessionTranscript::listSessions(
                 QStringLiteral("WorkBuddyAI.exe"), 10, wbHome)) {
            if (row.id != sid2) {
                continue;
            }
            found = true;
            check(!row.archived, "existing date dir session not archived");
        }
        check(found, "date dir session in catalog");
    }

    // ---- 分页：120 条，从末尾切 ----
    const QString bigSid = QStringLiteral("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const QString bigFile =
        wbHome + QStringLiteral("/projects/slug-a/") + bigSid + QStringLiteral(".jsonl");
    QStringList big;
    for (int i = 1; i <= 120; ++i) {
        big << wbLine(QStringLiteral("b%1").arg(i),
                      i % 2 ? QStringLiteral("user") : QStringLiteral("assistant"),
                      QStringLiteral("消息%1").arg(i), base + i * 1000, bigSid);
    }
    writeLines(bigFile, big);
    r = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"), bigSid, 50, 0,
                                wbHome);
    check(r.total == 120 && r.hasMore, "page0 total/hasMore");
    check(r.messages.size() == 50, "page0 size");
    check(r.messages.at(0).toObject().value("text").toString() ==
              QStringLiteral("消息71"),
          "page0 starts at 71 (latest 50)");
    r = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"), bigSid, 50, 50,
                                wbHome);
    check(r.messages.size() == 50 &&
              r.messages.at(0).toObject().value("text").toString() ==
                  QStringLiteral("消息21"),
          "page1 offset 50");
    r = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"), bigSid, 50, 100,
                                wbHome);
    check(r.messages.size() == 20 && !r.hasMore, "page2 tail 20 no more");
    check(r.messages.at(0).toObject().value("text").toString() ==
              QStringLiteral("消息1"),
          "page2 oldest first");

    // ---- 缓存失效：追加一行后必须读到 ----
    {
        QFile f(bigFile);
        f.open(QIODevice::Append | QIODevice::Text);
        QTextStream ts(&f);
        ts << wbLine("b121", "user", QStringLiteral("消息121"), base + 121000,
                     bigSid)
           << '\n';
    }
    r = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"), bigSid, 50, 0,
                                wbHome);
    check(r.total == 121, "cache invalidated on append");
    check(r.messages.at(49).toObject().value("text").toString() ==
              QStringLiteral("消息121"),
          "newest after append");

    // ---- Qoder 记法 + sidechain 过滤 ----
    const QString qHome = temp.filePath("qoder");
    const QString qFile =
        qHome + QStringLiteral("/projects/D--repo/") + sid + QStringLiteral(".jsonl");
    writeLines(qFile, {
        qoderLine("u1", "user", QStringLiteral("问"),
                  QStringLiteral("2026-11-11T01:43:19.058Z"), sid, false),
        qoderLine("a1", "assistant", QStringLiteral("答"),
                  QStringLiteral("2026-11-11T01:43:56.235Z"), sid, false),
        qoderLine("s1", "assistant", QStringLiteral("旁路"),
                  QStringLiteral("2026-11-11T01:44:00.000Z"), sid, true),
    });
    r = SessionTranscript::read(QStringLiteral("Qoder CN.exe"), sid, 50, 0, qHome);
    check(r.supported && r.messages.size() == 2, "qoder parse + sidechain skip");
    if (r.messages.size() == 2) {
        check(r.messages.at(1).toObject().value("id").toString() ==
                  QStringLiteral("a1"),
              "qoder uuid as id");
    }

    // ---- ZCode：message+part 两表，sequence 拼正文，工具行不进 ----
    const QString zRoot = temp.filePath("zcode");
    QDir().mkpath(zRoot + QStringLiteral("/cli/db"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("setup_z"));
        db.setDatabaseName(zRoot + QStringLiteral("/cli/db/db.sqlite"));
        db.open();
        QSqlQuery q(db);
        q.exec("CREATE TABLE session(id TEXT, title TEXT)");
        q.exec("CREATE TABLE message(id TEXT, session_id TEXT, time_created INTEGER, "
               "time_updated INTEGER, data TEXT, sequence INTEGER)");
        q.exec("CREATE TABLE part(id TEXT, message_id TEXT, session_id TEXT, "
               "time_created INTEGER, time_updated INTEGER, data TEXT, sequence INTEGER)");
        q.prepare("INSERT INTO session VALUES(?,?)");
        q.addBindValue(sid);
        q.addBindValue(QStringLiteral("ZCode 会话标题"));
        q.exec();
        const QString zsid = sid;
        auto insMsg = [&](const QString &id, const QString &role, int seq,
                          const QJsonObject &extra = {}) {
            QSqlQuery m(db);
            m.prepare("INSERT INTO message VALUES(?,?,?,?,?,?)");
            m.addBindValue(id);
            m.addBindValue(zsid);
            m.addBindValue(base + seq * 1000);
            m.addBindValue(base + seq * 1000);
            QJsonObject data{{QStringLiteral("role"), role}};
            for (auto it = extra.begin(); it != extra.end(); ++it) {
                data.insert(it.key(), it.value());
            }
            m.addBindValue(
                QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));
            m.addBindValue(seq);
            m.exec();
        };
        auto insPart = [&](const QString &mid, const QString &type,
                           const QString &text, int seq) {
            QSqlQuery p(db);
            p.prepare("INSERT INTO part VALUES(?,?,?,?,?,?,?)");
            p.addBindValue(QStringLiteral("p_%1_%2").arg(mid).arg(seq));
            p.addBindValue(mid);
            p.addBindValue(zsid);
            p.addBindValue(base);
            p.addBindValue(base);
            QJsonObject data{{QStringLiteral("type"), type}};
            if (type == QStringLiteral("text")) {
                data.insert(QStringLiteral("text"), text);
            }
            p.addBindValue(QString::fromUtf8(
                QJsonDocument(data).toJson(QJsonDocument::Compact)));
            p.addBindValue(seq);
            p.exec();
        };
        insMsg("zm1", "user", 1);
        insPart("zm1", "text", QStringLiteral("ZCode 问"), 0);
        insMsg("zm2", "assistant", 2);
        insPart("zm2", "step-start", QString(), 0);
        insPart("zm2", "text", QStringLiteral("第一段"), 1);
        insPart("zm2", "text", QStringLiteral("第二段"), 2);
        insMsg("zm3", "assistant", 3);  // 纯工具消息：无 text part → 不进
        insPart("zm3", "tool", QString(), 0);
        // harness 注入的 user 行（环境快照 / todo nudge / 静默轮提醒）→ 不进。
        // 三种标记形态照真实库记法各来一条。
        insMsg("zi1", "user", 4,
               QJsonObject{
                   {QStringLiteral("synthetic"), true},
                   {QStringLiteral("visibility"), QStringLiteral("model-only")},
                   {QStringLiteral("metadata"),
                    QJsonObject{{QStringLiteral("runtimeMessage"),
                                 QJsonObject{{QStringLiteral("source"),
                                              QStringLiteral("environment")}}}}}});
        insPart("zi1", "text",
                QStringLiteral("# Environment\nYou have been invoked in the "
                               "following environment:"),
                0);
        insMsg("zi2", "user", 5,
               QJsonObject{{QStringLiteral("visibility"),
                            QStringLiteral("model-only")}});  // 顶层 visibility
        insPart("zi2", "text",
                QStringLiteral("<total_tokens>15000000 tokens left</total_tokens>"),
                0);
        insMsg("zi3", "user", 6,
               QJsonObject{
                   {QStringLiteral("metadata"),
                    QJsonObject{{QStringLiteral("runtimeMessage"),
                                 QJsonObject{{QStringLiteral("source"),
                                              QStringLiteral(
                                                  "silent_turn_reminder")}}}}}});
        insPart("zi3", "text",
                QStringLiteral("The user hasn't heard from you in a while."), 0);
        insMsg("zm4", "user", 7);
        insPart("zm4", "text", QStringLiteral("第二问"), 0);
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("setup_z"));
    r = SessionTranscript::read(QStringLiteral("ZCode.exe"), sid, 50, 0, zRoot);
    check(r.supported, "zcode supported");
    check(r.title == QStringLiteral("ZCode 会话标题"), "zcode title");
    check(r.messages.size() == 3, "zcode tool-only + injected user rows skipped");
    if (r.messages.size() == 3) {
        check(r.messages.at(0).toObject().value("text").toString() ==
                  QStringLiteral("ZCode 问"),
              "zcode first user text");
        check(r.messages.at(1).toObject().value("text").toString() ==
                  QStringLiteral("第一段\n第二段"),
              "zcode parts joined by sequence");
        check(r.messages.at(2).toObject().value("text").toString() ==
                  QStringLiteral("第二问"),
              "zcode real user after injections");
    }
    {
        bool hasInject = false;
        for (const QJsonValue &m : r.messages) {
            const QString t = m.toObject().value("text").toString();
            if (t.contains(QStringLiteral("# Environment")) ||
                t.contains(QStringLiteral("total_tokens")) ||
                t.contains(QStringLiteral("hasn't heard"))) {
                hasInject = true;
            }
        }
        check(!hasInject, "zcode injected user rows filtered out");
    }
    check(r.total == 3 && !r.hasMore, "zcode total counts renderable only");
    {
        const auto catalog =
            SessionTranscript::listSessions(QStringLiteral("ZCode.exe"), 10, zRoot);
        check(catalog.size() == 1, "zcode session catalog count");
        if (!catalog.isEmpty()) {
            check(catalog.at(0).id == sid, "zcode catalog id");
            check(catalog.at(0).title == QStringLiteral("ZCode 会话标题"),
                  "zcode catalog title");
            check(!catalog.at(0).archived, "zcode open project not archived");
        }
    }

    // ---- OpenSquilla：transcript_entries 明文 ----
    const QString oRoot = temp.filePath("osq");
    QDir().mkpath(oRoot);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("setup_o"));
        db.setDatabaseName(oRoot + QStringLiteral("/sessions.db"));
        db.open();
        QSqlQuery q(db);
        q.exec("CREATE TABLE transcript_entries(id INTEGER PRIMARY KEY, "
               "session_key TEXT, role TEXT, content TEXT, created_at INTEGER)");
        const QString key = QStringLiteral("agent:main:cli:abcd1234");
        auto ins = [&](int id, const QString &role, const QString &text) {
            QSqlQuery i(db);
            i.prepare("INSERT INTO transcript_entries VALUES(?,?,?,?,?)");
            i.addBindValue(id);
            i.addBindValue(key);
            i.addBindValue(role);
            i.addBindValue(text);
            i.addBindValue(1794569091LL + id);  // 秒
            i.exec();
        };
        ins(1, "user", QStringLiteral("OSQ 问"));
        ins(2, "assistant", QStringLiteral("OSQ 答"));
        ins(3, "tool", QStringLiteral("工具输出不进"));
        db.close();
        r = SessionTranscript::read(QStringLiteral("OpenSquilla.exe"), key, 50, 0,
                                    oRoot);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("setup_o"));
    check(r.supported && r.messages.size() == 2, "osq parse + tool skip");
    check(r.messages.at(0).toObject().value("text").toString() ==
              QStringLiteral("OSQ 问"),
          "osq chronological");

    // ---- OpenSquilla WAL：新提交只在 -wal 里、主库还没 checkpoint 时也要看到 ----
    // 宿主库是 WAL：新行先落 sessions.db-wal，主库 mtime/size 一动不动。缓存 stamp
    // 只看主库的话，对话页永远停在上一份快照，点刷新也无效（2026-11-17 实测：
    // 主库停在 16:48，WAL 里攒了 7MB / 3 条新记录，用户报「停止同步、刷新没用」）。
    {
        const QString wRoot = temp.filePath("osq-wal");
        QDir().mkpath(wRoot);
        const QString wDb = wRoot + QStringLiteral("/sessions.db");
        const QString wKey = QStringLiteral("agent:main:cli:wal0001");
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                   QStringLiteral("setup_w"));
        db.setDatabaseName(wDb);
        check(db.open(), "osq wal: writer opens db");
        {
            QSqlQuery mode(db);
            mode.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            QSqlQuery acp(db);
            acp.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0"));  // 只让 -wal 长
            QSqlQuery ddl(db);
            check(ddl.exec(QStringLiteral(
                      "CREATE TABLE transcript_entries(id INTEGER PRIMARY KEY, "
                      "session_key TEXT, role TEXT, content TEXT, created_at INTEGER)")),
                  "osq wal: create table");
        }
        auto ins = [&](int id, const QString &role, const QString &text) {
            QSqlQuery i(db);
            i.prepare(QStringLiteral("INSERT INTO transcript_entries VALUES(?,?,?,?,?)"));
            i.addBindValue(id);
            i.addBindValue(wKey);
            i.addBindValue(role);
            i.addBindValue(text);
            i.addBindValue(1794569091LL + id);
            return i.exec();
        };
        check(ins(1, QStringLiteral("user"), QStringLiteral("WAL 旧")),
              "osq wal: first row");
        r = SessionTranscript::read(QStringLiteral("OpenSquilla.exe"), wKey, 50, 0,
                                    wRoot);
        check(r.supported && r.messages.size() == 1, "osq wal: warm cache on 1 row");
        const QFileInfo mainBefore(wDb);
        const QDateTime mtimeBefore = mainBefore.lastModified();
        const qint64 sizeBefore = mainBefore.size();
        check(ins(2, QStringLiteral("assistant"), QStringLiteral("WAL 新")),
              "osq wal: second row");
        const QFileInfo mainAfter(wDb);
        check(mainAfter.lastModified() == mtimeBefore && mainAfter.size() == sizeBefore,
              "osq wal: main db untouched (frames still in -wal)");
        r = SessionTranscript::read(QStringLiteral("OpenSquilla.exe"), wKey, 50, 0,
                                    wRoot);
        check(r.messages.size() == 2, "osq wal: cache invalidated by -wal growth");
        if (r.messages.size() == 2) {
            check(r.messages.at(1).toObject().value("text").toString() ==
                      QStringLiteral("WAL 新"),
                  "osq wal: newest row visible");
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("setup_w"));

    // ---- DSH：多帧 zstd + reasoning/tool 过滤 + session/title + 缓存失效 ----
    // fixture 由 rc_probe/gen_dsh_transcript_fixture.py 预生成（native 只 vendored
    // 解压子集，压缩靠 Python）：帧1+帧2 共 4 条对话，帧3 一条追加。
    const QString dRoot = temp.filePath("dsh");
    const QString dshSid = QStringLiteral("session-") + sid;
    const QString dFile = dRoot + QStringLiteral("/sessions/slug-x/") + dshSid +
                          QStringLiteral("/session.v4.jsonl.zstd");
    const QString fixture = QStringLiteral(FIXTURE_DIR);
    copyBytes(fixture + QStringLiteral("/dsh_session.zstd"), dFile);
    r = SessionTranscript::read(QStringLiteral("DeepSeek Harness.exe"), dshSid, 50, 0,
                                dRoot);
    check(r.supported, "dsh supported");
    check(r.title == QStringLiteral("DSH 测试标题"), "dsh title from session/title");
    check(r.messages.size() == 4, "dsh 4 msgs (reasoning+tool-only+sys-inject filtered)");
    {
        bool hasSysInject = false;
        for (const QJsonValue &m : r.messages) {
            if (m.toObject().value("text").toString().contains("Time sampled")) {
                hasSysInject = true;
            }
        }
        check(!hasSysInject, "dsh time-context system injection filtered out");
    }
    if (r.messages.size() == 4) {
        check(r.messages.at(0).toObject().value("text").toString() ==
                  QStringLiteral("第一句"),
              "dsh user text");
        check(r.messages.at(1).toObject().value("text").toString() ==
                  QStringLiteral("回复一"),
              "dsh assistant text only (reasoning/tool-call dropped)");
        check(r.messages.at(1).toObject().value("id").toString() ==
                  QStringLiteral("a1"),
              "dsh assistant id from data.message.id");
        check(r.messages.at(3).toObject().value("text").toString() ==
                  QStringLiteral("回复二"),
              "dsh multi-frame: frame2 message present");
        check(!r.messages.at(0).toObject().value("at").toString().isEmpty(),
              "dsh ts normalized");
    }
    check(r.total == 4 && !r.hasMore, "dsh total/hasMore");
    r = SessionTranscript::read(QStringLiteral("DeepSeek Harness.exe"), dshSid, 2, 0,
                                dRoot);
    check(r.messages.size() == 2 && r.hasMore, "dsh page size/hasMore");
    check(r.messages.at(0).toObject().value("text").toString() ==
              QStringLiteral("第二句"),
          "dsh page0 latest-2 starts at 第二句");
    {
        QFile f(dFile);
        f.open(QIODevice::Append);
        QFile ap(fixture + QStringLiteral("/dsh_append.zstd"));
        ap.open(QIODevice::ReadOnly);
        f.write(ap.readAll());
    }
    r = SessionTranscript::read(QStringLiteral("DeepSeek Harness.exe"), dshSid, 50, 0,
                                dRoot);
    check(r.total == 5, "dsh cache invalidated on frame append");
    check(r.messages.at(4).toObject().value("text").toString() ==
              QStringLiteral("追加句"),
          "dsh newest after append (multi-frame)");

    // ---- 不支持 / 边界 ----
    r = SessionTranscript::read(QStringLiteral("DeepSeek Harness.exe"),
                                QStringLiteral("session-abc"), 50, 0, dRoot);
    check(r.supported && !r.error.isEmpty(), "dsh missing file: supported with reason");
    r = SessionTranscript::read(QStringLiteral("Unknown.exe"), sid);
    check(!r.supported, "unknown unsupported");
    r = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"), QString());
    check(!r.error.isEmpty(), "empty session error");
    QString role, text, at;
    check(!SessionTranscript::messageOf(R"({"type":"ai-title","aiTitle":"x"})",
                                        &role, &text, &at),
          "non-message line rejected");

    // ---- 归档会话删除：备份成功才删；活跃项目拒绝；不支持宿主拒绝 ----
    {
        const QString cleanHome = temp.filePath("cleaner_home");
        const QString delSid = QStringLiteral("clean-sid-0001");
        const QString delFile = cleanHome +
            QStringLiteral("/.workbuddy/projects/slug-clean/") + delSid +
            QStringLiteral(".jsonl");
        const QString missingProject = temp.filePath("project-missing");
        writeLines(delFile, {
            QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("workspace-directories")},
                {QStringLiteral("directories"), QJsonArray{missingProject}}}).toJson(
                QJsonDocument::Compact)),
            wbLine(QStringLiteral("c1"), QStringLiteral("user"),
                   QStringLiteral("归档清理测试"), base, delSid),
        });
        const QString sidecar = delFile.left(delFile.size() - 6);
        QDir().mkpath(sidecar + QStringLiteral("/subagents"));
        QFile side(sidecar + QStringLiteral("/subagents/a.txt"));
        side.open(QIODevice::WriteOnly);
        side.write("side");
        side.close();

        const QJsonArray delItems{QJsonObject{
            {QStringLiteral("exe"), QStringLiteral("WorkBuddy.exe")},
            {QStringLiteral("session"), delSid},
            {QStringLiteral("project"), missingProject},
            {QStringLiteral("title"), QStringLiteral("归档清理测试")}}};
        QString why;
        const QJsonObject result = SessionCleaner::removeArchived(
            delItems, &why, cleanHome, temp.filePath("cleaner_trash"));
        check(result.value(QStringLiteral("deleted")).toInt() == 1,
              "cleaner deletes archived jsonl");
        check(!QFileInfo::exists(delFile), "cleaner removes session jsonl");
        check(!QFileInfo::exists(sidecar), "cleaner removes session sidecar");
        const QString backup = result.value(QStringLiteral("backup")).toString();
        check(QFileInfo::exists(backup + QStringLiteral("/manifest.json")),
              "cleaner writes manifest");
        check(QFileInfo::exists(backup + QStringLiteral("/WorkBuddy.exe/") + delSid +
                                QLatin1Char('/') + delSid + QStringLiteral(".jsonl")),
              "cleaner backup present");

        const QString liveProject = temp.filePath("cleaner_live_project");
        QDir().mkpath(liveProject);
        const QString liveSid = QStringLiteral("live-sid-0001");
        const QString liveFile = cleanHome +
            QStringLiteral("/.workbuddy/projects/slug-live/") + liveSid +
            QStringLiteral(".jsonl");
        writeLines(liveFile, {
            QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("workspace-directories")},
                {QStringLiteral("directories"), QJsonArray{liveProject}}}).toJson(
                QJsonDocument::Compact)),
            wbLine(QStringLiteral("l1"), QStringLiteral("user"),
                   QStringLiteral("活跃会话"), base, liveSid),
        });
        const QJsonArray liveItems{QJsonObject{
            {QStringLiteral("exe"), QStringLiteral("WorkBuddy.exe")},
            {QStringLiteral("session"), liveSid},
            {QStringLiteral("project"), liveProject},
            {QStringLiteral("title"), QStringLiteral("活跃会话")}}};
        const QJsonObject liveResult = SessionCleaner::removeArchived(
            liveItems, &why, cleanHome, temp.filePath("cleaner_trash2"));
        check(liveResult.value(QStringLiteral("deleted")).toInt() == 0,
              "cleaner refuses active project");
        check(liveResult.value(QStringLiteral("failed")).toArray().size() == 1,
              "cleaner reports active refusal");
        check(QFileInfo::exists(liveFile), "cleaner keeps active session");

        const QJsonArray badItems{QJsonObject{
            {QStringLiteral("exe"), QStringLiteral("OpenSquilla.exe")},
            {QStringLiteral("session"), QStringLiteral("agent:main:x:y")}}};
        const QJsonObject badResult = SessionCleaner::removeArchived(
            badItems, &why, cleanHome, temp.filePath("cleaner_trash3"));
        check(badResult.value(QStringLiteral("deleted")).toInt() == 0,
              "cleaner refuses unsupported host");
        check(!SessionCleaner::supportsHost(QStringLiteral("OpenSquilla.exe")),
              "cleaner supported-host matrix");
    }

    std::printf(ok ? "ALL PASS transcript (%d checks)\n"
                   : "FAIL transcript (%d checks)\n",
                checks);
    return ok ? 0 : 1;
}