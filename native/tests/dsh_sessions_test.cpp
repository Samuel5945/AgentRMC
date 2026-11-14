// DshSessions：DSH 会话投影缓存的只读索引。全部用临时目录里的假投影文件，
// 通过 AGENTRMC_DSH_HOME 覆盖宿主家目录——绝不碰真实 ~/.dsh。
#include "core/DshSessions.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;
int checks = 0;

void check(const char *name, bool ok) {
    ++checks;
    if (ok) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

// 在临时家目录里铺一份投影文件（宿主真实布局：storages/session_projcache/sessions/）。
bool writeProjection(const QString &home, const QString &sessionId,
                     const QByteArray &json) {
    const QString dir = home + QStringLiteral("/storages/session_projcache/sessions");
    if (!QDir().mkpath(dir)) {
        return false;
    }
    QFile f(dir + QLatin1Char('/') + sessionId + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    f.write(json);
    f.close();
    return true;
}

QByteArray projection(const QByteArray &rows, const QByteArray &identity =
                          R"({"createdAt":1793694812572,"cwd":"D:\\fallback"})") {
    return QByteArray("{\"version\":7,\"record\":{\"identity\":") + identity +
           ",\"rows\":" + rows + "}}";
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("FAIL temporary dir\n");
        return 1;
    }
    qputenv("AGENTRMC_DSH_HOME", tmp.path().toLocal8Bit());
    check("home: env override", DshSessions::home() == tmp.path());

    const QString sidA = QStringLiteral("session-1001ba4c-399e-45c5-9aa8-8262e01894b7");
    const QString sidB = QStringLiteral("session-2f796f01-2bae-4241-8e44-f5062d4430fe");
    const QString sidC = QStringLiteral("session-1d3d9c52-d043-457e-a1ac-023ac28d96b4");
    const QString sidD = QStringLiteral("session-67a6b61a-f8e8-4355-8a3e-aa5c6961a8ac");

    // 1) AI 标题 + workingDirectory 行优先。
    check("fixture: full projection",
          writeProjection(tmp.path(), sidA,
                          projection(R"({
        "title":{"ver":1,"seq":9,"val":"修复桌宠录音背景圆弧裁剪"},
        "workingDirectory":{"ver":1,"seq":9,"val":"D:\\githubrepos\\Agentrmc"},
        "titleInput":{"ver":3,"seq":9,"val":{"first":{"seq":1,"text":"修一下圆弧"}}}
      })")));
    const DshSessions::Info a = DshSessions::find(sidA);
    check("title: ai title wins over first user text",
          a.valid && a.title == QString::fromUtf8("修复桌宠录音背景圆弧裁剪"));
    check("cwd: workingDirectory row wins over identity",
          a.cwd == QStringLiteral("D:\\githubrepos\\Agentrmc"));

    // 2) 没有 AI 标题：退首条用户正文，压空白。
    check("fixture: title-less projection",
          writeProjection(tmp.path(), sidB,
                          projection(R"({
        "titleInput":{"ver":3,"seq":9,"val":{"first":{"seq":1,
          "text":"  继续优化待办的会话，\n 比如 dsh 的会话记录  "}}}
      })")));
    const DshSessions::Info b = DshSessions::find(sidB);
    check("title: falls back to first user text, whitespace simplified",
          b.valid &&
              b.title == QString::fromUtf8("继续优化待办的会话， 比如 dsh 的会话记录"));
    check("cwd: falls back to identity.cwd",
          b.cwd == QStringLiteral("D:\\fallback"));

    // 3) 首条正文超长：截 60 字加省略号（标题位不是正文位）。
    const QByteArray longText(200, 'x');
    check("fixture: long first text",
          writeProjection(tmp.path(), sidC,
                          projection(QByteArray("{\"titleInput\":{\"val\":{\"first\":{\"text\":\"") +
                                     longText + "\"}}}}")));
    const DshSessions::Info c = DshSessions::find(sidC);
    check("title: long first text capped at 60 + ellipsis",
          c.valid && c.title.size() == 61 && c.title.endsWith(QString::fromUtf8("…")));

    // 4) 文件缺失 / id 不合法：一律 invalid，不碰盘。
    check("missing file: invalid",
          !DshSessions::find(QStringLiteral("session-00000000-0000-0000-0000-000000000000")).valid);
    check("id guard: no prefix / traversal / separator rejected",
          !DshSessions::find(QStringLiteral("1001ba4c-399e")).valid &&
              !DshSessions::find(QStringLiteral("session-../../evil")).valid &&
              !DshSessions::find(QStringLiteral("session-a/b")).valid &&
              !DshSessions::find(QStringLiteral("session-a\\b")).valid &&
              !DshSessions::find(QString()).valid);

    // 5) 坏 JSON / 没有 record：invalid，不崩。
    check("fixture: corrupt json",
          writeProjection(tmp.path(), sidD, "{not json"));
    check("corrupt json: invalid", !DshSessions::find(sidD).valid);
    const QString sidE = QStringLiteral("session-73d11f5e-fd83-4268-866b-3797e869f9f8");
    check("fixture: empty record",
          writeProjection(tmp.path(), sidE, R"({"version":7,"record":{}})"));
    check("empty record: invalid", !DshSessions::find(sidE).valid);

    // 6) 缓存按 mtime+size 失效：宿主异步生成标题后（文件变了）要读出新值。
    const QString sidF = QStringLiteral("session-9befa0ea-1eb4-48e3-adfe-73b62fdc7551");
    check("fixture: before title generated",
          writeProjection(tmp.path(), sidF,
                          projection(R"({"titleInput":{"val":{"first":{"text":"先干活"}}}})")));
    const DshSessions::Info f1 = DshSessions::find(sidF);
    check("cache: first read sees fallback title",
          f1.valid && f1.title == QString::fromUtf8("先干活"));
    check("fixture: title generated later",
          writeProjection(tmp.path(), sidF,
                          projection(R"({"title":{"val":"后来的标题"},"titleInput":{"val":{"first":{"text":"先干活"}}}})")));
    const DshSessions::Info f2 = DshSessions::find(sidF);
    check("cache: file change invalidates, new title visible",
          f2.valid && f2.title == QString::fromUtf8("后来的标题"));

    // 7) 缓存命中：文件没变时重复查询结果一致（面板 5 秒一刷的路径）。
    const DshSessions::Info f3 = DshSessions::find(sidF);
    check("cache: stable read returns same info",
          f3.valid && f3.title == f2.title && f3.cwd == f2.cwd);

    // 8) 文件被删（宿主清理投影）：回到 invalid，不留陈旧缓存。
    const QString projFile = tmp.path() +
        QStringLiteral("/storages/session_projcache/sessions/") + sidF +
        QStringLiteral(".json");
    check("fixture: remove projection", QFile::remove(projFile));
    check("removed file: invalid again", !DshSessions::find(sidF).valid);

    std::printf(failures == 0 ? "PASS native dsh_sessions (%d checks)\n"
                              : "FAIL native dsh_sessions (%d failures)\n",
                failures == 0 ? checks : failures);
    return failures == 0 ? 0 : 1;
}