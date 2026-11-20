// SessionTranscript 自测：jsonl 两记法 / 分页游标 / 包装剥离 / ZCode 与
// OpenSquilla SQLite / 缓存失效 / 不支持宿主。全部用临时库，不碰真实会话库。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>

#include "core/SessionCleaner.h"
#include "core/SessionDeleteQueue.h"
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
            // ZCode 桌面端的无项目任务工作区：三种命名都见过（本机 2026-11-18）。
            QStringLiteral(
                "C:\\Users\\x\\.zcode\\workspace\\draft-e8dee688-0aef-495f-"
                "8469-72d0382bb317"),
            QStringLiteral(
                "C:/Users/x/.zcode/workspace/1794013923425-99fb64c5-393d-"
                "4bee-86b0-8b3d3cda47bf"),
            QStringLiteral("C:/Users/x/.zcode/workspace/default"),
        };
        for (const QString &dir : autoDirs) {
            check(SessionTranscript::generatedWorkspace(dir),
                  "generated workspace detected");
        }
        const QStringList realDirs{
            QStringLiteral("D:/githubrepos/Agentrmc"),
            QStringLiteral("C:/Users/x/Documents/report-2026"),
            // .zcode 下但不在 workspace/ 的路径不算：不能整个宿主目录一刀切。
            QStringLiteral("C:/Users/x/.zcode/skills/demo"),
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
    // --chat <exe> <session-id>：对真实会话冒烟，顺带打印每条的 files
    // （present_files 摆出来的产物，聊天页用它画下载卡片）。
    if (argc >= 4 && QByteArray(argv[1]) == "--chat") {
        const QString exe = QString::fromUtf8(argv[2]);
        const QString sid = QString::fromUtf8(argv[3]);
        const auto r = SessionTranscript::read(exe, sid, 8, 0);
        std::printf("supported=%d total=%d error=%s\n", r.supported, r.total,
                    r.error.toUtf8().constData());
        for (const QJsonValue &m : r.messages) {
            const QJsonObject o = m.toObject();
            const QJsonArray files = o.value(QStringLiteral("files")).toArray();
            std::printf("[%s] files=%d %s\n",
                        o.value(QStringLiteral("role")).toString().toUtf8().constData(),
                        static_cast<int>(files.size()),
                        o.value(QStringLiteral("text")).toString().left(50)
                            .toUtf8().constData());
            for (const QJsonValue &f : files) {
                std::printf("    -> %s\n", f.toString().toUtf8().constData());
            }
        }
        return r.supported ? 0 : 1;
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

    // ---- present_files 摆出来的产物要挂到 assistant 消息上 ----
    {
        // 正文只写「这个包只有 .hwt」、路径只在工具结果里时，聊天页以前既没有
        // 下载卡片、产物面板也扫不到（2026-11-19 凝蓝_OS8.hwt）。
        const QString sid4 = QStringLiteral("cccccccc-dddd-4eee-8fff-000000000000");
        const QJsonObject presented{
            {QStringLiteral("type"), QStringLiteral("function_call_result")},
            {QStringLiteral("name"), QStringLiteral("present_files")},
            {QStringLiteral("output"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("input_text")},
                 {QStringLiteral("text"),
                  QString::fromUtf8(QJsonDocument(QJsonObject{
                      {QStringLiteral("files"),
                       QJsonArray{QStringLiteral("d:/w/产物/OS8成品/凝蓝_OS8.hwt"),
                                  QStringLiteral("d:/w/产物/预览.jpg")}}})
                                        .toJson(QJsonDocument::Compact))}}}}};
        writeLines(wbHome + QStringLiteral("/projects/slug-d/") + sid4 +
                       QStringLiteral(".jsonl"),
                   {QStringLiteral(R"({"type":"message","cwd":"D:/w"})"),
                    wbLine("u1", "user", QStringLiteral("做个主题包"),
                           base + 7000, sid4),
                    QString::fromUtf8(
                        QJsonDocument(presented).toJson(QJsonDocument::Compact)),
                    wbLine("a1", "assistant", QStringLiteral("这个包只有 .hwt"),
                           base + 8000, sid4)});
        const auto rr = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"),
                                                sid4, 50, 0, wbHome);
        bool attached = false;
        for (const QJsonValue &v : rr.messages) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("role")).toString() !=
                QStringLiteral("assistant")) {
                continue;
            }
            const QJsonArray files = o.value(QStringLiteral("files")).toArray();
            if (files.size() == 2 &&
                files.at(0).toString().endsWith(QStringLiteral("凝蓝_OS8.hwt"))) {
                attached = true;
            }
        }
        check(attached, "present_files attached to assistant message");
    }

    // ---- 宿主注入的 user 行（后台任务通知）不能当用户气泡 ----
    {
        // WorkBuddy 把「后台命令跑完了」写成 user 角色的一坨 XML；以前原样渲染成
        // 「我」的气泡（2026-11-19 用户截图）。摘要/本地命令回显同理。
        const QString sid5 = QStringLiteral("dddddddd-eeee-4fff-8000-111111111111");
        writeLines(wbHome + QStringLiteral("/projects/slug-e/") + sid5 +
                       QStringLiteral(".jsonl"),
                   {QStringLiteral(R"({"type":"message","cwd":"D:/x"})"),
                    wbLine("n1", "user",
                           QStringLiteral("<task-notification>\n<task-id>67jRnr</task-id>\n"
                                          "<status>completed</status>\n</task-notification>"),
                           base + 10000, sid5),
                    wbLine("n2", "user",
                           QStringLiteral("<cb_summary>Summary of the conversation</cb_summary>"),
                           base + 11000, sid5),
                    wbLine("u1", "user", QStringLiteral("把主题包再核一遍"),
                           base + 12000, sid5),
                    wbLine("a1", "assistant", QStringLiteral("核完了，全绿"),
                           base + 13000, sid5)});
        const auto rr = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"),
                                                sid5, 50, 0, wbHome);
        // 通知变成一行系统信息（不再当「我」的气泡）；cb_summary 直接丢掉。
        check(rr.messages.size() == 3, "harness user rows filtered");
        if (rr.messages.size() == 3) {
            check(rr.messages.at(0).toObject().value(QStringLiteral("role")).toString() ==
                      QStringLiteral("system"),
                  "task notification becomes a system row");
            const QJsonObject first = rr.messages.at(1).toObject();
            check(first.value(QStringLiteral("role")).toString() ==
                      QStringLiteral("user") &&
                      first.value(QStringLiteral("text")).toString() ==
                          QStringLiteral("把主题包再核一遍"),
                  "first bubble is the real user text");
        }
        check(!rr.title.contains(QStringLiteral("task-notification")),
              "title not taken from harness row");
    }

    // ---- 长耗时工具调用 / 后台任务 → 一行系统信息 ----
    {
        // Agent 闷头跑工具时页面不能「什么都不显示」：≥2s 的调用落一行
        // 「执行命令 5.0s · …」，快的调用不刷屏，还在跑的那条标「进行中」。
        const QString sid6 = QStringLiteral("eeeeeeee-ffff-4000-8111-222222222222");
        auto call = [&sid6](const QString &id, const QString &name,
                            const QJsonObject &args, qint64 ts) {
            return QString::fromUtf8(
                QJsonDocument(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("function_call")},
                    {QStringLiteral("callId"), id},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("arguments"), args},
                    {QStringLiteral("timestamp"), static_cast<double>(ts)},
                    {QStringLiteral("sessionId"), sid6}})
                    .toJson(QJsonDocument::Compact));
        };
        auto result = [&sid6](const QString &id, qint64 ts) {
            return QString::fromUtf8(
                QJsonDocument(QJsonObject{
                    {QStringLiteral("type"),
                     QStringLiteral("function_call_result")},
                    {QStringLiteral("callId"), id},
                    {QStringLiteral("status"), QStringLiteral("completed")},
                    {QStringLiteral("timestamp"), static_cast<double>(ts)},
                    {QStringLiteral("sessionId"), sid6}})
                    .toJson(QJsonDocument::Compact));
        };
        writeLines(wbHome + QStringLiteral("/projects/slug-f/") + sid6 +
                       QStringLiteral(".jsonl"),
                   {QStringLiteral(R"({"type":"message","cwd":"D:/x"})"),
                    wbLine("u1", "user", QStringLiteral("重新构建一版"),
                           base + 14000, sid6),
                    call(QStringLiteral("c1"), QStringLiteral("Bash"),
                         QJsonObject{{QStringLiteral("command"),
                                      QStringLiteral("python build.py\n--fast")}},
                         base + 15000),
                    result(QStringLiteral("c1"), base + 20000),  // 5.0s
                    call(QStringLiteral("c2"), QStringLiteral("Read"),
                         QJsonObject{{QStringLiteral("file_path"),
                                      QStringLiteral("D:/x/a.md")}},
                         base + 21000),
                    result(QStringLiteral("c2"), base + 21100),  // 0.1s，不露
                    wbLine("a1", "assistant", QStringLiteral("这版构建好了"),
                           base + 22000, sid6),
                    call(QStringLiteral("c3"), QStringLiteral("Bash"),
                         QJsonObject{{QStringLiteral("command"),
                                      QStringLiteral("python build.py --slow")}},
                         base + 23000),  // 没有结果 → 还在跑
                    QString::fromUtf8(
                        QJsonDocument(QJsonObject{
                            {QStringLiteral("type"), QStringLiteral("message")},
                            {QStringLiteral("role"), QStringLiteral("user")},
                            {QStringLiteral("sessionId"), sid6},
                            {QStringLiteral("timestamp"),
                             static_cast<double>(base + 24000)},
                            {QStringLiteral("content"),
                             QJsonArray{QJsonObject{
                                 {QStringLiteral("type"),
                                  QStringLiteral("input_text")},
                                 {QStringLiteral("text"),
                                  QStringLiteral("<task-notification>\n<status>completed"
                                                 "</status>\n<summary>Background command "
                                                 "&quot;python build.py&quot;</summary>"
                                                 "\n</task-notification>")}}}}})
                                .toJson(QJsonDocument::Compact))});
        const auto rr = SessionTranscript::read(QStringLiteral("WorkBuddyAI.exe"),
                                                sid6, 50, 0, wbHome);
        int sysRows = 0;
        bool longCall = false, running = false, note = false;
        for (const QJsonValue &v : rr.messages) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("role")).toString() !=
                QStringLiteral("system")) {
                continue;
            }
            ++sysRows;
            const QString t = o.value(QStringLiteral("text")).toString();
            if (t.startsWith(QStringLiteral("执行命令 5.0s")) &&
                t.contains(QStringLiteral("python build.py"))) {
                longCall = true;
            }
            if (t.startsWith(QStringLiteral("进行中 · 执行命令"))) {
                running = true;
            }
            if (t.startsWith(QStringLiteral("后台任务完成"))) {
                note = true;
            }
            check(!t.contains(QStringLiteral("a.md")), "fast call not shown as system row");
        }
        check(longCall, "slow tool call becomes a system row");
        check(running, "in-flight tool call shown as running");
        check(note, "task notification becomes a system row");
        check(sysRows == 3, "only 3 system rows (slow + running + note)");
        // 气泡本体不能少
        bool sawAssistant = false;
        for (const QJsonValue &v : rr.messages) {
            if (v.toObject().value(QStringLiteral("role")).toString() ==
                QStringLiteral("assistant")) {
                sawAssistant = true;
            }
        }
        check(sawAssistant, "assistant bubble kept alongside system rows");
    }

    // ---- OpenSquilla：每轮耗时 → 一行系统信息 ----
    {
        const QString tRoot = temp.filePath("osq-turns");
        QDir().mkpath(tRoot);
        const QString key = QStringLiteral("agent:main:cli:turns1");
        const qint64 t0 = 1794569000000LL;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("setup_ot"));
            db.setDatabaseName(tRoot + QStringLiteral("/sessions.db"));
            db.open();
            QSqlQuery q(db);
            q.exec("CREATE TABLE transcript_entries(id INTEGER PRIMARY KEY, "
                   "session_key TEXT, role TEXT, content TEXT, created_at INTEGER)");
            q.exec("CREATE TABLE agent_tasks(task_id TEXT, session_key TEXT, "
                   "status TEXT, started_at INTEGER, finished_at INTEGER, "
                   "error_message TEXT)");
            auto entry = [&](int id, const QString &role, const QString &text,
                             qint64 ms) {
                QSqlQuery i(db);
                i.prepare("INSERT INTO transcript_entries VALUES(?,?,?,?,?)");
                i.addBindValue(id);
                i.addBindValue(key);
                i.addBindValue(role);
                i.addBindValue(text);
                i.addBindValue(ms);
                i.exec();
            };
            entry(1, "user", QStringLiteral("OSQ 长轮"), t0);
            entry(2, "assistant", QStringLiteral("OSQ 答完"), t0 + 5000);
            auto task = [&](const QString &id, const QString &status,
                            qint64 started, qint64 finished, const QString &err) {
                QSqlQuery i(db);
                i.prepare("INSERT INTO agent_tasks VALUES(?,?,?,?,?,?)");
                i.addBindValue(id);
                i.addBindValue(key);
                i.addBindValue(status);
                i.addBindValue(started);
                i.addBindValue(finished);
                i.addBindValue(err);
                i.exec();
            };
            // 2m5s 的一轮要露一行；5s 的短轮不露。
            task(QStringLiteral("t1"), QStringLiteral("succeeded"), t0,
                 t0 + 125000, QString());
            task(QStringLiteral("t2"), QStringLiteral("succeeded"), t0 + 126000,
                 t0 + 131000, QString());
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("setup_ot"));
        const auto rr = SessionTranscript::read(QStringLiteral("OpenSquilla.exe"),
                                                key, 50, 0, tRoot);
        check(rr.messages.size() == 3, "osq turn row added");
        if (rr.messages.size() == 3) {
            const QJsonObject last = rr.messages.at(2).toObject();
            const QString t = last.value(QStringLiteral("text")).toString();
            check(last.value(QStringLiteral("role")).toString() ==
                      QStringLiteral("system") &&
                      t.startsWith(QStringLiteral("本轮 2m5s")) &&
                      t.contains(QStringLiteral("完成")),
                  "osq turn row text");
        }
    }

    // ---- DSH：工具调用耗时 → 一行系统信息 ----
    {
        // 夹具是 zstd 压好的（vendored libzstd 只编解压子集，测试里压不了），
        // 生成脚本见 tests/fixtures/dsh/make_fixture.py。
        const auto rr = SessionTranscript::read(
            QStringLiteral("DeepSeek Harness.exe"),
            QStringLiteral("session-abc123"), 50, 0,
            QStringLiteral(FIXTURE_DIR) + QStringLiteral("/dsh"));
        check(rr.messages.size() == 3, "dsh tool row added");
        if (rr.messages.size() == 3) {
            const QJsonObject sys = rr.messages.at(1).toObject();
            check(sys.value(QStringLiteral("role")).toString() ==
                      QStringLiteral("system") &&
                      sys.value(QStringLiteral("text")).toString() ==
                          QStringLiteral("执行命令 5.0s · 构建前端"),
                  "dsh tool row text");
            check(rr.messages.at(2).toObject().value(QStringLiteral("text"))
                      .toString() == QStringLiteral("构建好了"),
                  "dsh assistant bubble kept");
        }
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
        // 桌面端索引要标记的两条：归档 / 删除。cli 库没有 directory 列，
        // 项目为空不按目录判归档，archived 只能来自 v2 任务索引。
        q.exec("INSERT INTO session VALUES('sess_arc','已归档任务')");
        q.exec("INSERT INTO session VALUES('sess_del','已删除任务')");
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
    // 桌面端任务索引（v2/tasks-index.sqlite）：archived=1 / deleted=1 各标一条。
    // 列名与真实库一致；cli 库的 time_archived 列在本 fixture 里根本没建。
    {
        QDir().mkpath(zRoot + QStringLiteral("/v2"));
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("setup_zv2"));
        db.setDatabaseName(zRoot + QStringLiteral("/v2/tasks-index.sqlite"));
        db.open();
        QSqlQuery q(db);
        q.exec("CREATE TABLE tasks(task_id TEXT, title TEXT, "
               "archived INTEGER, deleted INTEGER)");
        q.exec("INSERT INTO tasks VALUES('sess_arc','已归档任务',1,0)");
        q.exec("INSERT INTO tasks VALUES('sess_del','已删除任务',0,1)");
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("setup_zv2"));
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
        check(catalog.size() == 3, "zcode session catalog count");
        bool openArchived = true;
        bool deskArchived = false;
        bool deskDeleted = false;
        for (const auto &info : catalog) {
            if (info.id == sid) {
                openArchived = info.archived;
            } else if (info.id == QStringLiteral("sess_arc")) {
                deskArchived = info.archived;
            } else if (info.id == QStringLiteral("sess_del")) {
                deskDeleted = info.archived;
            }
        }
        check(!openArchived, "zcode open project not archived");
        check(deskArchived, "zcode desktop archived task marked archived");
        check(deskDeleted, "zcode desktop deleted task marked archived");
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

    // ---- OpenSquilla 会话目录标题：通道占位名「WebChat」不能吞掉真标题 ----
    // 宿主把每个 webchat 会话的 display_name 都写成「WebChat」，真标题在
    // derived_title；两个都没时退首条用户正文（带运行时时间头 / JSON 包壳），
    // 全没有才给短 id（用户 2026-11-19 报：派发页整屏都叫 WebChat）。
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                   QStringLiteral("setup_o_cat"));
        db.setDatabaseName(oRoot + QStringLiteral("/sessions.db"));
        check(db.open(), "osq catalog: writer opens db");
        QSqlQuery ddl(db);
        check(ddl.exec("CREATE TABLE sessions(session_key TEXT PRIMARY KEY, "
                       "display_name TEXT, derived_title TEXT, subject TEXT, "
                       "label TEXT, updated_at INTEGER, workspace_id TEXT)"),
              "osq catalog: sessions ddl");
        check(ddl.exec("CREATE TABLE project_workspaces(workspace_id TEXT, "
                       "path TEXT, removed_at INTEGER)"),
              "osq catalog: workspaces ddl");
        const qint64 nowMs = 1795070016446LL;
        auto sess = [&](const QString &key, const QString &dn, const QString &dt,
                        int idx) {
            QSqlQuery i(db);
            i.prepare("INSERT INTO sessions VALUES(?,?,?,?,?,?,?)");
            i.addBindValue(key);
            i.addBindValue(dn.isNull() ? QVariant() : QVariant(dn));
            i.addBindValue(dt.isNull() ? QVariant() : QVariant(dt));
            i.addBindValue(QVariant());
            i.addBindValue(QVariant());
            i.addBindValue(nowMs - idx);
            i.addBindValue(QStringLiteral("w") + QString::number(idx));
            check(i.exec(), "osq catalog: session insert");
        };
        sess(QStringLiteral("agent:main:webchat:aaaaaaaa"),
             QStringLiteral("WebChat"), QStringLiteral("真实AI标题"), 1);
        sess(QStringLiteral("agent:main:webchat:bbbbbbbb"),
             QStringLiteral("WebChat"), QString(), 2);
        sess(QStringLiteral("agent:main:webchat:cccccccc"),
             QStringLiteral("WebChat"), QString(), 3);
        sess(QStringLiteral("agent:main:cli:dddddddd"), QString(), QString(), 4);
        auto user = [&](const QString &key, int id, const QString &text) {
            QSqlQuery i(db);
            i.prepare("INSERT INTO transcript_entries VALUES(?,?,?,?,?)");
            i.addBindValue(id);
            i.addBindValue(key);
            i.addBindValue(QStringLiteral("user"));
            i.addBindValue(text);
            i.addBindValue(1794569091LL + id);
            i.exec();
        };
        user(QStringLiteral("agent:main:webchat:bbbbbbbb"), 101,
             QStringLiteral("[2026-11-19T14:20+08:00 Thu Asia/Shanghai]\n帮我打包"));
        user(QStringLiteral("agent:main:webchat:cccccccc"), 102,
             QStringLiteral(R"({"text":"研究MotionHarness优化技能"})"));
        db.close();
        QSqlDatabase::removeDatabase(QStringLiteral("setup_o_cat"));
        const auto cat = SessionTranscript::listSessions(
            QStringLiteral("OpenSquilla.exe"), 50, oRoot);
        QHash<QString, QString> byId;
        for (const auto &row : cat) {
            byId.insert(row.id, row.title);
        }
        check(byId.value(QStringLiteral("agent:main:webchat:aaaaaaaa")) ==
                  QStringLiteral("真实AI标题"),
              "osq catalog: placeholder WebChat yields to derived_title");
        check(byId.value(QStringLiteral("agent:main:webchat:bbbbbbbb")) ==
                  QStringLiteral("帮我打包"),
              "osq catalog: runtime header stripped from first user");
        check(byId.value(QStringLiteral("agent:main:webchat:cccccccc")) ==
                  QStringLiteral("研究MotionHarness优化技能"),
              "osq catalog: JSON content unwrapped");
        check(byId.value(QStringLiteral("agent:main:cli:dddddddd")) ==
                  QStringLiteral("会话 dddddddd"),
              "osq catalog: empty falls back to short id");
        // 聊天页头部同源：read 也要解出真标题而不是 WebChat。
        const auto rt = SessionTranscript::read(
            QStringLiteral("OpenSquilla.exe"),
            QStringLiteral("agent:main:webchat:aaaaaaaa"), 50, 0, oRoot);
        check(rt.title == QStringLiteral("真实AI标题"),
              "osq read title skips placeholder");
        const auto rt2 = SessionTranscript::read(
            QStringLiteral("OpenSquilla.exe"),
            QStringLiteral("agent:main:webchat:bbbbbbbb"), 50, 0, oRoot);
        check(rt2.title == QStringLiteral("帮我打包"),
              "osq read title falls back to first user");
    }

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

    // ---- 归档会话删除（回收站口径）：移动可恢复；活跃拒绝；越界拒绝 ----
    {
        const QString cleanHome = temp.filePath("cleaner_home");
        const QString trashBase = temp.filePath("cleaner_trash");
        const auto ageFile = [](const QString &p) {
            // Qt 的 QFile::setFileTime 对未打开文件在这里报「拒绝访问」；
            // 直接走 Win32：CreateFile(FILE_WRITE_ATTRIBUTES)+SetFileTime。
            HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(p.utf16()),
                                   FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                std::printf("FAIL age open %lu %s\n", GetLastError(),
                            p.toUtf8().constData());
                return;
            }
            SYSTEMTIME st{};
            st.wYear = 2026; st.wMonth = 1; st.wDay = 1;
            FILETIME local{};
            FILETIME utc{};
            SystemTimeToFileTime(&st, &local);
            LocalFileTimeToFileTime(&local, &utc);
            SetFileTime(h, nullptr, nullptr, &utc);
            CloseHandle(h);
        };
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
        // 刚写的 fixture 会被 60s 活跃窗口拦住——把 mtime 改陈，模拟真正的旧归档。
        ageFile(delFile);
        ageFile(sidecar + QStringLiteral("/subagents/a.txt"));

        const QJsonArray delItems{QJsonObject{
            {QStringLiteral("exe"), QStringLiteral("WorkBuddy.exe")},
            {QStringLiteral("session"), delSid},
            {QStringLiteral("project"), missingProject},
            {QStringLiteral("title"), QStringLiteral("归档清理测试")}}};
        QString why;
        const QJsonObject result = SessionCleaner::removeSessions(
            delItems, &why, cleanHome, trashBase);
        check(result.value(QStringLiteral("deleted")).toInt() == 1,
              "cleaner moves archived jsonl to trash");
        check(!QFileInfo::exists(delFile), "cleaner removes session jsonl");
        check(!QFileInfo::exists(sidecar), "cleaner removes session sidecar");
        const QString key0 = result.value(QStringLiteral("entries")).toArray()
                                 .at(0).toString();
        const QString entry0 = trashBase + QStringLiteral("/entries/") + key0;
        check(QFileInfo::exists(entry0 + QStringLiteral("/entry.json")),
              "cleaner writes entry manifest");
        check(QFileInfo::exists(entry0 + QStringLiteral("/files/0-") + delSid +
                                QStringLiteral(".jsonl")),
              "entry holds the jsonl");
        check(QFileInfo::exists(entry0 + QStringLiteral("/files/1-") + delSid +
                                QStringLiteral("/subagents/a.txt")),
              "entry holds the sidecar");

        // 列表→恢复→回位；回收站条目消失。
        const QJsonArray tRows = SessionCleaner::listTrash(trashBase);
        check(tRows.size() == 1 &&
                  tRows.at(0).toObject().value(QStringLiteral("key")).toString() ==
                      key0 &&
                  tRows.at(0).toObject().value(QStringLiteral("session")).toString() ==
                      delSid,
              "listTrash sees the entry");
        QString rerr;
        const QJsonObject rr =
            SessionCleaner::restore(key0, &rerr, cleanHome, trashBase);
        check(rr.value(QStringLiteral("ok")).toBool(),
              QStringLiteral("restore ok (%1)").arg(rerr).toUtf8().constData());
        check(QFileInfo::exists(delFile) &&
                  QFileInfo::exists(sidecar + QStringLiteral("/subagents/a.txt")),
              "restore puts files back");
        QFile restoredFile(sidecar + QStringLiteral("/subagents/a.txt"));
        const bool opened = restoredFile.open(QIODevice::ReadOnly);
        const QByteArray restoredBytes = opened ? restoredFile.readAll() : QByteArray();
        restoredFile.close();  // 不关掉句柄，下一轮删除会被自己锁住（Windows）
        check(opened && restoredBytes == QByteArray("side"),
              QStringLiteral("restored content intact (opened=%1 bytes=%2)")
                  .arg(opened)
                  .arg(QString::fromUtf8(restoredBytes.toHex()))
                  .toUtf8()
                  .constData());
        check(SessionCleaner::listTrash(trashBase).isEmpty(),
              "restore removes the entry");

        // 再删一次→原位被占→整体拒绝且不动任何文件；腾开后能恢复。
        const QJsonObject r2 =
            SessionCleaner::removeSessions(delItems, &why, cleanHome, trashBase);
        check(r2.value(QStringLiteral("deleted")).toInt() == 1,
              "second delete moves to trash again");
        const QString key2 = r2.value(QStringLiteral("entries")).toArray()
                                 .at(0).toString();
        {
            QFile occ(delFile);
            occ.open(QIODevice::WriteOnly);
            occ.write("宿主又建了同 id 的新会话");
        }
        rerr.clear();
        const QJsonObject rr2 =
            SessionCleaner::restore(key2, &rerr, cleanHome, trashBase);
        check(!rr2.value(QStringLiteral("ok")).toBool() &&
                  rerr.contains(QStringLiteral("原位已有东西")),
              "restore refuses when the slot is taken");
        check(QFileInfo::exists(delFile) &&
                  SessionCleaner::listTrash(trashBase).size() == 1,
              "refused restore touched nothing");
        QFile::remove(delFile);
        check(SessionCleaner::restore(key2, &rerr, cleanHome, trashBase)
                  .value(QStringLiteral("ok"))
                  .toBool(),
              "restore works again once the slot is free");
        ageFile(delFile);
        ageFile(sidecar + QStringLiteral("/subagents/a.txt"));

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
        // 对 Magpie：不再要求归档——项目目录还在的会话照样可删；刚写的
        // 由 60s 活跃窗口拦在下面的 hot 用例测。这里先把 mtime 改陈再删。
        ageFile(liveFile);
        const QJsonObject liveResult = SessionCleaner::removeSessions(
            liveItems, &why, cleanHome, temp.filePath("cleaner_trash2"));
        check(liveResult.value(QStringLiteral("deleted")).toInt() == 1,
              "cleaner deletes non-archived session too");
        check(!QFileInfo::exists(liveFile),
              "cleaner removes live-project jsonl too");

        const QJsonArray badItems{QJsonObject{
            {QStringLiteral("exe"), QStringLiteral("OpenSquilla.exe")},
            {QStringLiteral("session"), QStringLiteral("agent:main:x:y")}}};
        const QJsonObject badResult = SessionCleaner::removeSessions(
            badItems, &why, cleanHome, temp.filePath("cleaner_trash3"));
        check(badResult.value(QStringLiteral("deleted")).toInt() == 0,
              "cleaner refuses unsupported host");
        check(!SessionCleaner::supportsHost(QStringLiteral("OpenSquilla.exe")),
              "cleaner supported-host matrix");

        // 活跃窗口：刚写过的归档会话不碰（Magpie ActiveWindow 口径）。
        const QString hotSid = QStringLiteral("hot-sid-0001");
        const QString hotFile = cleanHome +
            QStringLiteral("/.workbuddy/projects/slug-hot/") + hotSid +
            QStringLiteral(".jsonl");
        writeLines(hotFile, {
            QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("workspace-directories")},
                {QStringLiteral("directories"), QJsonArray{missingProject}}}).toJson(
                QJsonDocument::Compact)),
            wbLine(QStringLiteral("h1"), QStringLiteral("user"),
                   QStringLiteral("热会话"), base, hotSid),
        });
        const QJsonArray hotItems{QJsonObject{
            {QStringLiteral("exe"), QStringLiteral("WorkBuddy.exe")},
            {QStringLiteral("session"), hotSid},
            {QStringLiteral("project"), missingProject},
            {QStringLiteral("title"), QStringLiteral("热会话")}}};
        const QJsonObject hotResult = SessionCleaner::removeSessions(
            hotItems, &why, cleanHome, trashBase);
        check(hotResult.value(QStringLiteral("deleted")).toInt() == 0 &&
                  hotResult.value(QStringLiteral("failed")).toArray()
                      .at(0).toObject()
                      .value(QStringLiteral("error")).toString()
                      .contains(QStringLiteral("一分钟内有写入")) &&
                  QFileInfo::exists(hotFile),
              "active window refuses the trash move");

        // 会话 id / 回收站 key 的路径穿越全拒。
        const QJsonArray evilItems{QJsonObject{
            {QStringLiteral("exe"), QStringLiteral("WorkBuddy.exe")},
            {QStringLiteral("session"), QStringLiteral("../../evil")}}};
        const QJsonObject evilResult = SessionCleaner::removeSessions(
            evilItems, &why, cleanHome, trashBase);
        check(evilResult.value(QStringLiteral("deleted")).toInt() == 0 &&
                  evilResult.value(QStringLiteral("failed")).toArray()
                      .at(0).toObject()
                      .value(QStringLiteral("error")).toString()
                      .contains(QStringLiteral("不合法")),
              "session id traversal refused");
        QString kerr;
        check(!SessionCleaner::purge(QStringLiteral("WorkBuddy.exe/../../evil"),
                                     &kerr, trashBase)
                   .value(QStringLiteral("ok"))
                   .toBool(),
              "trash key traversal refused");

        // purge 单条 / purgeAll 清空：只删条目目录，不碰宿主已消失的事实。
        const QJsonObject r3 =
            SessionCleaner::removeSessions(delItems, &why, cleanHome, trashBase);
        check(r3.value(QStringLiteral("deleted")).toInt() == 1,
              "third delete moves to trash");
        const QString key3 = r3.value(QStringLiteral("entries")).toArray()
                                 .at(0).toString();
        const QString entry3 = trashBase + QStringLiteral("/entries/") + key3;
        check(QFileInfo::exists(entry3), "entry exists before purge");
        QString perr;
        check(SessionCleaner::purge(key3, &perr, trashBase)
                  .value(QStringLiteral("ok"))
                  .toBool(),
              "purge removes one entry");
        check(!QFileInfo::exists(entry3) && !QFileInfo::exists(delFile),
              "purged entry gone, host file stays gone");
        QString paErr;
        const QJsonObject pa = SessionCleaner::purgeAll(&paErr, trashBase);
        check(!pa.value(QStringLiteral("ok")).toBool() &&
                  paErr.contains(QStringLiteral("空")),
              "purgeAll on empty reports, never touches hosts");
    }

    // ---- OpenSquilla 真删队列：入队/一次取件/ack 终态/在途去重/取件超时 ----
    {
        SessionDeleteQueue q(nullptr, 250);  // 取件超时缩到 250ms，别拖测试
        QStringList finishedIds;
        QStringList finishedErrs;
        int okCount = 0;
        QObject::connect(&q, &SessionDeleteQueue::finished,
                         [&](const QString &id, const QString &, const QString &,
                             bool ok, const QString &error) {
                             finishedIds.append(id);
                             finishedErrs.append(error);
                             if (ok) ++okCount;
                         });
        const QJsonArray itemA{QJsonObject{
            {QStringLiteral("session"), QStringLiteral("agent:main:webchat:a")},
            {QStringLiteral("title"), QStringLiteral("甲")}}};
        const QJsonArray items{itemA.at(0),
                               QJsonObject{{QStringLiteral("session"),
                                           QStringLiteral("agent:main:webchat:b")},
                                           {QStringLiteral("title"), QStringLiteral("乙")}}};
        QStringList ids;
        const int n = q.enqueue(items, &ids);
        const QJsonArray first = q.takePending();
        const QJsonArray again = q.takePending();
        check(n == 2 && first.size() == 2 && again.isEmpty() &&
                  first.at(0).toObject().value(QStringLiteral("delete_id")).toString() ==
                      ids.at(0),
              "delete queue: enqueue + take-once");
        const int dup = q.enqueue(itemA, nullptr);
        check(dup == 0, "delete queue: same session in-flight not re-queued");
        const bool acked = q.ack(ids.at(0), true, QString());
        check(acked && finishedIds.size() == 1 && okCount == 1,
              "delete queue: ack ok -> finished");
        check(!q.ack(QStringLiteral("nope"), true, QString()),
              "delete queue: unknown id refused");
        const int re = q.enqueue(itemA, nullptr);
        check(re == 1, "delete queue: session re-enqueues after its ack");
        // 未 ack 的两条（b + 重新入队的 a）靠取件超时兜底报失败。
        QEventLoop loop;
        QTimer::singleShot(600, &loop, &QEventLoop::quit);
        loop.exec();
        int timeouts = 0;
        for (const QString &why : finishedErrs) {
            if (why.contains(QStringLiteral("取件"))) ++timeouts;
        }
        check(finishedIds.size() == 3 && okCount == 1 && timeouts == 2,
              "delete queue: take timeout finishes with reason");
    }

    std::printf(ok ? "ALL PASS transcript (%d checks)\n"
                   : "FAIL transcript (%d checks)\n",
                checks);
    return ok ? 0 : 1;
}