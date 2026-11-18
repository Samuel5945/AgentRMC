#include "core/LanWebServer.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kMaxBodyBytes = 64 * 1024;
// 手机传文件单独放宽（图片/文档）；局域网传输，25MB 足够且能在 stale 超时内收完。
constexpr qint64 kMaxUploadBytes = 25 * 1024 * 1024;
constexpr int kStaleMs = 10000;
constexpr int kPairFailLimit = 5;
constexpr qint64 kPairLockMs = 15 * 60 * 1000;
constexpr qint64 kPersistThrottleMs = 30 * 1000;
constexpr qint64 kMaxThemeBytes = 32 * 1024 * 1024;

QByteArray randomBytes(int n) {
    QByteArray out(n, '\0');
    auto *rng = QRandomGenerator::system();
    for (int i = 0; i < n; ++i) {
        out[i] = char(rng->bounded(256));
    }
    return out;
}

// 配对助记词表：2048 个常用双字词（qrc，rc_probe/gen_pair_wordlist.py 生成）。
// 2048=2^11，12 词≈132-bit 熵。读一次缓存；读不到回退空表（生成退回 hex）。
const QStringList &pairWordList() {
    static QStringList words = [] {
        QFile f(QStringLiteral(":/pair_words_zh.txt"));
        QStringList list;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QStringList lines =
                QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString w = line.trimmed();
                if (!w.isEmpty()) {
                    list << w;
                }
            }
        }
        return list;
    }();
    return words;
}

// 6 个随机词，空格分隔；词表缺失时退回 32 位 hex（配对绝不能因词表坏掉）。
// 6 词×11bit=66bit，配合「每 IP 5 次失败锁 15 分钟 + 10 分钟时效」的在线防爆破，
// 局域网配对足够安全；用户裁决 6 词（12 词太长，2026-11-14）。
QString makePairCode() {
    const QStringList &words = pairWordList();
    if (words.size() < 2048) {
        return QString::fromLatin1(randomBytes(16).toHex());
    }
    auto *rng = QRandomGenerator::system();
    QStringList picked;
    for (int i = 0; i < 6; ++i) {
        picked << words.at(rng->bounded(2048));
    }
    return picked.join(QLatin1Char(' '));
}

// 配对码归一化：去掉所有空白、转小写。汉字无大小写、词间空格可有可无
// （手机上可能连着打「中国国家…」），hex 回退码也兼容。不降低熵：词序仍须一致。
QString normalizePairCode(const QString &raw) {
    QString s = raw;
    s.remove(QRegularExpression(QStringLiteral("\\s+")));
    return s.toLower();
}

// 令牌/配对码比较：长度不是秘密（都是定长 hex），内容逐字节累积异或，
// 不提前返回，避免按响应时间猜前缀。
bool constantTimeEqual(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^
                static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

QByteArray headerValue(const QList<QByteArray> &lines, const QByteArray &name) {
    for (const QByteArray &line : lines) {
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        if (line.left(colon).trimmed().toLower() == name) {
            return line.mid(colon + 1).trimmed();
        }
    }
    return QByteArray();
}

QByteArray cookieToken(const QList<QByteArray> &lines) {
    const QByteArray cookie = headerValue(lines, "cookie");
    const QList<QByteArray> parts = cookie.split(';');
    for (const QByteArray &part : parts) {
        const QByteArray p = part.trimmed();
        if (p.startsWith("armc_dev=")) {
            return p.mid(9);
        }
    }
    return QByteArray();
}

// CSRF 闸：POST 必须带自定义头。跨站表单带不上自定义头；跨站 fetch 带自定义头
// 会触发预检，而这个服务不开 CORS（OPTIONS 一律 404），预检过不去。
bool hasCsrfHeader(const QList<QByteArray> &lines) {
    return !headerValue(lines, "x-requested-with").isEmpty() ||
           !headerValue(lines, "x-agentrmc-token").isEmpty();
}

QString isoNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

}  // namespace

LanWebServer::LanWebServer(QObject *parent) : QObject(parent) {}

bool LanWebServer::start(quint16 port) {
    if (server_ != nullptr) {
        return true;
    }
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this,
            &LanWebServer::onConnected);
    // 与桥（回环）不同，这个口就是要给局域网手机用的；默认关闭由主窗把关。
    if (!server_->listen(QHostAddress::AnyIPv4, port)) {
        qWarning().noquote() << "[lanweb] 监听失败（" << server_->errorString()
                             << "），手机访问不可用";
        delete server_;
        server_ = nullptr;
        return false;
    }
    return true;
}

void LanWebServer::stop() {
    if (server_ == nullptr) {
        return;
    }
    server_->close();
    // abort 会同步触发 disconnected → buffers_.remove，先拷贝键再遍历。
    const QList<QTcpSocket *> sockets = buffers_.keys();
    for (QTcpSocket *socket : sockets) {
        socket->abort();
    }
    buffers_.clear();
    const QList<QTcpSocket *> draining = drain_.keys();
    for (QTcpSocket *socket : draining) {
        socket->abort();
    }
    drain_.clear();
    server_->deleteLater();
    server_ = nullptr;
}

quint16 LanWebServer::port() const {
    return server_ != nullptr ? server_->serverPort() : 0;
}

void LanWebServer::setStateProvider(std::function<QString()> provider) {
    stateProvider_ = std::move(provider);
}

void LanWebServer::setTranscriptProvider(
    std::function<QString(const QString &, int, int)> provider) {
    transcriptProvider_ = std::move(provider);
}

void LanWebServer::setSessionListProvider(std::function<QString()> provider) {
    sessionListProvider_ = std::move(provider);
}

void LanWebServer::setSessionTranscriptProvider(
    std::function<QString(const QString &, const QString &, int, int)> provider) {
    sessionTranscriptProvider_ = std::move(provider);
}

void LanWebServer::setActionHandler(
    std::function<QJsonObject(const QJsonObject &)> handler) {
    actionHandler_ = std::move(handler);
}

void LanWebServer::setThemeRoot(const QString &root) {
    themeRoot_ = root;
}

void LanWebServer::setUploadHandler(
    std::function<QJsonObject(const QString &, const QString &, const QByteArray &)>
        handler) {
    uploadHandler_ = std::move(handler);
}

void LanWebServer::setDevices(const QJsonArray &devices) {
    devices_ = devices;  // 恢复持久化数据，不回发 devicesChanged
}

QString LanWebServer::beginPairing() {
    pairingCode_ = makePairCode();
    pairingExpires_ = QDateTime::currentDateTimeUtc().addMSecs(pairingTtlMs_);
    return pairingCode_;
}

void LanWebServer::endPairing() {
    pairingCode_.clear();
    pairingExpires_ = QDateTime();
}

QJsonObject LanWebServer::pairingInfo() const {
    const bool active =
        !pairingCode_.isEmpty() &&
        QDateTime::currentDateTimeUtc() < pairingExpires_;
    return QJsonObject{
        {QStringLiteral("active"), active},
        {QStringLiteral("code"), active ? pairingCode_ : QString()},
        {QStringLiteral("expiresAt"),
         active ? pairingExpires_.toString(Qt::ISODate) : QString()},
    };
}

QJsonArray LanWebServer::devices() const {
    QJsonArray out;
    for (const QJsonValue &value : devices_) {
        QJsonObject device = value.toObject();
        device.remove(QStringLiteral("tokenHash"));  // 哈希只留在服务端
        out.append(device);
    }
    return out;
}

bool LanWebServer::revokeDevice(const QString &id) {
    for (int i = 0; i < devices_.size(); ++i) {
        if (devices_.at(i).toObject().value(QStringLiteral("id")).toString() ==
            id) {
            devices_.removeAt(i);
            emit devicesChanged(devices_);
            return true;
        }
    }
    return false;
}

bool LanWebServer::isAllowedAction(const QString &cmd) {
    // 白名单制：手机上点了会在 PC 弹 Qt 对话框或改本机配置的命令一律不进。
    // tts 会弹 TtsSettingsDialog，readLast 只是让 PC 念最近转写，可以进。
    return cmd == QStringLiteral("ack") || cmd == QStringLiteral("dispatch") ||
           cmd == QStringLiteral("dispatchSession") ||
           cmd == QStringLiteral("answer") ||
           cmd == QStringLiteral("open") ||
           cmd == QStringLiteral("raiseZcode") ||
           cmd == QStringLiteral("restartZcode") ||
           cmd == QStringLiteral("readLast");
}

QStringList LanWebServer::lanUrls(quint16 port) {
    QStringList preferred;  // 192.168 段（家用路由最常见）
    QStringList privateNet; // 10 / 172.16-31
    QStringList others;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) ||
            !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol ||
                addr.isLoopback()) {
                continue;
            }
            const QString ip = addr.toString();
            const QString url =
                QStringLiteral("http://%1:%2/m/").arg(ip).arg(port);
            if (ip.startsWith(QStringLiteral("192.168."))) {
                preferred << url;
            } else if (ip.startsWith(QStringLiteral("10."))) {
                privateNet << url;
            } else if (ip.startsWith(QStringLiteral("172."))) {
                const int second = ip.section('.', 1, 1).toInt();
                (second >= 16 && second <= 31 ? privateNet : others) << url;
            } else {
                others << url;
            }
        }
    }
    QStringList out;
    for (const QStringList &group : {preferred, privateNet, others}) {
        for (const QString &url : group) {
            if (!out.contains(url)) {
                out.append(url);
            }
        }
    }
    return out;
}

void LanWebServer::onConnected() {
    while (server_->hasPendingConnections()) {
        auto *socket = server_->nextPendingConnection();
        buffers_.insert(socket, QByteArray{});
        connect(socket, &QTcpSocket::disconnected, socket,
                &QTcpSocket::deleteLater);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            buffers_.remove(socket);
            drain_.remove(socket);
        });
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { onReadable(socket); });
        // 半截请求挂着不走：超时清掉（与桥同参数）。排干中的也算。
        QTimer::singleShot(kStaleMs, socket, [this, socket] {
            if (buffers_.contains(socket) || drain_.contains(socket)) {
                dropSocket(socket);
            }
        });
    }
}

void LanWebServer::onReadable(QTcpSocket *socket) {
    if (drain_.contains(socket)) {
        const QByteArray chunk = socket->readAll();
        drain_[socket] -= chunk.size();
        if (drain_[socket] <= 0) {
            drain_.remove(socket);
            finish(socket, "413 Payload Too Large");
        }
        return;
    }
    QByteArray &buffer = buffers_[socket];
    buffer.append(socket->readAll());
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer.size() > kMaxHeaderBytes) {
            finish(socket, "413 Payload Too Large");
        }
        return;
    }
    const QByteArray head = buffer.left(headerEnd);
    const QList<QByteArray> headerLines = head.split('\n');
    const QByteArray requestLine = headerLines.value(0).trimmed();
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2) {
        finish(socket, "400 Bad Request");
        return;
    }
    const QByteArray method = parts.at(0);
    QByteArray path = parts.at(1);
    QByteArray query;
    const int queryAt = path.indexOf('?');
    if (queryAt >= 0) {
        query = path.mid(queryAt + 1);
        path = path.left(queryAt);
    }

    if (method == "POST") {
        int contentLength = -1;
        const QByteArray lengthText = headerValue(headerLines, "content-length");
        if (!lengthText.isEmpty()) {
            contentLength = lengthText.toInt();
        }
        if (contentLength < 0) {
            finish(socket, "400 Bad Request");
            return;
        }
        // 上传路由单独放宽到 25MB；其余路由仍 64KB。
        const qint64 bodyLimit =
            (path == "/m/upload") ? kMaxUploadBytes : kMaxBodyBytes;
        if (contentLength > bodyLimit) {
            // 排干再回 413（见 drain_ 注释）。声称超过 1MB 的 body 不陪跑：
            // 直接断开，这种客户端不是浏览器就是攻击。
            constexpr qint64 kDrainCap = 1024 * 1024;
            const QByteArray body = buffer.mid(headerEnd + 4);
            buffers_.remove(socket);
            if (contentLength > kDrainCap) {
                dropSocket(socket);
                return;
            }
            const qint64 remaining = contentLength - body.size();
            if (remaining <= 0) {
                finish(socket, "413 Payload Too Large");
            } else {
                drain_.insert(socket, remaining);
            }
            return;
        }
        const QByteArray body = buffer.mid(headerEnd + 4);
        if (body.size() < contentLength) {
            return;  // 还没收全，等下一段
        }
        route(socket, method, path, query, headerLines, body.left(contentLength));
        return;
    }
    if (method == "GET") {
        route(socket, method, path, query, headerLines, QByteArray());
        return;
    }
    // OPTIONS 也落这里：不开 CORS，预检一律 404。
    finish(socket, "404 Not Found");
}

void LanWebServer::route(QTcpSocket *socket, const QByteArray &method,
                         const QByteArray &path, const QByteArray &query,
                         const QList<QByteArray> &headerLines,
                         const QByteArray &body) {
    // Origin 存在时必须与 Host 同源（浏览器跨站请求的第一道闸）。
    const QByteArray origin = headerValue(headerLines, "origin");
    if (!origin.isEmpty()) {
        const QByteArray host = headerValue(headerLines, "host");
        if (origin != "http://" + host && origin != "https://" + host) {
            finish(socket, "403 Forbidden");
            return;
        }
    }
    if (path == "/m" && method == "GET") {
        finish(socket, "301 Moved Permanently", QByteArray(), nullptr,
               "Location: /m/\r\n");
        return;
    }
    if ((path == "/m/" || path == "/m/index.html") && method == "GET") {
        // 静态页免鉴权：里面没有任何数据，数据全走要令牌的 /m/state。
        finish(socket, "200 OK", mobileHtml().toUtf8(),
               "text/html; charset=utf-8", QByteArray(),
               "no-store, must-revalidate");
        return;
    }
    if (path == "/m/pair" && method == "POST") {
        routePair(socket, headerLines, body);
        return;
    }
    if (path == "/m/state" && method == "GET") {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        const QString state =
            stateProvider_ ? stateProvider_() : QStringLiteral("{}");
        finish(socket, "200 OK", state.toUtf8(), "application/json");
        return;
    }
    if (path == "/m/sessions" && method == "GET") {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        const QString list =
            sessionListProvider_ ? sessionListProvider_() : QStringLiteral("{}");
        finish(socket, "200 OK", list.toUtf8(), "application/json");
        return;
    }
    if (path == "/m/session" && method == "GET") {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        QString exe, session;
        int offset = 0, limit = 50;
        const QList<QByteArray> pairs = query.split('&');
        for (const QByteArray &pair : pairs) {
            const int eq = pair.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            const QByteArray key = pair.left(eq);
            const QByteArray value =
                QByteArray::fromPercentEncoding(pair.mid(eq + 1));
            if (key == "exe") {
                exe = QString::fromUtf8(value);
            } else if (key == "session") {
                session = QString::fromUtf8(value);
            } else if (key == "offset") {
                offset = qMax(0, value.toInt());
            } else if (key == "limit") {
                limit = qBound(1, value.toInt(), 100);
            }
        }
        if (exe.isEmpty() || session.isEmpty() || !sessionTranscriptProvider_) {
            finish(socket, "400 Bad Request");
            return;
        }
        const QString chat =
            sessionTranscriptProvider_(exe, session, offset, limit);
        if (chat.isEmpty()) {
            finish(socket, "404 Not Found");
            return;
        }
        finish(socket, "200 OK", chat.toUtf8(), "application/json");
        return;
    }
    if (path == "/m/transcript" && method == "GET") {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        // 聊天页分页：id=待办 id，offset=从最新往旧跳过条数，limit=页大小。
        QString id;
        int offset = 0;
        int limit = 50;
        const QList<QByteArray> pairs = query.split('&');
        for (const QByteArray &pair : pairs) {
            const int eq = pair.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            const QByteArray key = pair.left(eq);
            const QString value =
                QUrl::fromPercentEncoding(pair.mid(eq + 1));
            if (key == "id") {
                id = value;
            } else if (key == "offset") {
                offset = value.toInt();
            } else if (key == "limit") {
                limit = value.toInt();
            }
        }
        const QString transcript =
            transcriptProvider_ ? transcriptProvider_(id, offset, limit)
                                : QString();
        if (transcript.isEmpty()) {
            finish(socket, "404 Not Found");
            return;
        }
        finish(socket, "200 OK", transcript.toUtf8(), "application/json");
        return;
    }
    if (path == "/m/action" && method == "POST") {
        if (!hasCsrfHeader(headerLines)) {
            finish(socket, "403 Forbidden");
            return;
        }
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        const QString cmd = obj.value(QStringLiteral("cmd")).toString();
        if (!isAllowedAction(cmd)) {
            finish(socket, "403 Forbidden",
                   QByteArrayLiteral("{\"ok\":false,\"code\":403}"),
                   "application/json");
            return;
        }
        const QJsonObject result =
            actionHandler_
                ? actionHandler_(obj)
                : QJsonObject{{QStringLiteral("ok"), false},
                              {QStringLiteral("code"), 400}};
        const int code = result.value(QStringLiteral("code")).toInt(400);
        const char *status = code == 200   ? "200 OK"
                             : code == 403 ? "403 Forbidden"
                             : code == 409 ? "409 Conflict"
                                           : "400 Bad Request";
        finish(socket, status,
               QJsonDocument(result).toJson(QJsonDocument::Compact),
               "application/json");
        return;
    }
    if (path == "/m/upload" && method == "POST") {
        if (!hasCsrfHeader(headerLines)) {
            finish(socket, "403 Forbidden");
            return;
        }
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        // id=待办 id（query）；文件名走 X-AgentRMC-Filename 头（percent 编码，
        // 兼容中文）。body 是文件原始字节。落盘与路径解析在主窗 uploadHandler。
        QString id;
        const QList<QByteArray> pairs = query.split('&');
        for (const QByteArray &pair : pairs) {
            const int eq = pair.indexOf('=');
            if (eq > 0 && pair.left(eq) == "id") {
                id = QUrl::fromPercentEncoding(pair.mid(eq + 1));
            }
        }
        const QString filename = QUrl::fromPercentEncoding(
            headerValue(headerLines, "x-agentrmc-filename"));
        const QJsonObject result =
            uploadHandler_
                ? uploadHandler_(id, filename, body)
                : QJsonObject{{QStringLiteral("ok"), false},
                              {QStringLiteral("code"), 400}};
        const int code = result.value(QStringLiteral("code")).toInt(400);
        const char *status = code == 200   ? "200 OK"
                             : code == 404 ? "404 Not Found"
                             : code == 413 ? "413 Payload Too Large"
                                           : "400 Bad Request";
        finish(socket, status,
               QJsonDocument(result).toJson(QJsonDocument::Compact),
               "application/json");
        return;
    }
    if (path.startsWith("/m/theme/") && method == "GET") {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        routeTheme(socket, path);
        return;
    }
    // 桥的 agent 侧路由（/notify /command /dispatch/* …）永不出现在这个口上。
    finish(socket, "404 Not Found");
}

