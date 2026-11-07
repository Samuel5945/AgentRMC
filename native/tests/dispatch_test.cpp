#include "core/SessionDispatch.h"
#include "core/UiDispatch.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

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
