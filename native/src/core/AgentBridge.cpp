#include "AgentBridge.h"

#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

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

bool AgentBridge::start(quint16 port) {
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
    const bool isNotify = requestLine.startsWith("POST /notify ");
    const bool isSpeak = requestLine.startsWith("POST /speak ");
    if (!isNotify && !isSpeak) {
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
    QString kind;
    QString message;
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
    }
    const QString title = obj.value(QStringLiteral("title")).toString()
                                  .isEmpty()
                              ? QStringLiteral("ZCode")
                              : obj.value(QStringLiteral("title")).toString();
    finish(socket, "204 No Content");
    emit notify(kind, title, message);
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
