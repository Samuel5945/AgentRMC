#include "core/LanWebServer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kMaxBodyBytes = 64 * 1024;
// 整包上传（没有流式 provider 时的兜底路径）会整个读进内存，所以卡在 64MB；
// 真正的大文件走流式（边收边写盘），上限 8GB，内存恒定。
constexpr qint64 kMaxUploadBytes = 64 * 1024 * 1024;
constexpr qint64 kMaxStreamBytes = 8LL * 1024 * 1024 * 1024;
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

// —— 手机浏览器排查追踪（2026-11-20）——
// 症状：某台手机的某个浏览器配对失败、红字是「连不上电脑」，而同一台机器用
// Chrome 正常。这种问题远程复现不了，得看服务端到底收到了什么：请求根本没到、
// 到了但被当垃圾（比如浏览器把 http 升级成 https，发来的是 TLS ClientHello）、
// 还是到了也回 200 但被浏览器拒收。
// 开关：存在 <AppData>/lan-trace.on，或环境变量 AGENTRMC_LAN_TRACE=<文件路径>。
// 关着的时候零开销（一次 stat）。
QString lanTracePath() {
    const QByteArray env = qgetenv("AGENTRMC_LAN_TRACE");
    if (!env.isEmpty()) {
        return QString::fromLocal8Bit(env);
    }
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        return QString();
    }
    if (!QFileInfo::exists(dir + QStringLiteral("/lan-trace.on"))) {
        return QString();
    }
    return dir + QStringLiteral("/lan-trace.log");
}

void lanTrace(const QByteArray &line) {
    const QString path = lanTracePath();
    if (path.isEmpty() || line.isEmpty()) {
        return;
    }
    // 别把盘写满：超过 256KB 从空文件重来（只留最近一段现场）。
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > 256 * 1024) {
        QFile::remove(path);
    }
    QFile f(path);
    if (!f.open(QIODevice::Append)) {
        return;
    }
    f.write(QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toUtf8());
    f.write(" ");
    f.write(line);
    f.write("\n");
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

// 上传/列目录/下载共用的定位字段（?id=&session=&project=&name=），
// 一律 percent 解码。白名单之外的键直接丢，不往下游传。
QJsonObject queryMeta(const QByteArray &query) {
    QJsonObject meta;
    const QList<QByteArray> pairs = query.split('&');
    for (const QByteArray &pair : pairs) {
        const int eq = pair.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        const QString key = QString::fromUtf8(pair.left(eq));
        // abs：项目外绝对路径（scope=abs 下载用）。漏了它参数会被静默丢掉，
        // 下载退回 404，Chrome 只能存成 file.txt（2026-11-20 用户实测）。
        if (key == QLatin1String("id") || key == QLatin1String("session")
            || key == QLatin1String("project") || key == QLatin1String("exe")
            || key == QLatin1String("name") || key == QLatin1String("scope")
            || key == QLatin1String("rel") || key == QLatin1String("abs")) {
            meta.insert(key, QUrl::fromPercentEncoding(pair.mid(eq + 1)));
        }
    }
    return meta;
}

// 单键取值：分片路由要 upload/index 这种不在 queryMeta 白名单里的键。
QString queryValue(const QByteArray &query, const char *key) {
    const QList<QByteArray> pairs = query.split('&');
    for (const QByteArray &pair : pairs) {
        const int eq = pair.indexOf('=');
        if (eq <= 0 || pair.left(eq) != key) {
            continue;
        }
        return QUrl::fromPercentEncoding(pair.mid(eq + 1));
    }
    return QString();
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
    const QList<QTcpSocket *> streaming = streams_.keys();
    for (QTcpSocket *socket : streaming) {
        streamAbort(socket);
        socket->abort();
    }
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
    std::function<QJsonObject(const QJsonObject &, const QByteArray &)> handler) {
    uploadHandler_ = std::move(handler);
}

void LanWebServer::setChunkUploadProviders(
    std::function<ChunkInit(const QJsonObject &, const QString &, qint64)> init,
    std::function<QJsonObject(const QString &, int, const QByteArray &)> chunk,
    std::function<QJsonObject(const QString &)> done,
    std::function<void(const QString &)> abortFn) {
    chunkInitProvider_ = std::move(init);
    chunkWriteProvider_ = std::move(chunk);
    chunkDoneProvider_ = std::move(done);
    chunkAbortProvider_ = std::move(abortFn);
}

void LanWebServer::setUploadStreamProviders(
    std::function<QString(const QJsonObject &, const QString &)> target,
    std::function<QJsonObject(const QJsonObject &, const QString &,
                              const QString &)> commit) {
    uploadTargetProvider_ = std::move(target);
    uploadCommitProvider_ = std::move(commit);
}

void LanWebServer::setInboxListProvider(
    std::function<QString(const QJsonObject &)> provider) {
    inboxListProvider_ = std::move(provider);
}

void LanWebServer::setInboxFileProvider(
    std::function<Download(const QJsonObject &)> provider) {
    inboxFileProvider_ = std::move(provider);
}

