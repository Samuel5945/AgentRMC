#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <functional>

class ZcodeDispatch;

// 第 3 批定向派发路由：把指令派回待办绑定的那条会话，并拿「确实到了」的回执。
// WorkBuddy 系走桌面可见 UIA 通道；DSH 走插件进程内通道（native 下发 → DSH 插件
// 轮询取件 → agent.followup() 注入 → ack 回执）；OpenSquilla 走同款插件通道
// （pet-bridge 取件 → 网关 sessions.send 注入 → ack 回执）；ZCode 走自带
// app-server 协议通道（spawn → resume → send，见 ZcodeDispatch）；Qoder 系
// CLI --resume 已被用户否决（桌面不可见），configHomeFor 返回空＝通道休眠。
// 证明不了归属就不发，绝不降级瞎发（裁决 6）。
class SessionDispatch final : public QObject {
    Q_OBJECT

public:
    explicit SessionDispatch(QObject *parent = nullptr);

    static QString configHomeFor(const QString &agentExe);
    // 桥插件取件通道的派发范围（DSH / OpenSquilla）：dispatch() 里还要过会话
    // id 格式哨兵。ZCode 不在这条——派发走 supportsZcodeChannel（自带
    // app-server 协议）；它的卡片代答范围在 supportsInlineAnswer。历史上
    // （35da13a）曾把 ZCode.exe 加进这条，插件分支在前把 ZCode 派发拦在
    // DSH 哨兵上，2026-11-18 修复。
    static bool supportsPluginChannel(const QString &agentExe);
    // 内联应答（卡片上「允许/拒绝/选项」点了能送进宿主）的宿主范围。
    // 与 supportsPluginChannel 分开：那条判的是「能不能派发新消息」，还要过
    // 会话 id 格式哨兵；应答链路只靠 1.2s 轮询的应答件 + 会话 id 对号，
    // WorkBuddy / ZCode 这类只有 hook 代答、没有 injection 通道的宿主只该进
    // 这一条。
    static bool supportsInlineAnswer(const QString &agentExe);
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
    // modelOverride：ZCode 通道可选的模型覆盖 {providerId, modelId}——账号
    // 套餐模型的会话必须带（外部实例注册表拿不到 account，2026-11-08 裁决
    // 不破解凭据），个人供应商会话不带＝用会话当前模型（零切换）。override
    // 由 ZcodeDispatch::personalModelAllowed 校验；reasoningLevel 保留会话
    // 原值，send 会把 override 写进会话库，turn 终态后恢复原模型。
    bool dispatch(const QString &agentExe, const QString &sessionId,
                  const QString &project, const QString &text, QString *reason,
                  QString *dispatchIdOut = nullptr,
                  const QJsonObject &modelOverride = QJsonObject());

    // —— 插件通道（DSH / OpenSquilla，AgentBridge 转发）——
    // 桥/插件轮询：取走该宿主（按 exe 过滤）尚未下发的派发（Pending 标记
    // handed，等 ack/超时）。
    QJsonArray takePluginDispatches(const QString &agentExe);
    // 插件应答：ok=true = followup 已被接受（durable inbox 落账，投递成立 →
    // receipt）；false = 插件侧守卫拒绝（reason 给用户 → failed）。
    // 返回 false = 不认识的 dispatch_id（重启丢账/重复 ack）：调用方要回 404，
    // 别把无效回执当成功。
    bool pluginAck(const QString &dispatchId, bool ok, const QString &error);

    // OpenSquilla 可选模型目录读数（MainWindow 把桥 POST /models 的账接进来）：
    // dispatch() 选模型时拿它预检，目录没推来就不拦（桥取件后自己反查拒）。
    void setOsqModelsProvider(std::function<QJsonArray()> provider) {
        osqModelsProvider_ = std::move(provider);
    }

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
    // ZCode：派发轮遇到授权/提问，外部通道已代为拒绝（what＝授权请求/提问）。
    void zcodeInteractionSkipped(const QString &dispatchId,
                                 const QString &agentExe,
                                 const QString &sessionId,
                                 const QString &project,
                                 const QString &what);

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
        // 插件通道（OpenSquilla）的选模型 override：{providerId?,modelId}，
        // 取件时原样带给桥，由桥拿网关目录反查后 sessions.patch。空对象=不切。
        QJsonObject modelOverride;
    };

    void poll();

    QTimer *timer_ = nullptr;
    std::function<QJsonArray()> osqModelsProvider_;
    QHash<QString, Pending> pending_;  // key = sessionId（同会话串行）
    // UI 通道依赖前台焦点：全局串行，一次只跑一条。
    class UiDispatch *ui_ = nullptr;
    bool uiBusy_ = false;
    // ZCode 通道不依赖焦点：每条派发一个独立 app-server 连接，允许并行。
    QList<ZcodeDispatch *> zcodeJobs_;
};
