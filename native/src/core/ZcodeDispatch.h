#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

// ZCode 桌面（Electron）定向派回通道：spawn 自带 app-server（ZCode.exe
// resources/glm/zcode.cjs app-server --stdio，ELECTRON_RUN_AS_NODE=1），走 ndjson
// 双向协议：resume → send（modelSelection 带该会话当前值，from==to 零切换）→
// 确认落库（sent＝回执）→ 保活轮 assistant 正文（turnFinished，给待办卡上回复）。
// 发完必须确认落库再收尾——落库是异步的，提前 kill 消息就丢了（2026-11-08 实测）。
// 对话区可见性：进程内驻留不热刷新，重开 ZCode 全量可见；UX 以卡片为中心。
// 配方来源：rc_probe/zcode_appserver_probe.py / zcode_dispatch_probe.py 与
// docs/mission-control-plan.md 的 ZCode 系实测节。
class ZcodeDispatch final : public QObject {
    Q_OBJECT

public:
    struct Job {
        QString dispatchId;
        QString sessionId;
        QString text;
        QJsonObject modelSelection;  // 会话当前模型（rollout model_io 读出）
        QString zcodeExe;            // ZCode.exe 完整路径
    };

    explicit ZcodeDispatch(QObject *parent = nullptr);
    ~ZcodeDispatch() override;

    static bool supports(const QString &exe);
    // 会话库根：默认 ~/.zcode，AGENTRMC_ZCODE_HOME 可覆盖（测试/便携）。
    static QString zcodeHome();
    // 桌面端 exe：AGENTRMC_ZCODE_EXE 覆盖优先，否则解析 HKCR\zcode 协议命令。
    static QString zcodeExeFromRegistry();
    static QString cjsPathFor(const QString &zcodeExe);
    // 会话索引（共享 SQLite，只读）：session 行存在且未删除＝归属证明；
    // session_entry 的 runtime/model_selection＝该会话当前模型；time_updated
    // 给活动门。rollout jsonl 只有开了完整保留的会话才有，不能当索引。
    static bool sessionRecord(const QString &sessionId, QString *directory,
                              QJsonObject *modelSelection,
                              qint64 *timeUpdatedMs);

    void start(const Job &job);
    bool busy() const { return process_ != nullptr; }

signals:
    void sent(const QString &dispatchId);  // 消息已确认落库＝投递回执
    void failed(const QString &dispatchId, const QString &reason);
    void turnFinished(const QString &dispatchId, const QString &reply);

private:
    enum class Stage { Spawn, Ready, Resume, Send, Persist, Turn };
    void onReadyRead();
    void handleLine(const QByteArray &line);
    void request(const QString &method, const QJsonObject &params);
    void evaluate(const QJsonObject &response);
    void tick();
    void fail(const QString &reason);
    void finishTurn(const QString &reply);
    void cleanup();
    static QString textOfMessage(const QJsonObject &message);

    QProcess *process_ = nullptr;
    QTimer *timer_ = nullptr;
    Stage stage_ = Stage::Spawn;
    Job job_;
    QByteArray buffer_;
    int nextId_ = 1;
    int awaitingId_ = -1;
    QElapsedTimer stageClock_;
    QElapsedTimer pollClock_;
    bool sent_ = false;
    bool done_ = false;
};

namespace ZcodeGuard {
// 会话守卫用的常量挂在命名空间里，SessionDispatch 与测试共用。
constexpr qint64 kActivityGuardMs = 10000;
}
