// LanWebServer 自测（移动端 M1）：配对/鉴权/白名单/路径越界/吊销/防爆破/开关。
// 顺带锁死 AgentBridge 必须回环——暴露到局域网的只能是 LanWebServer。
// 全部走本机临时端口（start(0)），不碰真实 45915/45916。
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>

#include "core/AgentBridge.h"
#include "core/LanWebServer.h"

namespace {

bool ok = true;
int checks = 0;

void check(bool cond, const char *what, const QByteArray &context = QByteArray()) {
    ++checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s ctx=%s\n", what, context.constData());
        ok = false;
    }
}

// 发一个请求，收全响应（服务端 Connection: close，以断开为收齐信号）再校验。
void request(QCoreApplication &app, quint16 port, const QByteArray &raw,
             const std::function<void(const QByteArray &)> &onResponse,
             const std::function<void()> &next) {
    auto *socket = new QTcpSocket(&app);
    auto *buf = new QByteArray();
    QObject::connect(socket, &QTcpSocket::connected, socket,
                     [socket, raw] { socket->write(raw); });
    QObject::connect(socket, &QTcpSocket::readyRead, socket,
                     [socket, buf] { buf->append(socket->readAll()); });
    QObject::connect(socket, &QTcpSocket::disconnected, socket,
                     [socket, buf, onResponse, next] {
                         onResponse(*buf);
                         delete buf;
                         socket->deleteLater();
                         next();
                     });
    socket->connectToHost(QHostAddress::LocalHost, port);
}

QByteArray post(const QByteArray &path, const QByteArray &body,
                const QByteArray &extraHeaders = QByteArray()) {
    return "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
           extraHeaders + "Content-Length: " + QByteArray::number(body.size()) +
           "\r\n\r\n" + body;
}

QByteArray get(const QByteArray &path, const QByteArray &extraHeaders = QByteArray()) {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extraHeaders +
           "\r\n";
}

QByteArray cookieHeader(const QByteArray &token) {
    return "Cookie: armc_dev=" + token + "\r\n";
}

QByteArray csrf() { return "X-Requested-With: fetch\r\n"; }

// 从响应里抠出 Set-Cookie 的令牌。
QByteArray tokenFromResponse(const QByteArray &resp) {
    const int at = resp.indexOf("Set-Cookie: armc_dev=");
    if (at < 0) {
        return QByteArray();
    }
    const int start = at + QByteArray("Set-Cookie: armc_dev=").size();
    const int end = resp.indexOf(';', start);
    return resp.mid(start, end - start);
}

