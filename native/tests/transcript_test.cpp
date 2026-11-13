// SessionTranscript 自测：jsonl 两记法 / 分页游标 / 包装剥离 / ZCode 与
// OpenSquilla SQLite / 缓存失效 / 不支持宿主。全部用临时库，不碰真实会话库。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <cstdio>

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
        auto insMsg = [&](const QString &id, const QString &role, int seq) {
            QSqlQuery m(db);
            m.prepare("INSERT INTO message VALUES(?,?,?,?,?,?)");
            m.addBindValue(id);
            m.addBindValue(zsid);
            m.addBindValue(base + seq * 1000);
            m.addBindValue(base + seq * 1000);
            m.addBindValue(QStringLiteral(R"({"role":")") + role + QStringLiteral(R"("})"));
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
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("setup_z"));
    r = SessionTranscript::read(QStringLiteral("ZCode.exe"), sid, 50, 0, zRoot);
    check(r.supported, "zcode supported");
    check(r.title == QStringLiteral("ZCode 会话标题"), "zcode title");
    check(r.messages.size() == 2, "zcode tool-only skipped");
    if (r.messages.size() == 2) {
        check(r.messages.at(1).toObject().value("text").toString() ==
                  QStringLiteral("第一段\n第二段"),
              "zcode parts joined by sequence");
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

    // ---- 不支持 / 边界 ----
    r = SessionTranscript::read(QStringLiteral("DeepSeek Harness.exe"),
                                QStringLiteral("session-abc"));
    check(!r.supported && !r.error.isEmpty(), "dsh unsupported with reason");
    r = SessionTranscript::read(QStringLiteral("Unknown.exe"), sid);
    check(!r.supported, "unknown unsupported");
    r = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"), QString());
    check(!r.error.isEmpty(), "empty session error");
    QString role, text, at;
    check(!SessionTranscript::messageOf(R"({"type":"ai-title","aiTitle":"x"})",
                                        &role, &text, &at),
          "non-message line rejected");

    std::printf(ok ? "ALL PASS transcript (%d checks)\n"
                   : "FAIL transcript (%d checks)\n",
                checks);
    return ok ? 0 : 1;
}