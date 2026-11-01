// AgentBridge 自测：起监听 → 模拟 Agent 请求 → 断言 notify 信号与响应码。
// 覆盖：/notify 全字段与默认文案、/speak 正常与缺参、/state、错路径 404。
#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>
#include <functional>

#include "core/AgentBridge.h"

namespace {

struct Notice {
    bool got = false;
    QString kind;
    QString title;
    QString message;
};

Notice lastNotice;
bool ok = true;

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
    socket->connectToHost(QHostAddress::LocalHost, AgentBridge::kPort);
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

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    AgentBridge bridge;
    if (!bridge.start()) {
        std::fprintf(stderr, "FAIL: bridge 没监听起来\n");
        return 1;
    }
    bridge.setStateProvider([] {
        return QString::fromUtf8(R"({"pet_visible":false,"listening":false})");
    });
    QObject::connect(&bridge, &AgentBridge::notify,
                     [](const QString &kind, const QString &title,
                        const QString &message) {
                         lastNotice.got = true;
                         lastNotice.kind = kind;
                         lastNotice.title = title;
                         lastNotice.message = message;
                     });
    QTimer::singleShot(15000, &app, [] {
        std::fprintf(stderr, "FAIL: 超时\n");
        QCoreApplication::exit(1);
    });

    const auto expectNotice = [&](const Notice &want,
                                  const QByteArray &resp) {
        if (!resp.startsWith("HTTP/1.1 204") || !lastNotice.got ||
            lastNotice.kind != want.kind || lastNotice.title != want.title ||
            lastNotice.message != want.message) {
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

    // 1. 全字段 stop：原样转发 + 回 204。
    // 2. 只带 event：message 补默认文案。
    // 3. /speak 带 message：kind=speak 转发。
    // 4. /speak 缺 message：400。
    // 5. GET /state：200 + 状态 JSON。
    // 6. 错路径：不崩，回 404。
    // 7. curl 式头序（Content-Length 后还有别的头）：回归——按行解析别再取错长度。
    request(app,
            withLength("POST /notify HTTP/1.1\r\n",
                       R"({"event":"stop","title":"ZCode","message":"任务完成：AgentRMC"})"),
            [&](const QByteArray &resp) {
                expectNotice(Notice{true, "stop", "ZCode", "任务完成：AgentRMC"}, resp);
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
                                                                                    [] {
                                                                                        QCoreApplication::exit(ok ? 0 : 1);
                                                                                    });
                                                                        });
                                                            });
                                                });
                                    });
                        });
            });

    return app.exec();
}
