#pragma once

#include <QObject>
#include <QString>

// WorkBuddy 系桌面可见派回通道（UIA）：唤醒主窗 → 滚顶/展开分组 → 按标题点
// 会话行 → 聊天区标题读回证明归属 → 粘贴读回 → Enter。全部步骤异步走主线程
// 事件循环，不阻塞 UI；证明不了归属就 proofFailed，绝不降级瞎发（裁决 6）。
// 机制来自 2026-11-07 实测探针 rc_probe/wb_open_session.py / wb_send_receipt.py。
class UiDispatch final : public QObject {
    Q_OBJECT

public:
    struct Job {
        QString dispatchId;
        QString exe;
        QString sessionId;
        QString title;      // 目标会话标题（ai-title 或首句），用于点行+读回证明
        QString altTitle;   // 首句备选：聊天区顶部有时渲染首句气泡而非标题
        QString groupHint;  // 侧栏分组名（项目目录名；无项目会话为「任务」）
        QString text;
    };

    explicit UiDispatch(QObject *parent = nullptr);
    static bool supports(const QString &exe);
    // 侧栏行名可能带时间后缀或被截断：清后缀 + 前 12 字双向匹配。
    static QString cleanRowName(const QString &name);
    static bool rowMatches(const QString &rowName, const QString &title);

    void start(const Job &job);
    bool busy() const { return busy_; }

signals:
    void sent(const QString &dispatchId);
    void proofFailed(const QString &dispatchId, const QString &reason);
    void failed(const QString &dispatchId, const QString &reason);

private:
    void step();
    void finishFail(const QString &reason, bool proof);
    void restoreClipboard();

    Job job_;
    bool busy_ = false;
    int stage_ = 0;
    int tries_ = 0;
    bool scrolled_ = false;
    int expandTries_ = 0;
    QString savedClipboard_;
    bool hasSavedClipboard_ = false;
    void *hwnd_ = nullptr;
};
