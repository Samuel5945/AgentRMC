#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

class ZcodeDispatch;

// 第 3 批定向派发路由：把指令派回待办绑定的那条会话，并拿「确实到了」的回执。
// WorkBuddy 系走桌面可见 UIA 通道；DSH 走插件进程内通道（native 下发 → DSH 插件
// 轮询取件 → agent.followup() 注入 → ack 回执）；ZCode 走自带 app-server 协议
// 通道（spawn → resume → send，见 ZcodeDispatch）；Qoder 系 CLI --resume 已被
// 用户否决（桌面不可见），configHomeFor 返回空＝通道休眠。证明不了归属就不发，
// 绝不降级瞎发（裁决 6）。
class SessionDispatch final : public QObject {
    Q_OBJECT

public:
    explicit SessionDispatch(QObject *parent = nullptr);

    static QString configHomeFor(const QString &agentExe);
    // DSH 的插件通道：会话库是 zstd 多帧压缩 native 读不了，归属守卫在插件侧
    // （活表 agents.get 命中才注入，拒绝走 ack 带原因）。
    static bool supportsPluginChannel(const QString &agentExe);
    // ZCode 通道：自带 app-server 协议（个人供应商模型会话可用；账号套餐模型
    // 会话在 dispatch() 里拒绝，等上游开放）。
    static bool supportsZcodeChannel(const QString &agentExe);
    static QString cliPathFor(const QString &configHome);
    // jsonl 一行里取用户正文（content 可能是字符串或 [{type:text}]）。
    static bool userTextOf(const QByteArray &line, QString *text);
    // 目标会话必须真的在该宿主会话库里且记录自报同一个 id，才允许派发。
    // 会话最后一条 assistant 正文（剥包装、压空白、截断），给待办卡/事件流
    // 展示回复内容；没有回复返回空串。
    static QString lastReply(const QString &configHome, const QString &sessionId,
                             int maxChars = 60);
    static bool findSession(const QString &configHome, const QString &sessionId,
                            QString *path, QString *cwd,
                            QString *title = nullptr,
                            QString *altTitle = nullptr);

    // 接受返回 true；拒绝返回 false 并给原因（调用方保留草稿）。
    bool dispatch(const QString &agentExe, const QString &sessionId,
                  const QString &project, const QString &text, QString *reason,
                  QString *dispatchIdOut = nullptr);

    // —— DSH 插件通道（AgentBridge 转发）——
    // 插件轮询：取走该宿主尚未下发的派发（Pending 标记 handed，等 ack/超时）。
    QJsonArray takePluginDispatches(const QString &agentExe);
    // 插件应答：ok=true = followup 已被接受（durable inbox 落账，投递成立 →
    // receipt）；false = 插件侧守卫拒绝（reason 给用户 → failed）。
    void pluginAck(const QString &dispatchId, bool ok, const QString &error);

signals:
    void receipt(const QString &dispatchId, const QString &agentExe,
                 const QString &sessionId, const QString &project,
                 const QString &text);
    void failed(const QString &dispatchId, const QString &reason);
    // ZCode：turn 跑完（回执只代表送达）。reply 为该轮 assistant 正文，空＝
    // 没等到（超时/turn 失败）。
    void zcodeTurnFinished(const QString &dispatchId, const QString &agentExe,
                           const QString &sessionId, const QString &project,
                           const QString &reply);

private:
    enum class Channel { Ui, Cli, Plugin, Zcode };

    struct Pending {
        QString dispatchId;
        QString agentExe;
        QString sessionId;
        QString project;
        QString text;
        QString path;
        qint64 offset = 0;
        QElapsedTimer clock;
        QProcess *process = nullptr;
        Channel channel = Channel::Cli;
        bool pluginHanded = false;  // Plugin 通道：已下发给插件，等 ack
    };

    void poll();

    QTimer *timer_ = nullptr;
    QHash<QString, Pending> pending_;  // key = sessionId（同会话串行）
    // UI 通道依赖前台焦点：全局串行，一次只跑一条。
    class UiDispatch *ui_ = nullptr;
    bool uiBusy_ = false;
    // ZCode 通道不依赖焦点：每条派发一个独立 app-server 连接，允许并行。
    QList<ZcodeDispatch *> zcodeJobs_;
};