void LanWebServer::routePair(QTcpSocket *socket,
                             const QList<QByteArray> &headerLines,
                             const QByteArray &body) {
    if (!hasCsrfHeader(headerLines)) {
        finish(socket, "403 Forbidden");
        return;
    }
    const QString ip = socket->peerAddress().toString();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    PairFail &fail = pairFails_[ip];
    if (fail.lockedUntil.isValid() && now < fail.lockedUntil) {
        finish(socket, "429 Too Many Requests");
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    const QString code =
        normalizePairCode(obj.value(QStringLiteral("code")).toString());
    const bool ok = !pairingCode_.isEmpty() && now < pairingExpires_ &&
                    constantTimeEqual(code.toUtf8(),
                                      normalizePairCode(pairingCode_).toUtf8());
    if (!ok) {
        ++fail.count;
        if (fail.count >= kPairFailLimit) {
            fail.lockedUntil = now.addMSecs(kPairLockMs);
            fail.count = 0;
        }
        finish(socket, "401 Unauthorized",
               QByteArrayLiteral("{\"ok\":false}"), "application/json");
        return;
    }
    fail = PairFail{};
    QString name =
        obj.value(QStringLiteral("name")).toString().simplified().left(40);
    if (name.isEmpty()) {
        name = QString::fromUtf8("手机");
    }
    const QByteArray token = randomBytes(16).toHex();
    const QJsonObject device{
        {QStringLiteral("id"), QString::fromLatin1(randomBytes(4).toHex())},
        {QStringLiteral("name"), name},
        {QStringLiteral("tokenHash"),
         QString::fromLatin1(
             QCryptographicHash::hash(token, QCryptographicHash::Sha256)
                 .toHex())},
        {QStringLiteral("pairedAt"), isoNow()},
        {QStringLiteral("lastSeen"), isoNow()},
    };
    devices_.append(device);
    lastPersist_ = now;
    emit devicesChanged(devices_);
    // 配对码/令牌不落日志。cookie 限 /m 路径，HttpOnly 防脚本偷，Strict 防跨站带。
    // Max-Age 必须带：不带是会话级 Cookie，手机浏览器一关就丢，每次重启宿主
    // 都要重新扫码（用户 2026-11-16）。设备随时可在系统页吊销，长 Cookie 无妨。
    const QByteArray cookie = "Set-Cookie: armc_dev=" + token +
                              "; Path=/m; HttpOnly; SameSite=Strict; " +
                              "Max-Age=31536000\r\n";
    finish(socket, "200 OK", QByteArrayLiteral("{\"ok\":true}"),
           "application/json", cookie);
}

void LanWebServer::routeTheme(QTcpSocket *socket, const QByteArray &path) {
    if (themeRoot_.isEmpty()) {
        finish(socket, "404 Not Found");
        return;
    }
    const QString rel = QUrl::fromPercentEncoding(
        path.mid(QByteArrayLiteral("/m/theme/").size()));
    if (rel.isEmpty() || rel.contains(QLatin1Char('\\')) ||
        QDir::isAbsolutePath(rel)) {
        finish(socket, "404 Not Found");
        return;
    }
    const QStringList segments = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        if (segment == QStringLiteral("..") || segment == QStringLiteral(".")) {
            finish(socket, "404 Not Found");
            return;
        }
    }
    // canonical 解析后必须仍在主题根内：软链指出去也不行。
    const QString rootCanon = QDir(themeRoot_).canonicalPath();
    const QFileInfo info(QDir(themeRoot_).absoluteFilePath(rel));
    const QString canon = info.canonicalFilePath();
    if (rootCanon.isEmpty() || canon.isEmpty() ||
        !canon.startsWith(rootCanon + QLatin1Char('/'))) {
        finish(socket, "404 Not Found");
        return;
    }
    const QString suffix = info.suffix().toLower();
    const char *type = suffix == QStringLiteral("png")    ? "image/png"
                       : suffix == QStringLiteral("jpg")  ? "image/jpeg"
                       : suffix == QStringLiteral("jpeg") ? "image/jpeg"
                       : suffix == QStringLiteral("webp") ? "image/webp"
                       : suffix == QStringLiteral("gif")  ? "image/gif"
                                                          : nullptr;
    if (type == nullptr) {
        finish(socket, "404 Not Found");
        return;
    }
    QFile file(canon);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxThemeBytes) {
        finish(socket, "404 Not Found");
        return;
    }
    finish(socket, "200 OK", file.readAll(), type, QByteArray(),
           "private, max-age=600");
}

int LanWebServer::authDevice(const QList<QByteArray> &headerLines) {
    QByteArray token = headerValue(headerLines, "x-agentrmc-token");
    if (token.isEmpty()) {
        token = cookieToken(headerLines);
    }
    if (token.isEmpty()) {
        return -1;
    }
    const QByteArray hash =
        QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex();
    for (int i = 0; i < devices_.size(); ++i) {
        const QJsonObject device = devices_.at(i).toObject();
        if (!constantTimeEqual(
                hash,
                device.value(QStringLiteral("tokenHash")).toString().toUtf8())) {
            continue;
        }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QJsonObject updated = device;
        updated.insert(QStringLiteral("lastSeen"), isoNow());
        devices_.replace(i, updated);
        // 手机 2.5s 一轮询：lastSeen 落盘节流 30s，别每轮写一次配置。
        if (!lastPersist_.isValid() ||
            lastPersist_.msecsTo(now) > kPersistThrottleMs) {
            lastPersist_ = now;
            emit devicesChanged(devices_);
        }
        return i;
    }
    return -1;
}

void LanWebServer::finish(QTcpSocket *socket, const char *status,
                          const QByteArray &body, const char *contentType,
                          const QByteArray &extraHeaders,
                          const char *cacheControl) {
    QByteArray response =
        QByteArray("HTTP/1.1 ") + status + "\r\nConnection: close\r\n";
    if (contentType != nullptr) {
        response += QByteArray("Content-Type: ") + contentType + "\r\n";
    }
    if (cacheControl != nullptr) {
        response += QByteArray("Cache-Control: ") + cacheControl + "\r\n";
    }
    response += extraHeaders;
    response += "Content-Length: " + QByteArray::number(body.size()) +
                "\r\n\r\n";
    response += body;
    socket->write(response);
    buffers_.remove(socket);
    socket->disconnectFromHost();
}

void LanWebServer::dropSocket(QTcpSocket *socket) {
    buffers_.remove(socket);
    drain_.remove(socket);
    socket->abort();
}

// 手机页：与桌面外壳同一份数据契约（/m/state = lanShellState 裁剪版），
// 动作走 /m/action 白名单 → 主窗 handleWebAction，页面里不另存状态机。
// 排版竖屏单列、底部页签（拇指区）、触控目标 ≥44px、滚动条隐藏。
// http:// 不是安全上下文：没有后台推送/麦克风，实时性靠页面可见时 2.5s 轮询。
QString LanWebServer::mobileHtml() const {
    return QString::fromUtf8(R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>AgentRMC</title>
<style>
 :root{
  --bg:#f3f5f8;--surface:#ffffff;--surface2:#eef1f5;
  --text:#171e2a;--text-soft:#1c2331;--text-dim:#2a323f;--muted:#48525f;--muted2:#5a6472;
  --ghost:#4a5361;--faint:#8f98a3;--line:#c9d0d9;--accent:#4f8cff;
  --accent-hover:#6299ff;--on-accent:#ffffff;--danger:#e5484d;--warn:#d29922;
  --ok:#3fb950;--pending:#f0883e;--info:#58a6ff;
  --w6:rgba(15,23,42,.047);--w8:rgba(15,23,42,.063);--w10:rgba(15,23,42,.078);
  --w14:rgba(15,23,42,.110);--w18:rgba(15,23,42,.141);--w30:rgba(15,23,42,.235);
  --a26:rgba(79,140,255,.102);--a36:rgba(79,140,255,.141);--a64:rgba(79,140,255,.251)}
 *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
 html,body{margin:0;height:100%;color:var(--text);
   font-family:"Microsoft YaHei UI","Segoe UI",system-ui,sans-serif}
 body{display:flex;flex-direction:column;height:100dvh;overflow:hidden;
   background:var(--bg)}
 [hidden]{display:none !important}
 button{font-family:inherit;touch-action:manipulation}
 /* 配对屏 */
 #pair{flex:1;display:flex;align-items:center;justify-content:center;padding:24px}
 .pbox{width:100%;max-width:360px;display:flex;flex-direction:column;gap:12px;
   padding:28px 22px;border-radius:18px;border:1px solid var(--w18);
   background:linear-gradient(180deg,var(--surface),var(--surface2))}
 .plogo{width:44px;height:44px;border-radius:12px;background:var(--accent);
   color:var(--on-accent);font-weight:700;font-size:22px;display:flex;
   align-items:center;justify-content:center}
 .pbox h2{margin:0;font-size:18px}
 .pbox p{margin:0;font-size:13px;color:var(--muted);line-height:1.6}
 .pbox input{font-size:16px;color:var(--text);background:var(--w6);
   border:1px solid var(--w14);border-radius:10px;padding:12px;font-family:inherit}
 .pbox input:focus{outline:none;border-color:var(--accent);background:var(--w8)}
 #pairErr{font-size:13px;color:var(--danger);min-height:18px;line-height:1.5}
 /* 应用骨架 */
 #app{flex:1;display:flex;flex-direction:column;min-height:0}
 #top{flex:0 0 auto;display:flex;align-items:center;gap:8px;padding:10px 14px;
   padding-top:calc(10px + env(safe-area-inset-top))}
 .logo{width:26px;height:26px;border-radius:7px;background:var(--accent);
   color:var(--on-accent);font-weight:700;font-size:14px;display:flex;
   align-items:center;justify-content:center;flex:0 0 26px}
 .brand{font-size:16px;font-weight:700}
 .dot{width:8px;height:8px;border-radius:4px;background:var(--faint);flex:0 0 8px}
 .dot.on{background:var(--ok)}
 .dot.bad{background:var(--danger)}
 .spacer{margin-left:auto}
 .pill{font-size:12px;color:var(--muted);border:1px solid var(--w14);
   background:var(--w6);border-radius:12px;padding:3px 10px}
 .pill.on{color:var(--ok);font-weight:600}
 #offline{flex:0 0 auto;margin:0 14px 6px;padding:8px 12px;border-radius:10px;
   font-size:13px;color:var(--pending);background:var(--w6);border:1px solid var(--w14)}
 #pages{flex:1 1 auto;min-height:0;display:flex;flex-direction:column}
 .page{flex:1;min-height:0;display:flex;flex-direction:column}
 .scroll{flex:1;min-height:0;overflow-y:auto;scrollbar-width:none;
   padding:4px 14px 14px;-webkit-overflow-scrolling:touch}
 .scroll::-webkit-scrollbar{width:0;height:0}
/* 待办卡分两层：.hint 是手势底下露出来的动作提示，.face 才是卡面。
   pan-y 把纵向滚动留给列表、横向留给手势；多选切换只改 class，不重建卡片。 */
.card{position:relative;margin-bottom:10px;border-radius:14px;overflow:hidden;
  background:transparent;touch-action:pan-y;-webkit-user-select:none;user-select:none;
  -webkit-touch-callout:none;isolation:isolate}
.face{position:relative;padding:13px 12px 13px 18px;
  border-radius:14px;border:1px solid var(--w18);
  background:linear-gradient(180deg,var(--surface),var(--surface2));
  box-shadow:0 1px 2px var(--w6);
  transition:transform .24s cubic-bezier(.2,.8,.2,1),opacity .18s ease,
    border-color .16s,background .16s,padding-left .2s ease,box-shadow .16s;
  will-change:transform}
.bar{position:absolute;left:8px;top:14px;width:4px;height:34px;border-radius:2px;
  transition:opacity .16s ease}
/* 滑动提示：右滑露左边「启动 Agent」，左滑露右边「标为已处理」。
   底色用 ::before 上色再压不透明度，换主题不用改这里。 */
.hint{position:absolute;inset:0;display:flex;align-items:center;gap:5px;
  font-size:12px;font-weight:600;opacity:0;pointer-events:none;
  transition:opacity .14s linear}
.hint::before{content:'';position:absolute;inset:0}
.hint>*{position:relative}
.hint .ic{width:24px;height:24px;border-radius:50%;display:flex;
  align-items:center;justify-content:center;font-size:11px;flex:0 0 24px;
  box-shadow:0 2px 8px var(--w14);transition:transform .16s ease}
