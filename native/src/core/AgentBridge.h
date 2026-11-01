#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class QTcpServer;
class QTcpSocket;

// 本机 Agent（ZCode 等）与桌宠的交互口：监听 127.0.0.1:45915——
//   POST /notify  {"event":"stop|permission|...","title":...,"message":...}
//                 → notify()：桌宠冒气泡（不出声），收起时走托盘
//   POST /speak   {"message":"..."} → notify(kind="speak")：气泡 + TTS 朗读
//   GET  /state   → 200 + 桌宠状态 JSON（pet_visible/listening/tts_ready/...）
// 纯回环、只此一用；端口被占就放弃监听，不影响主功能。
class AgentBridge final : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kPort = 45915;

    explicit AgentBridge(QObject *parent = nullptr);
    // 起监听；失败（端口被占等）返回 false。
    bool start(quint16 port = kPort);
    // GET /state 的应答来源（主窗接到 VoicePet::stateJson 上）；缺省回 "{}"。
    void setStateProvider(std::function<QString()> provider);

signals:
    // kind：/notify 透传 event 字段；/speak 固定为 "speak"。
    void notify(const QString &kind, const QString &title,
                const QString &message);

private:
    void onConnected();
    void onReadable(QTcpSocket *socket);
    // 回一条响应并关连接；带 body 时给 Content-Type。
    void finish(QTcpSocket *socket, const char *status,
                const QByteArray &body = QByteArray(),
                const char *contentType = nullptr);
    void dropSocket(QTcpSocket *socket);

    QTcpServer *server_ = nullptr;
    QHash<QTcpSocket *, QByteArray> buffers_;
    std::function<QString()> stateProvider_;
};
