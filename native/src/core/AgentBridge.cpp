#include "AgentBridge.h"

#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kMaxBodyBytes = 64 * 1024;
constexpr int kStaleMs = 10000;

QString defaultFor(const QString &kind) {
    if (kind == QStringLiteral("stop")) {
        return QString::fromUtf8("任务完成，等你回来验收");
    }
    if (kind == QStringLiteral("permission")) {
        return QString::fromUtf8("等你确认一个操作");
    }
    return QString::fromUtf8("有新消息");
}

}  // namespace

AgentBridge::AgentBridge(QObject *parent) : QObject(parent) {}

QHostAddress AgentBridge::serverAddress() const {
    return server_ != nullptr ? server_->serverAddress() : QHostAddress();
}

bool AgentBridge::start(quint16 port) {
    if (server_ != nullptr) {
        return true;
    }
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this,
            &AgentBridge::onConnected);
    if (!server_->listen(QHostAddress::LocalHost, port)) {
        qWarning().noquote() << "[bridge] 监听失败（" << server_->errorString()
                             << "），Agent 通知不可用";
        delete server_;
        server_ = nullptr;
        return false;
    }
    return true;
}

void AgentBridge::setStateProvider(std::function<QString()> provider) {
    stateProvider_ = std::move(provider);
}

void AgentBridge::setWorkItemsProvider(std::function<QString()> provider) {
    workItemsProvider_ = std::move(provider);
}

void AgentBridge::setWorkItemAckHandler(
    std::function<bool(const QString &)> handler) {
    workItemAckHandler_ = std::move(handler);
}

void AgentBridge::setWorkItemUpdateHandler(
    std::function<QJsonObject(const QJsonObject &)> handler) {
    workItemUpdateHandler_ = std::move(handler);
}

void AgentBridge::setWorkersProvider(std::function<QString()> provider) {
    workersProvider_ = std::move(provider);
}

void AgentBridge::setCommandHandler(
    std::function<QJsonObject(const QJsonObject &)> handler) {
    commandHandler_ = std::move(handler);
}

void AgentBridge::setHealthProvider(std::function<QString()> provider) {
    healthProvider_ = std::move(provider);
}

void AgentBridge::setDispatchPendingProvider(
    std::function<QString(const QString &exe)> provider) {
    dispatchPendingProvider_ = std::move(provider);
}

void AgentBridge::setDispatchAckHandler(
    std::function<bool(const QJsonObject &)> handler) {
    dispatchAckHandler_ = std::move(handler);
}

void AgentBridge::setRequestsHandler(
    std::function<void(const QJsonObject &)> handler) {
    requestsHandler_ = std::move(handler);
}

void AgentBridge::onConnected() {
    while (server_->hasPendingConnections()) {
        auto *socket = server_->nextPendingConnection();
        buffers_.insert(socket, QByteArray{});
        connect(socket, &QTcpSocket::disconnected, socket,
                &QTcpSocket::deleteLater);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            buffers_.remove(socket);
        });
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { onReadable(socket); });
        // 客户端半截请求挂着不走：超时清掉。
        QTimer::singleShot(kStaleMs, socket, [this, socket] {
            if (buffers_.contains(socket)) {
                dropSocket(socket);
            }
        });
    }
}