.hint .label{white-space:nowrap}
.hint.l{justify-content:flex-start;padding-left:10px;color:var(--accent)}
.hint.l::before{background:var(--accent);opacity:.14}
.hint.l .ic{background:var(--accent);color:var(--on-accent)}
.hint.r{justify-content:flex-end;padding-right:10px;color:var(--danger)}
.hint.r::before{background:var(--danger);opacity:.14}
.hint.r .ic{background:var(--danger);color:#fff}
.hint.armed{font-weight:700}
.hint.armed .ic{transform:scale(1.12)}
 .l1{display:flex;align-items:center;gap:7px;min-width:0}
 .name{font-weight:700;font-size:15px;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap;flex:1 1 auto;min-width:0}
 .stage{font-size:11px;flex:0 0 auto;line-height:1.4;padding:2px 7px;
   border-radius:10px;background:var(--w6);white-space:nowrap}
 .time{margin-left:auto;font-size:11.5px;color:var(--ghost);flex:0 0 auto;
   white-space:nowrap}
 .where{font-size:12px;color:var(--muted2);margin-top:3px;overflow:hidden;
   text-overflow:ellipsis;white-space:nowrap}
 .msg{font-size:14px;color:var(--text-soft);margin-top:7px;line-height:1.55;
   word-break:break-word;display:-webkit-box;-webkit-line-clamp:3;
   -webkit-box-orient:vertical;overflow:hidden}
 .notice{font-size:13px;color:var(--text-dim);margin-top:8px;padding:8px 10px;
   border-radius:8px;background:var(--w6);border:1px solid var(--w14);
   line-height:1.5;word-break:break-word}
 .acts{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}
 .acts.single{margin-top:8px;justify-content:flex-end}
 .acts .btn{flex:1 1 0;min-width:0}
 .icobtn{min-width:44px;height:40px;padding:0 12px;font-size:16px;
   border-radius:10px;border:1px solid var(--w18);background:var(--w6);
   color:var(--text-dim)}
 .icobtn:active{background:var(--w10)}
 .hbtn{min-height:38px;padding:6px 11px;font-size:13px;border-radius:10px;
   border:1px solid var(--w18);background:var(--w6);color:var(--text-dim)}
 .hbtn.on{background:var(--accent);border-color:var(--accent);color:var(--on-accent)}
 .hbtn:disabled{opacity:.45}
 /* 多选是低频操作：从顶栏移入手势提示行，保留 44px 命中区但只显示弱化文字。 */
 #multiSw{flex:0 0 auto;min-height:44px;min-width:42px;padding:0 4px;
   border:0;background:transparent;color:var(--muted2);font-size:12px;
   font-weight:600;border-radius:8px}
 #multiSw.on{color:var(--accent);font-weight:700}
 #multiSw:active{background:var(--w6)}
 #multiSw:disabled{opacity:.4}
 #workHint{flex:0 0 auto;margin:0 8px 2px 14px;font-size:11.5px;line-height:1.5;
   color:var(--muted2);display:flex;align-items:center;gap:4px;min-height:44px}
 #workHintText{flex:1 1 auto;min-width:0;display:flex;align-items:center;gap:6px;
   overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
 #workHintText::before{content:'';width:4px;height:4px;border-radius:50%;
   background:var(--accent);opacity:.7;flex:0 0 4px}
/* 多选：卡片原位让出勾选圈，选中描边、淡色底和勾选对齐；不再整表重建。 */
.card.sel .face{border-color:var(--accent);
  background:linear-gradient(180deg,var(--a26),var(--a26)),
    linear-gradient(180deg,var(--surface),var(--surface2));
  box-shadow:0 0 0 1px var(--a36),0 3px 12px var(--w8)}
.card.sel .msg{color:var(--text)}
.card.multi .face{padding-left:48px}
.card.multi .bar{opacity:0}
.card.multi .zrestart{display:none}
.chk{position:absolute;left:12px;top:13px;width:24px;height:24px;border-radius:50%;
  border:2px solid var(--w30);background:var(--surface);display:flex;align-items:center;
  justify-content:center;color:var(--on-accent);opacity:0;transform:scale(.55);
  transition:opacity .16s ease,transform .2s cubic-bezier(.2,.9,.3,1.25),
    background .14s,border-color .14s,box-shadow .14s}
.card.multi .chk{opacity:1;transform:scale(1)}
.chk.on{background:var(--accent);border-color:var(--accent);
  box-shadow:0 2px 7px var(--a36)}
.chk.on::before{content:'';width:8px;height:4px;border-left:2px solid currentColor;
  border-bottom:2px solid currentColor;transform:rotate(-45deg) translate(1px,-1px)}
.chk.pop{animation:chkpop .24s ease}
@keyframes chkpop{0%{transform:scale(1)}45%{transform:scale(1.18)}100%{transform:scale(1)}}
/* 销账动效：手势松手时从当前位置滑出；批量操作则先滑出再收高。 */
.card.gone{overflow:hidden;pointer-events:none;
  transition:max-height .22s ease .12s,margin-bottom .22s ease .12s}
/* 批量销账：原位轻移淡出，高度等字快看不见了再收，避免正文被横向切一块。 */
.card.gone:not(.swiped){transition:max-height .18s ease .18s,
  margin-bottom .18s ease .18s}
.card.gone:not(.swiped) .face{animation:ackOut .18s ease-in forwards}
.card.gone.swiped .face{animation:none}
@keyframes ackOut{55%{opacity:.5}to{transform:translateX(-16px) scale(.985);
  opacity:0}}
#batchBar{flex:0 0 auto;display:flex;gap:7px;align-items:center;
  padding:9px;margin:0 12px 9px;background:var(--surface);
  border:1px solid var(--w18);border-radius:15px;
  box-shadow:0 7px 22px var(--w14);animation:rise .22s cubic-bezier(.22,.61,.36,1)}
@keyframes rise{from{transform:translateY(12px);opacity:0}to{transform:none;opacity:1}}
#batchBar .btn{min-height:44px;padding:8px 11px}
#batchBar .btn.primary{flex:1 1 auto;min-width:0;white-space:nowrap}
#batchCount{flex:0 0 auto;font-size:12px;font-weight:700;color:var(--accent);
  background:var(--a26);border-radius:10px;padding:5px 8px;white-space:nowrap}
#batchAll{flex:0 0 auto;min-height:44px;padding:0 9px;font-size:13px}
#batchCancel{flex:0 0 auto;padding-left:10px;padding-right:10px}
/* 底部提示：颜色反转（浅色主题深底白字、深色主题浅底黑字），不用另配一套。 */
#toast{position:fixed;left:50%;bottom:calc(92px + env(safe-area-inset-bottom));
  transform:translate(-50%,10px);opacity:0;pointer-events:none;z-index:20;
  max-width:76vw;text-align:center;line-height:1.45;font-size:13px;font-weight:600;
  padding:9px 16px;border-radius:20px;background:var(--text);color:var(--bg);
  transition:opacity .18s ease,transform .18s ease}
#toast.on{opacity:1;transform:translate(-50%,0)}
@keyframes bump{0%{transform:scale(1)}45%{transform:scale(1.32)}100%{transform:scale(1)}}
 #tabCount.bump{animation:bump .3s ease}
 @media(prefers-reduced-motion:reduce){
  .face,.chk,.hint,#toast,#batchBar,.cnotice,.csend,.cbox{
   transition-duration:.01ms !important;animation-duration:.01ms !important}
  .chk.pop,#tabCount.bump,.dload::before,.cnotice{animation:none !important}
 }
 .btn{min-height:44px;border:1px solid var(--w18);background:var(--w6);
   color:var(--text-dim);font-size:14px;border-radius:10px;padding:8px 14px}
 .btn:active{background:var(--w10)}
 .btn.primary{background:var(--accent);border-color:transparent;
   color:var(--on-accent);font-weight:600}
 .btn.primary:active{background:var(--accent-hover)}
 .btn.warn{color:var(--warn);border-color:var(--w30)}
 .btn:disabled{opacity:.5}
 /* 聊天页：点卡片进入，打开即最新记录，旧记录上翻按页加载。
    Agent 回复平铺全文（长代码/长文不用气泡挤宽度），「我」的话保留右侧气泡。 */
 #pchat{padding:0 14px}
 .chathead{flex:0 0 auto;display:flex;align-items:center;gap:10px;padding:4px 0 8px}
 .chathead .btn{min-height:40px;padding:6px 12px;flex:0 0 auto}
 .chtitles{flex:1;min-width:0}
 .chtitles .name{font-weight:700;font-size:16px;overflow:hidden;
   text-overflow:ellipsis;white-space:nowrap}
 .chtitles .where{font-size:12px;color:var(--muted2);margin-top:1px;overflow:hidden;
   text-overflow:ellipsis;white-space:nowrap}
 .chatwrap{flex:1 1 auto;min-height:0;position:relative}
 .chatlist{position:absolute;inset:0;overflow-y:auto;scrollbar-width:none;
   display:flex;flex-direction:column;gap:10px;padding:10px 2px;
   -webkit-overflow-scrolling:touch}
 .chatlist::-webkit-scrollbar{width:0;height:0}
 .bub{flex:0 0 auto}
 .bub.user{align-self:flex-end;max-width:86%;padding:9px 12px;border-radius:13px;
   font-size:14px;line-height:1.6;white-space:pre-wrap;word-break:break-word;
   background:var(--a26);border:1px solid var(--a64);color:var(--text)}
 .bub.assistant{align-self:stretch;max-width:100%;padding:2px 2px;
   font-size:14px;line-height:1.65;white-space:pre-wrap;word-break:break-word;
   color:var(--text-soft)}
 .bub .who{display:block;font-weight:600;font-size:11px;color:var(--muted);
   margin-bottom:2px}
 .bub .bt{display:block;font-size:11px;color:var(--muted2);margin-top:4px}
 .reqcard{flex:0 0 auto;align-self:stretch;margin:6px 0;padding:10px 12px;
   border-radius:12px;background:var(--w8);border:1px solid var(--a64)}
 .reqbtn{display:block;width:100%;text-align:left;margin:4px 0;padding:9px 10px;
   border-radius:9px;border:1px solid var(--a36);background:transparent;
   font-size:13px;color:var(--text);min-height:38px}
 .reqbtn.on{border-color:var(--accent);background:var(--a26)}
 .reqcustom{display:block;width:100%;box-sizing:border-box;margin:4px 0;
   padding:9px 10px;border-radius:9px;border:1px solid var(--a36);
   background:transparent;font-size:13px;color:var(--text);min-height:38px}
 .reqsend{margin-top:6px}
 .reqrow{display:flex;gap:8px;margin-top:6px}
 .reqrow .reqbtn{flex:1;width:auto;text-align:center}
 .reqstate{font-size:12px;color:var(--muted);margin-top:4px}
 .reqhead{font-weight:600;font-size:13px;color:var(--text);margin-bottom:4px}
 .reqreason{font-size:12px;color:var(--text-soft);white-space:pre-wrap;
   word-break:break-word;margin-bottom:4px}
 .reqq{margin:4px 0}
 .reqqt{font-size:13px;color:var(--text);white-space:pre-wrap;word-break:break-word}
 .reqopt{font-size:12px;color:var(--text-soft);padding:1px 0 1px 8px}
 .reqhint{font-size:11px;color:var(--muted2);margin-top:6px}
 .chatload{align-self:center;font-size:12px;color:var(--muted2);padding:4px;
   flex:0 0 auto}
 .chatempty{flex:1;display:flex;align-items:center;justify-content:center;
   color:var(--muted);font-size:14px;text-align:center;padding:24px;
   line-height:1.7;white-space:pre-line}
 /* 底部输入区重构（2026-11-18，与桌面端同款）：composer 卡片——状态行
    （notice）在上，输入卡在下；textarea 多行自增高，附件/模型/发送收进卡内
    底行。左右留白 + 安全区，防圆角/手势条裁切；输入 16px 防 iOS 聚焦放大。 */
 .chatfoot{position:relative;padding:8px 0 calc(12px + env(safe-area-inset-bottom))}
 .cbar{display:flex;align-items:center;gap:8px}
 .cbar[hidden]{display:none !important}
 .cnotice{flex:1;min-width:0;font-size:12.5px;color:var(--text-dim);
   line-height:1.5;word-break:break-word;white-space:pre-wrap;padding:7px 10px;
   border-radius:10px;background:var(--w6);border:1px solid var(--w14);
   animation:crise .18s ease}
 .cnotice.good{color:var(--ok);border-color:var(--a64);background:var(--a26)}
 .cnotice.bad{color:var(--warn);border-color:var(--w30)}
 .cbox{display:flex;flex-direction:column;gap:2px;margin-top:6px;
   padding:4px 8px 6px;border-radius:16px;border:1px solid var(--w14);
   background:var(--w6);transition:border-color .14s ease,background .14s ease}
 .cbox:focus-within{border-color:var(--a64);background:var(--w8)}
 .cbox textarea{width:100%;box-sizing:border-box;min-height:44px;max-height:150px;
   overflow-y:auto;scrollbar-width:none;resize:none;border:none;outline:none;
   background:transparent;font-size:16px;line-height:1.5;color:var(--text);
   font-family:inherit;padding:10px 4px 0;display:block}
 .cbox textarea::-webkit-scrollbar{width:0;height:0}
 .cbox textarea::placeholder{color:var(--muted2)}
 .cbox textarea:disabled{color:var(--faint)}
 .crow{display:flex;align-items:center;gap:4px;flex-wrap:nowrap}
 .cicobtn{flex:0 0 auto;width:36px;height:36px;display:flex;align-items:center;
   justify-content:center;border:none;border-radius:10px;background:transparent;
   color:var(--text-dim);cursor:pointer}
 .cicobtn svg{width:18px;height:18px;display:block;pointer-events:none}
 .cicobtn:active{background:var(--w10)}
 .cicobtn:disabled{color:var(--faint)}
 .cchars{font-size:11px;color:var(--muted2);flex:0 0 auto;margin-left:2px;
   font-variant-numeric:tabular-nums}
 .cchars.warn{color:var(--pending)}
 .cchars.over{color:var(--danger)}
 .csend{flex:0 0 auto;margin-left:auto;display:inline-flex;align-items:center;
   gap:5px;border:none;border-radius:11px;min-height:36px;padding:8px 16px;
   cursor:pointer;font-family:inherit;font-size:14px;font-weight:600;
   color:var(--on-accent);background:var(--accent);
   transition:background .12s ease,opacity .12s ease}
 .csend svg{width:15px;height:15px;display:block;pointer-events:none}
 .csend:active{background:var(--accent-hover)}
 .csend:disabled{background:var(--w10);color:var(--faint)}
 @keyframes crise{from{opacity:0;transform:translateY(3px)}to{opacity:1;
   transform:none}}
 /* 模型选择（对齐 ZCode remote v4）：输入行灰底胶囊「选择模型 ⌄」/
   「模型名 档位 ⌄」；上浮面板一级＝‹ 上一步灰条 + 供应商行（立方体图标+
   名+API 徽标+当前 ✓）+ 底部「管理模型」，二级＝供应商头行 +「模型」分组
   + 模型行 + 思考强度（绿蓝渐变滑杆，最左档＝跟随会话）。 */
 .modelchip{flex:0 0 auto;max-width:44%;min-height:36px;display:flex;
   align-items:center;gap:5px;font-size:13px;color:var(--text-dim);
   background:transparent;border:none;border-radius:10px;padding:6px 8px;
   font-family:inherit;overflow:hidden;cursor:pointer}
 .modelchip:active{background:var(--w10);color:var(--text)}
 .modelchip b{font-weight:400;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap}
 .modelchip .lvl{flex:0 0 auto;color:var(--muted)}
 .modelchip .caret{flex:0 0 auto;color:var(--muted);font-size:12px}
 .mpanel{position:absolute;left:0;right:0;bottom:calc(100% + 8px);z-index:30;
   background:var(--surface);border:1px solid var(--w14);border-radius:14px;
   box-shadow:0 12px 32px rgba(15,23,42,.18);overflow:hidden}
 .mpback{display:block;width:100%;text-align:left;background:var(--w8);
   border:none;border-bottom:1px solid var(--w10);color:var(--text);
   font-size:14px;padding:12px 14px;font-family:inherit}
 .mplist{max-height:300px;overflow-y:auto;scrollbar-width:none}
 .mplist::-webkit-scrollbar{display:none}
 .mprow{width:100%;display:flex;align-items:center;gap:10px;padding:13px 14px;
   background:none;border:none;border-bottom:1px solid var(--w6);
   font-size:15px;color:var(--text);text-align:left;font-family:inherit}
 .mprow:last-child{border-bottom:none}
 .mprow .mpico{flex:0 0 auto;color:var(--muted);display:flex;align-items:center}
 .mprow .mpname{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap}
 .badge{font-size:11px;color:var(--muted);background:var(--w8);
   border-radius:6px;padding:2px 7px}
 .mprow .check{flex:0 0 auto;color:var(--muted)}
 .mprow .fwd{flex:0 0 auto;color:var(--muted)}
 .mpsec{padding:10px 14px 2px;font-size:12px;color:var(--muted)}
 .mpfoot{padding:12px 14px;border-top:1px solid var(--w10);
   font-size:14px;color:var(--text)}
 .mpfoot[hidden]{display:none}
 .mpthought{padding:12px 14px;border-top:1px solid var(--w10)}
 .mpthought[hidden]{display:none}
 .mpthought .trow{display:flex;align-items:center;gap:6px;margin-bottom:8px;
   font-size:13px;color:var(--muted)}
 .mpthought .trow .tkey{flex:1}
 .mpthought .trow b{color:var(--text);font-weight:600}
 .mpthought input[type=range]{width:100%;height:14px;-webkit-appearance:none;
   appearance:none;border-radius:8px;outline:none;
   background:linear-gradient(90deg,#2ea043,#58a6ff)}
 .mpthought input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;
   width:22px;height:22px;border-radius:11px;background:var(--surface);
   border:1px solid var(--w18);box-shadow:0 1px 4px rgba(15,23,42,.2)}
 .newpill{position:absolute;left:50%;transform:translateX(-50%);bottom:10px;
   border:1px solid var(--a64);background:var(--accent);color:var(--on-accent);
   font-size:13px;font-weight:600;border-radius:14px;padding:6px 14px}
 .empty{flex:1;display:flex;flex-direction:column;align-items:center;
   justify-content:center;gap:8px;padding:40px 24px;text-align:center}
 .empty h3{margin:0;font-size:16px}
 .empty p{margin:0;font-size:13px;color:var(--muted);line-height:1.7}
 /* 派发页：选 Agent → 按项目看会话 → 进聊天记录再派活 */
 .dispatchhead{flex:0 0 auto;display:flex;align-items:flex-start;gap:10px;
   padding:2px 14px 7px}
 .dheadtext{flex:1;min-width:0}
 .dtitle{margin:0;font-size:17px;line-height:1.35}
 .dsub{margin:3px 0 0;font-size:11.5px;line-height:1.5;color:var(--muted2)}
 #sessRefresh{flex:0 0 auto;min-height:44px;padding:0 4px;border:0;
   background:transparent;color:var(--accent);font-size:12.5px;font-weight:600}
 #sessRefresh:disabled{opacity:.45}
 #agentChips{flex:0 0 auto;display:flex;gap:7px;padding:0 14px 9px;
   overflow-x:auto;scrollbar-width:none}
 #agentChips::-webkit-scrollbar{display:none}
 .chip{flex:0 0 auto;display:flex;align-items:center;gap:6px;min-height:40px;
   padding:0 12px;border-radius:12px;border:1px solid var(--w14);
   background:var(--w6);color:var(--text-dim);font-size:13px}
 .chip .st{width:6px;height:6px;border-radius:50%;background:var(--faint)}
 .chip .st.on{background:var(--ok)}
 .chip .n{font-size:11px;color:var(--faint)}
 .chip.on{border-color:var(--a64);background:var(--a26);color:var(--accent);
   font-weight:600}
 .chip.on .n{color:var(--accent)}
 #sessList{padding:0 14px 14px}
 .projgroup{margin:0}
 .projhead{display:flex;align-items:center;gap:6px;width:100%;min-height:40px;
   margin:0;padding:6px 2px;border:0;background:transparent;
   font-size:11px;font-weight:600;color:var(--muted2);letter-spacing:.02em;
   text-align:left}
 .projhead:active{opacity:.7}
 .projchev{flex:0 0 auto;font-size:14px;line-height:1;color:var(--faint);
   transform:rotate(90deg);transition:transform .18s ease}
 .projgroup.closed .projchev{transform:rotate(0deg)}
 .projlabel{flex:0 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap}
 .projarch{flex:0 0 auto;font-size:9.5px;font-weight:600;color:var(--warn);
   background:var(--w6);border:1px solid var(--w14);border-radius:7px;
   padding:0 5px;line-height:1.5}
 .projcount{margin-left:auto;flex:0 0 auto;font-size:10px;font-weight:600;
   color:var(--faint);background:var(--w6);border-radius:8px;padding:1px 6px}
 .projbody[hidden]{display:none}
 /* 已归档在列表底部单独成区：分隔线隔开、整区默认收起，不跟活跃项目混排。 */
 .arcsec{margin-top:10px;border-top:1px solid var(--w14)}
 .archead{display:flex;align-items:center;gap:6px;width:100%;min-height:44px;
   margin:0;padding:8px 2px;border:0;background:transparent;font-size:11px;
   font-weight:600;color:var(--muted);letter-spacing:.02em;text-align:left}
 .archead:active{opacity:.7}
 .arcchev{flex:0 0 auto;font-size:14px;line-height:1;color:var(--faint);
   transform:rotate(90deg);transition:transform .18s ease}
 .arcsec.closed .arcchev{transform:rotate(0deg)}
 .arcname{flex:0 0 auto}
 .arccount{flex:0 0 auto;font-size:10px;font-weight:600;color:var(--faint);
   background:var(--w6);border-radius:8px;padding:1px 6px}
 .arcbody[hidden]{display:none}
 .arcbody .projhead{color:var(--muted)}
 .sescard{display:flex;align-items:center;gap:8px;margin-bottom:7px;
   touch-action:manipulation;padding:11px 11px 11px 13px;border-radius:13px;
   border:1px solid var(--w14);
   background:linear-gradient(180deg,var(--surface),var(--surface2));
   box-shadow:0 1px 2px var(--w6);transition:background .14s,border-color .14s}
 .sescard:active{background:var(--w8);border-color:var(--a36)}
 .sescard.readonly{opacity:.82}
 .sesmain{flex:1;min-width:0}
 .sestop{display:flex;align-items:center;gap:6px;min-width:0}
 .sestitle{flex:1;min-width:0;font-size:14.5px;font-weight:600;
   overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
 .sesbadge{flex:0 0 auto;font-size:10.5px;padding:1px 6px;border-radius:9px;
   background:var(--w6);color:var(--muted2)}
 .sesmeta{display:flex;align-items:center;gap:7px;margin-top:4px;
   font-size:11.5px;color:var(--muted2);min-width:0}
 .sesproj{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap}
 .sestime,.sesread{flex:0 0 auto;color:var(--faint)}
 .sestime{color:var(--muted2)}
 .sesread{color:var(--warn)}
 .searrow{flex:0 0 auto;font-size:20px;line-height:1;color:var(--faint)}
 .dempty{flex:1;display:flex;align-items:center;justify-content:center;
   flex-direction:column;gap:8px;padding:34px 24px;text-align:center}
 .dempty h3{margin:0;font-size:15px}
 .dempty p{margin:0;font-size:12.5px;line-height:1.7;color:var(--muted)}
 .dload{display:flex;align-items:center;justify-content:center;gap:8px;
   padding:30px 0;color:var(--muted2);font-size:13px}
 .dload::before{content:'';width:12px;height:12px;border-radius:50%;
   border:2px solid var(--w18);border-top-color:var(--accent);
   animation:spin .8s linear infinite}
 @keyframes spin{to{transform:rotate(360deg)}}
 /* 状态页 */
 .srow{display:flex;gap:10px;padding:10px 4px;border-bottom:1px solid var(--w8);
   font-size:14px;line-height:1.5}
 .srow .k{flex:0 0 88px;color:var(--muted2)}
 .srow .v{flex:1;word-break:break-word}
 .snote{font-size:12px;color:var(--muted);line-height:1.7;margin-top:12px}
 /* 底部页签 */
 #tabbar{flex:0 0 auto;display:flex;border-top:1px solid var(--w14);
   background:linear-gradient(180deg,var(--surface),var(--surface2));
   padding-bottom:env(safe-area-inset-bottom)}
 .tab{flex:1;min-height:54px;border:0;background:transparent;color:var(--muted);
   font-size:14px;display:flex;align-items:center;justify-content:center;gap:6px}
 .tab.active{color:var(--accent);font-weight:700}
 .tab .count{font-size:11px;font-weight:700;color:var(--on-accent);
   background:var(--pending);border-radius:9px;padding:1px 7px;min-width:18px}
