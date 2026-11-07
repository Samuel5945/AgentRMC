#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

// 第 3 批定向派发路由：把指令派回待办绑定的那条会话，并拿「确实到了」的回执。
// Qoder 系走 CLI --resume（headless、不抢前台，2026-11-07 实测回执与身份）；
// 其它宿主本轮不接：返回拒绝而不是降级瞎发（裁决 6：证明不了目标就不发）。
class SessionDispatch final : public QObject {
    Q_OBJECT

public:
    explicit SessionDispatch(QObject *parent = nullptr);

    static QString configHomeFor(const QString &agentExe);
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

signals:
    void receipt(const QString &dispatchId, const QString &agentExe,
                 const QString &sessionId, const QString &project,
                 const QString &text);
    void failed(const QString &dispatchId, const QString &reason);

private:
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
    };

    void poll();

    QTimer *timer_ = nullptr;
    QHash<QString, Pending> pending_;  // key = sessionId（同会话串行）
    // UI 通道依赖前台焦点：全局串行，一次只跑一条。
    class UiDispatch *ui_ = nullptr;
    bool uiBusy_ = false;
};