void AgentBridge::onReadable(QTcpSocket *socket) {
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
    // 按行拆头：request line + 每行一个 header，兼容 curl / node / 手拼请求。
    const QList<QByteArray> headerLines = head.split('\n');
    const QByteArray requestLine = headerLines.value(0).trimmed();

    if (requestLine.startsWith("GET /state ")) {
        const QString state = stateProvider_ ? stateProvider_()
                                             : QStringLiteral("{}");
        finish(socket, "200 OK", state.toUtf8(), "application/json");
        return;
    }
    if (requestLine.startsWith("GET /work-items ")) {
        const QString items = workItemsProvider_
                                  ? workItemsProvider_()
                                  : QStringLiteral(
                                        "{\"attention_count\":0,\"items\":[]}");
        finish(socket, "200 OK", items.toUtf8(), "application/json");
        return;
    }
    if (requestLine.startsWith("GET /workers ")) {
        const QString workers =
            workersProvider_ ? workersProvider_() : QStringLiteral("{\"workers\":[]}");
        finish(socket, "200 OK", workers.toUtf8(), "application/json");
        return;
    }
    if (requestLine.startsWith("GET /health ")) {
        const QString health =
            healthProvider_ ? healthProvider_() : QStringLiteral("{}");
        finish(socket, "200 OK", health.toUtf8(), "application/json");
        return;
    }
    if (requestLine.startsWith("GET /dispatch/pending")) {
        // DSH 插件轮询取件。exe 是查询参数（映像名带空格，%20 编码）。
        QString exe;
        const int queryAt = requestLine.indexOf('?');
        if (queryAt >= 0) {
            const QByteArray query = requestLine.mid(queryAt + 1);
            const int spaceAt = query.indexOf(' ');
            const QByteArray pairs =
                query.left(spaceAt >= 0 ? spaceAt : query.size());
            for (const QByteArray &pair : pairs.split('&')) {
                if (pair.startsWith("exe=")) {
                    exe = QUrl::fromPercentEncoding(pair.mid(4));
                }
            }
        }
        const QString items = dispatchPendingProvider_
                                  ? dispatchPendingProvider_(exe)
                                  : QStringLiteral("[]");
        finish(socket, "200 OK", items.toUtf8(), "application/json");
        return;
    }
    const bool isNotify = requestLine.startsWith("POST /notify ");
    const bool isSpeak = requestLine.startsWith("POST /speak ");
    const bool isAck = requestLine.startsWith("POST /work-items/ack ");
    const bool isUpdate = requestLine.startsWith("POST /work-items/update ");
    const bool isCommand = requestLine.startsWith("POST /command ");
    const bool isDispatchAck = requestLine.startsWith("POST /dispatch/ack ");
    const bool isRequests = requestLine.startsWith("POST /requests ");
    if (!isNotify && !isSpeak && !isAck && !isUpdate && !isCommand &&
        !isDispatchAck && !isRequests) {
        finish(socket, "404 Not Found");
        return;
    }

    int contentLength = -1;
    for (const QByteArray &line : headerLines) {
        if (line.toLower().startsWith("content-length:")) {
            contentLength = line.mid(15).trimmed().toInt();
            break;
        }
    }
    if (contentLength < 0) {
        finish(socket, "400 Bad Request");
        return;
    }
    if (contentLength > kMaxBodyBytes) {
        finish(socket, "413 Payload Too Large");
        return;
    }
    const QByteArray body = buffer.mid(headerEnd + 4);
    if (body.size() < contentLength) {
        return;  // 还没收全，等下一段
    }

    const QJsonObject obj =
        QJsonDocument::fromJson(body.left(contentLength)).object();
    if (isDispatchAck) {
        // 插件注入回执：ok=已 followup（投递成立），false=守卫拒绝带原因。
        // dispatch_id 不认识（重启丢账 / 重复 ack）回 404，别静默当成功。
        const QString dispatchId =
            obj.value(QStringLiteral("dispatch_id")).toString().trimmed();
        const bool handled =
            !dispatchId.isEmpty() && dispatchAckHandler_ &&
            dispatchAckHandler_(obj);
        finish(socket, handled ? "204 No Content" : "404 Not Found");
        return;
    }
    if (isRequests) {
        // DSH 插件推送某会话当前待答请求（B1 只读展示）。无 handler 也回 204，
        // 别让插件以为失败反复重试。
        if (requestsHandler_) {
            requestsHandler_(obj);
        }
        finish(socket, "204 No Content");
        return;
    }
    if (isAck) {
        const QString id = obj.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty()) {
            finish(socket, "400 Bad Request");
            return;
        }
        const bool ok = workItemAckHandler_ && workItemAckHandler_(id);
        finish(socket, ok ? "204 No Content" : "404 Not Found");
        return;
    }
    if (isUpdate) {
        // 宿主 Agent 主动汇报阶段。stage 不认识就 400，别静默当成功——
        // 静默成功会让 Agent 以为报上去了，而列表里什么都没变。
        const QJsonObject result =
            workItemUpdateHandler_
                ? workItemUpdateHandler_(obj)
                : QJsonObject{{QStringLiteral("ok"), false},
                              {QStringLiteral("error"),
                               QStringLiteral("bridge has no update handler")}};
        finish(socket,
               result.value(QStringLiteral("ok")).toBool() ? "200 OK"
                                                           : "400 Bad Request",
               QJsonDocument(result).toJson(QJsonDocument::Compact),
               "application/json");
        return;
    }
    if (isCommand) {
        // 系统控制：说法解析、开关、危险动作确认都在主窗那边判，这里只把
        // 它给的 code 翻成 HTTP 状态——缺确认必须是 409，不能静默当成功。
        const QJsonObject result =
            commandHandler_
                ? commandHandler_(obj)
                : QJsonObject{{QStringLiteral("ok"), false},
                              {QStringLiteral("code"), 400},
                              {QStringLiteral("message"),
                               QStringLiteral("bridge has no command handler")}};
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
    QString kind;
    QString message;
    QString raise;
    QString sessionId;
    QString project;
    QString eventId;
    QString pluginVersion;
    if (isSpeak) {
        kind = QStringLiteral("speak");
        message = obj.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            finish(socket, "400 Bad Request");
            return;
        }
    } else {
        kind = obj.value(QStringLiteral("event")).toString().trimmed();
        if (kind.isEmpty()) {
            kind = QStringLiteral("message");
        }
        message = obj.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = defaultFor(kind);
        }
        raise = obj.value(QStringLiteral("raise")).toString().trimmed();
        sessionId =
            obj.value(QStringLiteral("session_id")).toString().trimmed();
        project = obj.value(QStringLiteral("project")).toString().trimmed();
        eventId = obj.value(QStringLiteral("event_id")).toString().trimmed();
    }
    const QString title = obj.value(QStringLiteral("title")).toString()
                                  .isEmpty()
                              ? QStringLiteral("ZCode")
                              : obj.value(QStringLiteral("title")).toString();
    // hook 和 MCP 都可以带自己那份插件版本（/speak 走 MCP，同样要能报出自己是谁）。
    pluginVersion =
        obj.value(QStringLiteral("plugin_version")).toString().trimmed();
    // 语音专用短文案，可选；缺省时原生端退回念 message。
    const QString speech = obj.value(QStringLiteral("speech")).toString().trimmed();
    finish(socket, "204 No Content");
    emit notify(kind, title, message, raise, sessionId, project, eventId,
                pluginVersion, speech);
}

void AgentBridge::finish(QTcpSocket *socket, const char *status,
                         const QByteArray &body, const char *contentType) {
    QByteArray response = QByteArray("HTTP/1.1 ") + status +
                          "\r\nConnection: close\r\n";
    if (contentType != nullptr) {
        response += QByteArray("Content-Type: ") + contentType + "\r\n";
    }
    response += "Content-Length: " + QByteArray::number(body.size()) +
                "\r\n\r\n";
    response += body;
    socket->write(response);
    buffers_.remove(socket);
    socket->disconnectFromHost();
}

void AgentBridge::dropSocket(QTcpSocket *socket) {
    buffers_.remove(socket);
    socket->abort();
}