</style></head><body>
<div id="pair" hidden>
 <div class="pbox">
  <div class="plogo">A</div>
  <h2>配对这台手机</h2>
  <p>在电脑上打开 AgentRMC → 系统 → 手机访问，扫二维码；扫不了就输入 6 个配对词（空格隔开，连着打也行）。10 分钟内有效。</p>
  <input id="pairName" placeholder="设备名字" maxlength="40">
  <input id="pairCode" placeholder="6 个配对词" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false">
  <button id="pairBtn" class="btn primary">配对</button>
  <div id="pairErr"></div>
 </div>
</div>
<div id="app" hidden>
 <div id="top">
  <span class="logo">A</span><span class="brand">AgentRMC</span>
  <span class="dot" id="netDot"></span>
  <span class="spacer"></span>
  <span class="pill" id="listenPill">未监听</span>
 </div>
 <div id="offline" hidden>连接不上电脑，正在重试…</div>
 <div id="pages">
  <div id="pwork" class="page">
   <div id="workHint">
    <span id="workHintText">左滑标为已处理 · 右滑回 Agent · 长按多选</span>
    <button id="multiSw" aria-pressed="false">多选</button>
   </div>
   <div id="list" class="scroll"></div>
   <div id="batchBar" hidden>
    <span id="batchCount">已选 0</span>
    <button class="hbtn" id="batchAll">全选</button>
    <button class="btn primary" id="batchAck">✓ 标为已处理</button>
    <button class="btn" id="batchCancel">取消</button>
   </div>
   <div id="empty" class="empty" hidden>
    <h3>没有需要你处理的事</h3>
    <p>Agent 等你确认、或任务做完时，这里会自动出现一条。<br>喊你靠电脑桌宠的语音；这一页开着时自动刷新。</p>
   </div>
  </div>
  <div id="pdispatch" class="page" hidden>
   <div class="dispatchhead">
    <div class="dheadtext">
     <h2 class="dtitle">选会话派任务</h2>
     <p class="dsub" id="dispatchCount">先看上下文，再决定派给谁</p>
    </div>
    <button id="sessRefresh">刷新</button>
   </div>
   <div id="agentChips"></div>
   <div id="sessList" class="scroll"></div>
   <div id="sessEmpty" class="dempty" hidden>
    <h3>还没有可读的会话</h3>
    <p>电脑上的 Agent 产生过会话后，这里会按项目列出来。<br>只读本地会话库，不会改动 PC 当前会话。</p>
   </div>
  </div>
  <div id="pstatus" class="page" hidden><div id="statusBody" class="scroll"></div></div>
  <div id="pchat" class="page" hidden>
   <div class="chathead">
    <button class="btn" id="chatBack">‹ 待办</button>
    <div class="chtitles"><div class="name" id="chatTitle"></div><div class="where" id="chatSub"></div></div>
   </div>
   <div class="chatwrap">
    <div class="chatlist" id="chatList"></div>
    <button class="newpill" id="chatNew" hidden>有新消息 ↓</button>
   </div>
   <div class="chatfoot">
    <div class="cbar" id="chatBar" hidden>
      <div class="cnotice" id="chatNotice"></div>
    </div>
    <div class="cbox">
     <input id="chatFile" type="file" hidden>
     <textarea id="chatText" rows="1" placeholder="写一句任务，派到这个会话…" enterkeyhint="send"></textarea>
     <div class="crow">
      <button class="modelchip" id="chatModelChip" hidden><b id="chatModelLabel">选择模型</b><span class="lvl" id="chatModelLvl" hidden></span><span class="caret">⌄</span></button>
      <button class="cicobtn" id="chatAttach" title="传文件" aria-label="传文件"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M20.4 11.6 12 20a5 5 0 0 1-7-7l8.5-8.5a3.4 3.4 0 0 1 4.8 4.8L9.7 17.9a1.8 1.8 0 0 1-2.5-2.5l7.8-7.8"/></svg></button>
      <span class="cchars" id="chatChars" hidden></span>
      <button class="csend" id="chatSend" data-label="派发">派发<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12l16-7-6.3 16-2.9-6.4L4 12Z"/></svg></button>
     </div>
    </div>
    <div class="mpanel" id="chatModelPanel" hidden>
     <button class="mpback" id="chatMpBack">‹ 上一步</button>
     <div class="mplist" id="chatMpList"></div>
     <div class="mpthought" id="chatMpThought" hidden>
      <div class="trow"><span class="tkey">思考强度</span><b id="chatMpTVal">跟随会话</b></div>
      <input type="range" id="chatMpRange" min="0" max="5" value="0" step="1">
     </div>
     <div class="mpfoot" id="chatMpFoot">管理模型</div>
    </div>
   </div>
  </div>
 </div>
 <div id="tabbar">
  <button class="tab active" data-tab="0">待我处理<span class="count" id="tabCount" hidden></span></button>
  <button class="tab" data-tab="1">派发</button>
  <button class="tab" data-tab="2">状态</button>
 </div>