void LanWebServer::setInboxFileResolveProvider(
    std::function<Download(const QJsonObject &)> provider) {
    inboxFileResolveProvider_ = std::move(provider);
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
           cmd == QStringLiteral("control") ||
           cmd == QStringLiteral("switchModel") ||
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
            streamAbort(socket);  // 断连留下半截 .part 直接删，不占盘
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

// 把收到的字节写进临时文件；收满 Content-Length 后交给 commit 收尾。
// 中途写盘失败 / 断连都会清掉 .part，不留半成品。
void LanWebServer::streamFeed(QTcpSocket *socket, const QByteArray &chunk) {
    auto it = streams_.find(socket);
    if (it == streams_.end() || it->file == nullptr) {
        return;
    }
    StreamUpload &st = *it;
    if (!chunk.isEmpty()) {
        const qint64 left = st.total - st.written;
        const QByteArray part =
            chunk.size() > left ? chunk.left(static_cast<int>(left)) : chunk;
        if (st.file->write(part) != part.size()) {
            streamAbort(socket);
            finish(socket, "500 Internal Server Error");
            return;
        }
        st.written += part.size();
    }
    if (st.written < st.total) {
        return;  // 还没收满，等下一段
    }
    st.file->close();
    const QJsonObject result =
        uploadCommitProvider_(st.meta, st.tmpPath, st.filename);
    const QString tmp = st.tmpPath;
    delete st.file;
    streams_.remove(socket);
    if (!result.value(QStringLiteral("ok")).toBool()) {
        QFile::remove(tmp);  // 收尾失败别把半成品留在 inbox 里
        finish(socket, "500 Internal Server Error",
               QJsonDocument(result).toJson(QJsonDocument::Compact),
               "application/json");
        return;
    }
    finish(socket, "200 OK",
           QJsonDocument(result).toJson(QJsonDocument::Compact),
           "application/json");
}

void LanWebServer::streamAbort(QTcpSocket *socket) {
    auto it = streams_.find(socket);
    if (it == streams_.end()) {
        return;
    }
    if (it->file != nullptr) {
        it->file->close();
        delete it->file;
    }
    QFile::remove(it->tmpPath);
    streams_.remove(socket);
}

void LanWebServer::onReadable(QTcpSocket *socket) {
    // 流式上传在途：数据直接喂给文件，不进 buffers_（内存恒定）。
    if (streams_.contains(socket)) {
        streamFeed(socket, socket->readAll());
        return;
    }
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
        // 追踪：一个连接收到了字节却迟迟凑不出请求头——最常见的就是浏览器把
        // http 请求升级成 https（TLS ClientHello 里没有 \r\n\r\n），记下头几个
        // 字节就能一眼认出来。
        if (!traceReq_.contains(socket) && buffer.size() >= 8 &&
            !lanTracePath().isEmpty()) {
            QByteArray head = buffer.left(16).toHex(' ');
            traceReq_.insert(socket, QByteArray());
            lanTrace("RAW " + socket->peerAddress().toString().toUtf8() +
                     " bytes=" + QByteArray::number(buffer.size()) +
                     " hex=" + head);
        }
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
    // 追踪：一条请求 = 一行；UA 决定是哪个浏览器，Origin 用来判它认的源是谁
    // （被代理/加速改写过的页面，Origin 会和 Host 对不上，正好解释 fetch 失败）。
    // path 记到 300 字符：下载链接的 rel/abs 参数动辄上百字符，截 80 就永远
    // 看不到「到底请求了哪个文件」（2026-11-21 排查产物 404 就卡在这）。
    if (!lanTracePath().isEmpty()) {
        const QByteArray ua = headerValue(headerLines, "user-agent").left(140);
        const QByteArray origin = headerValue(headerLines, "origin");
        const QByteArray host = headerValue(headerLines, "host");
        const QByteArray referer = headerValue(headerLines, "referer").left(90);
        traceReq_.insert(
            socket, method + " " + path.left(300) + " host=" + host +
                        " origin=" + origin + " ref=" + referer +
                        " ua=" + ua);
        lanTrace("REQ " + socket->peerAddress().toString().toUtf8() + " " +
                 traceReq_.value(socket));
    }
    QByteArray query;
    const int queryAt = path.indexOf('?');
    if (queryAt >= 0) {
        query = path.mid(queryAt + 1);
        path = path.left(queryAt);
    }

    if (method == "POST" || method == "PUT") {
        // qint64：流式上传不限大小后，>2GB 用 int 会溢出成负数。
        qint64 contentLength = -1;
        const QByteArray lengthText = headerValue(headerLines, "content-length");
        if (!lengthText.isEmpty()) {
            contentLength = lengthText.toLongLong();
        }
        if (contentLength < 0) {
            finish(socket, "400 Bad Request");
            return;
        }
        // 流式上传：/m/upload 且配了 target+commit，就边收边写盘。大文件不再
        // 整个读进内存——内存只有一次 socket 读取的几十 KB，大小只受磁盘限制。
        if (path == "/m/upload" && uploadTargetProvider_ && uploadCommitProvider_) {
            QJsonObject meta = queryMeta(query);
            const QString rawName = QUrl::fromPercentEncoding(
                headerValue(headerLines, "x-agentrmc-filename"));
            meta.insert(QStringLiteral("filename"), rawName);
            const QString tmp = uploadTargetProvider_(meta, rawName);
            if (tmp.isEmpty()) {
                buffers_.remove(socket);
                finish(socket, "400 Bad Request");
                return;
            }
            auto *file = new QFile(tmp);
            if (!file->open(QIODevice::WriteOnly)) {
                delete file;
                buffers_.remove(socket);
                finish(socket, "500 Internal Server Error");
                return;
            }
            // header 之后已经收到的那截先落进文件，剩下的等后续 readyRead。
            const QByteArray head = buffer.mid(headerEnd + 4);
            buffers_.remove(socket);
            StreamUpload st;
            st.file = file;
            st.tmpPath = tmp;
            st.filename = rawName;
            st.meta = meta;
            st.total = contentLength;
            st.written = 0;
            streams_.insert(socket, st);
            streamFeed(socket, head);
            return;
        }
        // body 上限按路由分档：
        //   整包 /m/upload  + 流式 provider → 8GB（内存不随大小涨）
        //   整包 /m/upload  无 provider     → 64MB 兜底
        //   分片 /m/upload/chunk            → 16MB（一片 8MB + 余量）
        //   其余                            → 64KB
        const bool streaming =
            path == "/m/upload" && uploadTargetProvider_ && uploadCommitProvider_;
        qint64 bodyLimit = kMaxBodyBytes;
        if (streaming) {
            bodyLimit = kMaxStreamBytes;
        } else if (path == "/m/upload") {
            bodyLimit = kMaxUploadBytes;
        } else if (path == "/m/upload/chunk") {
            bodyLimit = 16 * 1024 * 1024;
        }
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
        route(socket, method, path, query, headerLines,
              body.left(static_cast<int>(contentLength)));
        return;
    }
    // HEAD 与 GET 同路由、不带 body：下载前探活用（见 /m/file 的 serveDownload）。
    if (method == "GET" || method == "HEAD") {
        route(socket, method, path, query, headerLines, QByteArray());
        return;
    }
    // OPTIONS 也落这里：不开 CORS，预检一律 404。
    finish(socket, "404 Not Found");
}

// 分片上传①：建会话。body 是 JSON（{session,project,name,size}），回
// {ok,uploadId,chunkSize,chunks}。前端据此切片并发 PUT。
void LanWebServer::routeChunkInit(QTcpSocket *socket,
                                  const QList<QByteArray> &headerLines,
                                  const QByteArray &query,
                                  const QByteArray &body) {
    if (!hasCsrfHeader(headerLines)) {
        finish(socket, "403 Forbidden");
        return;
    }
    if (authDevice(headerLines) < 0) {
        finish(socket, "401 Unauthorized");
        return;
    }
    if (!chunkInitProvider_) {
        finish(socket, "501 Not Implemented");
        return;
    }
    QJsonObject meta = queryMeta(query);
    const QJsonObject req = QJsonDocument::fromJson(body).object();
    for (auto it = req.begin(); it != req.end(); ++it) {
        meta.insert(it.key(), it.value());
    }
    const QString name = req.value(QStringLiteral("name")).toString();
    const qint64 size =
        static_cast<qint64>(req.value(QStringLiteral("size")).toDouble());
    const ChunkInit init = chunkInitProvider_(meta, name, size);
    QJsonObject out{{QStringLiteral("ok"), init.ok},
                    {QStringLiteral("chunkSize"), init.chunkSize},
                    {QStringLiteral("chunks"), init.chunks},
                    {QStringLiteral("received"), init.received}};
    if (init.ok) {
        out.insert(QStringLiteral("uploadId"), init.uploadId);
    } else {
        out.insert(QStringLiteral("error"), init.error);
    }
    finish(socket, init.ok ? "200 OK" : "400 Bad Request",
           QJsonDocument(out).toJson(QJsonDocument::Compact), "application/json");
}

void LanWebServer::routeChunkWrite(QTcpSocket *socket,
                                   const QList<QByteArray> &headerLines,
                                   const QByteArray &query,
                                   const QByteArray &body) {
    if (!hasCsrfHeader(headerLines)) {
        finish(socket, "403 Forbidden");
        return;
    }
    if (authDevice(headerLines) < 0) {
        finish(socket, "401 Unauthorized");
        return;
    }
    if (!chunkWriteProvider_) {
        finish(socket, "501 Not Implemented");
        return;
    }
    const QString uploadId = queryValue(query, "upload");
    const int index = queryValue(query, "index").toInt();
    if (uploadId.isEmpty() || index < 0) {
        finish(socket, "400 Bad Request");
        return;
    }
    const QJsonObject out = chunkWriteProvider_(uploadId, index, body);
    finish(socket, out.value(QStringLiteral("ok")).toBool() ? "200 OK"
                                                            : "400 Bad Request",
           QJsonDocument(out).toJson(QJsonDocument::Compact), "application/json");
}

// 分片上传③：合并 + ④：放弃。都只带 uploadId。
void LanWebServer::routeChunkFinish(QTcpSocket *socket, bool done,
                                    const QList<QByteArray> &headerLines,
                                    const QByteArray &query,
                                    const QByteArray &body) {
    if (!hasCsrfHeader(headerLines)) {
        finish(socket, "403 Forbidden");
        return;
    }
    if (authDevice(headerLines) < 0) {
        finish(socket, "401 Unauthorized");
        return;
    }
    const QJsonObject req = QJsonDocument::fromJson(body).object();
    QString uploadId = queryValue(query, "upload");
    if (uploadId.isEmpty()) {
        uploadId = req.value(QStringLiteral("uploadId")).toString();
    }
    if (uploadId.isEmpty()) {
        finish(socket, "400 Bad Request");
        return;
    }
    if (!done) {
        if (chunkAbortProvider_) {
            chunkAbortProvider_(uploadId);
        }
        finish(socket, "200 OK",
               QJsonDocument(QJsonObject{{QStringLiteral("ok"), true}})
                   .toJson(QJsonDocument::Compact),
               "application/json");
        return;
    }
    if (!chunkDoneProvider_) {
        finish(socket, "501 Not Implemented");
        return;
    }
    const QJsonObject out = chunkDoneProvider_(uploadId);
    finish(socket, out.value(QStringLiteral("ok")).toBool() ? "200 OK"
                                                            : "400 Bad Request",
           QJsonDocument(out).toJson(QJsonDocument::Compact), "application/json");
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
    // ---- 分片上传（并发 + 续传）：init → N 次 chunk(PUT) → done ----
    if (path == "/m/upload/init" && method == "POST") {
        routeChunkInit(socket, headerLines, query, body);
        return;
    }
    if (path == "/m/upload/chunk" && method == "PUT") {
        routeChunkWrite(socket, headerLines, query, body);
        return;
    }
    if (path == "/m/upload/done" && method == "POST") {
        routeChunkFinish(socket, true, headerLines, query, body);
        return;
    }
    if (path == "/m/upload/abort" && method == "POST") {
        routeChunkFinish(socket, false, headerLines, query, body);
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
        // 定位信息走 query：id=待办 id（可空）、session/project/exe 给「从派发页
        // 直接进会话」的那条路（没有待办 id，只认会话工作目录）。文件名走
        // X-AgentRMC-Filename 头（percent 编码，兼容中文）；body 是原始字节。
        // 落盘与路径解析在主窗 uploadHandler。
        QJsonObject meta = queryMeta(query);
        meta.insert(QStringLiteral("filename"),
                    QUrl::fromPercentEncoding(
                        headerValue(headerLines, "x-agentrmc-filename")));
        const QJsonObject result =
            uploadHandler_ ? uploadHandler_(meta, body)
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
    // 手机上翻「电脑文件」：列会话 inbox（GET /m/files）与下载（GET /m/file）。
    // 定位字段跟 /m/upload 同一套；下载只认 inbox 里的 basename，具体校验在
    // 主窗（canonical 前缀 + 40MB 上限）。
    if (path == "/m/files" && method == "GET") {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        const QString body =
            inboxListProvider_ ? inboxListProvider_(queryMeta(query)) : QString();
        if (body.isEmpty()) {
            finish(socket, "404 Not Found");
            return;
        }
        finish(socket, "200 OK", body.toUtf8(), "application/json");
        return;
    }
    if (path == "/m/file" && (method == "GET" || method == "HEAD")) {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        const Download file =
            inboxFileProvider_ ? inboxFileProvider_(queryMeta(query)) : Download{};
        if (!file.ok || file.data.isEmpty()) {
            // 失败也要回一份**明确不是文件**的响应：以前 404 是空 body、无
            // Content-Type，Chrome 对 <a download> 会把它当空文件存成
            // file.txt（用户 2026-11-20 第三次报同一个症状）。现在页面先
            // HEAD 探活，看到这份 JSON 就在页面上报「文件不在了」。
            finish(socket, "404 Not Found",
                   QByteArray("{\"ok\":false,\"error\":\"file_missing\"}"),
                   "application/json");
            return;
        }
        serveDownload(socket, file, method == "HEAD");
        return;
    }
    // /m/file/resolve：链接失效（文件被改名/移动）时的兜底。meta 与 /m/file 同
    // 一套，服务端负责在同目录里找真实文件；找不到同样回 JSON 404，不假装下成。
    if (path == "/m/file/resolve" && (method == "GET" || method == "HEAD")) {
        if (authDevice(headerLines) < 0) {
            finish(socket, "401 Unauthorized");
            return;
        }
        const Download file =
            inboxFileResolveProvider_
                ? inboxFileResolveProvider_(queryMeta(query))
                : Download{};
        if (!file.ok || file.data.isEmpty()) {
            finish(socket, "404 Not Found",
                   QByteArray("{\"ok\":false,\"error\":\"file_missing\"}"),
                   "application/json");
            return;
        }
        serveDownload(socket, file, method == "HEAD");
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
    if (!lanTracePath().isEmpty()) {
        const QByteArray label = traceReq_.take(socket);
        lanTrace("RES " + QByteArray(status) + " len=" +
                 QByteArray::number(body.size()) + " " + label);
    }
    buffers_.remove(socket);
    socket->disconnectFromHost();
}

void LanWebServer::serveDownload(QTcpSocket *socket, const Download &file,
                                 bool headOnly) {
    // Content-Disposition 双写：filename* 用 RFC 5987 带中文，filename 给老
    // 客户端一个纯 ASCII 兜底（非 ASCII 一律换下划线）。文件名只来自**解析后的
    // 真实文件**，不信页面传来的 rel/name——否则斜杠、空名都会被浏览器当成
    // 「没有文件名」，最后又存成 file.txt。
    QByteArray ascii;
    for (const QChar c : file.name) {
        ascii += (c.unicode() < 128 && c != QLatin1Char('"') &&
                  c != QLatin1Char('\\'))
                     ? QByteArray(1, static_cast<char>(c.unicode()))
                     : QByteArray("_");
    }
    const QByteArray headers =
        QByteArray("Content-Disposition: attachment; filename=\"") + ascii +
        "\"; filename*=UTF-8''" + QUrl::toPercentEncoding(file.name) + "\r\n" +
        "X-Content-Type-Options: nosniff\r\n";
    const QByteArray type = file.contentType.isEmpty()
                                ? QByteArray("application/octet-stream")
                                : file.contentType;
    if (!headOnly) {
        finish(socket, "200 OK", file.data, type.constData(), headers,
               "no-store");
        return;
    }
    // HEAD：与 GET 同头、无 body。页面只拿 200/404 判活，不读内容。
    QByteArray response = QByteArray("HTTP/1.1 200 OK\r\nConnection: close\r\n") +
                          "Content-Type: " + type + "\r\n" +
                          "Cache-Control: no-store\r\n" + headers +
                          "Content-Length: " +
                          QByteArray::number(file.data.size()) + "\r\n\r\n";
    socket->write(response);
    buffers_.remove(socket);
    socket->disconnectFromHost();
}

void LanWebServer::dropSocket(QTcpSocket *socket) {
    buffers_.remove(socket);
    drain_.remove(socket);
    streamAbort(socket);
    traceReq_.remove(socket);
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
 #pairTech{font-size:11px;color:var(--muted2);word-break:break-all;line-height:1.45}
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
 .chathead{flex:0 0 auto;display:flex;align-items:center;gap:8px;padding:4px 0 8px}
 .chathead .btn{min-height:40px;padding:6px 12px;flex:0 0 auto}
 /* 返回键只留箭头图标（不写「待办/派发」文字），直接复用 .cicobtn：和右侧
    文件按钮同规格 18px 图标 / 无边框透明底，一行里左右对称。
    顶栏这两颗把容器收窄到 28 宽（高仍 36 保触控），图标两侧留白 9→5，
    箭头不显得悬空。只作用于 .chathead，输入区的附件按钮不受影响。
    语义放在 aria-label 里，renderChatFrame 只改 aria-label。 */
 /* 顶栏图标统一 32×32 / SVG 16：原来只盖了宽度，高度还是 36 的默认值，
    刷新/文件夹变成 28×36 的竖长条（2026-11-19 用户指出偏大）。 */
 .chathead .cicobtn{width:32px;height:32px;border-radius:9px}
 .chathead .cicobtn svg{width:16px;height:16px}
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
 /* 系统信息行：工具调用耗时 / 后台任务完成。不是对话，别抢气泡的视觉重量。 */
 .bubsys{flex:0 0 auto;align-self:stretch;display:flex;align-items:baseline;gap:8px;
   padding:2px 2px 2px 8px;border-left:2px solid var(--w14);
   font-size:12px;line-height:1.5;color:var(--muted)}
 .bubsys .t{flex:1;min-width:0;word-break:break-word}
 .bubsys .bt{flex:0 0 auto;color:var(--faint);font-size:11px}
 /* 底部实时行：还在跑——呼吸点 + 主色竖线，跟历史系统信息区分开。 */
 .bublive{border-left-color:var(--accent);color:var(--text-soft)}
 .bublive .dot{flex:0 0 auto;width:6px;height:6px;border-radius:50%;
   background:var(--accent);align-self:center;animation:bublive 1.6s ease-in-out infinite}
 @keyframes bublive{0%,100%{opacity:.35}50%{opacity:1}}
 /* Agent 产出的文件条：气泡下方一行一个，右侧「下载到手机」。 */
 .bubfiles{display:flex;flex-direction:column;gap:6px;margin-top:8px;
   white-space:normal}
 .bubfile{display:flex;align-items:center;gap:8px;padding:8px 10px;
   border-radius:10px;background:var(--w6);border:1px solid var(--w14);
   text-decoration:none;color:var(--text);font-size:13px;min-height:44px}
 .bubfile:active{background:var(--w10)}
 .bubfile .ic{flex:0 0 auto;display:flex;color:var(--muted)}
 .bubfile .ic svg{width:16px;height:16px;display:block}
 .bubfile b{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap;font-weight:500}
 .bubfile .dl{flex:0 0 auto;color:var(--accent);font-size:12px;font-weight:600}
 /* 用户气泡里的附件：图片给缩略图，其它给文件条（正文里的路径行已摘掉）。 */
 .bubatts{display:flex;flex-wrap:wrap;gap:6px;margin-top:6px;white-space:normal}
 .bubatt{display:flex;align-items:center;gap:7px;padding:7px 10px;
   border-radius:10px;background:var(--w10);border:1px solid var(--w14);
   text-decoration:none;color:var(--text);font-size:12px;min-height:40px;
   max-width:100%}
 .bubatt img{display:block;width:96px;height:96px;object-fit:cover;
   border-radius:8px}
 .bubatt.img{padding:0;border:none;background:transparent;min-height:0}
 .bubatt .ic{flex:0 0 auto;display:flex;color:var(--muted)}
 .bubatt .ic svg{width:15px;height:15px;display:block}
 .bubatt b{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap;font-weight:500}
 .bubatt .dl{flex:0 0 auto;color:var(--accent);font-size:12px;font-weight:600}
 /* 图片查看器（用户 2026-11-22）：对话里的图点一下是「看」，
    不再直接落盘；下载收到浮层里的「保存到手机」。
    黑底 + 居中缩放，点背景/叉关闭；层级高于聊天页面任何浮层。 */
 /* 层级要足够高：頂栏/tabbar/输入框都在它下面。
    实测 z-index:90 不够——页面顶栏会浮在遮罩上面，
    字直接压在黑底上看得见（用户截图里那一行）。 */
 .imgview{position:fixed;inset:0;z-index:2147483000;
   background:#070a0f;
   display:flex;flex-direction:column;overscroll-behavior:contain}
 .imgview[hidden]{display:none !important}
 .imgview .ivbar{flex:0 0 auto;display:flex;align-items:center;gap:10px;
   padding:calc(env(safe-area-inset-top,0px) + 12px) 14px 10px;
   color:#eef2f8;font-size:13px}
 .imgview .ivname{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;
   white-space:nowrap;color:#eef2f8;opacity:.9}
 .imgview .ivbtn{flex:0 0 auto;border:1px solid rgba(255,255,255,.26);
   background:rgba(255,255,255,.10);color:#eef2f8;font-family:inherit;
   font-size:13px;padding:7px 13px;border-radius:9px}
 .imgview .ivbtn:active{background:rgba(255,255,255,.2)}
 .imgview .ivstage{flex:1 1 auto;min-height:0;display:flex;align-items:center;
   justify-content:center;padding:4px 10px calc(env(safe-area-inset-bottom,0px) + 18px);
   overflow:auto}
 .imgview .ivstage img{max-width:100%;max-height:100%;object-fit:contain;
   border-radius:6px;display:block}
 .imgview .iverr{color:#eef2f8;opacity:.8;font-size:13px;text-align:center;
   line-height:1.7;padding:0 20px}
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
 /* 底部留白：12→16px（含安全区）。原来 12px 在全面屏上仍显得贴底，
    与桌面端对齐成 16（用户 2026-11-18：留个空隙有呼吸感）。 */
 .chatfoot{position:relative;padding:8px 0 calc(16px + env(safe-area-inset-bottom))}
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
 /* 附件条：chip 横排，超出横向滚动（隐藏滚动条，UI 约定）；图片给缩略图。
    上传中单独一行细进度条 + 文件名，多选时串行一个个传。 */
 .catts{display:flex;gap:6px;overflow-x:auto;scrollbar-width:none;
   padding:1px 0 2px}
 .catts[hidden]{display:none !important}
 .catts::-webkit-scrollbar{width:0;height:0}
 .catt{flex:0 0 auto;display:flex;align-items:center;gap:6px;max-width:230px;
   padding:4px 5px 4px 4px;border-radius:9px;background:var(--w10);
   border:1px solid var(--w14)}
 .catt img{width:26px;height:26px;object-fit:cover;border-radius:6px;
   flex:0 0 auto;display:block}
 .catt .ic{width:26px;height:26px;display:flex;align-items:center;
   justify-content:center;border-radius:6px;background:var(--w8);
   color:var(--text-dim);flex:0 0 auto}
 .catt .ic svg{width:14px;height:14px;display:block}
 .catt b{font-weight:400;font-size:12px;overflow:hidden;
   text-overflow:ellipsis;white-space:nowrap;min-width:0}
 .catt i{font-style:normal;font-size:10.5px;color:var(--muted2);
   flex:0 0 auto}
 .catt .x{flex:0 0 auto;width:20px;height:20px;border:none;background:transparent;
   color:var(--muted2);font-size:15px;line-height:1;cursor:pointer;padding:0;
   font-family:inherit;border-radius:50%}
 .catt .x:active{background:var(--w14);color:var(--text)}
 .cup{display:flex;align-items:center;gap:8px;padding:4px 2px 1px}
 .cup[hidden]{display:none !important}
 .cupbar{flex:1;min-width:0;height:3px;border-radius:2px;background:var(--w10);
   overflow:hidden}
 .cupbar i{display:block;height:100%;width:0;border-radius:2px;
   background:var(--accent);transition:width .12s linear}
 .cup span{flex:0 0 auto;max-width:62%;font-size:11px;color:var(--muted2);
   overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
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
   justify-content:center;border:none;border-radius:50%;width:36px;height:36px;
   padding:0;cursor:pointer;color:var(--on-accent);background:var(--accent);
   transition:background .12s ease,opacity .12s ease}
 .csend svg{width:15px;height:15px;display:block;pointer-events:none}
 .csend svg.cspin{animation:cspin .8s linear infinite}
 @keyframes cspin{to{transform:rotate(360deg)}}
 .csend:active{background:var(--accent-hover)}
 .csend:disabled{background:var(--w10);color:var(--faint)}
 /* 运行中：同一颗键变「停止生成」（实心方块 + 危险色描边，不抢眼但认得出来） */
 .csend.stopmode{background:var(--w10);color:var(--danger);
   box-shadow:inset 0 0 0 1.5px var(--danger)}
 .csend.stopmode svg{width:13px;height:13px}
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
 .modelchip .caret{flex:0 0 auto;color:var(--muted);display:inline-flex;
   align-items:center}
 .modelchip .caret svg{width:12px;height:12px;display:block}
 /* 面板必须实心：美术主题的 card.bg 是半透明，直接当背景会透出底下聊天气泡
    （用户 2026-11-18 反馈"切换模型背景透明的看不清"）。实色由 applyTheme 算
    完直接写内联样式；这里只是首帧兜底。别加 backdrop-filter、也别用
    overflow:hidden 裁圆角——两者都会在 WebView2 上把整块渲染成洋红。 */
 .mpanel{position:absolute;left:0;right:0;bottom:calc(100% + 8px);z-index:30;
   background:#fff;
   border:1px solid var(--w14);border-radius:14px;
   box-shadow:0 12px 32px rgba(15,23,42,.18)}
 .mpback{display:block;width:100%;text-align:left;background:var(--w8);
   border:none;border-bottom:1px solid var(--w10);color:var(--text);
   font-size:14px;padding:12px 14px;font-family:inherit}
 .fpsec{padding:8px 14px 4px;font-size:11px;font-weight:600;color:var(--muted);
   letter-spacing:.02em}
 .fdir{display:block;font-size:10px;color:var(--muted);font-weight:400;
   overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-style:normal;
   margin-top:1px}
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
 .mpcommit{display:block;width:100%;padding:12px 14px;border:0;
   border-top:1px solid var(--w10);background:var(--w8);color:var(--text);
   font-family:inherit;font-size:14px;text-align:left}
 .mpcommit[hidden]{display:none}
 /* 「电脑上的文件」面板：复用 mpanel 的上浮壳，行改成可点下载的 <a>。 */
 .fphead{display:flex;align-items:center;gap:8px;padding:11px 14px;
   background:var(--w8);border-bottom:1px solid var(--w10);
   font-size:14px;color:var(--text)}
 .fphead b{font-weight:600}
 .fphead span{color:var(--muted2);font-size:12px;margin-right:auto;
   font-variant-numeric:tabular-nums}
 .fphead .fpx{flex:0 0 auto;width:24px;height:24px;border:none;
   border-radius:50%;background:transparent;color:var(--muted2);
   font-size:17px;line-height:1;padding:0;font-family:inherit}
 .fphead .fpx:active{background:var(--w14);color:var(--text)}
 .fempty{padding:24px 18px;text-align:center;color:var(--muted2);
   font-size:13px;line-height:1.75;white-space:pre-line}
 .frow{text-decoration:none;color:var(--text)}
 .frow:active{background:var(--w6)}
 .frow .mpico svg{width:16px;height:16px;display:block}
 .fsize{flex:0 0 auto;font-size:11.5px;color:var(--muted2);
   font-variant-numeric:tabular-nums}
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
 <div id="pairTech"></div>
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
    <button class="cicobtn" id="chatBack" aria-label="返回待办"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 5 8 12l7 7"/></svg></button>
    <div class="chtitles"><div class="name" id="chatTitle"></div><div class="where" id="chatSub"></div></div>
    <button class="cicobtn" id="chatRefresh" title="刷新记录" aria-label="刷新记录"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M20.5 12a8.5 8.5 0 1 1-2.5-6L21 9"/><path d="M21 3v6h-6"/></svg></button>
    <button class="cicobtn" id="chatFiles" title="产物与文件" aria-label="产物与文件"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 7.5A2 2 0 0 1 5 5.5h4l2 2.5h8a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg></button>
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
     <input id="chatFile" type="file" multiple hidden>
     <div class="catts" id="chatAtts" hidden></div>
     <div class="cup" id="chatUp" hidden>
      <div class="cupbar"><i id="chatUpFill"></i></div>
      <span id="chatUpLabel"></span>
     </div>
     <textarea id="chatText" rows="1" placeholder="写一句任务，派到这个会话…" enterkeyhint="send"></textarea>
     <div class="crow">
      <button class="modelchip" id="chatModelChip" hidden><b id="chatModelLabel">选择模型</b><span class="lvl" id="chatModelLvl" hidden></span><span class="caret"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="m6 9 6 6 6-6"/></svg></span></button>
      <button class="cicobtn" id="chatAttach" title="传文件" aria-label="传文件"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M20.4 11.6 12 20a5 5 0 0 1-7-7l8.5-8.5a3.4 3.4 0 0 1 4.8 4.8L9.7 17.9a1.8 1.8 0 0 1-2.5-2.5l7.8-7.8"/></svg></button>
      <span class="cchars" id="chatChars" hidden></span>
      <button class="csend" id="chatSend" data-label="派发" aria-label="派发" title="派发"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12l16-7-6.3 16-2.9-6.4L4 12Z"/></svg></button>
     </div>
    </div>
    <div class="mpanel" id="chatModelPanel" hidden>
     <button class="mpback" id="chatMpBack">‹ 上一步</button>
     <div class="mplist" id="chatMpList"></div>
     <div class="mpthought" id="chatMpThought" hidden>
      <div class="trow"><span class="tkey">思考强度</span><b id="chatMpTVal">跟随会话</b></div>
      <input type="range" id="chatMpRange" min="0" max="5" value="0" step="1">
     </div>
     <button class="mpcommit" id="chatMpCommit" hidden>设为这条会话的模型</button>
     <div class="mpfoot" id="chatMpFoot">管理模型</div>
    </div>
    <div class="mpanel" id="chatFilePanel" hidden>
     <div class="fphead"><b>产物与文件</b><span id="chatFileCount"></span><button class="fpx" id="chatFileClose" aria-label="关闭">×</button></div>
     <div class="mplist" id="chatFileList"></div>
     <div class="mpfoot">点文件名下载到手机</div>
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
<div class="imgview" id="imgView" hidden>
 <div class="ivbar">
  <span class="ivname" id="ivName"></span>
  <button class="ivbtn" id="ivSave">保存到手机</button>
  <button class="ivbtn" id="ivClose" aria-label="关闭">关闭</button>
 </div>
 <div class="ivstage" id="ivStage"></div>
</div>
<div id="toast"></div>
<script>
)HTML") + QString::fromUtf8(R"HTML(let STATE=null,tab=0,timer=null,lastSig='';
// 会话工作目录：待办本身可能没有 project（无工作区任务），这时用 transcript
// 带回的 cwd 兜底——产物面板、上传、下载全靠它（2026-11-20 用户报：无工作区
// 任务里 agent 生成的主题文件在 RMC 里下载不了）。
function chatProjectPath(){
 return String(chatProject||(chatMeta&&chatMeta.project)||'');
}
let chatId='',chatMsgs=[],chatIds=new Set(),chatTotal=0,chatHasMore=false,chatLoading=false,chatAtBottom=true,chatMeta={},chatModel='',chatLive=null;
let chatKind='work',chatExe='',chatSession='',chatProject='',chatAgentName='',chatCanDispatch=true;
let CATALOG=null,catalogLoading=false,catalogAt=0,catalogError='',dispatchSig='',selectedExe='';
const GROUP_STATE=new Map();  // 项目折叠态：跨轮询/切换 Agent/进出聊天保持
const $=id=>document.getElementById(id);
const COLOR={'needs-me':'var(--pending)','review':'var(--ok)','working':'var(--info)'};
const LABEL={'needs-me':'等你处理','review':'待验收','working':'进行中'};
// 颜色解析/合成：只认 #rgb(a) 与 rgb(a)()，主题 token 就这两种写法。
function rgbaOf(v){
 if(!v)return null;
 const s=String(v).trim();
 let m=/^#([0-9a-f]{3,8})$/i.exec(s);
 if(m){
  let h=m[1];
  if(h.length===3||h.length===4)h=h.split('').map(c=>c+c).join('');
  const p=i=>h.length>i?parseInt(h.substr(i,2),16):255;
  return [p(0),p(1),p(2),h.length===8?p(6)/255:1];
 }
 m=/^rgba?\(([^)]+)\)$/i.exec(s);
 if(m){
  const p=m[1].split(',').map(x=>parseFloat(x));
  if(p.length>=3)return [p[0],p[1],p[2],p.length>3?p[3]:1];
 }
 return null;
}
// 半透明卡片色合成到不透明底：上浮面板（模型/电脑文件）必须实心，否则
// 透出底下聊天气泡就读不清了。底只用同色系 card.bg2 或按卡片亮度挑的中性色，
// 不能用主题 bg——插画主题的 bg 是插画主色，会把面板染成粉紫。
function panelSolid(t){
 const card=rgbaOf(t['card.bg']);
 if(!card)return '';
 if(card[3]>=0.999)return 'rgb('+(card[0]|0)+','+(card[1]|0)+','+(card[2]|0)+')';
 let base=rgbaOf(t['card.bg2']);
 if(!base||base[3]<0.999){
  const lum=card[0]*0.299+card[1]*0.587+card[2]*0.114;
  base=lum>140?[255,255,255,1]:[18,23,34,1];
 }
 const a=Math.max(0,Math.min(1,card[3]));
 const mix=i=>Math.round(card[i]*a+base[i]*(1-a));
 return 'rgb('+mix(0)+','+mix(1)+','+mix(2)+')';
}
// 实色直接写进面板元素：走 CSS 变量万一没生效，面板退回半透明，而 WebView2
// 里半透明区域合成出来是洋红。
function setPanelSolid(panel){
 document.querySelectorAll('.mpanel').forEach(el=>{
  el.style.background=panel||'';
 });
}
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
 // 上浮面板要实色底（主题 card.bg 可能是半透明，见 panelSolid 注释）。
 const panel=panelSolid(t);
 if(panel)s.setProperty('--panel',panel);
 setPanelSolid(panel);
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
// 老内核兜底（2026-11-20 实测）：AbortSignal.timeout 是 Chrome 103+ 的 API，
// 荣耀/百度浏览器的内核报 Chrome 97 — 直接调用会抛 TypeError，被 load() 的
// catch 吞成「连不上电脑」，页面永远停在配对屏（配对请求其实早就 200 成功了）。
function timeoutSignal(ms){
 try{
  if(typeof AbortSignal!=='undefined'&&typeof AbortSignal.timeout==='function')
   return AbortSignal.timeout(ms);
 }catch(e){}
 const c=new AbortController();
 setTimeout(()=>{try{c.abort();}catch(e){}},ms);
 return c.signal;
}
function errText(e){return (e&&(e.name||'Error'))+': '+((e&&e.message)||String(e));}
// 技术细节：红字是给人看的行动建议，真原因（异常/地址）落在灰字里，截图就能定位。
function pairTech(text){
 const el=$('pairTech');if(!el)return;
 el.textContent=text||'';
}
function showPair(){
 $('app').hidden=true;$('pair').hidden=false;
 if(timer){clearInterval(timer);timer=null;}
}
let netFails=0;  // 连续失败计数：2.5s 轮询偶发抖一两次不值得亮掉线横幅
async function load(){
 try{
  const r=await fetch('/m/state',{cache:'no-store',
   signal:timeoutSignal(8000)});
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
   $('pairErr').textContent='连不上电脑：确认手机和电脑在同一个 Wi-Fi、电脑端已开启「手机访问」、防火墙已放行。';
   pairTech(errText(e)+' · '+location.href);}
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
 $('pairErr').textContent='';pairTech('');$('pairBtn').disabled=true;
 const name=($('pairName').value||'').trim()||autoName();
 try{
  const r=await fetch('/m/pair',{method:'POST',
   headers:{'Content-Type':'application/json','X-Requested-With':'fetch'},
   body:JSON.stringify({code:(code||'').trim(),name})});
  if(r.ok){
   // 这两步哪一步炸了都不该说「连不上电脑」——那会让人去查 Wi-Fi。
   try{history.replaceState(null,'',location.pathname);}catch(e){pairTech('replaceState: '+errText(e));}
   try{startPoll();}catch(e){
    $('pairErr').textContent='配对成功，但页面没能启动（看下面灰字）。';
    pairTech('startPoll: '+errText(e));return;}
  }
  else if(r.status===429){$('pairErr').textContent='尝试次数太多，请 15 分钟后再试。';}
  else if(r.status===403){$('pairErr').textContent='请求被电脑拒了（跨站校验）。刷新页面再试一次。';}
  else{$('pairErr').textContent='配对码不对或已过期：在电脑「系统 → 手机访问」重新生成。';}
 }catch(e){
  $('pairErr').textContent='连不上电脑：确认同一 Wi-Fi、电脑端已开启「手机访问」、防火墙已放行。';
  pairTech(errText(e)+' · '+location.href);
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
// 图片查看器的三个出口：关闭按钮 / 点背景 / 保存到手机。
// 保存走的还是 saveFile（HEAD 探活 → 服务端 disposition），
// 下载契约不变，只是触发点从气泡换到浮层。
function ivBind(){
 const v=$('imgView');if(!v)return;
 $('ivClose').onclick=closeImgView;
 $('ivSave').onclick=()=>{if(ivUrl)saveFile(ivUrl);};
 // 点背景关闭，点图本身不关（否则想看细节就被弹出了）。
 v.onclick=e=>{if(e.target===v||e.target.id==='ivStage')closeImgView();};
}
ivBind();
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
// 静默时长写进标签：与桌面端同款文案（「静默 32 分」比「状态待确认」有用）。
function staleText(it){
 const t=Date.parse(it.updated_at||'');
 if(isNaN(t)) return '状态待确认';
 const m=Math.floor((Date.now()-t)/60000);
 if(m<60) return '静默 '+m+' 分';
 if(m<1440) return '静默 '+Math.floor(m/60)+' 小时';
 return '静默 '+Math.floor(m/1440)+' 天';
}
function stageColor(it){return isStale(it)?'var(--muted)':(COLOR[it.stage]||'var(--muted)');}
function stageLabel(it){return isStale(it)?staleText(it):(LABEL[it.stage]||'待办');}
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
// 手机上传的附件在正文里是「请查看文件：<PC 绝对路径>」——那是给 agent 按路径
// 读文件用的，对用户没意义。渲染时把这行摘掉，换成缩略图/附件条
// （2026-11-18 用户反馈：「不是局域网传输嘛，怎么是串路径」）。
// 两种行都摘：给 agent 的指令行（【附件】…）和路径行（请查看文件：…）。
const ATT_LINE_RE=/^(?:请查看文件：(.+)|【附件】.*)$/gm;
function toRelPath(p){
 if(!p)return '';
 // 正斜杠/反斜杠统一成 '/' 再比：present_files 给的是 d:/a/b.hwt，会话 project
 // 是 D:\a，以前把反斜杠整个删掉去比，前斜杠路径一条都过不了（2026-11-19）。
 let rel=String(p).replace(/\\/g,'/').trim();
 const root=chatProjectPath().replace(/\\/g,'/').replace(/\/+$/,'');
 if(/^[A-Za-z]:\//.test(rel)||rel[0]==='/'){
  if(!root||!rel.toLowerCase().startsWith(root.toLowerCase()+'/'))return '';
  rel=rel.slice(root.length+1);
 }
 return rel;
}
function splitAttLines(text){
 const files=[];
 const rest=(text||'').replace(ATT_LINE_RE,(m,p)=>{if(p&&p.trim())files.push(p.trim());return '';})
  .replace(/\n{3,}/g,'\n\n').trim();
 return {files:files,rest:rest};
}
// Agent 回复里提到的产物路径 → 气泡下方的文件条（带「下载到手机」）。
// 只认产物类扩展名；绝对路径必须落在会话工作目录里（转成相对），相对路径
// 直接当工作区相对路径。真正的校验在服务端（canonical + 白名单）。
// hwt/hap/hsp：HarmonyOS 主题包/应用包/共享包，跟 apk 同级（2026-11-19 加）。
const MSG_FILE_EXT=new Set(('png jpg jpeg gif webp bmp svg ico mp4 mov webm mkv '+
 'mp3 wav m4a pdf md txt doc docx xls xlsx ppt pptx zip 7z rar exe msi apk '+
 'hwt hap hsp').split(' '));
// 绝对路径（盘符开头，允许含空格，如 D:/HONOR Share/…）或「以空白/引号/括号
// 为界」的相对路径。路径段不再限制 [\w.-]：中文目录名（产物/OS8成品/凝蓝.hwt）
// 以前整个认不出来。
// 扩展名显式白名单 + 路径段贪婪：以前是「非贪婪 + [A-Za-z0-9]{1,5}」，
// 文件名里的 1.4.7 会被当成后缀（base 截到「…演示-1」、ext='4'），
// 白名单一查就丢——2026-11-20 用户报「带路径的产物还是没有下载」的真凶。
const MSG_FILE_RE=/(?:^|[\s"'`（(【\[、，。：:)\]】》])((?:[A-Za-z]:[\\/][^"'<>|`\r\n，。；：、！？)\]}】》]{0,180})|(?:(?:[^\s"'<>|`\r\n\/，。；：、！？)\]}】》]+\/){1,6}[^\s"'<>|`\r\n\/，。；：、！？)\]}】》]{1,80}))\.(png|jpg|jpeg|gif|webp|bmp|svg|ico|mp4|mov|webm|mkv|mp3|wav|m4a|pdf|md|txt|doc|docx|xls|xlsx|ppt|pptx|zip|7z|rar|exe|msi|apk|hwt|hap|hsp)\b/gi;
function msgFiles(text){
 if(!text||text.length<5)return [];
 const out=[],seen=new Set();
 const root=chatProjectPath().replace(/\\/g,'/').replace(/\/+$/,'').toLowerCase();
 let m;
 MSG_FILE_RE.lastIndex=0;
 while((m=MSG_FILE_RE.exec(text))){
  const ext=(m[2]||'').toLowerCase();
  if(!MSG_FILE_EXT.has(ext))continue;
  // markdown 链接 `[文字](C:/…/x.jpg)`：路径边界会把开头的 `(` 一起吃进来
  // （2026-11-21 trace 实锤：rel=16:9（…）](C:/…）。开头的括号也要剥掉。
  const base=(m[1]||'').replace(/^[（(【\[]+/,'')
    .replace(/[，。；：、）】》,.;:)\]}]+$/,'');
  if(!base)continue;
  // 正则把扩展名拆到 m[2]，路径要拼回去：少了 .hwt 卡片名会变成「凝蓝_OS8」，
  // 下载的 rel 也少一段，浏览器只能存成 file.txt（2026-11-19 用户实测）。
  let rel=(base+'.'+ext).replace(/\\/g,'/');
  if(/^[A-Za-z]:\//.test(rel)||rel[0]==='/'){
   const lower=rel.toLowerCase();
   if(root&&lower.startsWith(root+'/')){
    rel=rel.slice(root.length+1);        // 工作区内 → 相对路径
   }else{
    // 项目外的绝对路径（如 D:/ld-bili-147/xxx.mp4）：登记成 abs 交给服务端
    // scope=abs（白名单扩展名 + 拒绝系统目录）。以前直接 continue，用户
    // 2026-11-20 报「带路径的非项目区产物也要能下载到手机」。
    const nm=rel.split('/').pop();
    if(!seen.has(rel)){seen.add(rel);out.push({abs:rel,name:nm});}
    if(out.length>=8)break;
    continue;
   }
  }
  if(!rel||seen.has(rel))continue;
  seen.add(rel);
  out.push({rel:rel,name:rel.split('/').pop()});
  if(out.length>=8)break;
 }
 return out;
}
function bubble(m){
 // 系统信息行（工具调用/后台任务）：不是对话气泡，灰色一行，别占半屏。
 if(m.role==='system'){
  const s=document.createElement('div');s.className='bubsys';
  const tx=document.createElement('span');tx.className='t';tx.textContent=m.text||'';
  s.append(tx);
  if(m.at){const t=document.createElement('span');t.className='bt';
   t.textContent=hhmm(m.at);s.append(t);}
  return s;
 }
 const isUser=m.role==='user';
 // 手机传的附件：正文里那几行「请查看文件：<PC 路径>」是给 agent 读文件的，
 // 对用户没意义 —— 摘掉，换成缩略图/文件条（2026-11-18 用户反馈）。
 let text=m.text||'', attFiles=[];
 if(isUser){const sp=splitAttLines(text);text=sp.rest;attFiles=sp.files;}
 const d=document.createElement('div');
 d.className='bub '+(isUser?'user':'assistant');
 const who=document.createElement('span');who.className='who';
 who.textContent=isUser?'我':'Agent';
 d.append(who);
 if(text){const body=document.createElement('span');body.textContent=text;d.append(body);}
 if(attFiles.length){
  const box=document.createElement('div');box.className='bubatts';
  attFiles.forEach(p=>{
   const name=p.split(/[\\/]/).pop()||'附件';
   const rel=toRelPath(p);
   // 工作区外的绝对路径（inbox 恰好不在项目里、或用户贴的是别处文件）：
   // 也走下载路由，别退化成不可点的 '#'。
   const norm=String(p||'').replace(/\\/g,'/').trim();
   const abs=(/^[A-Za-z]:\//.test(norm)||norm[0]==='/')?norm:'';
   const isImg=/\.(png|jpe?g|gif|webp|bmp)$/i.test(name);
   const a=document.createElement('a');
   a.className=isImg?'bubatt img':'bubatt';
   if(rel||abs){a.href=fpUrl(null,rel||undefined,abs||undefined);
    // 图片是「看」不是「下」（用户 2026-11-22）：点一下弹大图，
    // 想存再从浮层里点「保存到手机」。下载属性也去掉，
    // 否则长按/中键仍会跳过查看直接落盘。
    if(isImg){a.onclick=e=>{e.preventDefault();openImgView(a.href,name);};}
    else{a.setAttribute('download','');
     a.onclick=e=>{e.preventDefault();saveFile(a.href);};}}
   else{a.href='#';a.onclick=e=>e.preventDefault();}
   if(isImg){
    const im=document.createElement('img');im.src=a.href;im.alt=name;
    // 图坏了（读不到/已删）退回文件条，别留个破图方块。
    im.onerror=()=>{a.className='bubatt';a.replaceChildren();
     // 退化成文件行了：这时点一下就该是下载（看不了的图
     // 再弹一个空查看器没意义）。
     a.removeAttribute('download');
     a.setAttribute('download','');
     a.onclick=e=>{e.preventDefault();saveFile(a.href);};
     const ic=document.createElement('span');ic.className='ic';ic.innerHTML=ATT_ICON;
     const nm=document.createElement('b');nm.textContent=name;a.append(ic,nm);};
    a.append(im);
   }else{
    const ic=document.createElement('span');ic.className='ic';ic.innerHTML=ATT_ICON;
    const nm=document.createElement('b');nm.textContent=name;
    const dl=document.createElement('span');dl.className='dl';dl.textContent='下载到手机';
    a.append(ic,nm,dl);
   }
   box.append(a);
  });
  d.append(box);
 }
 if(!isUser){
  // 两条来源合一起，present_files 在前（那是 agent 主动"摆出来"的主产物，
  // 顺序也是它给的），正文里提到的路径补在后面：
  // ① present_files 工具结果里的文件（正文不一定再写一遍）；
  // ② 正文里写出来的路径。
  const files=[],seen=new Set();
  (m.files||[]).forEach(p=>{
   const rel=toRelPath(p);
   if(rel){
    if(seen.has(rel))return;
    seen.add(rel);
    files.push({rel:rel,name:rel.split('/').pop()});
    return;
   }
   // present_files 给的是项目外绝对路径（如 D:/ld-bili-147/xxx.mp4）：
   // 走 abs 下载，别丢。
   const norm=String(p||'').replace(/\\/g,'/').trim();
   if(/^[A-Za-z]:\//.test(norm)||norm[0]==='/'){
    if(seen.has(norm))return;
    seen.add(norm);
    files.push({abs:norm,name:norm.split('/').pop()});
   }
  });
  msgFiles(text).forEach(f=>{
   // abs 项没有 rel：以前这里只认 rel，项目外的绝对路径刚提取出来就被丢了
   // （2026-11-20 用户第二次报「还是没有」的真凶）。
   const key=f.rel||f.abs;
   if(!key||seen.has(key))return;
   seen.add(key);
   files.push(f);
  });
  if(files.length){
   const box=document.createElement('div');box.className='bubfiles';
   files.slice(0,8).forEach(f=>{
    const a=document.createElement('a');
    a.className='bubfile';
    a.href=fpUrl(null,f.rel,f.abs);
    a.setAttribute('download','');
    a.onclick=e=>{e.preventDefault();saveFile(a.href);};
    const ic=document.createElement('span');ic.className='ic';ic.innerHTML=ATT_ICON;
    const nm=document.createElement('b');nm.textContent=f.name;
    const dl=document.createElement('span');dl.className='dl';dl.textContent='下载到手机';
    a.append(ic,nm,dl);
    box.append(a);
   });
   d.append(box);
  }
 }
 if(m.at){const t=document.createElement('span');t.className='bt';
  t.textContent=hhmm(m.at);d.append(t);}
 return d;
}
// 底部实时行：OpenSquilla 一轮能跑四十分钟，transcript 里这期间没有新行，页面
// 只剩旧消息＝「半天没动静」（用户 2026-11-20 报）。native 把进行中那张卡的活动
// 回声（思考中/调用工具…）和本轮已跑时长塞进 live，这里原位改文字不重排版。
function renderLive(){
  const list=$('chatList');if(!list)return;
  let el=list.querySelector('.bublive');
  const live=chatLive;
  if(!live||!live.text){if(el)el.remove();return;}
  if(!el){
    el=document.createElement('div');el.className='bubsys bublive';
    const dot=document.createElement('span');dot.className='dot';
    const t=document.createElement('span');t.className='t';
    const bt=document.createElement('span');bt.className='bt';
    el.append(dot,t,bt);
    list.append(el);
  }
  el.querySelector('.t').textContent=live.text;
  el.querySelector('.bt').textContent=live.at?hhmm(live.at):'';
  if(list.lastElementChild!==el)list.append(el);
  if(chatAtBottom)list.scrollTop=list.scrollHeight;
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
 renderLive();
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
const C_SPIN_SVG='<svg class="cspin" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M12 3a9 9 0 1 1-9 9"/></svg>';
// 运行中：同一颗键变「停止生成」（实心方块，与桌面两端同款）。
const C_STOP_SVG='<svg viewBox="0 0 24 24" fill="currentColor"><rect x="7" y="7" width="10" height="10" rx="2"/></svg>';
// ---------- 附件：多选队列 / 串行上传 / 进度 / 附件条 ----------
// 手机这条是**真的把字节传上来**：PC 落盘进会话目录、回来绝对路径，发送时
// 拼进派发正文（agent 按路径读）。桌面端相反——本机路径直接引用，不复制。
// 流式上传：字节边收边写盘，内存不随大小涨，所以前端不再卡体积（8GB 兜底防误传）。
const ATT_MAX=8*1024*1024*1024;
const ATT_ICON='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3v5h5"/><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8l-5-5Z"/></svg>';
const ATTS={};  // prefix -> [{id,name,size,path,url(图片预览 blob)}]
function attList(p){return ATTS[p]||(ATTS[p]=[]);}
function attHas(p){return attList(p).length>0;}
function attFmt(n){
 if(n<1024)return n+'B';
 if(n<1048576)return (n/1024).toFixed(1)+'KB';
 return (n/1048576).toFixed(1)+'MB';
}
function attIcon(){
 const ic=document.createElement('span');ic.className='ic';
 ic.innerHTML=ATT_ICON;return ic;
}
function attRender(p){
 const box=$(p+'chatAtts');if(!box)return;
 box.replaceChildren();
 attList(p).forEach(a=>{
  const el=document.createElement('div');el.className='catt';
  if(a.url){
   const im=document.createElement('img');im.src=a.url;im.alt='';
   // 解码失败（改后缀/坏图）退回文档图标，别留一个破图方块。
   im.onerror=()=>im.replaceWith(attIcon());
   el.appendChild(im);
  }else el.appendChild(attIcon());
  const nm=document.createElement('b');nm.textContent=a.name;el.appendChild(nm);
  const sz=document.createElement('i');sz.textContent=attFmt(a.size);
  el.appendChild(sz);
  const x=document.createElement('button');x.className='x';x.textContent='×';
  x.setAttribute('aria-label','移除附件 '+a.name);
  x.onclick=()=>attDrop(p,a.id);
  el.appendChild(x);box.appendChild(el);
 });
 box.hidden=!attHas(p);
 cSync(p);
}
function attDrop(p,id){
 const list=attList(p),i=list.findIndex(a=>a.id===id);
 if(i<0)return;
 if(list[i].url)URL.revokeObjectURL(list[i].url);
 list.splice(i,1);attRender(p);
}
function attClear(p){
 attList(p).forEach(a=>{if(a.url)URL.revokeObjectURL(a.url);});
 ATTS[p]=[];attRender(p);
}
// 附件 → 派发正文：每行一个「请查看文件：<PC 绝对路径>」。agent 读的是路径，
// 所以正文可以完全由附件构成（只发文件不写字也成立）。
// 附件正文：**必须**带一条"先读再答、不许猜"的指令。只给路径时 agent 经常
// 拿不到内容就开始编（2026-11-18 用户点出这个根因：我拿到路径没读图就猜了
// 两轮）。路径本身还是必要的——宿主 CLI 只吃文本，读文件要它自己调 Read。
function attText(p){
 const list=attList(p);
 if(!list.length)return '';
 // 小文本文件上传时服务端已经把内容带回来了（a.text）：直接内联，agent 想
 // "没读就编"都没有机会。图片/大文件只能给路径 + 强制 Read 的指令。
 const inline=list.filter(a=>a.text);
 const byPath=list.filter(a=>!a.text);
 const parts=[];
 if(inline.length){
  parts.push('【附件】以下 '+inline.length+' 份文本内容已直接附在正文里，直接据此回答：');
  inline.forEach(a=>parts.push('--- '+a.name+' ---\n'+a.text));
 }
 if(byPath.length){
  parts.push('【附件】'+byPath.length+' 个文件已保存在本机。请先用 Read 工具逐个读取'
   +'文件内容再回答；读不到或读取失败就直接说明，不要凭文件名或路径猜测内容。');
  byPath.forEach(a=>parts.push('请查看文件：'+a.path));
 }
 return parts.join('\n');
}
// 上传定位：能拿待办 id 就带上；从派发页直接进会话没有待办 id，必须把
// session/project/exe 一起带上——否则 native 侧按待办查不到，只能回 404
// （这条路径在改之前是坏的）。
function attMeta(id){
 const m={};
 if(id)m.id=id;
 if(chatKind==='session'){
  m.session=chatSession||'';m.project=chatProjectPath();m.exe=chatExe||'';
 }
 return m;
}
function attUrl(id){
 let q='/m/upload?id='+encodeURIComponent(id||'');
 if(chatKind==='session')
  q+='&session='+encodeURIComponent(chatSession||'')
    +'&project='+encodeURIComponent(chatProjectPath())
    +'&exe='+encodeURIComponent(chatExe||'');
 return q;
}
function attPostJson(url,obj){
 return new Promise((res,rej)=>{
  const x=new XMLHttpRequest();
  x.open('POST',url,true);
  x.setRequestHeader('X-Requested-With','fetch');
  x.setRequestHeader('Content-Type','application/json');
  x.timeout=30000;
  x.onload=()=>{
   let j={};try{j=JSON.parse(x.responseText);}catch(_){}
   if(x.status>=200&&x.status<300)res(j);
   else rej(new Error(j.error||('HTTP '+x.status)));
  };
  x.onerror=()=>rej(new Error('连不上电脑'));
  x.ontimeout=()=>rej(new Error('等待电脑响应超时'));
  x.send(JSON.stringify(obj));
 });
}
function attPutChunk(uploadId,index,blob){
 return new Promise((res,rej)=>{
  const x=new XMLHttpRequest();
  x.open('PUT','/m/upload/chunk?upload='+encodeURIComponent(uploadId)
   +'&index='+index,true);
  x.setRequestHeader('X-Requested-With','fetch');
  x.setRequestHeader('Content-Type','application/octet-stream');
  x.timeout=180000;
  x.onload=()=>{
   let j={};try{j=JSON.parse(x.responseText);}catch(_){}
   if(x.status>=200&&x.status<300&&j.ok)res(j);
   else rej(new Error(j.error||('HTTP '+x.status)));
  };
  x.onerror=()=>rej(new Error('连不上电脑'));
  x.ontimeout=()=>rej(new Error('分片传输超时'));
  x.send(blob);
 });
}
// 大文件：切片 + 4 路并发 + 单片重试 + 缺片补传。
// 为什么并发：单个 TCP 流吃不满手机上行，几条一起才跑得满 WiFi。
// 为什么按片重试：8MB 一片重传代价小，整文件重来在弱网下基本传不完。
async function attUploadChunked(meta,f,onp){
 const init=await attPostJson('/m/upload/init',
  Object.assign({},meta,{name:f.name,size:f.size,type:f.type||''}));
 if(!init.ok)throw new Error(init.error||'初始化失败');
 const uploadId=init.uploadId,chunkSize=init.chunkSize||(8*1024*1024),
       chunks=init.chunks;
 const queue=[];
 for(let i=0;i<chunks;i++)queue.push(i);
 let sent=0;
 const sliceOf=i=>{
  const s=i*chunkSize,e=Math.min(s+chunkSize,f.size);
  return f.slice(s,e);
 };
 const worker=async()=>{
  while(queue.length){
   const i=queue.shift();
   let err=null;
   for(let t=0;t<3;t++){
    try{await attPutChunk(uploadId,i,sliceOf(i));err=null;break;}
    catch(e){err=e;await new Promise(r=>setTimeout(r,300*(t+1)));}
   }
   if(err)throw err;
   sent++;onp(sent/chunks);
  }
 };
 await Promise.all([worker(),worker(),worker(),worker()]);
 let fin=await attPostJson('/m/upload/done',{uploadId});
 // 服务端回 missing：只补这几片再合并一次，不用从头传。
 if(!fin.ok&&Array.isArray(fin.missing)&&fin.missing.length){
  for(const i of fin.missing)await attPutChunk(uploadId,i,sliceOf(i));
  fin=await attPostJson('/m/upload/done',{uploadId});
 }
 if(!fin.ok)throw new Error(fin.error||'合并失败');
 return fin;
}
// fetch 拿不到上传进度，只能 XHR。
function attPost(url,file,onp){
 return new Promise((res,rej)=>{
  const x=new XMLHttpRequest();
  x.open('POST',url,true);
  x.setRequestHeader('X-Requested-With','fetch');
  x.setRequestHeader('X-AgentRMC-Filename',encodeURIComponent(file.name));
  x.setRequestHeader('Content-Type','application/octet-stream');
  x.timeout=120000;
  if(x.upload)x.upload.onprogress=e=>{
   if(e.lengthComputable)onp(e.loaded/e.total);};
  x.onload=()=>{
   let j={};try{j=JSON.parse(x.responseText);}catch(_){}
   if(x.status>=200&&x.status<300&&j.path)res(j);
   else rej(new Error(j.error||('HTTP '+x.status)));
  };
  x.onerror=()=>rej(new Error('连不上电脑'));
  x.ontimeout=()=>rej(new Error('上传超时'));
  x.send(file);
 });
}
let attBusy=false;
async function attUpload(files){
 if(!chatCanDispatch||!chatId)return;
 const arr=Array.prototype.slice.call(files||[]);
 if(!arr.length)return;
 if(attBusy){cNotice('','上一批还在传，稍等一下','bad');return;}
 attBusy=true;
 const url=attUrl(chatKind==='session'?'':chatId);
 let okN=0,skipN=0,failN=0;
 for(let i=0;i<arr.length;i++){
  const f=arr[i];
  if(f.size>ATT_MAX){skipN++;continue;}
  $('chatUp').hidden=false;
  $('chatUpLabel').textContent=(arr.length>1?('('+(i+1)+'/'+arr.length+') '):'')
   +f.name;
  $('chatUpFill').style.width='0%';
  try{
   const prog=r=>{$('chatUpFill').style.width=(r*100).toFixed(0)+'%';};
   // 小文件单请求最省事（少两次往返）；大文件切片并发，才能跑满上行。
   const j=(f.size>8*1024*1024)
    ? await attUploadChunked(attMeta(chatKind==='session'?'':chatId),f,prog)
    : await attPost(url,f,prog);
   attList('').push({id:j.path,name:j.name||f.name,size:f.size,path:j.path,
    text:j.text||'',   // 小文本文件服务端已把内容带回来，派发时直接内联
    url:/^image\//i.test(f.type)?URL.createObjectURL(f):''});
   okN++;
  }catch(e){failN++;cNotice('','上传失败 '+f.name+'：'+(e.message||e),'bad');}
 }
 $('chatUp').hidden=true;$('chatUpFill').style.width='0%';
 attRender('');
 if(okN&&!failN)cNotice('','已附上 '+okN+' 个文件'
  +(skipN?('（'+skipN+' 个超过 8GB，没传）'):''),'good');
 else if(skipN&&!failN)cNotice('','文件超过 8GB，传不了','bad');
 attBusy=false;
}
// ---------- 电脑上的文件（会话 inbox）：列表 + 下载 ----------
// 手机只能看到「会话 inbox」一个目录：手机传上来的、电脑上 Ctrl+V 粘的图都
// 落在那里，所以这份列表天然就是「这次对话相关的文件」。下载用 <a download>
// 同源跳转，设备令牌 cookie 自动带上；路由侧只认 basename，跳不出 inbox。
const FP_ICON='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3v5h5"/><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8l-5-5Z"/></svg>';
let fpOpen=false;
// rel 有值＝下工作区里的产物（scope=project，服务端做 canonical + 白名单）；
// 否则按 inbox 里的 basename 下。
function fpUrl(name,rel,abs){
 let q='?id='+encodeURIComponent(chatKind==='session'?'':chatId);
 if(chatKind==='session')
  q+='&session='+encodeURIComponent(chatSession||'')
    +'&project='+encodeURIComponent(chatProjectPath());
 if(chatExe)q+='&exe='+encodeURIComponent(chatExe);
 if(rel)q+='&scope=project&rel='+encodeURIComponent(rel);
 else if(abs)q+='&scope=abs&abs='+encodeURIComponent(abs);
 else if(name)q+='&name='+encodeURIComponent(name);
 // 三种来源都算「取一个文件」：以前 abs 漏在判断外，项目外的产物会拼成
 // /m/files?...&scope=abs——那是**列表**路由，直接 404，页面只能报「文件不在」
 // （2026-11-20 用户：文件明明在桌面上）。单点修在这，别在调用处绕。
 return '/m/'+((name||rel||abs)?'file':'files')+q;
}
// 下载统一入口。文件可能已经被移走/删掉：直接 <a download> 的话，浏览器会把
// 那条 404 也存下来，叫 file.txt（用户 2026-11-20 第三次报同一个症状）。
// 这里先 HEAD 探活：200 才跳转，文件名由服务端 Content-Disposition 给；
// 探不到就在页面上说清楚。**新增下载入口也只准走这里**（见下载契约文档）。
// 图片查看：点一下先看大图，不直接下载（用户 2026-11-22）。
// 下载仍走 saveFile：浮层里的「保存到手机」才真落盘，
// 这样下载契约（HEAD 探活 → 404 只 toast）一字不改。
let ivUrl='';
function openImgView(url,name){
 if(!url||url==='#')return;
 ivUrl=url;
 $('ivName').textContent=name||'';
 const st=$('ivStage');st.replaceChildren();
 const im=document.createElement('img');im.src=url;im.alt=name||'';
 // 图不在了（删了/移走了）：直接说清楚，不给破图。
 im.onerror=()=>{st.replaceChildren();
  const e=document.createElement('div');e.className='iverr';
  e.textContent='这张图取不到了（可能已在电脑上删除或移走）';
  st.append(e);};
 st.append(im);
 $('imgView').hidden=false;
}
function closeImgView(){$('imgView').hidden=true;$('ivStage').replaceChildren();ivUrl='';}
async function saveFile(url){
 if(!url||url==='#')return;
 const downloadAt=(u)=>{const a=document.createElement('a');
  a.href=u;a.setAttribute('download','');a.rel='noopener';
  document.body.append(a);a.click();a.remove();};
 try{
  const r=await fetch(url,{method:'HEAD',cache:'no-store'});
  if(r.status===401){showPair();return;}
  if(r.ok){downloadAt(url);return;}
  // 404：正文给的路径可能已被改名/移动（agent 常在产出后又改一次名）。
  // 让服务端在同目录里找真实文件，找到就下那个（页面上看到的仍是真实文件名），
  // 找不到才报错。
  const alt=url.replace('/m/file?','/m/file/resolve?');
  if(alt!==url){
   const r2=await fetch(alt,{method:'HEAD',cache:'no-store'});
   if(r2.ok){toast('原路径已失效，已找到同名文件');downloadAt(alt);return;}
  }
  toast('取不到这个文件（可能已移动或删除）');
 }catch(e){toast('下载失败：网络不通');}
}
// 产物行第二行原来放 rel 路径（「dist/AgentRMC-Setup.exe」）——上级目录对
// 认文件没用，名字里已经带得出（用户 2026-11-19）；换成修改时间，今天
// 只给时分、昨天/更早给日期，扫一眼就知道哪个是刚出炉的。
function fpTime(iso){
 const t=Date.parse(iso||'');if(isNaN(t))return '';
 const d=new Date(t),n=new Date();
 const hm=String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');
 if(d.toDateString()===n.toDateString())return hm;
 n.setDate(n.getDate()-1);
 if(d.toDateString()===n.toDateString())return '昨天 '+hm;
 return (d.getMonth()+1)+'-'+String(d.getDate()).padStart(2,'0')+' '+hm;
}
function fpClose(){$('chatFilePanel').hidden=true;fpOpen=false;}
async function fpLoad(){
 const list=$('chatFileList');if(!list)return;
 list.replaceChildren();
 const ld=document.createElement('div');ld.className='fempty';
 ld.textContent='正在读产物与文件…';list.append(ld);
 try{
  const r=await fetch(fpUrl(''),{cache:'no-store'});
  if(r.status===401){showPair();return;}
  const j=await r.json();
  const files=(j&&j.files)||[];
  // 产物两类，混在一起按修改时间排，面板只展示「能下的东西」：
  // ① 工作区扫出来的（rel）；② 正文里提到的项目外产物（abs，桌面/别处）。
  // 以前只有 ①，agent 写到桌面的封面在面板里一条都看不到（用户 2026-11-21）。
  const arts=(j&&j.artifacts)||[];
  const mentioned=(j&&j.mentioned)||[];
  const prods=arts.concat(mentioned)
   .sort((a,b)=>((Date.parse(b.mtime||0)||0)-(Date.parse(a.mtime||0)||0)))
   .slice(0,30);
  list.replaceChildren();
  $('chatFileCount').textContent=
   ('产物 '+prods.length)+(files.length?(' · 文件 '+files.length):'');
  if(!prods.length&&!files.length){
   const e=document.createElement('div');e.className='fempty';
   e.textContent='还没有产物，也没有传上来的文件。';
   list.append(e);return;
  }
  // 产物行：图/文档/安装包，下载走工作区相对路径或项目外绝对路径。
  function fpRow(f){
   const a=document.createElement('a');a.className='mprow frow';
   a.href=fpUrl(null,f.rel,f.abs);a.download=f.name||'';
   a.onclick=e=>{e.preventDefault();saveFile(a.href);};
   const ic=document.createElement('span');ic.className='mpico';
   ic.innerHTML=FP_ICON;
   const wrap=document.createElement('span');wrap.className='mpname';
   const nm=document.createElement('b');nm.textContent=f.name||'';
   const dir=document.createElement('i');dir.className='fdir';
   dir.textContent=fpTime(f.mtime);
   wrap.append(nm,dir);
   const sz=document.createElement('span');sz.className='fsize';
   sz.textContent=attFmt(f.size||0);
   a.append(ic,wrap,sz);return a;
  }
  if(prods.length){
   const sec=document.createElement('div');sec.className='fpsec';
   sec.textContent='产物';list.append(sec);
   prods.forEach(f=>list.append(fpRow(f)));
  }
  // inbox 区在下：手机传上来的、电脑粘的图（粘图下载没意义，但留着）。
  if(files.length){
   const sec=document.createElement('div');sec.className='fpsec';
   sec.textContent='传到电脑的（inbox）';list.append(sec);
   files.forEach(f=>{
    const a=document.createElement('a');a.className='mprow frow';
    a.href=fpUrl(f.name);a.download=f.name||'';
    a.onclick=e=>{e.preventDefault();saveFile(a.href);};
    const ic=document.createElement('span');ic.className='mpico';
    ic.innerHTML=FP_ICON;
    const wrap=document.createElement('span');wrap.className='mpname';
    const nm=document.createElement('b');nm.textContent=f.name||'';
    const dir=document.createElement('i');dir.className='fdir';
    dir.textContent=fpTime(f.mtime);
    wrap.append(nm,dir);
    const sz=document.createElement('span');sz.className='fsize';
    sz.textContent=attFmt(f.size||0);
    a.append(ic,wrap,sz);list.append(a);
   });
  }
 }catch(e){
  list.replaceChildren();
  const er=document.createElement('div');er.className='fempty';
  er.textContent='连不上电脑';list.append(er);
 }
}
$('chatFiles').onclick=()=>{
 if(fpOpen){fpClose();return;}
 fpOpen=true;
 // 两个上浮面板抢同一块位置：开一个就关另一个。
 mpOpen=false;$('chatModelPanel').hidden=true;
 $('chatFilePanel').hidden=false;
 fpLoad();
};
$('chatRefresh').onclick=()=>{
 if(!chatId)return;
 cNotice('','正在刷新记录…');
 chatMsgs=[];chatIds=new Set();chatTotal=0;chatHasMore=false;
 fetchTranscript(0,'init');
};
$('chatFileClose').onclick=fpClose;
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
 // 宿主报「这一轮正在跑」且输入框空着：这颗键让位给「停止生成」。
 if(cRunning()&&n===0&&!attHas(p)){cStopReady(p,!ta.disabled);return;}
 // 只有附件没有正文也能发（正文由路径自动拼）。
 cSendReady(p,!ta.disabled&&(n>0||attHas(p)));
}
// 这条会话此刻在不在跑：宿主插件每 5s 报一次 live（running 数组）。
// 两种入口都要判：从「派发页」进（chatKind='session'，startChat 就带了会话 id），
// 从「待我处理」卡片进（chatKind='work'，会话 id 要等 mergeChatPage 从载荷补）。
// 早先按 chatKind 分流过一次，结果工单面永远判不出 running——就是「停止没有」
// 的第二个原因。会话 id 一律以 chatSession 为准，没有就不判。
function chatSid(){return chatSession||'';}
function cRunning(){
 const sid=chatSid();
 if(!sid||!chatExe)return false;
 const live=((STATE||{}).dispatch||{}).live||{};
 let v=live[chatExe];
 if(!v){const want=String(chatExe).toLowerCase();
  for(const k in live){if(String(k).toLowerCase()===want){v=live[k];break;}}}
 return !!(v&&(v.running||[]).indexOf(sid)>=0);
}
function cStopReady(p,on){
 const b=$(p+'chatSend');if(!b)return;
 b.dataset.busy='0';b.disabled=!on;
 b.title='停止生成';b.setAttribute('aria-label','停止生成');
 if(b.classList)b.classList.add('stopmode');
 b.innerHTML=C_STOP_SVG;
}
function cSendReady(p,on){
 const b=$(p+'chatSend');if(!b)return;
 b.dataset.busy='0';b.disabled=!on;
 if(b.classList)b.classList.remove('stopmode');
 // 只留图标（用户 2026-11-18），文字退到 title/aria-label。
 b.title='派发';b.setAttribute('aria-label','派发');
 b.innerHTML=C_PLANE_SVG;
}
function cBusy(p){
 const b=$(p+'chatSend');if(!b)return;
 b.dataset.busy='1';b.disabled=true;b.innerHTML=C_SPIN_SVG;
}
// 解在途锁：只清标志，按钮状态交给下一次 cSync 按内容重算。手机这端
// postResult 是同步等 HTTP 回执的，失败/成功都必须自己复位，否则
// cSync 见 busy=1 会一直 return，按钮永远停在「派发中…」。
function cIdle(p){
 const b=$(p+'chatSend');if(b)b.dataset.busy='0';
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
 // 返回键纯图标：这里只同步无障碍标签，别写 textContent（会把 SVG 冲掉）。
 $('chatBack').setAttribute('aria-label',chatKind==='session'?'返回派发':'返回待办');
 const title=chatMeta.sessionTitle||chatMeta.title||
  (chatKind==='session'?'会话 '+String(chatSession||'').slice(0,8):'会话');
 $('chatTitle').textContent=title;
 // 副标题只放跟大标题不重复的东西：待办页大标题往往就是宿主名/会话名，
 // 再写一遍 WorkBuddy.exe 加「共 N 条」是纯噪声——这里只留项目名。
 // 派发页多一个 Agent 名（大标题是会话标题，不会重复）。
 // 副标题两面同构：Agent 名 · 项目名（有谁放谁）。以前 work 面不报 Agent
 // 名，跟派发面的顶栏对不上（2026-11-19 用户指出）。
 const bits=[];
 const who=chatAgentName||chatMeta.agentName||'';
 if(who)bits.push(who);
 const proj=chatProject||chatMeta.project||'';
 if(proj)bits.push(projectName(proj));
 $('chatSub').textContent=bits.join(' · ');
 $('chatSub').hidden=!bits.length;
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
const MP_LVL_LABELS={off:'关',minimal:'极低',low:'极低',medium:'低',high:'中',xhigh:'极高',max:'最高',adaptive:'自适应'};
const MP_CUBE_SVG='<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><path d="M12 2.5 20.5 7v10L12 21.5 3.5 17V7L12 2.5Z"/><path d="M3.5 7 12 11.7 20.5 7"/><path d="M12 11.7V21.5"/></svg>';
let mpOpen=false,mpLevel='providers',mpProvider=null;
let mpSel=null; // {providerId,modelId,level?}；null=不切换
function mpModels(){return ((STATE||{}).dispatch||{}).models||[];}
// 会话当前宿主：模型目录按它分流（与桌面端同款口径）——ZCode 读个人
// 供应商目录，OpenSquilla 读桥从网关 models.list 推来的 osqModels。
function mpExeNow(){
 return chatKind==='session'?chatExe:(chatMeta.agentExe||'');
}
function mpListFor(){
 const d=((STATE||{}).dispatch)||{};
 const e=mpExeNow();
 if(e==='OpenSquilla.exe')return d.osqModels||[];
 if(e==='DeepSeek Harness.exe')return d.dshModels||[];
 return d.models||[];
}
// 目录里某个模型的元数据（DSH 的 details：{name,efforts,defaultEffort}）。
function mpDetail(providerId,modelId){
 const list=mpListFor();
 for(const p of list){
  if(p&&p.providerId===providerId&&p.details)return p.details[modelId]||null;
 }
 return null;
}
function mpThoughts(){
 // 思考强度只有 ZCode 通道吃；OpenSquilla 档位值域不同且网关不校验
 //（实测瞎写也收），不给假答应——滑杆藏掉，一律跟随会话。
 if(mpExeNow()==='OpenSquilla.exe')return [];
 // DSH：档位由所选模型的适配器公布（每个模型不一样）；目录没带 details
 //（插件旧版）就不给滑杆，别拿固定五档瞎猜。
 if(mpExeNow()==='DeepSeek Harness.exe'){
  if(!mpSel)return [];
  const d=mpDetail(mpSel.providerId,mpSel.modelId);
  return d&&d.efforts&&d.efforts.length?d.efforts.map(e=>e.id):[];
 }
 return ((STATE||{}).dispatch||{}).thoughtLevels||
 ['low','medium','high','xhigh','max'];
}
function mpActive(){
 // 只要有会话就显示胶囊：用户要看的是「这条会话在用什么模型」，跟能不能在
 // 这里切模型是两件事。非 ZCode 时只报当前模型，不列可选。
 if(!chatCanDispatch)return false;
 return !!(chatKind==='session'?chatExe:chatMeta.agentExe);
}
// 有没有可切换的模型目录：ZCode 个人供应商 / OpenSquilla 网关目录（桥在线才有）/
// DSH 目录（插件读 ctx.llm 推来）。
function mpSwitchable(){
 if(mpListFor().length===0)return false;
 const e=mpExeNow();
 return e==='ZCode.exe'||e==='OpenSquilla.exe'||e==='DeepSeek Harness.exe';
}
// 这一家的切换是不是「会话级」（DSH 的模型本来就是会话级持久状态）。
function mpSessionLevel(){return mpExeNow()==='DeepSeek Harness.exe';}
function mpResetState(){mpOpen=false;mpLevel='providers';mpProvider=null;mpSel=null;}
function syncModelChip(){
 const chip=$('chatModelChip'),panel=$('chatModelPanel');
 const on=mpActive();
 chip.hidden=!on;
 if(!on){panel.hidden=true;return;}
 $('chatModelLabel').textContent=mpSel?mpSel.modelId:(chatModel||'选择模型');
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
       thought=$('chatMpThought'),commit=$('chatMpCommit');
 if(!mpOpen||!mpActive()){panel.hidden=true;return;}
 panel.hidden=false;
 back.hidden=mpLevel==='providers'&&!mpProvider;
 list.replaceChildren();
 thought.hidden=true;
 commit.hidden=true;
 if(mpLevel==='providers'){
  foot.hidden=false;
  // 第一行是「会话自己当前的模型」：读到真实模型名就报出来，读不到才写
  // 「不切换」。选中态＝没做 override。
  const switchable=mpSwitchable();
  foot.textContent=switchable
   ?(mpSessionLevel()?'选一个模型，再点「设为这条会话的模型」':'管理模型')
   :'当前宿主不支持在 RMC 里切换模型';
  list.append(mpTextRow(chatModel?('会话当前 · '+chatModel):'会话当前（不切换）',!mpSel,()=>{
   mpSel=null;mpOpen=false;syncModelChip();panel.hidden=true;
  }));
  if(!switchable)return;
  mpListFor().forEach(p=>{
   list.append(mpSvgRow(MP_CUBE_SVG,p.name||p.providerId,
    mpExeNow()==='OpenSquilla.exe'?'网关':'API',
    mpSel&&mpSel.providerId===p.providerId,
    ()=>{mpProvider=p;mpLevel='models';mpRenderPanel();}));
  });
 }else{
  foot.hidden=true;
  const sec=document.createElement('div');sec.className='mpsec';
  sec.textContent='模型';
  list.append(sec);
  (mpProvider&&mpProvider.models||[]).forEach(mid=>{
   const d=mpProvider.details&&mpProvider.details[mid];
   list.append(mpTextRow(d&&d.name?d.name:mid,
    mpSel&&mpSel.providerId===mpProvider.providerId&&
    mpSel.modelId===mid,
    ()=>{
     // 档位随模型重算：不同模型公布的强度值域不一样，留着旧档会选出不存在的值。
     const prev=mpSel&&mpSel.level;
     mpSel={providerId:mpProvider.providerId,modelId:mid,level:null};
     const lv=mpThoughts();
     if(prev&&lv.indexOf(prev)>=0)mpSel.level=prev;
     else if(d&&d.defaultEffort&&lv.indexOf(d.defaultEffort)>=0)
      mpSel.level=d.defaultEffort;
     mpRenderPanel();syncModelChip();
    }));
  });
  // 思考强度：选中模型后出现（0=跟随会话）
  if(mpSel){
   const lv=mpThoughts();
   if(lv.length){
    thought.hidden=false;
    const range=$('chatMpRange');
    range.max=String(lv.length);
    range.value=mpSel.level?String(lv.indexOf(mpSel.level)+1):'0';
    $('chatMpTVal').textContent=mpSel.level
     ?(MP_LVL_LABELS[mpSel.level]||mpSel.level):'跟随会话';
   }
   // DSH 走会话级切换：多给一颗提交键（与派发的一次性 override 分开）。
   const commit=$('chatMpCommit');
   commit.hidden=!mpSessionLevel();
  }
 }
}
$('chatModelChip').onclick=()=>{
 if(!mpActive())return;
 mpOpen=!mpOpen;
 if(mpOpen){fpClose();mpRenderPanel();}else $('chatModelPanel').hidden=true;
};
// 会话级切换（DSH）：立刻下发 switchModel，结果走 sendChat 之外的一条路
//（postResult 直接拿回执，不等 5 秒整窗）。
$('chatMpCommit').onclick=async()=>{
 if(!mpSel||!mpSessionLevel())return;
 const model={providerId:mpSel.providerId,modelId:mpSel.modelId,
  level:mpSel.level||''};
 mpOpen=false;$('chatModelPanel').hidden=true;
 cNotice('','正在切模型，等宿主回执…','');
 const res=await postResult({cmd:'switchModel',exe:mpExeNow(),
  session:chatSession,providerId:model.providerId,modelId:model.modelId,
  level:model.level});
 mpResetState();syncModelChip();
 if(res.ok){
  cNotice('','已切到 '+(model.modelId||'新模型')+'（下一步生效）','good');
  toast('已切模型');
  load();fetchTranscript(0,'append');
 }else{
  cNotice('','没切成功：'+((res.data&&res.data.error)||('HTTP '+res.status)),'bad');
 }
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
 chatTotal=p.total||0; chatModel=p.model||chatModel; chatLive=p.live||null;
 // 工单面（chatKind='work'）进来时只有工单 id：从载荷里补会话 id 与宿主 exe，
 // 否则「停止/插话」判不出这是哪条会话（两条路都要同一套 id）。
 if(!chatSession&&p.sessionId)chatSession=p.sessionId;
 if(!chatExe&&p.agentExe)chatExe=p.agentExe;
 if(!chatAgentName&&p.agentName)chatAgentName=p.agentName;
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
 renderLive();  // 没有新消息也要刷：本轮已跑时长在走
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
// 视图栈：手机的系统返回键/侧边手势在网页里就是 history.back()。单页应用
// 不压栈的话，它会直接把页面退掉回到浏览器主页（2026-11-19 用户反馈）。
// 这里只给「列表 → 会话聊天」压一层，返回键就变成「回到上一级」。
const VIEW_LIST='list', VIEW_CHAT='chat';
try{history.replaceState({v:VIEW_LIST},'');}catch(_){}
function pushChatView(){
 try{
  if(!(history.state&&history.state.v===VIEW_CHAT))history.pushState({v:VIEW_CHAT},'');
 }catch(_){}
}
window.addEventListener('popstate',e=>{
 const v=(e.state&&e.state.v)||VIEW_LIST;
 // 栈已经回到 list，这里只负责关掉聊天页——**不要**再调 history.back()，
 // 否则会连退两级直接离开页面。
 if(v!==VIEW_CHAT&&chatId)closeChat();
});
function startChat(kind,id,meta){
 chatKind=kind;chatId=id;chatMsgs=[];chatIds=new Set();chatLoading=false;chatMeta={};chatLive=null;
 mpResetState();
 chatExe=meta&&meta.exe||'';chatSession=meta&&meta.session||'';
 chatProject=meta&&meta.project||'';chatAgentName=meta&&meta.agentName||'';
 chatCanDispatch=meta&&meta.canDispatch!==false;
 cValue('','');cNotice('','');attClear('');fpClose();
 pushChatView();
 render();fetchTranscript(0,'init');
}
function openChat(id){startChat('work',id,{});}
function openSession(agent,row){
 tab=1;
 startChat('session',row.id,{exe:agent.exe,session:row.id,project:row.project,
  agentName:agent.name,canDispatch:row.can_dispatch!==false});
}
function closeChat(){
 chatId='';chatMsgs=[];chatIds=new Set();chatModel='';chatLive=null;
 attClear('');fpClose();
 chatKind='work';chatExe='';chatSession='';chatProject='';chatAgentName='';
 chatCanDispatch=true;
 render();
}
async function sendChat(){
 const typed=$('chatText').value.trim();
 const refs=attText('');
 const t=typed?(refs?typed+'\n'+refs:typed):refs;
 if(!chatId||!chatCanDispatch)return;
 // 运行中：空输入＝停止（同桌面两端、同 DSH 自己的停止键）；有正文＝插话。
 if(cRunning()&&!t){
  cBusy('');
  const res=await postResult({cmd:'control',action:'stop',
   exe:chatExe,session:chatSession});
  cIdle('');cSync('');
  if(res.ok)cNotice('','已停止这一轮','good');
  else cNotice('','没停下来：'+((res.data&&res.data.error)||('HTTP '+res.status)),'bad');
  load();
  return;
 }
 if(!t)return;
 if(cRunning()){
  cBusy('');
  const res=await postResult({cmd:'control',action:'steer',
   exe:chatExe,session:chatSession,text:t});
  cIdle('');
  if(!res.ok){
   // 插话没落地：正文留在框里（多半是这一轮刚好跑完了，直接再发就是新一轮）。
   cValue('',typed);
   cNotice('','没插进去：'+((res.data&&res.data.error)||('HTTP '+res.status))
    +'（草稿已保留）','bad');
   return;
  }
  cValue('','');attClear('');
  cNotice('','已插话：并进当前这一轮，不再另起一轮','good');
  toast('已插话');load();fetchTranscript(0,'append');
  return;
 }
 cBusy('');
 const model=pickedModel();
 const res=chatKind==='session'
  ?await postResult({cmd:'dispatchSession',agent:chatExe,session:chatSession,
     project:chatProject,text:t,model:model||undefined})
  :await postResult({cmd:'dispatch',id:chatId,text:t,model:model||undefined});
 if(!res.ok){
  // 失败：正文留在框里改完可重发（cValue 会把高度/计数/按钮一起复位）；
  // 附件不清——路径还在 native 落盘目录里，直接再发一次即可。
  cIdle('');
  cValue('',typed);
  const why=(res.data&&res.data.error)||('HTTP '+res.status);
  cNotice('','没发出去：'+why+'（草稿已保留）','bad');
  return;
 }
 cIdle('');
 cValue('','');attClear('');
 // 派发成功：模型选择是一次性的，关掉面板并重置，下一发默认回「会话当前」。
 mpOpen=false;$('chatModelPanel').hidden=true;mpResetState();
 cNotice('',chatKind==='session'
  ?'已派发。送达回执到了会进「待我处理」；这里继续看会话记录。'
  :'已派回这条会话，等回执…','good');
 toast('已派发');
 load();fetchTranscript(0,'append');
}
$('chatBack').onclick=()=>{
 // 栈里压着 chat 就让系统返回（和手势返回走同一条路），否则直接关。
 if(history.state&&history.state.v===VIEW_CHAT)history.back();
 else closeChat();
};
$('chatSend').onclick=sendChat;
$('chatAttach').onclick=()=>$('chatFile').click();
$('chatFile').onchange=()=>{
 // 先把 FileList 快照成数组再清 input.value——value 一清，FileList 当场变空。
 const files=Array.prototype.slice.call($('chatFile').files||[]);
 $('chatFile').value='';
 if(files.length)attUpload(files);
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
   signal:timeoutSignal(12000)});
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
// 未分类任务的条数不进摘要（用户 2026-11-19）：数字叠三个把行挤成
// 两行，组头本来也带数量，摘要里重复。
const liveCount=rows.filter(r=>!r.archived).length;
$('dispatchCount').textContent=agent.name+' · '+active.length+' 个项目'
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