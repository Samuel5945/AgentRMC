#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QString>

class QPainter;
class QTimer;
class QWidget;

// Agent 专用指针「画」的那一半：每块屏一个置顶透明窗口，指针 / 署名标签 / 点击
// 涟漪全画在上面。三条硬约束，缺一条它就退化成「又一个抢鼠标的东西」：
//   1. 输入穿透：点它 = 点它下面的窗口；
//   2. 永不激活：不抢焦点、不进 Alt+Tab；
//   3. 只画不动真光标：SetCursorPos / SendInput 一次都不调。
// 坐标对外一律物理像素（与 POST /cursor/* 同一套数），逐屏 DPI 换算在内部做。
class CursorOverlay final : public QObject {
    Q_OBJECT

public:
    explicit CursorOverlay(QObject *parent = nullptr);
    ~CursorOverlay() override;

    // 把指针挪到物理像素点。holdMs>0：这么久没有新动作就自动淡出（0 = 一直留着）。
    void moveTo(const QPoint &physical, int durationMs, const QString &agent,
                int holdMs = 0);
    // 点击的视觉反馈：以当前指针位置为中心扩散涟漪。
    void pulse(const QString &button);
    // 异步点击的回执：调用方点完立刻拿到 200，真实结果要回 /cursor/state 查。
    // 不记这一笔，「点了但没生效」在接口上就永远看不见。
    void reportClick(bool ok, const QString &backend, bool verified,
                     const QString &target, const QString &error);
    void hide(int fadeMs = 260);
    void show();
    bool visible() const { return visible_; }
    QJsonObject stateJson() const;
    // 显示器热插拔后重建各屏窗口；幂等，随时可调。
    void rebuild();

private:
    void ensureLayers();
    void tick();
    void paintLayer(QPainter *painter, const QPoint &layerOrigin);
    // 物理 → 全局逻辑坐标；找不到落在哪块屏返回 false。
    bool toLogical(const QPoint &physical, QPoint *logical) const;

    QList<QWidget *> layers_;
    QTimer *timer_ = nullptr;
    QTimer *holdTimer_ = nullptr;
    bool visible_ = false;
    qreal opacity_ = 0.0;
    bool fadingOut_ = false;
    qint64 fadeStartedAt_ = 0;
    int fadeDurationMs_ = 0;
    qreal opacityAtFadeStart_ = 0.0;
    QPoint from_;     // 缓动起点（全局逻辑坐标）
    QPoint current_;  // 当前（全局逻辑坐标）
    QPoint target_;
    qint64 moveStartedAt_ = 0;
    int moveDurationMs_ = 240;
    QString agent_;
    QString color_;
    bool pulsing_ = false;
    qint64 pulseStartedAt_ = 0;
    QString pulseButton_;
    int pulseCount_ = 0;
    int holdMs_ = 0;
    // 最近一次点击的回执（异步点击的结果只能靠这里读回）。
    bool lastClickOk_ = false;
    QString lastClickBackend_;
    bool lastClickVerified_ = false;
    QString lastClickTarget_;
    QString lastClickError_;
    qint64 lastClickAt_ = 0;
};
