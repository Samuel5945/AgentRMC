#include "core/SessionDispatch.h"
#include "core/UiDispatch.h"
#include "core/AgentWindow.h"
#include "core/ZcodeDispatch.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

int failures = 0;

void check(const char *name, bool ok) {
    if (ok) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

}  // namespace

int main(int argc, char *argv[]) {
    // _core-only  harness 默认 DPI unaware：GetWindowRect 给虚拟化坐标而 UIA
    // 给物理坐标，侧栏相对坐标全错。主程序走 Qt Widgets 自带 PMv2，无此问题。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QCoreApplication app(argc, argv);

    // WorkBuddy 系走桌面可见 UIA 通道；Qoder CLI 被否决后休眠。
    check("channel: WorkBuddy mapped, Qoder dormant",
          SessionDispatch::configHomeFor("WorkBuddy.exe").endsWith("/.workbuddy") &&
              SessionDispatch::configHomeFor("WorkBuddyAI.exe")
                  .endsWith("/.workbuddy-ai") &&
              SessionDispatch::configHomeFor("Qoder CN.exe").isEmpty() &&
              SessionDispatch::configHomeFor("qoder.exe").isEmpty());
    check("ui channel supports WorkBuddy family only",
          UiDispatch::supports("WorkBuddy.exe") &&
              UiDispatch::supports("WorkBuddyAI.exe") &&
              !UiDispatch::supports("Qoder CN.exe"));
    check("row name: time suffix stripped, prefix match both ways",
          UiDispatch::cleanRowName("暮霞蓝橙昼夜双动态图标制作 2天前") ==
              QStringLiteral("暮霞蓝橙昼夜双动态图标制作") &&
              UiDispatch::rowMatches("链路探针要求只回复收到 1分钟前",
                                     QStringLiteral("链路探针要求只回复收到")) &&
              UiDispatch::rowMatches("链路探针要求只回复收到",
                                     QStringLiteral("链路探针要求只回复收到：请只回复「收到」")) &&
              !UiDispatch::rowMatches("随便给个选项 21小时前",
                                      QStringLiteral("设计一套新的OS11景深原案")));
    check("cli path per variant",
          SessionDispatch::cliPathFor("C:/x/.qoder-cn")
                  .endsWith("/bin/qoderclicn/qoderclicn.exe") &&
              SessionDispatch::cliPathFor("C:/x/.qoder")
                  .endsWith("/bin/qodercli/qodercli.exe"));

    QString text;
    check("user text: string content",
          SessionDispatch::userTextOf(
              R"({"type":"user","message":{"role":"user","content":"hello"}})",
              &text) &&
              text == "hello");
    check("user text: list content",
          SessionDispatch::userTextOf(
              R"({"type":"user","message":{"content":[{"type":"text","text":"a"},{"type":"tool_result","text":"x"},{"type":"text","text":"b"}]}})",
              &text) &&
              text == "ab");
    check("user text: assistant line ignored",
          !SessionDispatch::userTextOf(
              R"({"type":"assistant","message":{"content":"no"}})", &text));
    check("user text: WorkBuddy message+role=user",
          SessionDispatch::userTextOf(
              R"({"type":"message","message":{"role":"user","content":"wb"}})",
              &text) &&
              text == "wb");
    check("user text: WorkBuddy top-level role + input_text parts",
          SessionDispatch::userTextOf(
              R"({"type":"message","role":"user","content":[{"type":"input_text","text":"<system-reminder>x</system-reminder>real"}]})",
              &text) &&
              text.contains("real"));

    QTemporaryDir dir;
    const QString home = dir.path();
    const QString sid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    QDir().mkpath(home + "/projects/D--repo");
    QFile f(home + "/projects/D--repo/" + sid + ".jsonl");
    f.open(QIODevice::WriteOnly | QIODevice::Text);
    // 头部记录有 id 没 cwd：扫描必须继续到带 cwd 的记录（实测坑）。
    f.write(QStringLiteral(
                R"({"type":"workspace-directories","sessionId":"%1","directories":[]})")
                .arg(sid)
                .toUtf8());
    f.write("\n");
    f.write(QStringLiteral(
                R"({"type":"user","sessionId":"%1","cwd":"D:/repo","message":{"content":"hi"}})")
                .arg(sid)
                .toUtf8());
    f.write("\n");
    f.close();
    QString path, cwd;
    check("findSession: identity self-report + cwd",
          SessionDispatch::findSession(home, sid, &path, &cwd) &&
              path.endsWith(sid + ".jsonl") && cwd == "D:/repo");
    check("findSession: unknown id refused",
          !SessionDispatch::findSession(home, "ffffffff-0000-4000-8000-000000000000",
                                        &path, &cwd));
    // 记录自报别的 id 不算目标会话（证明不了归属就不发）。
    QFile g(home + "/projects/D--repo/ffffffff-0000-4000-8000-000000000000.jsonl");
    g.open(QIODevice::WriteOnly | QIODevice::Text);
    g.write(QStringLiteral(
                R"({"type":"user","sessionId":"%1","message":{"content":"hi"}})")
                .arg(sid)
                .toUtf8());
    g.close();
    check("findSession: mismatched self-report refused",
          !SessionDispatch::findSession(home, "ffffffff-0000-4000-8000-000000000000",
                                        &path, &cwd));

    {
        QFile r(home + "/projects/D--repo/" + sid + ".jsonl");
        r.open(QIODevice::Append | QIODevice::Text);
        r.write(QStringLiteral(
                    R"({"type":"message","role":"assistant","content":[{"type":"output_text","text":"收到"}]})")
                    .toUtf8());
        r.write("\n");
        r.close();
        check("lastReply: WorkBuddy assistant output_text",
              SessionDispatch::lastReply(home, sid) == QStringLiteral("收到"));
        check("lastReply: unknown session empty",
              SessionDispatch::lastReply(home, "ffffffff-0000-4000-8000-000000000000")
                  .isEmpty());
    }

    SessionDispatch d;
    QString reason;
    check("dispatch: unsupported host refused without sending",
          !d.dispatch("Qoder CN.exe", sid, "", "task", &reason) &&
              reason.contains("未接"));
    check("dispatch: WorkBuddy unknown session refused (proof before send)",
          !d.dispatch("WorkBuddy.exe", sid, "", "task", &reason) &&
              reason.contains("证明不了"));
    check("dispatch: empty session refused",
          !d.dispatch("Qoder CN.exe", "", "", "task", &reason) &&
              reason.contains("未关联"));

    // —— DSH 插件通道：native 下发 → 插件轮询取件 → ack 回执（2026-11-07）——
    // 会话库是 zstd 多帧压缩 native 读不了：不在 dispatch() 里 findSession，
    // 归属守卫在插件侧（agents.get 命中才注入，拒绝走 ack 带原因）。
    check("plugin channel: DSH supported, configHome stays empty",
          SessionDispatch::supportsPluginChannel(
              QStringLiteral("DeepSeek Harness.exe")) &&
              !SessionDispatch::supportsPluginChannel(QStringLiteral("WorkBuddy.exe")) &&
              SessionDispatch::configHomeFor(QStringLiteral("DeepSeek Harness.exe"))
                  .isEmpty());
    {
        const QString dshSid =
            QStringLiteral("session-9c08421f-080c-4a79-9325-c23464226a62");
        QString rid;
        SessionDispatch dsh;
        bool receiptOk = false;
        QString failedWhy;
        QObject::connect(&dsh, &SessionDispatch::receipt,
                         [&](const QString &, const QString &exe,
                             const QString &sid, const QString &,
                             const QString &text) {
                             receiptOk = exe == QStringLiteral("DeepSeek Harness.exe") &&
                                         sid == dshSid &&
                                         text == QStringLiteral("探针指令");
                         });
        QObject::connect(&dsh, &SessionDispatch::failed,
                         [&](const QString &, const QString &why) { failedWhy = why; });

        check("plugin channel: bare uuid refused (id format sentinel)",
              !dsh.dispatch(QStringLiteral("DeepSeek Harness.exe"),
                            QStringLiteral("9c08421f-080c-4a79-9325-c23464226a62"),
                            QString(), QStringLiteral("task"), &reason) &&
                  reason.contains(QStringLiteral("归属可疑")));
        check("plugin channel: accepted without session library",
              dsh.dispatch(QStringLiteral("DeepSeek Harness.exe"), dshSid,
                           QStringLiteral("D:/w"), QStringLiteral("探针指令"),
                           &reason, &rid));
        check("plugin channel: same-session in-flight refused",
              !dsh.dispatch(QStringLiteral("DeepSeek Harness.exe"), dshSid,
                            QString(), QStringLiteral("第二发"), &reason) &&
                  reason.contains(QStringLiteral("在途")));

        const QJsonArray taken = dsh.takePluginDispatches(
            QStringLiteral("DeepSeek Harness.exe"));
        check("plugin channel: take returns one item with fields",
              taken.size() == 1 &&
                  taken.at(0).toObject()
                          .value(QStringLiteral("dispatch_id"))
                          .toString() == rid &&
                  taken.at(0).toObject()
                          .value(QStringLiteral("session_id"))
                          .toString() == dshSid &&
                  taken.at(0).toObject().value(QStringLiteral("text")).toString() ==
                      QStringLiteral("探针指令"));
        check("plugin channel: take-once (handed items not re-sent)",
              dsh.takePluginDispatches(QStringLiteral("DeepSeek Harness.exe"))
                  .isEmpty());
        check("plugin channel: other exe takes nothing",
              dsh.takePluginDispatches(QStringLiteral("WorkBuddy.exe")).isEmpty());
        dsh.pluginAck(rid, true, QString());
        check("plugin channel: ack ok → receipt",
              receiptOk && failedWhy.isEmpty());

        bool gotFail = false;
        check("plugin channel: accepted again after receipt",
              dsh.dispatch(QStringLiteral("DeepSeek Harness.exe"), dshSid,
                           QString(), QStringLiteral("第二条"), &reason, &rid));
        QObject::connect(&dsh, &SessionDispatch::failed,
                         [&](const QString &, const QString &why) {
                             gotFail = true;
                             failedWhy = why;
                         });
        dsh.pluginAck(rid, false, QStringLiteral("会话不在活表"));
        check("plugin channel: ack failure → failed with plugin reason",
              gotFail && failedWhy == QStringLiteral("会话不在活表"));
        check("plugin channel: unknown dispatch id reported as not handled",
              !dsh.pluginAck(QStringLiteral("nonexistent"), true, QString()));
    }

    // —— OpenSquilla 插件通道：同款取件/ack，哨兵 agent: 前缀（2026-11-08）——
    // 桥在技能目录（pet-bridge），取件后 sessions.resolve 守卫 → 网关
    // sessions.send 注入；native 不读它的 SQLite 会话库。
    check("plugin channel: OpenSquilla supported, configHome stays empty",
          SessionDispatch::supportsPluginChannel(QStringLiteral("OpenSquilla.exe")) &&
              SessionDispatch::configHomeFor(QStringLiteral("OpenSquilla.exe"))
                  .isEmpty());
    {
        const QString osqSid = QStringLiteral("agent:main:webchat:probe0001");
        const QString dshSid2 =
            QStringLiteral("session-aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
        SessionDispatch mix;
        QString osqRid, dshRid;
        check("osq channel: bare uuid refused (id format sentinel)",
              !mix.dispatch(QStringLiteral("OpenSquilla.exe"),
                            QStringLiteral("09f054f8-2076-4553-ac71-76d06838dae6"),
                            QString(), QStringLiteral("task"), &reason) &&
                  reason.contains(QStringLiteral("可疑")));
        check("osq channel: agent: key accepted",
              mix.dispatch(QStringLiteral("OpenSquilla.exe"), osqSid, QString(),
                           QStringLiteral("OSQ 指令"), &reason, &osqRid));
        check("dsh channel: session- key accepted alongside",
              mix.dispatch(QStringLiteral("DeepSeek Harness.exe"), dshSid2,
                           QString(), QStringLiteral("DSH 指令"), &reason, &dshRid));
        // 双宿主同在 Plugin 通道：取件必须按 exe 过滤，互不领走。
        const QJsonArray osqTaken =
            mix.takePluginDispatches(QStringLiteral("OpenSquilla.exe"));
        check("plugin channel: take filtered by exe (osq only)",
              osqTaken.size() == 1 &&
                  osqTaken.at(0).toObject()
                          .value(QStringLiteral("session_id")).toString() == osqSid &&
                  osqTaken.at(0).toObject()
                          .value(QStringLiteral("dispatch_id")).toString() == osqRid);
        const QJsonArray dshTaken =
            mix.takePluginDispatches(QStringLiteral("DeepSeek Harness.exe"));
        check("plugin channel: take filtered by exe (dsh only)",
              dshTaken.size() == 1 &&
                  dshTaken.at(0).toObject()
                          .value(QStringLiteral("session_id")).toString() == dshSid2);
        bool osqReceipt = false;
        QObject::connect(&mix, &SessionDispatch::receipt,
                         [&](const QString &, const QString &exe,
                             const QString &sid, const QString &, const QString &) {
                             osqReceipt =
                                 exe == QLatin1String("OpenSquilla.exe") && sid == osqSid;
                         });
        mix.pluginAck(osqRid, true, QString());
        check("osq channel: ack ok → receipt with OpenSquilla exe", osqReceipt);
        mix.pluginAck(dshRid, true, QString());
    }

    // —— ZCode 通道：app-server 协议（2026-11-08 探针定案）——
    // 会话索引 = ~/.zcode/cli/db/db.sqlite（只读）；守卫全在 native：会话缺失 /
    // 账号套餐模型 / 活动门。测试用 AGENTRMC_ZCODE_HOME 指到临时目录，自建
    // 同构 sqlite，不碰真实会话库。
    check("zcode channel: supported only for ZCode.exe",
          ZcodeDispatch::supports(QStringLiteral("ZCode.exe")) &&
              !ZcodeDispatch::supports(QStringLiteral("WorkBuddy.exe")) &&
              SessionDispatch::supportsZcodeChannel(QStringLiteral("ZCode.exe")) &&
              !SessionDispatch::supportsZcodeChannel(
                  QStringLiteral("WorkBuddy.exe")));
    // 桥有些宿主不带 raise（ZCode stop hook）：落卡前按显示名反查白名单
    // exe，否则卡上 agent_exe 为空，外壳派发行永远不亮。
    check("exeForDisplayName: ZCode backfill + unknown empty",
          AgentWindow::exeForDisplayName(QStringLiteral("ZCode")) ==
              QStringLiteral("ZCode.exe") &&
              AgentWindow::exeForDisplayName(QStringLiteral("DeepSeek Harness")) ==
                  QStringLiteral("DeepSeek Harness.exe") &&
              AgentWindow::exeForDisplayName(QStringLiteral("不存在宿主"))
                  .isEmpty());
    {
        qputenv("AGENTRMC_ZCODE_HOME",
                (dir.path() + QStringLiteral("/zc")).toUtf8());
        QDir().mkpath(dir.path() + QStringLiteral("/zc/cli/db"));
        const QString dbPath =
            dir.path() + QStringLiteral("/zc/cli/db/db.sqlite");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(
                QStringLiteral("QSQLITE"), QStringLiteral("test-db"));
            db.setDatabaseName(dbPath);
            if (db.open()) {
                QSqlQuery q(db);
                q.exec(QStringLiteral(
                    "CREATE TABLE session (id TEXT, directory TEXT, "
                    "time_updated TEXT, time_deleted TEXT)"));
                q.exec(QStringLiteral(
                    "CREATE TABLE session_entry (session_id TEXT, type TEXT, "
                    "data TEXT)"));
                const QString zs =
                    QStringLiteral("11111111-2222-4333-8444-555555555555");
                const QString acctSid =
                    QStringLiteral("22222222-3333-4444-8555-666666666666");
                const QString recentSid =
                    QStringLiteral("33333333-4444-4555-8666-777777777777");
                // 个人供应商 + 老时间：守卫全过。
                q.prepare(QStringLiteral(
                    "INSERT INTO session VALUES (?, ?, ?, NULL)"));
                q.addBindValue(zs);
                q.addBindValue(QStringLiteral("D:/repo"));
                q.addBindValue(QStringLiteral("1000000000000"));
                q.exec();
                // 账号套餐模型。
                q.prepare(QStringLiteral(
                    "INSERT INTO session VALUES (?, ?, ?, NULL)"));
                q.addBindValue(acctSid);
                q.addBindValue(QStringLiteral("D:/repo"));
                q.addBindValue(QStringLiteral("1000000000000"));
                q.exec();
                // 刚活动过（活动门）。
                q.prepare(QStringLiteral(
                    "INSERT INTO session VALUES (?, ?, ?, NULL)"));
                q.addBindValue(recentSid);
                q.addBindValue(QStringLiteral("D:/repo"));
                q.addBindValue(QString::number(QDateTime::currentMSecsSinceEpoch()));
                q.exec();
                q.prepare(QStringLiteral(
                    "INSERT INTO session_entry VALUES (?, ?, ?)"));
                q.addBindValue(zs);
                q.addBindValue(QStringLiteral("runtime/model_selection"));
                q.addBindValue(QStringLiteral(
                    R"({"modelSelection":{"providerId":"p1","modelId":"m1","options":{"reasoningLevel":"max"}}})"));
                q.exec();
                q.prepare(QStringLiteral(
                    "INSERT INTO session_entry VALUES (?, ?, ?)"));
                q.addBindValue(acctSid);
                q.addBindValue(QStringLiteral("runtime/model_selection"));
                q.addBindValue(QStringLiteral(
                    R"({"modelSelection":{"providerId":"account:zai-start-plan","modelId":"GLM-5.3-Flash"}})"));
                q.exec();
                q.prepare(QStringLiteral(
                    "INSERT INTO session_entry VALUES (?, ?, ?)"));
                q.addBindValue(recentSid);
                q.addBindValue(QStringLiteral("runtime/model_selection"));
                q.addBindValue(QStringLiteral(
                    R"({"modelSelection":{"providerId":"p1","modelId":"m1"}})"));
                q.exec();
                // 已删除会话：不算归属。
                q.prepare(QStringLiteral(
                    "INSERT INTO session VALUES (?, ?, ?, ?)"));
                q.addBindValue(
                    QStringLiteral("44444444-5555-4666-8777-888888888888"));
                q.addBindValue(QStringLiteral("D:/repo"));
                q.addBindValue(QStringLiteral("1000000000000"));
                q.addBindValue(QStringLiteral("1000000001000"));
                q.exec();
                db.close();
            }
            QSqlDatabase::removeDatabase(QStringLiteral("test-db"));
        }

        const QString zs =
            QStringLiteral("11111111-2222-4333-8444-555555555555");
        const QString acctSid =
            QStringLiteral("22222222-3333-4444-8555-666666666666");
        QString zdir;
        QJsonObject sel;
        qint64 ztime = 0;
        check("zcode sessionRecord: hit with model",
              ZcodeDispatch::sessionRecord(zs, &zdir, &sel, &ztime) &&
                  zdir == QStringLiteral("D:/repo") &&
                  sel.value(QStringLiteral("modelId")).toString() ==
                      QStringLiteral("m1") &&
                  sel.value(QStringLiteral("options"))
                          .toObject()
                          .value(QStringLiteral("reasoningLevel"))
                          .toString() == QStringLiteral("max"));
        check("zcode sessionRecord: unknown id refused",
              !ZcodeDispatch::sessionRecord(
                  QStringLiteral("99999999-0000-4000-8000-000000000000"),
                  &zdir, &sel, &ztime));

        SessionDispatch zd;
        check("zcode dispatch: unknown session refused (proof before send)",
              !zd.dispatch(QStringLiteral("ZCode.exe"),
                           QStringLiteral("99999999-0000-4000-8000-000000000000"),
                           QString(), QStringLiteral("task"), &reason) &&
                  reason.contains(QStringLiteral("证明不了")));
        check("zcode dispatch: account-plan model refused",
              !zd.dispatch(QStringLiteral("ZCode.exe"), acctSid, QString(),
                           QStringLiteral("task"), &reason) &&
                  reason.contains(QStringLiteral("账号套餐模型")));
        // 刚活动过：活动门拦（可能正在跑）。
        check("zcode dispatch: activity guard",
              !zd.dispatch(QStringLiteral("ZCode.exe"),
                           QStringLiteral("33333333-4444-4555-8666-777777777777"),
                           QString(), QStringLiteral("task"), &reason) &&
                  reason.contains(QStringLiteral("仍有写入")));
        // 个人供应商 + 老会话：守卫全过，走到 exe 解析。AGENTRMC_ZCODE_EXE
        // 指到不存在的路径 → 拒在「找不到入口」，不真 spawn。
        qputenv("AGENTRMC_ZCODE_EXE",
                (dir.path() + QStringLiteral("/no/ZCode.exe")).toUtf8());
        check("zcode dispatch: stale session passes guards, fails on exe",
              !zd.dispatch(QStringLiteral("ZCode.exe"), zs, QString(),
                           QStringLiteral("task"), &reason) &&
                  reason.contains(QStringLiteral("app-server 入口")));
        qunsetenv("AGENTRMC_ZCODE_EXE");
        qunsetenv("AGENTRMC_ZCODE_HOME");
    }

    // —— ZCode app-server 状态机：真 node 跑 mock 服务端（不碰真实会话）——
    // mock = 临时目录里的 ZCode.exe（node.exe 副本）+ resources/glm/zcode.cjs。
    // 覆盖三条审查实测的漏洞：首条正文当完成、交互请求被 -32000 吞、轮询重置
    // 阶段时钟导致超时不触发。
    {
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (node.isEmpty()) {
            std::printf("SKIP zcode mock: node not found on PATH\n");
        } else {
            const auto waitUntil = [](const std::function<bool()> &pred, int ms) {
                QElapsedTimer t;
                t.start();
                while (!pred() && t.elapsed() < ms) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(5);
                }
                return pred();
            };
            const auto runMock = [&](const QString &mode, bool *turnOk,
                                     bool *interactionDenied, bool *interactionDeclined,
                                     bool *skippedOnce, bool *persistTimedOut,
                                     bool *notTruncated, int *msgPolls) {
                QTemporaryDir ztmp;
                const QString root = ztmp.path();
                const QString exe = root + QStringLiteral("/ZCode.exe");
                const QString cjsDir = root + QStringLiteral("/resources/glm");
                QDir().mkpath(cjsDir);
                *turnOk = *interactionDenied = *interactionDeclined = false;
                *skippedOnce = *persistTimedOut = *notTruncated = false;
                *msgPolls = 0;
                if (!QFile::copy(node, exe)) {
                    std::printf("SKIP zcode mock: cannot copy node.exe\n");
                    return;
                }
                QFile scenario(cjsDir + QStringLiteral("/scenario.json"));
                scenario.open(QIODevice::WriteOnly);
                scenario.write(QStringLiteral(R"({"mode":"%1"})").arg(mode).toUtf8());
                scenario.close();
                QFile mock(cjsDir + QStringLiteral("/zcode.cjs"));
                mock.open(QIODevice::WriteOnly);
                mock.write(QStringLiteral(R"JS(
const fs = require('fs');
const path = require('path');
const dir = __dirname;
const scenario = JSON.parse(fs.readFileSync(path.join(dir, 'scenario.json'), 'utf8'));
let msgPolls = 0, evPolls = 0, interactionsSent = false;
function out(o) { process.stdout.write(JSON.stringify(o) + '\n'); }
function result(id, r) { out({ id: id, result: r }); }
out({ method: 'startup/storageState', params: { phase: 'ready' } });
process.stdin.setEncoding('utf8');
let buf = '';
process.stdin.on('data', (chunk) => {
  buf += chunk;
  for (;;) {
    const nl = buf.indexOf('\n');
    if (nl < 0) break;
    const line = buf.slice(0, nl); buf = buf.slice(nl + 1);
    if (!line.trim()) continue;
    const f = JSON.parse(line);
    if (f.method === undefined) {
      if (f.id === 501 || f.id === 502) {
        fs.appendFileSync(path.join(dir, 'interactions.jsonl'), JSON.stringify(f) + '\n');
      }
      continue;
    }
    if (f.method === 'session/resume') { result(f.id, { sessionId: 'mock' }); continue; }
    if (f.method === 'session/send') { result(f.id, {}); continue; }
    if (f.method === 'session/messages') {
      msgPolls += 1;
      fs.writeFileSync(path.join(dir, 'msgpolls.txt'), String(msgPolls));
      if (scenario.mode === 'no-persist') { result(f.id, { messages: [] }); continue; }
      result(f.id, { messages: [
        { info: { role: 'user' }, parts: [{ type: 'text', text: '派发探针任务' }] },
        { info: { role: 'assistant' }, parts: [{ type: 'text', text: '我先检查一下' }] },
      ] });
      continue;
    }
    if (f.method === 'session/events') {
      if (f.params && f.params.afterSeq !== undefined) {
        evPolls += 1;
        fs.writeFileSync(path.join(dir, 'evpolls.txt'), String(evPolls));
        if (scenario.mode === 'interaction' && !interactionsSent) {
          interactionsSent = true;
          out({ id: 501, method: 'interaction/requestPermission', params: {
            sessionId: 'mock', requestId: 'r1', toolCallId: 'c1', toolName: 'Bash',
            riskLevel: 'high', reason: 'run tests', input: {},
            options: [{ optionId: 'allow', kind: 'allow_once', name: 'Allow',
                        response: { decision: 'allow' } }] } });
          out({ id: 502, method: 'interaction/requestUserInput', params: {
            sessionId: 'mock', requestId: 'r2', prompt: 'which?', questions: [] } });
        }
        if (evPolls === 1) {
          result(f.id, { events: [
            { seq: 8, type: 'turn.started', turnId: 't1', payload: { input: 'task' } },
            { seq: 9, type: 'message.upserted', turnId: 't1', payload: {} },
          ] });
        } else {
          result(f.id, { events: [
            { seq: 10, type: 'turn.completed', turnId: 't1',
              payload: { response: '最终答复：完成' } },
          ] });
        }
        continue;
      }
      result(f.id, { events: [{ seq: 7, type: 'message.upserted', payload: {} }] });
      continue;
    }
    result(f.id, {});
  }
});
process.stdin.on('end', () => process.exit(0));
)JS").toUtf8());
                mock.close();
                qputenv("AGENTRMC_ZCODE_TIME_SCALE", "0.02");
                ZcodeDispatch zd;
                QString reply;
                QString failWhy;
                int skipped = 0;
                QObject::connect(&zd, &ZcodeDispatch::turnFinished,
                                 [&](const QString &, const QString &text) {
                                     reply = text;
                                 });
                QObject::connect(&zd, &ZcodeDispatch::failed,
                                 [&](const QString &, const QString &why) {
                                     failWhy = why;
                                 });
                QObject::connect(&zd, &ZcodeDispatch::interactionSkipped,
                                 [&](const QString &, const QString &) { ++skipped; });
                ZcodeDispatch::Job job;
                job.dispatchId = QStringLiteral("mock-dispatch");
                job.sessionId = QStringLiteral("mock-session");
                job.text = QStringLiteral("派发探针任务");
                job.modelSelection = QJsonObject{{QStringLiteral("providerId"),
                                                  QStringLiteral("mock")}};
                job.zcodeExe = exe;
                zd.start(job);
                const bool finished = waitUntil(
                    [&] { return !reply.isEmpty() || !failWhy.isEmpty(); }, 8000);
                if (!finished) {
                    failWhy = QStringLiteral("mock 8s 内无终态");
                }
                if (!failWhy.isEmpty() && !persistTimedOut) {
                    std::printf("  mock[%s] reason=%s\n", mode.toUtf8().constData(),
                                failWhy.toUtf8().constData());
                }
                *turnOk = failWhy.isEmpty();
                *persistTimedOut = failWhy.contains(QStringLiteral("落库确认超时"));
                *notTruncated = reply == QStringLiteral("最终答复：完成");
                QFile polls(cjsDir + QStringLiteral("/msgpolls.txt"));
                if (polls.open(QIODevice::ReadOnly)) {
                    *msgPolls = polls.readAll().toInt();
                }
                QFile interactions(cjsDir + QStringLiteral("/interactions.jsonl"));
                if (interactions.open(QIODevice::ReadOnly)) {
                    for (const QByteArray &line : interactions.readAll().split('\n')) {
                        const QJsonObject f = QJsonDocument::fromJson(line).object();
                        const QJsonObject res = f.value(QStringLiteral("result")).toObject();
                        if (f.value(QStringLiteral("id")).toInt() == 501 &&
                            res.value(QStringLiteral("decision")).toString() ==
                                QStringLiteral("deny")) {
                            *interactionDenied = true;
                        }
                        if (f.value(QStringLiteral("id")).toInt() == 502 &&
                            res.value(QStringLiteral("action")).toString() ==
                                QStringLiteral("decline")) {
                            *interactionDeclined = true;
                        }
                    }
                }
                *skippedOnce = skipped == 1;
                qunsetenv("AGENTRMC_ZCODE_TIME_SCALE");
            };

            bool turnOk = false, denied = false, declined = false, skipped = false;
            bool timedOut = false, notTruncated = false;
            int msgPolls = 0;
            runMock(QStringLiteral("turn"), &turnOk, &denied, &declined, &skipped,
                    &timedOut, &notTruncated, &msgPolls);
            check("zcode mock: turn.completed carries the final reply",
                  turnOk && notTruncated);
            check("zcode mock: intermediate assistant text does not finish the turn",
                  turnOk && notTruncated && msgPolls >= 1);

            runMock(QStringLiteral("interaction"), &turnOk, &denied, &declined,
                    &skipped, &timedOut, &notTruncated, &msgPolls);
            check("zcode mock: permission request answered with explicit deny",
                  denied);
            check("zcode mock: user-input request answered with explicit decline",
                  declined);
            check("zcode mock: interaction noticed exactly once", skipped);

            runMock(QStringLiteral("no-persist"), &turnOk, &denied, &declined,
                    &skipped, &timedOut, &notTruncated, &msgPolls);
            check("zcode mock: persist timeout still fires while polling",
                  timedOut && msgPolls >= 3);
        }
    }

    // 活体 harness（默认不跑）：AGENTRMC_DISPATCH_LIVE=<session id> 时真派一条
    // 探针进该会话并等回执，用来把路由问题和 UI 探针问题分开。
    const QByteArray live = qgetenv("AGENTRMC_DISPATCH_LIVE");
    if (!live.isEmpty()) {
        SessionDispatch sd;
        QObject::connect(&sd, &SessionDispatch::receipt,
                         [](const QString &id, const QString &exe,
                            const QString &sid, const QString &, const QString &) {
                             std::printf("LIVE RECEIPT %s %s %s\n",
                                         id.left(8).toUtf8().constData(),
                                         exe.toUtf8().constData(),
                                         sid.left(8).toUtf8().constData());
                             QCoreApplication::exit(0);
                         });
        QObject::connect(&sd, &SessionDispatch::failed,
                         [](const QString &, const QString &reason) {
                             std::printf("LIVE FAILED %s\n",
                                         reason.toUtf8().constData());
                             QCoreApplication::exit(2);
                         });
        QString reason;
        const QByteArray liveExe = qgetenv("AGENTRMC_DISPATCH_LIVE_EXE");
        const bool ok = sd.dispatch(
            liveExe.isEmpty() ? QStringLiteral("WorkBuddy.exe")
                              : QString::fromUtf8(liveExe),
            QString::fromUtf8(live), QString(),
            QStringLiteral("定向派发探针 LIVE-%1：只回复收到")
                .arg(QDateTime::currentMSecsSinceEpoch()),
            &reason);
        std::printf("live accepted=%d reason=%s\n", ok,
                    reason.toUtf8().constData());
        if (!ok) {
            return 2;
        }
        QTimer::singleShot(90000, [] {
            std::printf("LIVE TIMEOUT\n");
            QCoreApplication::exit(3);
        });
        return app.exec();
    }

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