// 顺序执行器：一步验完再走下一步，避免 bridge_test 那种嵌套金字塔。
class Seq {
public:
    using Step = std::function<void(std::function<void()>)>;
    void add(Step step) { steps_.append(std::move(step)); }
    void start(std::function<void()> done) {
        done_ = std::move(done);
        run(0);
    }

private:
    void run(int i) {
        if (i >= steps_.size()) {
            done_();
            return;
        }
        steps_.at(i)([this, i] { run(i + 1); });
    }
    QList<Step> steps_;
    std::function<void()> done_;
};

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::fprintf(stderr, "FAIL: 临时目录建不出来\n");
        return 1;
    }
    // 主题根里放一张「图」，根外放一个秘密文件：越界请求必须拿不到。
    const QString themeRoot = temp.filePath("themes");
    QDir().mkpath(themeRoot);
    {
        QFile png(QDir(themeRoot).filePath("ok.png"));
        png.open(QIODevice::WriteOnly);
        png.write("PNGTESTBYTES");
        QFile secret(temp.filePath("secret.txt"));
        secret.open(QIODevice::WriteOnly);
        secret.write("TOPSECRET");
    }

    LanWebServer lan;
    lan.setThemeRoot(themeRoot);
    QString providerState = QStringLiteral(
        R"({"work":{"items":[]},"meta":{"version":"test"}})");
    lan.setStateProvider([&] { return providerState; });
    QString lastCmd;
    int actionCalls = 0;
    lan.setActionHandler([&](const QJsonObject &action) {
        lastCmd = action.value(QStringLiteral("cmd")).toString();
        ++actionCalls;
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("code"), 200}};
    });
    QString lastTranscriptId;
    int lastOffset = -1;
    lan.setTranscriptProvider([&](const QString &id, int offset, int limit) {
        lastTranscriptId = id;
        lastOffset = offset;
        if (id == QStringLiteral("item-1")) {
            return QStringLiteral(
                R"({"id":"item-1","supported":true,"messages":[],"total":0})");
        }
        return QString();  // 未知待办 → 404
    });
    QJsonArray persisted;
    int persistCalls = 0;
    QObject::connect(&lan, &LanWebServer::devicesChanged,
                     [&](const QJsonArray &devices) {
                         persisted = devices;
                         ++persistCalls;
                     });

    if (!lan.start(0)) {
        std::fprintf(stderr, "FAIL: LAN 服务没监听起来\n");
        return 1;
    }
    const quint16 port = lan.port();
    check(port > 0, "auto port");

    // 桥必须仍是回环：无鉴权 IPC 暴露到局域网就是裸奔。
    AgentBridge bridge;
    check(bridge.start(0), "bridge start");
    check(bridge.serverAddress() == QHostAddress(QHostAddress::LocalHost),
          "bridge must stay loopback");

    QTimer::singleShot(30000, &app, [] {
        std::fprintf(stderr, "FAIL: 超时\n");
        QCoreApplication::exit(1);
    });

    QByteArray token;       // 配对成功后拿到的设备令牌
    QString deviceId;
    QByteArray pairingCode;

    Seq seq;
    const auto ask = [&](const QByteArray &raw,
                         const std::function<void(const QByteArray &)> &verify) {
        return [&, raw, verify](std::function<void()> next) {
            request(app, port, raw, verify, next);
        };
    };

    // 1. 静态页免鉴权。
    seq.add(ask(get("/m/"), [](const QByteArray &resp) {
        check(resp.startsWith("HTTP/1.1 200"), "GET /m/ 200", resp);
        check(resp.contains("text/html"), "GET /m/ html", resp);
        check(resp.contains("配对"), "GET /m/ has pair screen", resp);
    }));
    // 2. /m 重定向到 /m/。
    seq.add(ask(get("/m"), [](const QByteArray &resp) {
        check(resp.startsWith("HTTP/1.1 301"), "GET /m 301", resp);
        check(resp.contains("Location: /m/"), "GET /m location", resp);
    }));
    // 3. 无令牌读状态 → 401。
    seq.add(ask(get("/m/state"), [](const QByteArray &resp) {
        check(resp.startsWith("HTTP/1.1 401"), "state no token 401", resp);
    }));
    // 4. 桥的 agent 路由永不出现在 LAN 口。
    seq.add(ask(get("/notify"), [](const QByteArray &resp) {
        check(resp.startsWith("HTTP/1.1 404"), "agent route 404", resp);
    }));
    seq.add(ask(post("/command", R"({"action":"关机"})", csrf()),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 404"), "command 404", resp);
                }));
    // 5. OPTIONS（CORS 预检）一律 404：不开 CORS。
    seq.add(ask("OPTIONS /m/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 404"), "options 404", resp);
                }));
    // 6. 动作：有 CSRF 头但无令牌 → 401。
    seq.add(ask(post("/m/action", R"({"cmd":"ack","id":"x"})", csrf()),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 401"), "action no token 401",
                          resp);
                }));
    // 7. 配对：没带 CSRF 头 → 403（跨站表单进不来）。
    seq.add(ask(post("/m/pair", R"({"code":"whatever"})"),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 403"), "pair no csrf 403",
                          resp);
                }));
    // 8. 没开配对窗口时，配对码不对 → 401。
    seq.add(ask(post("/m/pair", R"({"code":"whatever"})", csrf()),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 401"), "pair inactive 401",
                          resp);
                }));
    // 9. 开配对窗口 → pairingInfo 有码；正确码 + 设备名 → 200 + Set-Cookie。
    seq.add([&](std::function<void()> next) {
        pairingCode = lan.beginPairing().toUtf8();
        const QJsonObject info = lan.pairingInfo();
        check(info.value(QStringLiteral("active")).toBool(), "pairing active");
        check(info.value(QStringLiteral("code")).toString().toUtf8() ==
                  pairingCode,
              "pairing info code");
        const QByteArray body = QByteArray("{\"code\":\"") + pairingCode +
                                "\",\"name\":\"测试手机\"}";
        request(app, port, post("/m/pair", body, csrf()),
                [&](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"), "pair 200", resp);
                    check(resp.contains("Set-Cookie: armc_dev="), "pair cookie",
                          resp);
                    check(resp.contains("HttpOnly"), "cookie httponly", resp);
                    check(resp.contains("SameSite=Strict"), "cookie samesite",
                          resp);
                    token = tokenFromResponse(resp);
                    check(!token.isEmpty(), "token parsed", resp);
                    check(persistCalls >= 1, "devicesChanged emitted");
                    check(!persisted.isEmpty(), "devices persisted");
                    const QJsonObject dev = persisted.at(0).toObject();
                    deviceId = dev.value(QStringLiteral("id")).toString();
                    check(dev.value(QStringLiteral("name")).toString() ==
                              QString::fromUtf8("测试手机"),
                          "device name");
                    // 落盘的只能是哈希：原始令牌与配对码都不许出现。
                    const QByteArray dump =
                        QJsonDocument(persisted).toJson();
                    check(!dump.contains(token), "no raw token in persist", dump);
                    check(!dump.contains(pairingCode), "no code in persist",
                          dump);
                    const QByteArray expectHash =
                        QCryptographicHash::hash(token, QCryptographicHash::Sha256)
                            .toHex();
                    check(dev.value(QStringLiteral("tokenHash")).toString() ==
                              QString::fromLatin1(expectHash),
                          "hash stored");
                    // 给 UI 的设备列表不带哈希。
                    check(!QJsonDocument(lan.devices()).toJson().contains(
                              "tokenHash"),
                          "ui devices no hash");
                },
                next);
    });
    // 9b. 助记词格式 + 空格归一化：12 个词；手机连着打（去空格）也能配对。
    seq.add([&](std::function<void()> next) {
        const QString code = lan.beginPairing();
        const QStringList words = code.split(QLatin1Char(' '));
        check(words.size() == 12, "pair code is 12 words", code.toUtf8());
        QString noSpace = code;
        noSpace.remove(QLatin1Char(' '));
        const QByteArray body =
            QByteArray("{\"code\":\"") + noSpace.toUtf8() + "\"}";
        request(app, port, post("/m/pair", body, csrf()),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"),
                          "pair no-space normalized 200", resp);
                },
                next);
    });
    // 9c. 推荐地址排序：192.168 段（若有）必须排在其它网卡前面。
    {
        const QStringList urls = LanWebServer::lanUrls(45916);
        int first192 = -1, firstOther = -1;
        for (int i = 0; i < urls.size(); ++i) {
            if (urls.at(i).contains(QStringLiteral("://192.168."))) {
                if (first192 < 0) first192 = i;
            } else if (firstOther < 0) {
                firstOther = i;
            }
        }
        check(first192 < 0 || firstOther < 0 || first192 < firstOther,
              "192.168 recommended url first");
    }
    // 10. cookie 鉴权读状态。
    seq.add([&](std::function<void()> next) {
        request(app, port, get("/m/state", cookieHeader(token)),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"), "state cookie 200",
                          resp);
                    check(resp.contains("\"meta\""), "state payload", resp);
                },
                next);
    });
    // 11. 头令牌鉴权（等价路径）。
    seq.add([&](std::function<void()> next) {
        request(app, port,
                get("/m/state", "X-AgentRMC-Token: " + token + "\r\n"),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"), "state header 200",
                          resp);
                },
                next);
    });
    // 12. 假令牌 → 401。
    seq.add(ask(get("/m/state", cookieHeader("deadbeef")),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 401"), "bad token 401",
                          resp);
                }));
    // 13. 白名单内动作 → 处理器收到。
    seq.add([&](std::function<void()> next) {
        request(app, port,
                post("/m/action", R"({"cmd":"ack","id":"x"})",
                     csrf() + cookieHeader(token)),
                [&](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"), "ack 200", resp);
                    check(lastCmd == QStringLiteral("ack"), "ack handler");
                    check(actionCalls == 1, "action call count");
                },
                next);
    });
    // 14. 白名单外动作 → 403 且处理器不被调用（手机不能升级插件/改键）。
    seq.add([&](std::function<void()> next) {
        request(app, port,
                post("/m/action", R"({"cmd":"sysUpgrade"})",
                     csrf() + cookieHeader(token)),
                [&](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 403"), "sysUpgrade 403",
                          resp);
                    check(actionCalls == 1, "blocked action not executed");
                },
                next);
    });
    // 15. 有令牌但没 CSRF 头的 POST → 403。
    seq.add([&](std::function<void()> next) {
        request(app, port,
                post("/m/action", R"({"cmd":"ack","id":"x"})",
                     cookieHeader(token)),
                [&](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 403"), "action no csrf 403",
                          resp);
                    check(actionCalls == 1, "csrf blocked not executed");
                },
                next);
    });
    // 16. 跨站 Origin → 403。
    seq.add([&](std::function<void()> next) {
        request(app, port,
                post("/m/action", R"({"cmd":"ack","id":"x"})",
                     csrf() + cookieHeader(token) +
                         "Origin: http://evil.example\r\n"),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 403"), "evil origin 403",
                          resp);
                },
                next);
    });
    // 17. 聊天页记录路由：无令牌 401；有令牌 200 且分页参数透传；未知待办 404。
    seq.add([&](std::function<void()> next) {
        request(app, port, get("/m/transcript?id=item-1"),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 401"), "transcript no token 401",
                          resp);
                },
                next);
    });
    seq.add([&](std::function<void()> next) {
        request(app, port,
                get("/m/transcript?id=item-1&offset=50&limit=20",
                    cookieHeader(token)),
                [&](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"), "transcript 200", resp);
                    check(resp.contains("item-1"), "transcript payload", resp);
                    check(lastTranscriptId == QStringLiteral("item-1"),
                          "transcript id passthrough");
                    check(lastOffset == 50, "transcript offset passthrough");
                },
                next);
    });
    seq.add([&](std::function<void()> next) {
        request(app, port, get("/m/transcript?id=nope", cookieHeader(token)),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 404"), "transcript unknown 404",
                          resp);
                },
                next);
    });
    // 18. 主题素材：正常图 200；越界（../ 与编码变体）404；非图 404；无令牌 401。
    seq.add([&](std::function<void()> next) {
        request(app, port, get("/m/theme/ok.png", cookieHeader(token)),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"), "theme 200", resp);
                    check(resp.contains("image/png"), "theme type", resp);
                    check(resp.contains("PNGTESTBYTES"), "theme body", resp);
                },
                next);
    });
    seq.add([&](std::function<void()> next) {
        request(app, port, get("/m/theme/../secret.txt", cookieHeader(token)),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 404"), "traversal 404",
                          resp);
                    check(!resp.contains("TOPSECRET"), "no leak", resp);
                },
                next);
    });
    seq.add([&](std::function<void()> next) {
        request(app, port,
                get("/m/theme/%2e%2e/secret.txt", cookieHeader(token)),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 404"), "encoded traversal 404",
                          resp);
                },
                next);
    });
    seq.add([&](std::function<void()> next) {
        request(app, port, get("/m/theme/ok.png"),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 401"), "theme no token 401",
                          resp);
                },
                next);
    });
    // 18. 配对码过期 → 401。
    seq.add([&](std::function<void()> next) {
        lan.setPairingTtlMs(1);
        const QByteArray shortCode = lan.beginPairing().toUtf8();
        QTimer::singleShot(60, &app, [&, shortCode, next] {
            const QByteArray body =
                QByteArray("{\"code\":\"") + shortCode + "\"}";
            request(app, port, post("/m/pair", body, csrf()),
                    [](const QByteArray &resp) {
                        check(resp.startsWith("HTTP/1.1 401"), "expired 401",
                              resp);
                    },
                    next);
        });
    });
    // 19. 吊销 → 旧令牌立即失效。
    seq.add([&](std::function<void()> next) {
        check(lan.revokeDevice(deviceId), "revoke ok");
        check(!lan.revokeDevice(deviceId), "revoke twice false");
        request(app, port, get("/m/state", cookieHeader(token)),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 401"), "revoked 401", resp);
                },
                next);
    });
    // 20. 防爆破：连续错码到第 5 次后锁 15 分钟（429）。前面过期测试已计 1 次失败。
    seq.add([&](std::function<void()> next) {
        lan.setPairingTtlMs(10 * 60 * 1000);
        lan.beginPairing();
        auto attempt = std::make_shared<std::function<void(int)>>();
        *attempt = [&, attempt, next](int i) {
            if (i >= 8) {
                check(false, "lockout never triggered");
                next();
                return;
            }
            request(app, port,
                    post("/m/pair", R"({"code":"wrong"})", csrf()),
                    [&, attempt, next, i](const QByteArray &resp) {
                        if (resp.startsWith("HTTP/1.1 429")) {
                            check(i >= 3, "lockout after enough failures",
                                  QByteArray::number(i));
                            next();
                            return;
                        }
                        check(resp.startsWith("HTTP/1.1 401"),
                              "wrong code 401", resp);
                        (*attempt)(i + 1);
                    },
                    [] {});
        };
        (*attempt)(0);
    });
    // 21. 超大 body → 413。
    seq.add(ask(post("/m/pair", QByteArray(70000, 'x'), csrf()),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 413"), "big body 413",
                          resp);
                }));
    // 22. 关服务 → 端口拒连；再开 → 恢复。
    seq.add([&](std::function<void()> next) {
        lan.stop();
        check(!lan.isRunning(), "stopped");
        auto *socket = new QTcpSocket(&app);
        QObject::connect(socket, &QTcpSocket::errorOccurred, socket,
                         [socket, next](QAbstractSocket::SocketError error) {
                             check(error == QAbstractSocket::ConnectionRefusedError,
                                   "port closed after stop");
                             socket->deleteLater();
                             next();
                         });
        socket->connectToHost(QHostAddress::LocalHost, port);
    });
    seq.add([&](std::function<void()> next) {
        check(lan.start(port), "restart on same port");
        request(app, port, get("/m/"),
                [](const QByteArray &resp) {
                    check(resp.startsWith("HTTP/1.1 200"), "alive again", resp);
                },
                next);
    });

    seq.start([&] {
        lan.stop();
        std::printf(ok ? "ALL PASS lanweb (%d checks)\n"
                       : "FAIL lanweb (%d checks)\n",
                    checks);
        std::fflush(stdout);
        QCoreApplication::exit(ok ? 0 : 1);
    });

    return app.exec();
}