</div>
<div id="toast"></div>
<script>
)HTML") + QString::fromUtf8(R"HTML(let STATE=null,tab=0,timer=null,lastSig='';
let chatId='',chatMsgs=[],chatIds=new Set(),chatTotal=0,chatHasMore=false,chatLoading=false,chatAtBottom=true,chatMeta={};
let chatKind='work',chatExe='',chatSession='',chatProject='',chatAgentName='',chatCanDispatch=true;
let CATALOG=null,catalogLoading=false,catalogAt=0,catalogError='',dispatchSig='',selectedExe='';
const GROUP_STATE=new Map();  // 项目折叠态：跨轮询/切换 Agent/进出聊天保持
const $=id=>document.getElementById(id);
const COLOR={'needs-me':'var(--pending)','review':'var(--ok)','working':'var(--info)'};
const LABEL={'needs-me':'等你处理','review':'待验收','working':'进行中'};
const TOKENS={'--bg':'bg','--surface':'card.bg','--surface2':'card.bg2',
 '--text':'text','--text-soft':'text.soft','--text-dim':'text.dim',
 '--muted':'muted','--muted2':'muted.2','--ghost':'ghost','--faint':'faint',
 '--line':'line','--accent':'accent','--accent-hover':'accent.hover',
 '--on-accent':'on.accent','--danger':'danger','--warn':'warn','--ok':'ok',
 '--pending':'pending','--info':'info','--w6':'w6','--w8':'w8','--w10':'w10',
 '--w14':'w14','--w18':'w18','--w30':'w30','--a26':'a26','--a36':'a36','--a64':'a64'};
function applyTheme(t){
 const s=document.documentElement.style;
 Object.keys(TOKENS).forEach(v=>{if(t[TOKENS[v]]) s.setProperty(v,t[TOKENS[v]]);});
 const b=document.body.style;
 b.backgroundColor=t.bg||'var(--bg)';
 if(t.image){
  // 桌面走 WebView2 虚拟域名；手机走要令牌的 /m/theme/ 真实路由（cookie 自动带）。
  const url=t.image.replace('https://agentrmc.theme/','/m/theme/');
  const veil=t.veil||'transparent';
  b.backgroundImage='linear-gradient('+veil+','+veil+'), url("'+url+'")';
  b.backgroundSize='cover, cover';b.backgroundPosition='center, center';
 }else{b.backgroundImage='none';}
}
function autoName(){
 const u=navigator.userAgent;
 if(/Android/i.test(u)) return 'Android 手机';
 if(/iPhone|iPad/i.test(u)) return 'iPhone';
 return '手机';
}
function setNet(ok){
 $('netDot').className='dot'+(ok?' on':' bad');
 $('offline').hidden=ok||!STATE;
}
function showPair(){
 $('app').hidden=true;$('pair').hidden=false;
 if(timer){clearInterval(timer);timer=null;}
}
let netFails=0;  // 连续失败计数：2.5s 轮询偶发抖一两次不值得亮掉线横幅
async function load(){
 try{
  const r=await fetch('/m/state',{cache:'no-store',
   signal:AbortSignal.timeout(8000)});
  if(r.status===401){showPair();return;}
  if(!r.ok) throw new Error('http '+r.status);
  netFails=0;
  setNet(true);
  const s=await r.json();
  $('pair').hidden=true;$('app').hidden=false;
  adopt(s);
 }catch(e){
  // 抖动容忍：连挂 3 轮（约 8s）才判掉线亮横幅，期间保留旧画面不闪。
  netFails++;
  if(!STATE){showPair();
   $('pairErr').textContent='连不上电脑：确认手机和电脑在同一个 Wi-Fi、电脑端已开启「手机访问」、防火墙已放行。';}
  else if(netFails>=3) setNet(false);
 }
}
function startPoll(){
 if(timer)clearInterval(timer);
 load();
 timer=setInterval(()=>{if(!document.hidden){load();
  if(chatId)fetchTranscript(0,'append');
  else if(tab===1&&!catalogLoading&&Date.now()-catalogAt>20000)loadSessions();}},2500);
}
document.addEventListener('visibilitychange',()=>{
 if(!document.hidden&&STATE){load();if(chatId)fetchTranscript(0,'append');}
});
async function doPair(code){
 $('pairErr').textContent='';$('pairBtn').disabled=true;
 const name=($('pairName').value||'').trim()||autoName();
 try{
  const r=await fetch('/m/pair',{method:'POST',
   headers:{'Content-Type':'application/json','X-Requested-With':'fetch'},
   body:JSON.stringify({code:(code||'').trim(),name})});
  if(r.ok){history.replaceState(null,'',location.pathname);startPoll();}
  else if(r.status===429){$('pairErr').textContent='尝试次数太多，请 15 分钟后再试。';}
  else{$('pairErr').textContent='配对码不对或已过期：在电脑「系统 → 手机访问」重新生成。';}
 }catch(e){
  $('pairErr').textContent='连不上电脑：确认同一 Wi-Fi、电脑端已开启「手机访问」、防火墙已放行。';
 }
 $('pairBtn').disabled=false;
}
$('pairBtn').onclick=()=>doPair($('pairCode').value);
$('pairCode').addEventListener('keydown',e=>{if(e.key==='Enter')doPair($('pairCode').value);});
function buzz(ms){try{if(navigator.vibrate)navigator.vibrate(ms);}catch(e){}}
let toastTimer=null;
function toast(msg,ms){
 const el=$('toast');if(!el)return;
 el.textContent=msg;el.classList.add('on');
 if(toastTimer)clearTimeout(toastTimer);
 toastTimer=setTimeout(()=>el.classList.remove('on'),ms||1700);
}
async function postResult(obj){
 try{
  const r=await fetch('/m/action',{method:'POST',
   headers:{'Content-Type':'application/json','X-Requested-With':'fetch'},
   body:JSON.stringify(obj)});
  if(r.status===401){showPair();return {ok:false,status:401,data:{}};}
  let data={};try{data=await r.json();}catch(e){}
  if(r.status>=500)setNet(false);
  return {ok:r.ok,status:r.status,data};
 }catch(e){setNet(false);return {ok:false,status:0,data:{}};}
}
async function post(obj){return (await postResult(obj)).ok;}
async function act(obj){await post(obj);load();}
function base(p){const t=(p||'').replace(/[\/\\]+$/,'');
 const i=Math.max(t.lastIndexOf('/'),t.lastIndexOf('\\'));return i<0?t:t.slice(i+1);}
function hasProject(p){
 const b=base(p);
 return !!b&&b!=='.'&&b!=='..';
}
function projectName(p){return hasProject(p)?base(p):'未分类任务';}
// 同一 basename 可能是两个不同路径（例如 githubrepos\tuwen-editor 与
// Desktop\tuwen-editor）：归档状态必须按完整路径分开，不能合组。
function projectKey(p){
 return String(p||'').trim().replace(/[\\/]+$/,'').replace(/\//g,'\\').toLowerCase();
}
function projectParentName(p){
 const t=String(p||'').trim().replace(/[\\/]+$/,'');
 const i=Math.max(t.lastIndexOf('/'),t.lastIndexOf('\\'));
 return i<0?'':base(t.slice(0,i));
}
function agoText(iso){
 const t=Date.parse(iso||'');if(isNaN(t))return '';
 const s=Math.max(0,(Date.now()-t)/1000);
 if(s<60)return '刚刚';
 if(s<3600)return Math.floor(s/60)+' 分钟前';
 if(s<86400)return Math.floor(s/3600)+' 小时前';
 if(s<86400*30)return Math.floor(s/86400)+' 天前';
 return new Date(t).toLocaleDateString();
}
function where(it){if(!it.session_id) return '未关联会话';
 const b=base(it.project);
 if(b!=='.'&&b!=='..'&&b) return b;
 if(it.session_title) return it.session_title;
 return '会话 '+String(it.session_id).slice(0,8);}
// 同宿主同项目重名时补会话身份：优先标题（截 12 字），没标题才退短 id。
let WHERE_HITS={};
function whereLabel(it){
 let w=where(it);
 if(!it.session_id||w.indexOf('会话 ')===0||w==='未关联会话') return w;
 if((WHERE_HITS[(it.title||'')+'|'+w]||0)>1){
  if(it.session_title&&w!==it.session_title){
   const t=it.session_title;
   w+=' · '+(t.length>12?t.slice(0,12)+'…':t);
  }else{
   w+=' · '+String(it.session_id).slice(0,6);
  }
 }
 return w;
}
function hhmm(iso){const t=Date.parse(iso||'');if(isNaN(t)) return '';
 const d=new Date(t);
 return String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');}
function waitText(it){if(!it.attention_since) return '';
 const secs=(Date.now()-Date.parse(it.attention_since))/1000;
 if(!(secs>=600)) return '';const m=secs/60;
 if(m<60) return ' · 等 '+Math.floor(m)+' 分';
 if(m<1440) return ' · 等 '+Math.floor(m/60)+' 小时';
 return ' · 等 '+Math.floor(m/1440)+' 天';}
function isStale(it){return it.stage==='working'&&it.updated_at&&
 (Date.now()-Date.parse(it.updated_at))>1800000;}
function stageColor(it){return isStale(it)?'var(--muted)':(COLOR[it.stage]||'var(--muted)');}
function stageLabel(it){return isStale(it)?'状态待确认':(LABEL[it.stage]||'待办');}
function rank(s){return s==='needs-me'?0:s==='review'?1:s==='working'?2:3;}
function sortedItems(){
 const items=((STATE||{}).work||{}).items||[];
 return items.filter(i=>!i.done).sort((a,b)=>{
  if(rank(a.stage)!==rank(b.stage)) return rank(a.stage)-rank(b.stage);
  if(rank(a.stage)<=1){
   const wa=Date.parse(a.attention_since||a.updated_at||0);
   const wb=Date.parse(b.attention_since||b.updated_at||0);
   if(wa!==wb) return wa-wb;}
  return Date.parse(b.updated_at||0)-Date.parse(a.updated_at||0);});
}
// 回复正文开头常带「Agent名：」前缀（stop 提醒拼的），卡片标题已经是它，
// 剥掉免得一张卡里同一名字出现两遍（用户 2026-11-16）。
function stripAgentPrefix(msg,title){
 const t=String(title||'');const s=String(msg||'');
 if(!t) return s;
 if(s.startsWith(t+'：')) return s.slice(t.length+1);
 if(s.startsWith(t+': ')) return s.slice(t.length+2);
 if(s.startsWith(t+':')) return s.slice(t.length+1);
 return s;
}
// 手势阈值：先锁方向再跟手；过阈值后加阻尼，松手文案明确告诉用户会执行什么。
const SWIPE_MIN=60,SWIPE_MAX=116;
let ACTIVE_CARD=null;
function swipeOffset(dx){
 const a=Math.abs(dx),v=a<=SWIPE_MIN?a:SWIPE_MIN+(a-SWIPE_MIN)*.34;
 return Math.sign(dx)*Math.min(SWIPE_MAX,v);
}
function shortTitle(it,max){
 const t=String((it&&it.title)||'');
 const n=max||8;
 return t.length>n?t.slice(0,n)+'…':t;
}
// 销账动画：卡面滑出去 + 行高收掉，动画跑完列表才真的少一行。
function collapse(el){
 if(!el||el.classList.contains('gone'))return;
 // 先把当前高度钉成 px 并强制一次布局，再挂 .gone 收到 0——否则起始值是 auto，
 // max-height 不可插值，会直接跳到 0（卡片「啪」一下消失，没有滑出感）。
 el.style.maxHeight=el.offsetHeight+'px';
 void el.offsetHeight;
 el.classList.add('gone');
 requestAnimationFrame(()=>{el.style.maxHeight='0px';el.style.marginBottom='0px';});
}
function card(it){
 const d=(STATE||{}).dispatch||{};
 const notice=(d.noticeId===it.id)?(d.notice||''):'';
 const zcodeNotice=!!(notice&&d.noticeExe==='ZCode.exe');
 const selected=MULTI.sel.has(it.id);
 const el=document.createElement('div');
 el.className='card'+(MULTI.on?' multi':'')+(selected?' sel':'');
 el.dataset.id=it.id;el.tabIndex=0;
 // 手势底层：右滑（卡面往右走）露左边「打开 Agent」，左滑露右边「标为已处理」。
 const hl=document.createElement('div');hl.className='hint l';
 const hli=document.createElement('span');hli.className='ic';hli.textContent='▶';
 const hlt=document.createElement('span');hlt.className='label';hlt.textContent='打开';
 hl.append(hli,hlt);
 const hr=document.createElement('div');hr.className='hint r';
 const hrt=document.createElement('span');hrt.className='label';hrt.textContent='已处理';
 const hri=document.createElement('span');hri.className='ic';hri.textContent='✓';
 hr.append(hrt,hri);
 // 卡面
 const face=document.createElement('div');face.className='face';
 const bar=document.createElement('div');bar.className='bar';
 bar.style.background=stageColor(it);
 const chk=document.createElement('div');chk.className='chk'+(selected?' on':'');
 chk.setAttribute('aria-hidden','true');
 const l1=document.createElement('div');l1.className='l1';
 const name=document.createElement('span');name.className='name';name.textContent=it.title||'';
 const stage=document.createElement('span');stage.className='stage';
 stage.style.color=stageColor(it);stage.textContent=stageLabel(it);
 const time=document.createElement('span');time.className='time';
 time.textContent=hhmm(it.updated_at)+waitText(it);
 l1.append(name,stage,time);
 const wh=document.createElement('div');wh.className='where';wh.textContent=whereLabel(it);
 const msg=document.createElement('div');msg.className='msg';
 msg.textContent=stripAgentPrefix(it.message,it.title);
 face.append(bar,chk,l1,wh,msg);
 if(notice){const n=document.createElement('div');n.className='notice';n.textContent=notice;face.append(n);}
 // ZCode 通知态的「重启」是配套动作，多选时暂时收起，退出多选原位恢复。
 if(zcodeNotice){
  const rs=document.createElement('button');rs.className='btn warn zrestart';
  rs.textContent='重启 ZCode';
  rs.onclick=(e)=>{e.stopPropagation();if(confirm('重启 ZCode 桌面？会中断它里面正在跑的其它轮次；重启后打开会话即可看到派发消息全文。'))act({cmd:'restartZcode'});};
  face.append(rs);
 }
 el.append(hl,hr,face);
 el.onclick=()=>{if(MULTI.on)toggleSel(it.id);else openChat(it.id);};
 el.onkeydown=(e)=>{if(e.key!=='Enter'&&e.key!==' ')return;e.preventDefault();
  if(MULTI.on)toggleSel(it.id);else openChat(it.id);};
 let sx=0,sy=0,gesture='',lastDx=0,lpTimer=null,lpFired=false,armed=false;
 let suppressClick=false,suppressTimer=null,settleTimer=null;
 const cancelLp=()=>{if(lpTimer){clearTimeout(lpTimer);lpTimer=null;}};
 const hideHints=()=>{hl.style.opacity='0';hr.style.opacity='0';};
 const clearArmed=()=>{armed=false;hl.classList.remove('armed');hr.classList.remove('armed');
  hlt.textContent='打开';hrt.textContent='已处理';};
 const swallowClick=()=>{suppressClick=true;if(suppressTimer)clearTimeout(suppressTimer);
  suppressTimer=setTimeout(()=>{suppressClick=false;},420);};
 const springBack=()=>{
  if(settleTimer)clearTimeout(settleTimer);
  face.style.transition='';face.style.transform='translateX(0px)';face.style.opacity='1';
  hideHints();clearArmed();
  settleTimer=setTimeout(()=>{face.style.transform='';face.style.opacity='';},260);
 };
 el.addEventListener('touchstart',(e)=>{
  if(MULTI.on||e.touches.length!==1)return;
  const t=e.touches[0];sx=t.clientX;sy=t.clientY;gesture='';lastDx=0;
  ACTIVE_CARD=el;lpFired=false;clearArmed();hideHints();
  if(settleTimer)clearTimeout(settleTimer);
  face.style.transition='none';
  lpTimer=setTimeout(()=>{if(ACTIVE_CARD!==el||gesture)return;
   lpFired=true;swallowClick();buzz(16);setMulti(true,it.id);},460);
 },{passive:true});
 el.addEventListener('touchmove',(e)=>{
  if(MULTI.on||ACTIVE_CARD!==el)return;
  const t=e.touches[0],dx=t.clientX-sx,dy=t.clientY-sy;lastDx=dx;
  if(!gesture&&Math.max(Math.abs(dx),Math.abs(dy))<8)return;
  if(!gesture){gesture=Math.abs(dx)>Math.abs(dy)?'x':'y';cancelLp();}
  if(gesture==='y')return;
  e.preventDefault();
  const lim=swipeOffset(dx);
  face.style.transform='translateX('+lim+'px)';
  const p=Math.min(1,Math.abs(dx)/SWIPE_MIN);
  hl.style.opacity=dx>0?String(.18+.82*p):'0';
  hr.style.opacity=dx<0?String(.18+.82*p):'0';
  const over=Math.abs(dx)>=SWIPE_MIN;
  if(over&&!armed){armed=true;(dx>0?hl:hr).classList.add('armed');
   if(dx>0)hlt.textContent='松手';else hrt.textContent='松手';buzz(10);}
  else if(!over&&armed)clearArmed();
  else if(over&&armed){
   hl.classList.toggle('armed',dx>0);hr.classList.toggle('armed',dx<0);
   hlt.textContent=dx>0?'松手':'打开';
   hrt.textContent=dx<0?'松手':'已处理';}
 },{passive:false});
 el.addEventListener('touchend',(e)=>{
  if(ACTIVE_CARD!==el)return;
  cancelLp();ACTIVE_CARD=null;
  const t=e.changedTouches[0],dx=t.clientX-sx,dy=t.clientY-sy;
  const wasLong=lpFired;lpFired=false;
  if(wasLong||MULTI.on){swallowClick();gesture='';return;}
  if(gesture!=='x'){face.style.transition='';face.style.transform='';clearArmed();hideHints();return;}
  swallowClick();
  const commit=Math.abs(dx)>=SWIPE_MIN&&Math.abs(dx)>Math.abs(dy)*1.1;
  if(commit&&dx<0){setTimeout(()=>{hideHints();clearArmed();},130);ackOne(it,el);}
  else if(commit){
   buzz(12);springBack();toast('正在打开 '+shortTitle(it)+'…');
   act({cmd:zcodeNotice?'raiseZcode':'open',id:it.id});
  }else springBack();
  gesture='';lastDx=0;
 },{passive:true});
 el.addEventListener('touchcancel',()=>{
  if(ACTIVE_CARD===el)ACTIVE_CARD=null;
  cancelLp();if(gesture==='x')swallowClick();lpFired=false;gesture='';springBack();
 },{passive:true});
 // 新的主动按下会解除上一手势的 click 防误触；手势自己补发的 click 没有新 pointerdown。
 el.addEventListener('pointerdown',()=>{suppressClick=false;
  if(suppressTimer)clearTimeout(suppressTimer);},{passive:true});
 // 滑动/长按后的合成 click 必须吃掉，否则右滑会顺便打开聊天页。
 el.addEventListener('click',(e)=>{
  if(suppressClick){e.stopPropagation();e.preventDefault();suppressClick=false;
   if(suppressTimer)clearTimeout(suppressTimer);}
 },true);
 return el;
}
// ---------- 聊天页：点卡片进入，打开即最新，旧记录上翻按页加载 ----------
function msgKey(m){return m.id||(m.at+'|'+m.role+'|'+(m.text||'').slice(0,80));}
function bubble(m){
 const d=document.createElement('div');
 d.className='bub '+(m.role==='user'?'user':'assistant');
 const who=document.createElement('span');who.className='who';
 who.textContent=m.role==='user'?'我':'Agent';
 const body=document.createElement('span');body.textContent=m.text||'';
 d.append(who,body);
 if(m.at){const t=document.createElement('span');t.className='bt';
  t.textContent=hhmm(m.at);d.append(t);}
 return d;
}
// B1：待答请求只读卡片（授权/提问）。内联应答是 B2/B3，这里先展示。
function requestCard(req){
 const d=document.createElement('div');d.className='reqcard';
 d.dataset.reqId=req.id||'';   // 失败回执按 id 找卡（见 applyAnswerFailure）
 const head=document.createElement('div');head.className='reqhead';
 if(req.kind==='approval'){
  head.textContent='🔐 等你授权'+(req.toolName?'：'+req.toolName:'');
  d.append(head);
  if(req.reason){const r=document.createElement('div');r.className='reqreason';
   r.textContent=req.reason;d.append(r);}
  if(req.answerable){
   // B3：可答授权——允许/拒绝两键，提交 outcome 走 answer 动作。
   const row=document.createElement('div');row.className='reqrow';
   const mk=(label,outcome,primary)=>{const b=document.createElement('button');
    b.className='btn reqbtn'+(primary?' primary':'');b.textContent=label;
    b.onclick=()=>{row.querySelectorAll('button').forEach(x=>x.disabled=true);
     b.textContent='已提交…';
     act({cmd:'answer',id:chatId,requestId:req.id,outcome:outcome});};
    return b;};
   row.append(mk('允许','allowed-once',true),mk('拒绝','rejected',false));
   d.append(row);
   // 回执行：失败原因写在卡上（原来只飘页顶 notice，滚动后看不见）。
   const st=document.createElement('div');st.className='reqstate';st.hidden=true;
   d.append(st);d._reqState=st;
  }
 }else{
  head.textContent='❓ 等你选方案';d.append(head);
  if(!req.answerable){
   (req.questions||[]).forEach(q=>{
    const qd=document.createElement('div');qd.className='reqq';
    const qt=document.createElement('div');qt.className='reqqt';
    qt.textContent=q.question||q.header||'';qd.append(qt);
    (q.options||[]).forEach(o=>{const od=document.createElement('div');
     od.className='reqopt';od.textContent='· '+o.label;qd.append(od);});
    d.append(qd);
   });
  }else{
   // B2：可答——每题一列按钮，点即提交；选项不够时给「自定义」输入。
   const sel={};const custom={};
   (req.questions||[]).forEach(q=>{
    const qd=document.createElement('div');qd.className='reqq';
    const qt=document.createElement('div');qt.className='reqqt';
    qt.textContent=q.question||q.header||'';qd.append(qt);
    sel[q.id]=new Set();
    (q.options||[]).forEach(o=>{
     const b=document.createElement('button');b.className='reqbtn';
     b.textContent=o.label;
     if(o.description){b.title=o.description;}
     b.onclick=()=>{
      if(q.multi_select){sel[q.id].has(o.label)?sel[q.id].delete(o.label):sel[q.id].add(o.label);}
      else{sel[q.id]=new Set([o.label]);custom[q.id]='';renderReqBtns(qd,q,sel,custom);}
      renderReqBtns(qd,q,sel,custom);
     };
     qd.append(b);
    });
    const ci=document.createElement('input');ci.className='reqcustom';
    ci.type='text';ci.placeholder='或自己写一个回答…';
    ci.oninput=()=>{custom[q.id]=ci.value;
     if(ci.value.trim()){sel[q.id].clear();renderReqBtns(qd,q,sel,custom);}};
    qd.append(ci);
    d.append(qd);
    renderReqBtns(qd,q,sel,custom);
   });
   const send=document.createElement('button');send.className='btn primary reqsend';
   send.textContent='提交回答';
   send.onclick=()=>{
    const batch=(req.questions||[]).map(q=>({id:q.id,
     selected:[...(sel[q.id]||[])],
     ...(String(custom[q.id]||'').trim()?{custom:String(custom[q.id]).trim()}:{}),
    }));
    if(batch.some(a=>!a.selected.length&&!a.custom)){
     reqState(d,'每个问题都要选一项或写一句');return;}
    send.disabled=true;send.textContent='已提交，等确认…';
    act({cmd:'answer',id:chatId,requestId:req.id,answers:batch});
   };
   d.append(send);
   const st=document.createElement('div');st.className='reqstate';st.hidden=true;d.append(st);
   d._reqState=st;
  }
 }
 const hint=document.createElement('div');hint.className='reqhint';
 hint.textContent=!req.answerable?'暂在宿主界面应答（内联应答未开启）'
   :(req.kind==='approval'?'允许/拒绝直接生效——等于替你在电脑上点了按钮'
   :'点选项后提交——等于替你在电脑 DSH 界面里点了按钮');
 d.append(hint);
 return d;
}
// 选中态渲染：高亮所选按钮（纯文本比对，重建不丢状态：状态在 sel/custom 里）。
function renderReqBtns(qd,q,sel,custom){
 qd.querySelectorAll('.reqbtn').forEach((b,i)=>{
  const o=(q.options||[])[i];if(!o)return;
  b.classList.toggle('on',(sel[q.id]||new Set()).has(o.label));
 });
}
function reqState(card,msg){
 const el=card._reqstate||(card.parentElement&&card.parentElement._reqstate);
 if(el){el.hidden=false;el.textContent=msg;}
}
// 应答失败回执：把原因写进**对应那张卡**并恢复按钮（能重试）。卡片每次重绘
// 都重建，所以由 renderChatFrame 每帧调用，按 dataset.reqId 找卡。
function applyAnswerFailure(){
 const d=(STATE||{}).dispatch||{};
 if(!d.noticeRequest||d.noticeId!==chatId)return;
 const card=[...document.querySelectorAll('.reqcard')]
  .find(c=>c.dataset.reqId===d.noticeRequest);
 if(!card)return;
 if(card._reqState){card._reqState.hidden=false;
  card._reqState.textContent=d.notice||'提交失败，请重试';
  card._reqState.style.color='var(--danger)';}
 card.querySelectorAll('.reqbtn').forEach(b=>{b.disabled=false;});
}
function maybeAutoLoad(){
 const list=$('chatList');
 if(chatHasMore&&!chatLoading&&chatId&&
    list.scrollHeight<=list.clientHeight+80){
  chatLoading=true;
  const ld=$('chatLoad');if(ld)ld.textContent='正在加载更早的记录…';
  fetchTranscript(chatMsgs.length,'older');
 }
}
function paintChat(mode,prevH,prevTop){
 const list=$('chatList');
 list.replaceChildren();
 if(chatHasMore){
  const ld=document.createElement('div');ld.className='chatload';ld.id='chatLoad';
  ld.textContent=chatLoading?'正在加载更早的记录…':'上翻加载更早的记录';
  list.append(ld);
 }
 if(!chatMsgs.length){
  const e=document.createElement('div');e.className='chatempty';
  e.textContent=(chatMeta.supported===false)
   ?(chatMeta.error||'暂不支持聊天记录查看'):'这条会话还没有对话记录';
  list.append(e);
 }
 chatMsgs.forEach(m=>list.append(bubble(m)));
 (chatMeta.requests||[]).forEach(req=>list.append(requestCard(req)));
 if(mode==='init'){
  list.scrollTop=list.scrollHeight;chatAtBottom=true;$('chatNew').hidden=true;
  // 刚解除隐藏的元素布局可能下一帧才落定：双 rAF 兜底再定位。
  requestAnimationFrame(()=>{list.scrollTop=list.scrollHeight;
   requestAnimationFrame(()=>{list.scrollTop=list.scrollHeight;});});
 }else if(mode==='prepend'){
  list.scrollTop=prevTop+(list.scrollHeight-prevH);
 }else{
  if(chatAtBottom){list.scrollTop=list.scrollHeight;$('chatNew').hidden=true;}
  else{$('chatNew').hidden=false;}
 }
 maybeAutoLoad();
}
// ---------- 底部 composer：自增高 / 字符计数 / 发送态 / 状态行 ----------
// 手机页只有会话聊天一个面，前缀传空串（id 即 chatText/chatSend/…）。
const C_MAX=4000;  // 派发正文软上限：native 侧另有守卫，这里只提前提醒
const C_PLANE_SVG='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12l16-7-6.3 16-2.9-6.4L4 12Z"/></svg>';
function cGrow(p){
 const ta=$(p+'chatText');if(!ta)return;
 ta.style.height='auto';
 ta.style.height=Math.min(ta.scrollHeight,150)+'px';
}
function cValue(p,v){
 const ta=$(p+'chatText');if(!ta)return;
 ta.value=v;cGrow(p);cSync(p);
}
function cSync(p){
 const ta=$(p+'chatText');if(!ta)return;
 cGrow(p);
 cNoticeDropOnEmpty(p);
 const n=ta.value.trim().length,cc=$(p+'chatChars');
 cc.hidden=n===0;
 if(n){cc.textContent=String(n);
  cc.className='cchars'+(n>C_MAX?' over':(n>C_MAX*0.8?' warn':''));}
 const b=$(p+'chatSend');
 if(b&&b.dataset.busy==='1')return;  // 在途：保持「派发中…」
 cSendReady(p,!ta.disabled&&n>0);
}
function cSendReady(p,on){
 const b=$(p+'chatSend');if(!b)return;
 b.dataset.busy='0';b.disabled=!on;
 b.innerHTML='派发'+C_PLANE_SVG;
}
function cBusy(p){
 const b=$(p+'chatSend');if(!b)return;
 b.dataset.busy='1';b.disabled=true;b.innerHTML='派发中…';
}
function cNotice(p,text,kind){
 const bar=$(p+'chatBar'),el=$(p+'chatNotice');
 el.textContent=text||'';
 el.className='cnotice'+(kind?' '+kind:'');
 bar.hidden=!text;
}
// 输入清空 → 中性/成功类 notice 收起（"已派回，等回执…"这类不该在发下一发
// 之前一直占行）；失败类（bad）留着，草稿没了它还是唯一的拒绝原因可读。
function cNoticeDropOnEmpty(p){
 const bar=$(p+'chatBar');
 if(bar&&!bar.hidden&&!$(p+'chatNotice').classList.contains('bad')
    &&!$(p+'chatText').value.trim())bar.hidden=true;
}
function renderChatFrame(){
 $('chatBack').textContent=chatKind==='session'?'‹ 派发':'‹ 待办';
 const title=chatMeta.sessionTitle||chatMeta.title||
  (chatKind==='session'?'会话 '+String(chatSession||'').slice(0,8):'会话');
 $('chatTitle').textContent=title;
 const who=chatKind==='session'?(chatAgentName||chatMeta.agentName||''):
  (chatMeta.agentExe||'');
 const bits=[];
 if(who)bits.push(who);
 if(chatKind==='session'&&chatProject)bits.push(projectName(chatProject));
 bits.push('共 '+chatTotal+' 条');
 $('chatSub').textContent=bits.join(' · ');
 $('chatText').placeholder=chatCanDispatch
  ?(chatKind==='session'?'写一句任务，派到这个会话…':'派回这条会话…')
  :'这个 Agent 暂不支持手机派发，记录只读';
 $('chatText').disabled=!chatCanDispatch;
 $('chatAttach').disabled=!chatCanDispatch;
 syncModelChip();
 mpRenderPanel();
 if(!chatCanDispatch)
  cNotice('','只读：该 Agent 的手机派发通道未开通；记录仍可查看。');
 if(chatKind!=='session'){
  const d=(STATE||{}).dispatch||{};
  const notice=(d.noticeId===chatId)?(d.notice||''):'';
  cNotice('',notice,d.noticeKind||'');
  if(d.draftId===chatId&&d.draftText&&!$('chatText').value)
   cValue('',d.draftText);
 }
 // 发送键只在有正文时点亮；派发未回执的在途态不接管。
 cSync('');
 // 失败原因也落到那张卡上（页顶 notice 在长记录里容易被滚出视野）。
 applyAnswerFailure();
}
// —— 模型选择（对齐 ZCode remote v4）：胶囊「选择模型 ⌄」/「模型名 档位 ⌄」；
// 一级面板＝‹ 上一步灰条（收起态隐藏）+ 供应商行（立方体图标+名+API 徽标，
// 选中 ✓ 否则 ›）+ 底部「管理模型」；二级＝「模型」分组标签 + 模型行（选中 ✓）
// + 思考强度滑杆（0＝跟随会话，1..5＝low/medium/high/xhigh/max）。
// 目录来自 STATE.dispatch.models（个人供应商清单，native 只读
// provider_config.json）；空目录 / 非 ZCode / 只读会话整块隐藏。
// 派发成功后选择一次性作废，胶囊回「选择模型」。
const MP_LVL_LABELS={low:'极低',medium:'低',high:'中',xhigh:'极高',max:'最高'};
const MP_CUBE_SVG='<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><path d="M12 2.5 20.5 7v10L12 21.5 3.5 17V7L12 2.5Z"/><path d="M3.5 7 12 11.7 20.5 7"/><path d="M12 11.7V21.5"/></svg>';
let mpOpen=false,mpLevel='providers',mpProvider=null;
let mpSel=null; // {providerId,modelId,level?}；null=不切换
function mpModels(){return ((STATE||{}).dispatch||{}).models||[];}
function mpThoughts(){return ((STATE||{}).dispatch||{}).thoughtLevels||
 ['low','medium','high','xhigh','max'];}
function mpActive(){
 if(!chatCanDispatch||mpModels().length===0)return false;
 const exe=chatKind==='session'?chatExe:(chatMeta.agentExe||'');
 return exe==='ZCode.exe';
}
function mpResetState(){mpOpen=false;mpLevel='providers';mpProvider=null;mpSel=null;}
function syncModelChip(){
 const chip=$('chatModelChip'),panel=$('chatModelPanel');
 const on=mpActive();
 chip.hidden=!on;
 if(!on){panel.hidden=true;return;}
 $('chatModelLabel').textContent=mpSel?mpSel.modelId:'选择模型';
 const lvl=$('chatModelLvl');
 lvl.hidden=!(mpSel&&mpSel.level);
 if(mpSel&&mpSel.level)lvl.textContent=MP_LVL_LABELS[mpSel.level]||mpSel.level;
 if(!mpOpen&&!panel.hidden)panel.hidden=true;
}
function mpSvgRow(iconHtml,text,tail,on,onclick){
 const row=document.createElement('button');row.className='mprow';
 const ico=document.createElement('span');ico.className='mpico';
 ico.innerHTML=iconHtml||'';
 const name=document.createElement('span');name.className='mpname';
 name.textContent=text;
 row.append(ico,name);
 if(tail){const b=document.createElement('span');b.className='badge';
  b.textContent=tail;row.append(b);}
 const mark=document.createElement('span');
 mark.className=on?'check':'fwd';
 mark.textContent=on?'✓':'›';
 row.append(mark);
 row.onclick=onclick;
 return row;
}
function mpTextRow(text,on,onclick){
 const row=document.createElement('button');row.className='mprow';
 const name=document.createElement('span');name.className='mpname';
 name.textContent=text;
 const mark=document.createElement('span');mark.className='check';
 mark.textContent=on?'✓':'';
 row.append(name,mark);
 row.onclick=onclick;
 return row;
}
function mpRenderPanel(){
 const panel=$('chatModelPanel'),list=$('chatMpList'),
       back=$('chatMpBack'),foot=$('chatMpFoot'),
       thought=$('chatMpThought');
 if(!mpOpen||!mpActive()){panel.hidden=true;return;}
 panel.hidden=false;
 back.hidden=mpLevel==='providers'&&!mpProvider;
 list.replaceChildren();
 thought.hidden=true;
 if(mpLevel==='providers'){
  foot.hidden=false;
  list.append(mpTextRow('会话当前（不切换）',!mpSel,()=>{
   mpSel=null;mpOpen=false;syncModelChip();panel.hidden=true;
  }));
  mpModels().forEach(p=>{
   list.append(mpSvgRow(MP_CUBE_SVG,p.name||p.providerId,'API',
    mpSel&&mpSel.providerId===p.providerId,
    ()=>{mpProvider=p;mpLevel='models';mpRenderPanel();}));
  });
 }else{
  foot.hidden=true;
  const sec=document.createElement('div');sec.className='mpsec';
  sec.textContent='模型';
  list.append(sec);
  (mpProvider&&mpProvider.models||[]).forEach(mid=>{
   list.append(mpTextRow(mid,mpSel&&mpSel.providerId===mpProvider.providerId&&
    mpSel.modelId===mid,
    ()=>{
     mpSel={providerId:mpProvider.providerId,modelId:mid,
            level:mpSel?mpSel.level:null};
     mpRenderPanel();syncModelChip();
    }));
  });
  // 思考强度：选中模型后出现（0=跟随会话）
  if(mpSel){
   thought.hidden=false;
   const lv=mpThoughts(),range=$('chatMpRange');
   range.max=String(lv.length);
   range.value=mpSel.level?String(lv.indexOf(mpSel.level)+1):'0';
   $('chatMpTVal').textContent=mpSel.level
    ?(MP_LVL_LABELS[mpSel.level]||mpSel.level):'跟随会话';
  }
 }
}
$('chatModelChip').onclick=()=>{
 if(!mpActive())return;
 mpOpen=!mpOpen;
 if(mpOpen)mpRenderPanel();else $('chatModelPanel').hidden=true;
};
$('chatMpBack').onclick=()=>{mpLevel='providers';mpRenderPanel();};
$('chatMpRange').addEventListener('input',()=>{
 if(!mpSel)return;
 const lv=mpThoughts(),i=+$('chatMpRange').value;
 mpSel.level=i>0?lv[i-1]:null;
 $('chatMpTVal').textContent=mpSel.level
  ?(MP_LVL_LABELS[mpSel.level]||mpSel.level):'跟随会话';
 syncModelChip();
});
function pickedModel(){
 if(!mpSel)return null;
 const model={providerId:mpSel.providerId,modelId:mpSel.modelId};
 if(mpSel.level)model.options={reasoningLevel:mpSel.level};
 return model;
}
function mergeChatPage(p,mode){
 chatTotal=p.total||0; const msgs=p.messages||[];
 if(mode==='init'){
  chatMsgs=msgs.slice();chatIds=new Set(chatMsgs.map(msgKey));
  paintChat('init');
 }else{
  const fresh=msgs.filter(m=>!chatIds.has(msgKey(m)));
  if(fresh.length){
   fresh.forEach(m=>{chatIds.add(msgKey(m));chatMsgs.push(m);});
   paintChat('append');
  }
 }
 // 本地算剩余：轮询推的是最新页，它的 hasMore 不代表已加载范围之外还有旧页。
 chatHasMore=chatMsgs.length<chatTotal;
 renderChatFrame();
}
async function fetchTranscript(offset,mode){
 if(!chatId)return;
 const url=chatKind==='session'
  ?'/m/session?exe='+encodeURIComponent(chatExe)+'&session='
    +encodeURIComponent(chatSession)+'&offset='+offset+'&limit=50'
  :'/m/transcript?id='+encodeURIComponent(chatId)+'&offset='+offset+'&limit=50';
 try{
  const r=await fetch(url,{cache:'no-store'});
  if(r.status===401){showPair();return;}
  if(r.status===404){closeChat();return;}
  if(!r.ok)return;
  const p=await r.json();
  chatMeta=p;
  if(mode==='older'){
   chatLoading=false;chatTotal=p.total||chatTotal;
   const fresh=(p.messages||[]).filter(m=>!chatIds.has(msgKey(m)));
   if(fresh.length){
    const list=$('chatList');const prevH=list.scrollHeight,prevTop=list.scrollTop;
    fresh.forEach(m=>chatIds.add(msgKey(m)));
    chatMsgs=fresh.concat(chatMsgs);
    chatHasMore=chatMsgs.length<chatTotal;
    paintChat('prepend',prevH,prevTop);
   }else{const ld=$('chatLoad');if(ld)ld.textContent='上翻加载更早的记录';
    chatHasMore=chatMsgs.length<chatTotal;}
  }else{
   mergeChatPage(p,mode);
  }
 }catch(e){/* 网络抖一下，下一轮再试 */}
}
function startChat(kind,id,meta){
 chatKind=kind;chatId=id;chatMsgs=[];chatIds=new Set();chatLoading=false;chatMeta={};
 mpResetState();
 chatExe=meta&&meta.exe||'';chatSession=meta&&meta.session||'';
 chatProject=meta&&meta.project||'';chatAgentName=meta&&meta.agentName||'';
 chatCanDispatch=meta&&meta.canDispatch!==false;
 cValue('','');cNotice('','');
 render();fetchTranscript(0,'init');
}
function openChat(id){startChat('work',id,{});}
function openSession(agent,row){
 tab=1;
 startChat('session',row.id,{exe:agent.exe,session:row.id,project:row.project,
  agentName:agent.name,canDispatch:row.can_dispatch!==false});
}
function closeChat(){
 chatId='';chatMsgs=[];chatIds=new Set();
 chatKind='work';chatExe='';chatSession='';chatProject='';chatAgentName='';
 chatCanDispatch=true;
 render();
}
async function sendChat(){
 const t=$('chatText').value.trim();
 if(!t||!chatId||!chatCanDispatch)return;
 cBusy('');
 const model=pickedModel();
 const res=chatKind==='session'
  ?await postResult({cmd:'dispatchSession',agent:chatExe,session:chatSession,
     project:chatProject,text:t,model:model||undefined})
  :await postResult({cmd:'dispatch',id:chatId,text:t,model:model||undefined});
 if(!res.ok){
  // 失败：正文留在框里改完可重发（cValue 会把高度/计数/按钮一起复位）。
  cValue('',t);
  const why=(res.data&&res.data.error)||('HTTP '+res.status);
  cNotice('','没发出去：'+why+'（草稿已保留）','bad');
  return;
 }
 cValue('','');
 // 派发成功：模型选择是一次性的，关掉面板并重置，下一发默认回「会话当前」。
 mpOpen=false;$('chatModelPanel').hidden=true;mpResetState();
 cNotice('',chatKind==='session'
  ?'已派发。送达回执到了会进「待我处理」；这里继续看会话记录。'
  :'已派回这条会话，等回执…','good');
 toast('已派发');
 load();fetchTranscript(0,'append');
}
$('chatBack').onclick=closeChat;
$('chatSend').onclick=sendChat;
$('chatAttach').onclick=()=>$('chatFile').click();
$('chatFile').onchange=async()=>{
 const f=$('chatFile').files[0];$('chatFile').value='';
 if(!f||!chatId)return;
 if(f.size>25*1024*1024){cNotice('','文件超过 25MB，传不了','bad');return;}
 cNotice('','上传中… '+f.name);
 try{
  const r=await fetch('/m/upload?id='+encodeURIComponent(chatId),{method:'POST',
   headers:{'X-Requested-With':'fetch','X-AgentRMC-Filename':encodeURIComponent(f.name),'Content-Type':'application/octet-stream'},
   body:f});
  const j=await r.json();
  if(r.ok&&j.path){
   const cur=$('chatText').value;
   cValue('',(cur?cur+'\n':'')+'请查看文件：'+j.path);
   cNotice('','已上传 '+j.name+'，路径已填入输入框，点发送让 agent 读取','good');
   $('chatText').focus();
  }else{cNotice('','上传失败：'+(j.error||('HTTP '+r.status)),'bad');}
 }catch(e){cNotice('','上传失败：连不上电脑','bad');}
};
$('chatText').addEventListener('keydown',e=>{
 // 手机软键盘 enterkeyhint=send：Enter 直接发送；组合输入（拼音）中不发送。
 if(e.key==='Enter'&&!e.shiftKey&&!e.isComposing&&e.keyCode!==229){
  e.preventDefault();sendChat();}});
$('chatText').addEventListener('input',()=>cSync(''));
$('chatNew').onclick=()=>{const l=$('chatList');l.scrollTop=l.scrollHeight;
 chatAtBottom=true;$('chatNew').hidden=true;};
$('chatList').addEventListener('scroll',()=>{
 const list=$('chatList');
 chatAtBottom=list.scrollHeight-list.scrollTop-list.clientHeight<60;
 if(chatAtBottom)$('chatNew').hidden=true;
 if(list.scrollTop<80&&chatHasMore&&!chatLoading&&chatId){
  chatLoading=true;
  const ld=$('chatLoad');if(ld)ld.textContent='正在加载更早的记录…';
  fetchTranscript(chatMsgs.length,'older');
 }
});
// 多选的所有视觉状态都原位同步：轮询/勾选不再 replaceChildren，滚动与按压不断。
function syncMultiUi(){
 const shown=sortedItems().filter(i=>!ACKED.has(i.id));
 const on=MULTI.on,count=MULTI.sel.size;
 $('multiSw').textContent=on?'完成':'多选';
 $('multiSw').classList.toggle('on',on);
 $('multiSw').setAttribute('aria-pressed',String(on));
 $('multiSw').disabled=!on&&shown.length===0;
 $('workHintText').textContent=on?'轻点卡片选择，再点一次取消':'左滑标为已处理 · 右滑回 Agent · 长按多选';
 $('workHint').hidden=shown.length===0;
 $('batchBar').hidden=!on;
 $('batchCount').textContent='已选 '+count+' 条';
 $('batchAck').disabled=!on||count===0;
 $('batchAll').disabled=shown.length===0;
 $('batchAll').textContent=(count>0&&count===shown.length)?'全不选':'全选';
 document.querySelectorAll('#list .card').forEach(c=>{
  const sel=MULTI.sel.has(c.dataset.id);
  c.classList.toggle('multi',on);c.classList.toggle('sel',on&&sel);
  c.setAttribute('role',on?'checkbox':'button');
  c.setAttribute('aria-checked',String(on&&sel));
  const title=c.querySelector('.name');
  c.setAttribute('aria-label',(title?title.textContent:'待办')+(on?(sel?'，已选':'，未选'):'，打开会话'));
  const chk=c.querySelector('.chk');if(chk)chk.classList.toggle('on',on&&sel);
 });
}
function setMulti(on,selectedId){
 MULTI.on=on;
 if(!on)MULTI.sel.clear();
 if(selectedId)MULTI.sel.add(selectedId);
 syncMultiUi();
}
$('multiSw').onclick=()=>{buzz(MULTI.on?6:12);setMulti(!MULTI.on);};
function toggleSel(id){
 if(MULTI.sel.has(id))MULTI.sel.delete(id);else MULTI.sel.add(id);
 buzz(6);syncMultiUi();
 const chk=document.querySelector('#list .card[data-id="'+CSS.escape(id)+'"] .chk');
 if(chk){chk.classList.remove('pop');void chk.offsetWidth;chk.classList.add('pop');}
}
// 左滑松手：从手指离开的位置继续滑出，不先跳回卡中；随后再收掉行高。
function ackOne(it,el){
 if(ACKED.has(it.id))return;
 ACKED.set(it.id,Date.now());buzz(8);
 const face=el&&el.querySelector('.face');
 if(face){el.classList.add('swiped');
  face.style.transition='transform .18s cubic-bezier(.4,0,1,1),opacity .18s ease';
  requestAnimationFrame(()=>{face.style.transform='translateX(-108%)';face.style.opacity='0';});}
 collapse(el);toast('已标记「'+shortTitle(it,12)+'」');
 setTimeout(()=>{act({cmd:'ack',id:it.id});},340);
}
$('batchAll').onclick=()=>{
 const items=sortedItems().filter(i=>!ACKED.has(i.id));
 if(MULTI.sel.size&&MULTI.sel.size===items.length)MULTI.sel.clear();
 else items.forEach(i=>MULTI.sel.add(i.id));
 buzz(6);syncMultiUi();
};
$('batchAck').onclick=()=>{
 const ids=[...MULTI.sel];
 if(!ids.length)return;
 const nodes=[...document.querySelectorAll('#list .card')];
 $('batchAck').disabled=true;$('batchAck').textContent='标记中…';
 // 逐条错开 45ms 收走，批量也有节奏；动画跑完再一次性发请求。
 ids.forEach((id,i)=>setTimeout(()=>{
  ACKED.set(id,Date.now());
  collapse(nodes.find(c=>c.dataset.id===id));
 },i*45));
 setTimeout(async()=>{
  let failed=0;
  for(const id of ids){if(!await post({cmd:'ack',id}))failed++;}
  setMulti(false);
  $('batchAck').disabled=false;$('batchAck').textContent='✓ 标为已处理';
  toast(failed?'已提交 '+(ids.length-failed)+' 条，'+failed+' 条待重试':'已标记 '+ids.length+' 条为已处理');
  load();
 },ids.length*45+380);
};
$('batchCancel').onclick=()=>setMulti(false);

function renderWork(){
 // 手指还按着卡片时不做结构更新；下一轮轮询会再带上这份 state。
 if(ACTIVE_CARD)return;
 const items=sortedItems();
 // 已点销账但还没被 state 带走的条目先藏起来；请求失败（state 里还在）
 // 超过 5 秒就放回来，别让一次失败把卡片永久吞掉。
 const now=Date.now();
 const present=new Set(items.map(i=>i.id));
 ACKED.forEach((ts,id)=>{if(!present.has(id)||now-ts>5000)ACKED.delete(id);});
 const shown=items.filter(i=>!ACKED.has(i.id));
 const visible=new Set(shown.map(i=>i.id));
 MULTI.sel.forEach(id=>{if(!visible.has(id))MULTI.sel.delete(id);});
 // 多选和勾选不进入结构签名，它们由 syncMultiUi 原位处理。
 const sig=JSON.stringify(shown)+'|'+JSON.stringify((STATE||{}).dispatch||{})+
   '|'+[...ACKED.keys()].join(',');
 const ae=document.activeElement;
 // 正在打字时不重排：2.5s 一轮的刷新不能把输入焦点冲掉。
 if(ae&&(ae.tagName==='TEXTAREA'||ae.tagName==='INPUT'))return;
 if(sig===lastSig){syncMultiUi();return;}
 lastSig=sig;
 const list=$('list'),keep=list.scrollTop;
 list.replaceChildren();WHERE_HITS={};
 shown.forEach(it=>{const k=(it.title||'')+'|'+where(it);
  WHERE_HITS[k]=(WHERE_HITS[k]||0)+1;});
 shown.forEach(it=>list.append(card(it)));
 list.scrollTop=keep;
 $('empty').hidden=shown.length>0;list.hidden=shown.length===0;
 syncMultiUi();
}
// ---------- 派发页：Agent 筛选 + 项目分组会话 + 点进聊天记录 ----------
async function loadSessions(force){
 if(catalogLoading)return;
 if(!force&&CATALOG&&Date.now()-catalogAt<15000){
  if(tab===1&&!chatId)renderDispatch();return;
 }
 catalogLoading=true;catalogError='';catalogAt=Date.now();
 if(tab===1&&!chatId)renderDispatch();
 try{
  const r=await fetch('/m/sessions',{cache:'no-store',
   signal:AbortSignal.timeout(12000)});
  if(r.status===401){showPair();return;}
  if(!r.ok)throw new Error('HTTP '+r.status);
  CATALOG=await r.json();
  catalogAt=Date.now();dispatchSig='';
  const agents=CATALOG.agents||[];
  if(!agents.some(a=>a.exe===selectedExe))selectedExe=agents.length?agents[0].exe:'';
 }catch(e){
  catalogError='读不到会话目录：确认电脑上的 Agent 还开着，或点「刷新」重试。';
  toast(catalogError,2600);
 }finally{
  catalogLoading=false;
  if(tab===1&&!chatId)renderDispatch();
 }
}
function agentChip(agent){
 const b=document.createElement('button');
 b.className='chip'+(agent.exe===selectedExe?' on':'');
 b.setAttribute('aria-pressed',String(agent.exe===selectedExe));
 const st=document.createElement('span');st.className='st'+(agent.alive?' on':'');
 const name=document.createElement('span');name.textContent=agent.name||agent.exe;
 const n=document.createElement('span');n.className='n';
 n.textContent=String((agent.sessions||[]).length);
 b.append(st,name,n);
 b.onclick=()=>{
  if(selectedExe===agent.exe)return;
  selectedExe=agent.exe;dispatchSig='';
  $('sessList').scrollTop=0;renderDispatch();
 };
 return b;
}
function sessionCard(agent,row){
 const el=document.createElement('div');el.className='sescard';
 if(row.can_dispatch===false)el.classList.add('readonly');
 el.tabIndex=0;el.setAttribute('role','button');
 el.setAttribute('aria-label',(row.title||'会话')+'，'+projectName(row.project)+
  '，'+agoText(row.updated_at));
 const main=document.createElement('div');main.className='sesmain';
 const top=document.createElement('div');top.className='sestop';
 const title=document.createElement('span');title.className='sestitle';
 title.textContent=row.title||('会话 '+String(row.id||'').slice(0,8));
 top.append(title);
 if(row.stage){
  const badge=document.createElement('span');badge.className='sesbadge';
  badge.textContent=LABEL[row.stage]||'待办';top.append(badge);
 }
 const meta=document.createElement('div');meta.className='sesmeta';
 const time=document.createElement('span');time.className='sestime';
 time.textContent='最近更新 · '+agoText(row.updated_at);
 meta.append(time);
 if(row.can_dispatch===false){
  const ro=document.createElement('span');ro.className='sesread';ro.textContent='只读';
  meta.append(ro);
 }
 main.append(top,meta);
 const arrow=document.createElement('span');arrow.className='searrow';arrow.textContent='›';
 el.append(main,arrow);
 const open=()=>{buzz(5);openSession(agent,row);};
 el.onclick=open;
 el.onkeydown=(e)=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();open();}};
 return el;
}
function projectGroup(agent,label,rows,fullProject,archived,defaultOpen,stateKeyOverride,hideBadge){
 const groupKey=fullProject?projectKey(fullProject):'__ungrouped';
 const stateKey=stateKeyOverride||(agent.exe+'|'+groupKey);
 const group=document.createElement('div');group.className='projgroup';
 group.dataset.group=groupKey;
 if(archived)group.classList.add('archived');
 const head=document.createElement('button');head.className='projhead';
 const chev=document.createElement('span');chev.className='projchev';chev.textContent='›';
 const name=document.createElement('span');name.className='projlabel';name.textContent=label;
 head.append(chev,name);
 if(archived&&!hideBadge){
  const arc=document.createElement('span');arc.className='projarch';arc.textContent='已归档';
  head.append(arc);
 }
 const count=document.createElement('span');count.className='projcount';
 count.textContent=rows.length+' 个';
 head.append(count);
 head.title=fullProject||'没有项目路径的会话';
 const body=document.createElement('div');body.className='projbody';
 rows.forEach(row=>body.append(sessionCard(agent,row)));
 const stored=GROUP_STATE.get(stateKey);
 const open=stored===undefined?!!defaultOpen:stored;
 group.classList.toggle('closed',!open);
 body.hidden=!open;
 head.setAttribute('aria-expanded',String(open));
 head.setAttribute('aria-label',label+(archived?'，已归档':'')+'，'+rows.length+' 个会话，'+(open?'已展开':'已收起'));
 head.onclick=()=>{
  const opening=body.hidden;
  body.hidden=!opening;
  group.classList.toggle('closed',!opening);
  head.setAttribute('aria-expanded',String(opening));
  head.setAttribute('aria-label',label+(archived?'，已归档':'')+'，'+rows.length+' 个会话，'+(opening?'已展开':'已收起'));
  GROUP_STATE.set(stateKey,opening);
 };
 group.append(head,body);
 return group;
}
// 已归档单独成区（跟桌面同款）：整区默认收起，展开后内部仍按项目分组。
function archiveSection(agent,count,defaultOpen){
 const sec=document.createElement('div');sec.className='arcsec';
 const head=document.createElement('button');head.className='archead';
 const chev=document.createElement('span');chev.className='arcchev';chev.textContent='›';
 const name=document.createElement('span');name.className='arcname';name.textContent='已归档';
 const cnt=document.createElement('span');cnt.className='arccount';
 cnt.textContent=count+' 个会话';
 head.append(chev,name,cnt);
 head.title='已归档会话：项目目录已不存在，或宿主侧已归档。展开可查看记录。';
 const body=document.createElement('div');body.className='arcbody';
 const stateKey=agent.exe+'|__archive-section';
 const stored=GROUP_STATE.get(stateKey);
 const open=stored===undefined?!!defaultOpen:stored;
 const labelOf=opening=>'已归档，'+count+' 个会话，'+(opening?'已展开':'已收起');
 sec.classList.toggle('closed',!open);
 body.hidden=!open;
 head.setAttribute('aria-expanded',String(open));
 head.setAttribute('aria-label',labelOf(open));
 head.onclick=()=>{
  const opening=body.hidden;
  body.hidden=!opening;
  sec.classList.toggle('closed',!opening);
  head.setAttribute('aria-expanded',String(opening));
  head.setAttribute('aria-label',labelOf(opening));
  GROUP_STATE.set(stateKey,opening);
 };
 sec.append(head,body);
 return {el:sec,body};
}
function renderDispatch(){
 if(!CATALOG){
  $('agentChips').replaceChildren();
  $('sessEmpty').hidden=true;
  $('sessList').hidden=false;
  const list=$('sessList');list.replaceChildren();
  if(catalogLoading){
   const d=document.createElement('div');d.className='dload';d.textContent='正在读取会话目录…';
   list.append(d);
  }else{
   const box=document.createElement('div');box.className='dempty';
   const h=document.createElement('h3');h.textContent='会话目录还没加载';
   const p=document.createElement('p');p.textContent=catalogError||'点右上角「刷新」读取最近会话。';
   box.append(h,p);list.append(box);
  }
  $('dispatchCount').textContent='先看上下文，再决定派给谁';
  return;
 }
 const agents=CATALOG.agents||[];
 if(!agents.length){
  $('agentChips').replaceChildren();
  $('sessList').hidden=true;$('sessEmpty').hidden=false;
  $('dispatchCount').textContent='没有找到最近的 Agent 会话';
  return;
 }
 if(!agents.some(a=>a.exe===selectedExe))selectedExe=agents[0].exe;
 const sig=selectedExe+'|'+JSON.stringify(CATALOG);
 if(sig===dispatchSig)return;
 dispatchSig=sig;
 const chips=$('agentChips');chips.replaceChildren();
 agents.forEach(a=>chips.append(agentChip(a)));
 const agent=agents.find(a=>a.exe===selectedExe)||agents[0];
 const rows=agent.sessions||[];
 const list=$('sessList');const keep=list.scrollTop;
 list.replaceChildren();list.hidden=false;$('sessEmpty').hidden=true;
 if(!rows.length){
  $('dispatchCount').textContent=agent.name+' · 暂无会话';
  list.hidden=true;$('sessEmpty').hidden=false;return;
 }
const groups=new Map();
const unclassified=[];
rows.forEach(row=>{
 // 没有项目，或项目是宿主自动生成的临时工作区（WorkBuddy 的日期目录等）：
 // 都是「单任务」，统一进未分类任务，不按目录名/日期各开一组。
 if(!hasProject(row.project)||row.generated){unclassified.push(row);return;}
 const key=projectKey(row.project);
 if(!groups.has(key))groups.set(key,{path:row.project,items:[]});
 groups.get(key).items.push(row);
});
 const allGroups=[...groups.values()];
 const nameHits=new Map();
 allGroups.forEach(g=>{
  const n=base(g.path).toLowerCase();
  nameHits.set(n,(nameHits.get(n)||0)+1);
 });
const active=[],archived=[];
allGroups.forEach(g=>{
 let label=base(g.path);
 if(nameHits.get(label.toLowerCase())>1){
  const parent=projectParentName(g.path);
  if(parent)label+=' · '+parent;
 }
 // 同一项目按每行自己的归档状态拆两组：活跃组只放未归档的会话，已归档的
 // （老路径、宿主归档）单独进「已归档」区，两边不混在一组里。
 const key=projectKey(g.path);
 const live=g.items.filter(r=>!r.archived);
 const dead=g.items.filter(r=>!!r.archived);
 if(live.length)active.push({key,label,items:live,fullProject:g.path});
 if(dead.length)archived.push({key,label,items:dead,fullProject:g.path});
});
const uncLive=unclassified.filter(r=>!r.archived);
const uncDead=unclassified.filter(r=>!!r.archived);
// 摘要只报活跃面：归档数量交给底部「已归档」区头。
const liveCount=rows.filter(r=>!r.archived).length;
$('dispatchCount').textContent=agent.name+' · '+active.length+' 个项目'
 +(uncLive.length?' · '+uncLive.length+' 个未分类任务':'')
 +' · '+liveCount+' 个活跃会话';
// 默认只展开最近的一个活跃项目，其余收起；项目再多也不会铺成一条长墙。
active.forEach((g,i)=>list.append(
 projectGroup(agent,g.label,g.items,g.fullProject,false,i===0)));
// 单任务会话统一收进一个「未分类任务」组，默认折叠。
if(uncLive.length)list.append(
 projectGroup(agent,'未分类任务',uncLive,'',false,false,agent.exe+'|__unclassified'));
 // 已归档收进底部独立分区：不跟活跃项目平铺，整区默认收起；
 // 展开后内部仍按项目分组，第一个默认展开。
const archTotal=archived.reduce((n,g)=>n+g.items.length,0)+uncDead.length;
if(archTotal){
 const box=archiveSection(agent,archTotal,false);
 archived.forEach((g,i)=>box.body.append(
  projectGroup(agent,g.label,g.items,g.fullProject,true,i===0,
   agent.exe+'|__archived|'+g.key,true)));
 if(uncDead.length)box.body.append(projectGroup(agent,'未分类任务',uncDead,'',true,false,
  agent.exe+'|__unclassified-archived',true));
 list.append(box.el);
}
 list.scrollTop=keep;
}
function renderStatus(){
 const s=STATE||{},h=s.health||{},meta=s.meta||{};
 const body=$('statusBody');body.replaceChildren();
 const workers=(h.workers||[]).filter(w=>w.alive).length;
 const rows=[
  ['桥接',h.bridge?'正常（127.0.0.1:45915）':'未启动'],
  ['版本','v'+(meta.version||'')],
  ['运行时长',Math.floor((h.uptime_seconds||0)/60)+' 分钟'],
  ['Worker',workers+' 个在线'],
  ['离线待补',(h.spool_pending||0)+' 条'],
  ['已配对设备',(meta.devices||0)+' 台'],
  ['问题',(h.problems||[]).length?(h.problems||[]).join('；'):'无']];
 rows.forEach(([k,v])=>{
  const r=document.createElement('div');r.className='srow';
  const kk=document.createElement('span');kk.className='k';kk.textContent=k;
  const vv=document.createElement('span');vv.className='v';vv.textContent=v;
  r.append(kk,vv);body.append(r);});
 const btn=document.createElement('button');btn.className='btn';
 btn.style.marginTop='14px';btn.textContent='朗读最近一条转写';
 btn.title='让电脑桌宠用 TTS 念出来（声音在电脑上）';
 btn.onclick=()=>act({cmd:'readLast'});
 body.append(btn);
 const note=document.createElement('div');note.className='snote';
 note.textContent='手机页开着时约 2.5 秒刷新一次；锁屏/后台不推送（局域网 http 的浏览器限制），喊你靠电脑桌宠语音。派回会话与电脑端同一条通道：证明不了目标会话就不发，草稿会留在输入框。';
 body.append(note);
}
// 多选批量销账 + 左滑销账的暂存：MULTI.sel 存待办 id，ACKED 是刚点过销账、
// 等 state 追认的那些（下一轮刷新前先别让它们回来）。
const MULTI={on:false,sel:new Set()};
// 已点「标为已处理」的 id → 时间戳。乐观更新：先把卡片收走，等下一轮 state 追认；
// 追认不到（请求失败）5 秒后放回来。
const ACKED=new Map();
function render(){
 const work=(STATE||{}).work||{};
 const att=work.attention_count||0;
 // 计数变了弹一下，扫一眼就知道又压了一条。
 const cnt=$('tabCount');const prev=cnt.textContent;
 cnt.hidden=!att;cnt.textContent=String(att);
 if(att&&prev!==String(att)){cnt.classList.remove('bump');void cnt.offsetWidth;cnt.classList.add('bump');}
 const meta=(STATE||{}).meta||{};
 $('listenPill').textContent=meta.listening?'监听中':'未监听';
 $('listenPill').className='pill'+(meta.listening?' on':'');
 const inChat=!!chatId;
 $('tabbar').hidden=inChat;
 $('pchat').hidden=!inChat;
 $('multiSw').hidden=inChat||tab!==0;  // 多选开关只在待办页有意义
 $('pwork').hidden=inChat||tab!==0;
 $('pdispatch').hidden=inChat||tab!==1;
 $('pstatus').hidden=inChat||tab!==2;
 document.querySelectorAll('#tabbar .tab').forEach((b,i)=>b.classList.toggle('active',i===tab));
 if(inChat){renderChatFrame();return;}
 if(tab===0)renderWork();
 else if(tab===1){
  renderDispatch();
  if(!CATALOG&&!catalogLoading&&Date.now()-catalogAt>5000)loadSessions();
 }else renderStatus();
}
function adopt(s){
 // 相同 state 不再清空结构签名：2.5s 轮询不该把整列卡片重建一遍。
 STATE=s;
 applyTheme(s.theme||{});
 render();
}
$('sessRefresh').onclick=()=>{
 $('sessRefresh').disabled=true;
 loadSessions(true).finally(()=>{$('sessRefresh').disabled=false;});
};
document.querySelectorAll('#tabbar .tab').forEach(b=>{
 b.onclick=()=>{
  tab=+b.dataset.tab;
  if(tab===1&&(!CATALOG||Date.now()-catalogAt>15000))loadSessions();
  render();
 };
});
(function(){
 $('pairName').value=autoName();
 const m=location.hash.match(/pair=([^&]+)/);
 if(m){let c=m[1];try{c=decodeURIComponent(c);}catch(e){}
  $('pairCode').value=c;doPair(c);}
 else{startPoll();}
})();
</script></body></html>)HTML");
}