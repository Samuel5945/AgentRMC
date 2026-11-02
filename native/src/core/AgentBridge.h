#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class QTcpServer;
class QTcpSocket;

// 本机 Agent（ZCode / Qoder 等）与桌宠的交互口：监听 127.0.0.1:45915——
//   POST /notify  {"event":"stop|permission|...","title":...,"message":...,
//                  "raise":"ZCode.exe","session_id":"...","project":"...",
//                  "event_id":"..."}
//                 → notify()：桌宠冒气泡（不出声），收起时走托盘；
//                   raise 给定时把该进程映像名的窗口拉到前台（谁发的就拉谁）
//   POST /speak   {"message":"..."} → notify(kind="speak")：气泡 + TTS 朗读
//   GET  /state   → 200 + 桌宠状态 JSON（pet_visible/listening/tts_ready/...）
//   GET  /work-items → 注意力队列 JSON（attention_count / items）
//   POST /work-items/ack {"id":"..."} → 标记一条待办已处理
//   GET  /workers → 当前在跑的 Worker 列表（alive / foreground / 待办计数）
//   GET  /health  → 桥接 / 待补投 / Worker / 最近事件健康快照
// 纯回环、只此一用；端口被占就放弃监听，不影响主功能。
class AgentBridge final : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kPort = 45915;

    explicit AgentBridge(QObject *parent = nullptr);
    // 起监听；失败（端口被占等）返回 false。
    bool start(quint16 port = kPort);
    bool isRunning() const { return server_ != nullptr; }
    // GET /state 的应答来源（主窗接到 VoicePet::stateJson 上）；缺省回 "{}"。
    void setStateProvider(std::function<QString()> provider);
    // GET /work-items 的应答来源；缺省回空队列。
    void setWorkItemsProvider(std::function<QString()> provider);
    // POST /work-items/ack 的处理器；返回 false 表示 id 不存在。
    void setWorkItemAckHandler(std::function<bool(const QString &)> handler);
    // GET /workers 的应答来源。
    void setWorkersProvider(std::function<QString()> provider);
    // GET /health 的应答来源。
    void setHealthProvider(std::function<QString()> provider);

signals:
    // kind：/notify 透传 event 字段；/speak 固定为 "speak"。
    // raise：/notify 的可选进程映像名（"/speak" 恒为空，念话不抢前台）。
    // sessionId / project / eventId：可选，给 Work Item 去重和分组用。
    void notify(const QString &kind, const QString &title,
                const QString &message, const QString &raise,
                const QString &sessionId, const QString &project,
                const QString &eventId);

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
    std::function<QString()> workItemsProvider_;
    std::function<bool(const QString &)> workItemAckHandler_;
    std::function<QString()> workersProvider_;
    std::function<QString()> healthProvider_;
};
