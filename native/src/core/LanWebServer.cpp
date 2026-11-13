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
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kMaxBodyBytes = 64 * 1024;
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

void LanWebServer::setActionHandler(
    std::function<QJsonObject(const QJsonObject &)> handler) {
    actionHandler_ = std::move(handler);
}

void LanWebServer::setThemeRoot(const QString &root) {
    themeRoot_ = root;
}

void LanWebServer::setDevices(const QJsonArray &devices) {
    devices_ = devices;  // 恢复持久化数据，不回发 devicesChanged
}

QString LanWebServer::beginPairing() {
    pairingCode_ = QString::fromLatin1(randomBytes(16).toHex());
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
        if (contentLength > kMaxBodyBytes) {
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
               "text/html; charset=utf-8");
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
        obj.value(QStringLiteral("code")).toString().trimmed().toLower();
    const bool ok = !pairingCode_.isEmpty() && now < pairingExpires_ &&
                    constantTimeEqual(code.toUtf8(), pairingCode_.toUtf8());
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
    const QByteArray cookie = "Set-Cookie: armc_dev=" + token +
                              "; Path=/m; HttpOnly; SameSite=Strict\r\n";
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
 /* 待办卡 */
 .card{position:relative;margin-bottom:10px;padding:12px 12px 12px 18px;
   border-radius:14px;border:1px solid var(--w18);
   background:linear-gradient(180deg,var(--surface),var(--surface2))}
 .bar{position:absolute;left:8px;top:14px;width:4px;height:34px;border-radius:2px}
 .l1{display:flex;align-items:baseline;gap:8px}
 .name{font-weight:700;font-size:15px;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap}
 .stage{font-size:12px;flex:0 0 auto}
 .time{margin-left:auto;font-size:12px;color:var(--ghost);flex:0 0 auto}
 .where{font-size:12px;color:var(--muted2);margin-top:2px;overflow:hidden;
   text-overflow:ellipsis;white-space:nowrap}
 .msg{font-size:14px;color:var(--text-soft);margin-top:6px;line-height:1.55;
   word-break:break-word}
 .notice{font-size:13px;color:var(--text-dim);margin-top:8px;padding:8px 10px;
   border-radius:8px;background:var(--w6);border:1px solid var(--w14);
   line-height:1.5;word-break:break-word}
 .acts{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}
 .acts .btn{flex:1 1 0;min-width:0}
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
 .chatload{align-self:center;font-size:12px;color:var(--muted2);padding:4px;
   flex:0 0 auto}
 .chatempty{flex:1;display:flex;align-items:center;justify-content:center;
   color:var(--muted);font-size:14px;text-align:center;padding:24px;
   line-height:1.7;white-space:pre-line}
 /* 底部输入行：左右留白 + 安全区，防手机圆角/手势条裁切发送按钮。 */
 .chatfoot{flex:0 0 auto;display:flex;gap:8px;
   padding:8px 0 calc(12px + env(safe-area-inset-bottom))}
 .chatfoot input{flex:1;min-width:0;min-height:44px;font-size:15px;color:var(--text);
   background:var(--w6);border:1px solid var(--w14);border-radius:10px;
   padding:8px 12px;font-family:inherit}
 .chatfoot input:focus{outline:none;border-color:var(--accent);background:var(--w8)}
 .chatfoot .btn{flex:0 0 auto}
 .newpill{position:absolute;left:50%;transform:translateX(-50%);bottom:10px;
   border:1px solid var(--a64);background:var(--accent);color:var(--on-accent);
   font-size:13px;font-weight:600;border-radius:14px;padding:6px 14px}
 .empty{flex:1;display:flex;flex-direction:column;align-items:center;
   justify-content:center;gap:8px;padding:40px 24px;text-align:center}
 .empty h3{margin:0;font-size:16px}
 .empty p{margin:0;font-size:13px;color:var(--muted);line-height:1.7}
 /* 事件流 */
 .ev{padding:8px 10px;border-radius:8px;font-size:13px;line-height:1.55;
   white-space:pre-wrap;word-break:break-word;margin-bottom:2px}
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
  <p>在电脑上打开 AgentRMC → 系统 → 手机访问，扫二维码；扫不了就输入配对码。配对码 10 分钟内有效。</p>
  <input id="pairName" placeholder="设备名字" maxlength="40">
  <input id="pairCode" placeholder="配对码" autocomplete="off" autocapitalize="off" spellcheck="false">
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
   <div id="list" class="scroll"></div>
   <div id="empty" class="empty" hidden>
    <h3>没有需要你处理的事</h3>
    <p>Agent 等你确认、或任务做完时，这里会自动出现一条。<br>喊你靠电脑桌宠的语音；这一页开着时自动刷新。</p>
   </div>
  </div>
  <div id="pevents" class="page" hidden><div id="evList" class="scroll"></div></div>
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
    <input id="chatText" type="text" placeholder="派回这条会话…" enterkeyhint="send">
    <button class="btn primary" id="chatSend">发送</button>
   </div>
   <div class="notice" id="chatNotice" hidden></div>
  </div>
 </div>
 <div id="tabbar">
  <button class="tab active" data-tab="0">待我处理<span class="count" id="tabCount" hidden></span></button>
  <button class="tab" data-tab="1">事件流</button>
  <button class="tab" data-tab="2">状态</button>
 </div>
</div>
<script>
)HTML") + QString::fromUtf8(R"HTML(let STATE=null,tab=0,timer=null,lastSig='',evSeen=0;
let chatId='',chatMsgs=[],chatIds=new Set(),chatTotal=0,chatHasMore=false,chatLoading=false,chatAtBottom=true,chatMeta={};
const $=id=>document.getElementById(id);
const COLOR={'needs-me':'var(--pending)','review':'var(--ok)','working':'var(--info)'};
const LABEL={'needs-me':'等你处理','review':'待验收','working':'进行中'};
const EVCOLOR={error:'var(--danger)',voice:'var(--ok)',system:'var(--ok)',status:'var(--muted)'};
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
async function load(){
 try{
  const r=await fetch('/m/state',{cache:'no-store'});
  if(r.status===401){showPair();return;}
  if(!r.ok){setNet(false);return;}
  const s=await r.json();
  setNet(true);
  $('pair').hidden=true;$('app').hidden=false;
  adopt(s);
 }catch(e){
  if(!STATE){showPair();
   $('pairErr').textContent='连不上电脑：确认手机和电脑在同一个 Wi-Fi、电脑端已开启「手机访问」、防火墙已放行。';}
  else setNet(false);
 }
}
function startPoll(){
 if(timer)clearInterval(timer);
 load();
 timer=setInterval(()=>{if(!document.hidden){load();if(chatId)fetchTranscript(0,'append');}},2500);
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
async function act(obj){
 try{
  const r=await fetch('/m/action',{method:'POST',
   headers:{'Content-Type':'application/json','X-Requested-With':'fetch'},
   body:JSON.stringify(obj)});
  if(r.status===401){showPair();return;}
 }catch(e){setNet(false);}
 load();
}
function base(p){const t=(p||'').replace(/[\/\\]+$/,'');
 const i=Math.max(t.lastIndexOf('/'),t.lastIndexOf('\\'));return i<0?t:t.slice(i+1);}
function where(it){if(!it.session_id) return '未关联会话';
 const b=base(it.project);
 return (b==='.'||b==='..'||!b)?('会话 '+String(it.session_id).slice(0,8)):b;}
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
function card(it){
 const d=(STATE||{}).dispatch||{};
 const notice=(d.noticeId===it.id)?(d.notice||''):'';
 const zcodeNotice=!!(notice&&d.noticeExe==='ZCode.exe');
 const el=document.createElement('div');el.className='card';
 const bar=document.createElement('div');bar.className='bar';
 bar.style.background=stageColor(it);
 const l1=document.createElement('div');l1.className='l1';
 const name=document.createElement('span');name.className='name';name.textContent=it.title||'';
 const stage=document.createElement('span');stage.className='stage';
 stage.style.color=stageColor(it);stage.textContent=stageLabel(it);
 const time=document.createElement('span');time.className='time';
 time.textContent=hhmm(it.updated_at)+waitText(it);
 l1.append(name,stage,time);
 const wh=document.createElement('div');wh.className='where';wh.textContent=where(it);
 const msg=document.createElement('div');msg.className='msg';msg.textContent=it.message||'';
 el.append(bar,l1,wh,msg);
 // 点卡片进聊天页（打开即最新记录）；按钮各自阻止冒泡。
 el.onclick=()=>openChat(it.id);
 if(notice){const n=document.createElement('div');n.className='notice';n.textContent=notice;el.append(n);}
 const acts=document.createElement('div');acts.className='acts';
 const open=document.createElement('button');open.className='btn';
 open.textContent=zcodeNotice?'聚焦 ZCode':'回到 Agent';
 open.title='在电脑上打开对应会话';
 open.onclick=(e)=>{e.stopPropagation();act({cmd:zcodeNotice?'raiseZcode':'open',id:it.id});};
 const ack=document.createElement('button');ack.className='btn primary';
 ack.textContent='标为已处理';
 ack.onclick=(e)=>{e.stopPropagation();act({cmd:'ack',id:it.id});};
 acts.append(open,ack);
 // 派发统一在聊天页的输入框做（点卡片即进聊天）；卡片只留两个均分按钮。
 // ZCode 通知态的「重启」是配套动作，保留。
 if(zcodeNotice){
  const rs=document.createElement('button');rs.className='btn warn';
  rs.textContent='重启 ZCode';
  rs.onclick=(e)=>{e.stopPropagation();if(confirm('重启 ZCode 桌面？会中断它里面正在跑的其它轮次；重启后打开会话即可看到派发消息全文。'))act({cmd:'restartZcode'});};
  acts.append(rs);
 }
 el.append(acts);
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
function renderChatFrame(){
 $('chatTitle').textContent=chatMeta.sessionTitle||chatMeta.title||'会话';
 $('chatSub').textContent=(chatMeta.agentExe||'')+' · 共 '+chatTotal+' 条';
 const d=(STATE||{}).dispatch||{};
 const notice=(d.noticeId===chatId)?(d.notice||''):'';
 $('chatNotice').hidden=!notice;$('chatNotice').textContent=notice;
 if(d.draftId===chatId&&d.draftText&&!$('chatText').value)
  $('chatText').value=d.draftText;
}
function mergeChatPage(p,mode){
 chatTotal=p.total||0;
 const msgs=p.messages||[];
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
 try{
  const r=await fetch('/m/transcript?id='+encodeURIComponent(chatId)
   +'&offset='+offset+'&limit=50',{cache:'no-store'});
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
function openChat(id){
 chatId=id;chatMsgs=[];chatIds=new Set();chatLoading=false;chatMeta={};
 $('chatText').value='';
 render();
 fetchTranscript(0,'init');
}
function closeChat(){chatId='';chatMsgs=[];chatIds=new Set();render();}
function sendChat(){
 const t=$('chatText').value.trim();
 if(!t||!chatId)return;
 $('chatText').blur();
 act({cmd:'dispatch',id:chatId,text:t});
 $('chatText').value='';
 fetchTranscript(0,'append');
}
$('chatBack').onclick=closeChat;
$('chatSend').onclick=sendChat;
$('chatText').addEventListener('keydown',e=>{
 if(e.key==='Enter'){e.preventDefault();sendChat();}});
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
function renderWork(){
 const items=sortedItems();
 const sig=JSON.stringify(items)+'|'+JSON.stringify((STATE||{}).dispatch||{});
 const ae=document.activeElement;
 // 正在打字时不重排：2.5s 一轮的刷新不能把输入焦点冲掉。
 if(ae&&(ae.tagName==='TEXTAREA'||ae.tagName==='INPUT')) return;
 if(sig===lastSig) return;
 lastSig=sig;
 const list=$('list');const keep=list.scrollTop;
 list.replaceChildren();
 items.forEach(it=>list.append(card(it)));
 list.scrollTop=keep;
 $('empty').hidden=items.length>0;
 list.hidden=items.length===0;
}
function renderEvents(){
 const events=(STATE||{}).events||[];
 const list=$('evList');const keep=list.scrollTop;
 const grew=events.length>evSeen;
 list.replaceChildren();
 events.forEach(ev=>{
  const d=document.createElement('div');d.className='ev';
  d.style.color=EVCOLOR[ev.kind]||'var(--text-dim)';
  d.textContent=ev.text||'';list.append(d);});
 list.scrollTop=grew?list.scrollHeight:keep;
 evSeen=events.length;
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
function render(){
 const work=(STATE||{}).work||{};
 const att=work.attention_count||0;
 $('tabCount').hidden=!att;$('tabCount').textContent=String(att);
 const meta=(STATE||{}).meta||{};
 $('listenPill').textContent=meta.listening?'监听中':'未监听';
 $('listenPill').className='pill'+(meta.listening?' on':'');
 const inChat=!!chatId;
 $('tabbar').hidden=inChat;
 $('pchat').hidden=!inChat;
 $('pwork').hidden=inChat||tab!==0;
 $('pevents').hidden=inChat||tab!==1;
 $('pstatus').hidden=inChat||tab!==2;
 document.querySelectorAll('#tabbar .tab').forEach((b,i)=>b.classList.toggle('active',i===tab));
 if(inChat){renderChatFrame();return;}
 if(tab===0)renderWork();else if(tab===1)renderEvents();else renderStatus();
}
function adopt(s){
 STATE=s;lastSig='';
 applyTheme(s.theme||{});
 render();
}
document.querySelectorAll('#tabbar .tab').forEach(b=>{
 b.onclick=()=>{tab=+b.dataset.tab;render();};
});
(function(){
 $('pairName').value=autoName();
 const m=location.hash.match(/pair=([0-9a-fA-F]+)/);
 if(m){$('pairCode').value=m[1];doPair(m[1]);}
 else{startPoll();}
})();
</script></body></html>)HTML");
}