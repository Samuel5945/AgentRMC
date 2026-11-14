// AgentBridge 自测：起监听 → 模拟 Agent 请求 → 断言 notify 信号与响应码。
// 覆盖：/notify 全字段与默认文案、raise 透传、/speak 正常与缺参、/state、错路径 404。
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <memory>

#include "core/AgentBridge.h"

namespace {

struct Notice {
    bool got = false;
    QString kind;
    QString title;
    QString message;
    QString raise;
};

Notice lastNotice;
bool ok = true;
// B1：/requests 路由把插件推送的待答请求原样交给 handler。
QJsonObject lastRequests;
bool requestsGot = false;
// 默认走真端口；本机装着 AgentRMC 时端口被占，测试可以指一个空闲端口跑。
quint16 testPort = AgentBridge::kPort;

// 发一个请求，等响应回来交给 onResponse 校验；校验完调 next。
QTcpSocket *request(QCoreApplication &app, const QByteArray &raw,
                    const std::function<void(const QByteArray &)> &onResponse,
                    const std::function<void()> &next) {
    auto *socket = new QTcpSocket(&app);
    QObject::connect(socket, &QTcpSocket::connected, socket,
                     [socket, raw] { socket->write(raw); });
    QObject::connect(socket, &QTcpSocket::readyRead, socket,
                     [socket, onResponse, next] {
                         const QByteArray response = socket->readAll();
                         socket->disconnectFromHost();
                         socket->deleteLater();
                         onResponse(response);
                         next();
                     });
    socket->connectToHost(QHostAddress::LocalHost, testPort);
    return socket;
}

QByteArray withLength(const QByteArray &requestLine, const QByteArray &body) {
    return requestLine + "Host: 127.0.0.1\r\nContent-Length: " +
           QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

bool fail(const char *what, const QByteArray &resp) {
    std::fprintf(stderr, "FAIL %s resp=%s\n", what, resp.constData());
    ok = false;
    return false;
}

struct CommandCall {
    QString action;
    bool confirm = false;
};

CommandCall lastCommand;
int commandCalls = 0;

// /command：状态码由主窗给的 code 翻出来。这里用一个假 handler 验证翻译规则
// 和 body 透传，真实执行在 MainWindow::onBridgeCommand 那边。
void runCommandTests(QCoreApplication &app, AgentBridge &bridge,
                     const std::function<void()> &done) {
    bridge.setCommandHandler([](const QJsonObject &body) {
        lastCommand.action = body.value(QStringLiteral("action")).toString();
        lastCommand.confirm = body.value(QStringLiteral("confirm")).toBool(false);
        ++commandCalls;
        // 中文一律 fromUtf8：MSVC 下窄字面量按本地代码页解释，QLatin1String 比不中。
        const QString shutdown = QString::fromUtf8("关机");
        const QString disabled = QString::fromUtf8("关掉的");
        const QString volume = QString::fromUtf8("音量调到50");
        QJsonObject reply{{QStringLiteral("ok"), false},
                          {QStringLiteral("code"), 400}};
        if (lastCommand.action == shutdown && !lastCommand.confirm) {
            reply[QStringLiteral("code")] = 409;
            reply[QStringLiteral("needs_confirm")] = true;
        } else if (lastCommand.action == disabled) {
            reply[QStringLiteral("code")] = 403;
        } else if (lastCommand.action == volume ||
                   (lastCommand.action == shutdown && lastCommand.confirm)) {
            reply[QStringLiteral("ok")] = true;
            reply[QStringLiteral("code")] = 200;
        }
        return reply;
    });

    struct Case {
        const char *body;
        const char *status;
        const char *fragment;
        bool confirm;
    };
    static const Case cases[] = {
        {R"({"action":"音量调到50"})", "HTTP/1.1 200", "\"ok\":true", false},
        {R"({"action":"关机"})", "HTTP/1.1 409", "needs_confirm", false},
        {R"({"action":"关机","confirm":true})", "HTTP/1.1 200", "\"ok\":true", true},
        {R"({"action":"关掉的"})", "HTTP/1.1 403", "", false},
        {R"({"action":"不认识的话"})", "HTTP/1.1 400", "", false},
    };
    auto step = std::make_shared<std::function<void(int)>>();
    *step = [&, step, done](int index) {
        if (index >= int(sizeof(cases) / sizeof(cases[0]))) {
            if (commandCalls != 5) {
                fail("command handler call count", QByteArray::number(commandCalls));
            }
            done();
            return;
        }
        const Case &test = cases[index];
        request(app,
                withLength("POST /command HTTP/1.1\r\n", test.body),
                [test](const QByteArray &resp) {
                    if (!resp.startsWith(test.status)) {
                        fail(test.status, resp);
                    }
                    if (test.fragment[0] != '\0' && !resp.contains(test.fragment)) {
                        fail(test.fragment, resp);
                    }
                    if (lastCommand.confirm != test.confirm) {
                        fail("confirm flag passthrough", resp);
                    }
                },
                [step, index] { (*step)(index + 1); });
    };
    (*step)(0);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        testPort = quint16(QByteArray(argv[1]).toUShort());
    }

    AgentBridge bridge;
    if (!bridge.start(testPort)) {
        std::fprintf(stderr, "FAIL: bridge 没监听起来（端口 %u）\n", uint(testPort));
        return 1;
    }
    bridge.setStateProvider([] {
        return QString::fromUtf8(R"({"pet_visible":false,"listening":false})");
    });
    // B1：/requests 处理器（状态在 namespace 作用域）。
    bridge.setRequestsHandler([](const QJsonObject &payload) {
        lastRequests = payload;
        requestsGot = true;
    });
    QObject::connect(&bridge, &AgentBridge::notify,
                     [](const QString &kind, const QString &title,
                        const QString &message, const QString &raise) {
                         lastNotice.got = true;
                         lastNotice.kind = kind;
                         lastNotice.title = title;
                         lastNotice.message = message;
                         lastNotice.raise = raise;
                     });
    QTimer::singleShot(15000, &app, [] {
        std::fprintf(stderr, "FAIL: 超时\n");
        QCoreApplication::exit(1);
    });

    const auto expectNotice = [&](const Notice &want,
                                  const QByteArray &resp) {
        if (!resp.startsWith("HTTP/1.1 204") || !lastNotice.got ||
            lastNotice.kind != want.kind || lastNotice.title != want.title ||
            lastNotice.message != want.message ||
            lastNotice.raise != want.raise) {
            std::fprintf(stderr,
                         "FAIL want kind=%s title=%s message=%s; got "
                         "kind=%s title=%s message=%s resp=%s\n",
                         qPrintable(want.kind), qPrintable(want.title),
                         qPrintable(want.message), qPrintable(lastNotice.kind),
                         qPrintable(lastNotice.title),
                         qPrintable(lastNotice.message), resp.constData());
            ok = false;
        }
        lastNotice = Notice{};
    };
    const auto expectStatus = [&](const char *prefix, const QByteArray &resp) {
        if (!resp.startsWith(prefix)) {
            fail(prefix, resp);
        }
    };

    // 1. 全字段 stop + raise（Qoder 这类非 ZCode 宿主靠 raise 指定窗口）：原样转发 + 回 204。
    // 2. 只带 event：message 补默认文案，raise 为空。
    // 3. /speak 带 message：kind=speak 转发，不带 raise。
    // 4. /speak 缺 message：400。
    // 5. GET /state：200 + 状态 JSON。
    // 6. 错路径：不崩，回 404。
    // 7. curl 式头序（Content-Length 后还有别的头）：回归——按行解析别再取错长度。
    request(app,
            withLength("POST /notify HTTP/1.1\r\n",
                       R"({"event":"stop","title":"Qoder","message":"任务完成：AgentRMC","raise":"Qoder CN.exe"})"),
            [&](const QByteArray &resp) {
                expectNotice(Notice{true, "stop", "Qoder", "任务完成：AgentRMC",
                                    "Qoder CN.exe"},
                             resp);
            },
            [&] {
                request(app,
                        withLength("POST /notify HTTP/1.1\r\n",
                                   R"({"event":"permission"})"),
                        [&](const QByteArray &resp) {
                            expectNotice(Notice{true, "permission", "ZCode",
                                                "等你确认一个操作"},
                                         resp);
                        },
                        [&] {
                            request(app,
                                    withLength("POST /speak HTTP/1.1\r\n",
                                               R"({"message":"测试一句话"})"),
                                    [&](const QByteArray &resp) {
                                        expectNotice(Notice{true, "speak",
                                                            "ZCode",
                                                            "测试一句话"},
                                                     resp);
                                    },
                                    [&] {
                                        request(app,
                                                withLength(
                                                    "POST /speak HTTP/1.1\r\n",
                                                    "{}"),
                                                [&](const QByteArray &resp) {
                                                    expectStatus("HTTP/1.1 400",
                                                                 resp);
                                                },
                                                [&] {
                                                    request(app,
                                                            "GET /state "
                                                            "HTTP/1.1\r\nHost: "
                                                            "127.0.0.1\r\n\r\n",
                                                            [&](const QByteArray
                                                                    &resp) {
                                                                if (!resp.startsWith(
                                                                        "HTTP/1.1 "
                                                                        "200") ||
                                                                    !resp.contains(
                                                                        "pet_visible")) {
                                                                    fail("state",
                                                                         resp);
                                                                }
                                                            },
                                                            [&] {
                                                                request(app,
                                                                        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                                                                        [](const QByteArray
                                                                                &resp) {
                                                                            if (!resp.startsWith(
                                                                                    "HTTP/1.1 404")) {
                                                                                fail("404",
                                                                                     resp);
                                                                            }
                                                                        },
                                                                        [&] {
                                                                            const QByteArray curlBody = R"({"message":"curl 式"})";
                                                                            const QByteArray curlRequest =
                                                                                "POST /speak HTTP/1.1\r\n"
                                                                                "Host: 127.0.0.1\r\n"
                                                                                "Content-Length: " + QByteArray::number(curlBody.size()) + "\r\n"
                                                                                "User-Agent: curl/8.9.1\r\n"
                                                                                "Accept: */*\r\n"
                                                                                "Content-Type: application/json\r\n\r\n" + curlBody;
                                                                            request(app,
                                                                                    curlRequest,
                                                                                    [&](const QByteArray &resp) {
                                                                                        expectNotice(Notice{true, "speak", "ZCode", "curl 式"}, resp);
                                                                                    },
                                                                                    [&] {
                                                                                                                                                                                 request(app,
                                                                                             withLength("POST /requests HTTP/1.1\r\n",
                                                                                                 R"({"exe":"DeepSeek Harness.exe","session_id":"session-x","requests":[{"kind":"approval","id":"a1","toolName":"Bash"}]})"),
                                                                                             [&](const QByteArray &resp) {
                                                                                                 expectStatus("HTTP/1.1 204", resp);
                                                                                                 if (!requestsGot ||
                                                                                                     lastRequests.value("session_id").toString() != "session-x" ||
                                                                                                     lastRequests.value("requests").toArray().size() != 1) {
                                                                                                     fail("requests handler payload", resp);
                                                                                                 }
                                                                                             },
                                                                                             [&] {
runCommandTests(app, bridge, [] {
                                                                                            // 正面通过标记：原来只在失败时打印，空输出分不清「全过」和「没跑完」。
                                                                                            std::printf(ok ? "ALL PASS bridge\n" : "FAIL bridge\n");
                                                                                            std::fflush(stdout);
                                                                                            QCoreApplication::exit(ok ? 0 : 1);
                                                                                        });
                                                                                     });
                                                                                    });
                                                                        });
                                                            });
                                                });
                                    });
                        });
            });

    return app.exec();
}
