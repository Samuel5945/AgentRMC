#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QTcpServer;
class QTcpSocket;

// 本机 Agent（ZCode / Qoder 等）与桌宠的交互口：监听 127.0.0.1:45915——
//   POST /notify  {"event":"stop|permission|...","title":...,"message":...,
//                  "speech":"Qoder 等你确认","raise":"ZCode.exe",
//                  "session_id":"...","project":"...","event_id":"...",
//                  "plugin_version":"0.3.7"}
//                 → notify()：桌宠冒气泡（不出声），收起时走托盘；
//                   raise 给定时把该进程映像名的窗口拉到前台（谁发的就拉谁）
//   POST /speak   {"message":"..."} → notify(kind="speak")：气泡 + TTS 朗读
//   GET  /state   → 200 + 桌宠状态 JSON（pet_visible/listening/tts_ready/...）
//   GET  /work-items → 注意力队列 JSON（attention_count / items）
//   POST /work-items/ack {"id":"..."} → 标记一条待办已处理
//   POST /work-items/update {"stage":"progress|blocked|ready","message":"...",
//                  "title":"WorkBuddy AI","raise":"WorkBuddyAI.exe",
//                  "session_id":"...","project":"..."}
//                 → 宿主 Agent 主动汇报阶段（MCP 工具 pet_work_update）。
//                   progress 只落工单；blocked / ready 走提醒通道（气泡 + 语音，
//                   要人动手的事必须喊到），同一件事 hook 的 stop 不会再念一遍。
//   POST /command {"action":"音量调到 50","confirm":false}
//                 → 系统控制（音量/亮度/媒体/电源），与语音命令同一套实现。
//                   200 执行成功；400 说法不认识；403 开关关着；
//                   409 危险动作（关机/重启/睡眠/休眠）缺 confirm:true。
//   GET  /workers → 当前在跑的 Worker 列表（alive / foreground / 待办计数）
//   GET  /health  → 桥接 / 待补投 / Worker / 最近事件健康快照
//   GET  /dispatch/pending?exe=<映像名> → DSH 定向派发取件：待注入清单 JSON 数组
//   POST /dispatch/ack {"dispatch_id":..,"ok":true|false,"error":".."}
//                 → 插件注入回执：ok=投递成立（durable inbox），false=守卫拒绝
// 纯回环、只此一用；端口被占就放弃监听，不影响主功能。
class AgentBridge final : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kPort = 45915;

    explicit AgentBridge(QObject *parent = nullptr);
    // 起监听；失败（端口被占等）返回 false。
    bool start(quint16 port = kPort);
    bool isRunning() const { return server_ != nullptr; }
    // 实际监听地址：桥必须是回环（无鉴权本机 IPC）。lanweb_test 拿它锁死
    // 「别顺手改成 Any」——暴露到局域网的是 LanWebServer，不是这个口。
    QHostAddress serverAddress() const;
    // GET /state 的应答来源（主窗接到 VoicePet::stateJson 上）；缺省回 "{}"。
    void setStateProvider(std::function<QString()> provider);
    // GET /work-items 的应答来源；缺省回空队列。
    void setWorkItemsProvider(std::function<QString()> provider);
    // POST /work-items/ack 的处理器；返回 false 表示 id 不存在。
    void setWorkItemAckHandler(std::function<bool(const QString &)> handler);
    // POST /work-items/update 的处理器；返回给 MCP 工具看的 JSON
    // （{"ok":true,"id":...,"stage":...} 或 {"ok":false,"error":...}）。
    void setWorkItemUpdateHandler(
        std::function<QJsonObject(const QJsonObject &)> handler);
    // POST /command 的处理器；返回 {"ok":bool,"code":200|400|403|409,...}，
    // code 直接当 HTTP 状态用（危险命令缺确认 = 409，开关关着 = 403）。
    void setCommandHandler(
        std::function<QJsonObject(const QJsonObject &)> handler);
    // GET /workers 的应答来源。
    void setWorkersProvider(std::function<QString()> provider);
    // GET /health 的应答来源。
    void setHealthProvider(std::function<QString()> provider);
    // GET /dispatch/pending?exe=<映像名> 的应答来源：DSH 插件轮询取件，
    // 返回 JSON 数组文本（可空数组）。
    void setDispatchPendingProvider(
        std::function<QString(const QString &exe)> provider);
    // POST /dispatch/ack 的处理器：{"dispatch_id":..,"ok":true|false,"error":".."}
    // （插件注入成功 / 守卫拒绝的回执）。返回 false = dispatch_id 不认识。
    void setDispatchAckHandler(
        std::function<bool(const QJsonObject &)> handler);

signals:
    // kind：/notify 透传 event 字段；/speak 固定为 "speak"。
    // raise：/notify 的可选进程映像名（"/speak" 恒为空，念话不抢前台）。
    // sessionId / project / eventId：可选，给 Work Item 去重和分组用。
    // pluginVersion：发这条通知的 agentrmc-bridge 是哪一版（hook 自己读包里的
    //   VERSION）。空 = 宿主没带（旧插件）。它是唯一能证明「哪一版代码真的在跑」
    //   的读数——注册表只说明盘上装成哪版，宿主不重启还是旧代码。
    // speech：只给语音念的短文案（带宿主名，不含工具名/路径）。空就退回念 message。
    //   气泡和工单仍用 message——「等你确认 Bash：D:\…」看着有用，念出来是噪音。
    void notify(const QString &kind, const QString &title,
                const QString &message, const QString &raise,
                const QString &sessionId, const QString &project,
                const QString &eventId, const QString &pluginVersion,
                const QString &speech);

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
    std::function<QJsonObject(const QJsonObject &)> workItemUpdateHandler_;
    std::function<QJsonObject(const QJsonObject &)> commandHandler_;
    std::function<QString()> workersProvider_;
    std::function<QString()> healthProvider_;
    std::function<QString(const QString &exe)> dispatchPendingProvider_;
    std::function<bool(const QJsonObject &)> dispatchAckHandler_;
};
