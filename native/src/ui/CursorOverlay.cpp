#include "ui/CursorOverlay.h"

#include "core/AgentCursor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QDateTime>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QRadialGradient>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <functional>

namespace {

constexpr int kFrameMs = 16;
constexpr int kFadeInMs = 140;
constexpr int kPulseMs = 520;
constexpr int kPulseRings = 3;
constexpr int kMaxDurationMs = 4000;

// 一块屏的「物理矩形 ↔ Qt 逻辑矩形」对照。Qt 只报逻辑坐标，而 Agent 给的坐标
// 来自截图 / GetCursorPos，是物理像素——125%、150% 缩放的屏上，这两套数差着
// 一个 DPR，不换算指针就会画到别的地方去。
struct MonitorMap {
    QRect physical;
    QRect logical;
    qreal dpr = 1.0;
};

struct MonitorEnumCtx {
    QList<QRect> physical;
    QStringList devices;
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto *ctx = reinterpret_cast<MonitorEnumCtx *>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        ctx->physical.append(QRect(info.rcMonitor.left, info.rcMonitor.top,
                                   info.rcMonitor.right - info.rcMonitor.left,
                                   info.rcMonitor.bottom - info.rcMonitor.top));
        ctx->devices.append(QString::fromWCharArray(info.szDevice));
    }
    return TRUE;
}

QList<MonitorMap> monitorMaps() {
    MonitorEnumCtx ctx;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor,
                        reinterpret_cast<LPARAM>(&ctx));
    QList<MonitorMap> maps;
    for (QScreen *screen : QGuiApplication::screens()) {
        MonitorMap map;
        map.logical = screen->geometry();
        map.dpr = screen->devicePixelRatio() > 0.0 ? screen->devicePixelRatio()
                                                   : 1.0;
        // 按设备名对齐（"\\.\DISPLAY1"）：只有它能保证「物理矩形」和「Qt 逻辑
        // 矩形」说的是同一块屏，而不是按枚举顺序碰巧排到一起。
        const int index = ctx.devices.indexOf(screen->name());
        if (index >= 0) {
            map.physical = ctx.physical.at(index);
        } else {
            map.physical = QRect(qRound(map.logical.x() * map.dpr),
                                 qRound(map.logical.y() * map.dpr),
                                 qRound(map.logical.width() * map.dpr),
                                 qRound(map.logical.height() * map.dpr));
        }
        maps.append(map);
    }
    return maps;
}

const MonitorMap *monitorFor(const QList<MonitorMap> &maps,
                             const QPoint &physical) {
    for (const MonitorMap &map : maps) {
        if (map.physical.contains(physical)) {
            return &map;
        }
    }
    // 落在所有屏之外（虚拟桌面的空隙、或坐标本身是坏的）：取最近的一块屏照画，
    // 宁可画到边屏上，也不要静默什么都不显示让调用方以为成功了。
    const MonitorMap *best = nullptr;
    qint64 bestDistance = 0;
    for (const MonitorMap &map : maps) {
        const QPoint center = map.physical.center();
        const qint64 dx = qint64(center.x()) - physical.x();
        const qint64 dy = qint64(center.y()) - physical.y();
        const qint64 distance = dx * dx + dy * dy;
        if (best == nullptr || distance < bestDistance) {
            best = &map;
            bestDistance = distance;
        }
    }
    return best;
}

qreal easeOutCubic(qreal t) {
    const qreal inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

// 指针本体：特意不画成系统箭头的样子（免得用户以为自己的鼠标被抢了），是
// 「彩色箭头 + 准心环 + 光晕」，颜色跟着调它的那个 Agent 走。
void paintPointer(QPainter *painter, const QPointF &tip, const QColor &color,
                  const QString &label) {
    painter->save();

    QRadialGradient glow(tip, 26);
    QColor inner = color;
    inner.setAlphaF(0.30);
    QColor outer = color;
    outer.setAlphaF(0.0);
    glow.setColorAt(0.0, inner);
    glow.setColorAt(1.0, outer);
    painter->setPen(Qt::NoPen);
    painter->setBrush(glow);
    painter->drawEllipse(tip, 26.0, 26.0);

    QColor ring = color;
    ring.setAlphaF(0.8);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(ring, 1.6));
    painter->drawEllipse(tip, 13.5, 13.5);

    // 箭头热点在尖端 = 点击落点，和系统指针同一套几何直觉。
    QPolygonF arrow;
    arrow << QPointF(0, 0) << QPointF(0, 17) << QPointF(4.4, 12.9)
          << QPointF(7.4, 19.8) << QPointF(10.2, 18.4) << QPointF(7.2, 11.6)
          << QPointF(12.4, 11.6);
    painter->setPen(QPen(QColor(255, 255, 255, 235), 1.4, Qt::SolidLine,
                         Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(color);
    painter->drawPolygon(arrow.translated(tip));

    if (!label.isEmpty()) {
        QFont font = painter->font();
        font.setPixelSize(11);
        font.setBold(true);
        painter->setFont(font);
        const QFontMetricsF metrics(font);
        const QRectF box(tip.x() + 16.0, tip.y() + 7.0,
                         metrics.horizontalAdvance(label) + 13.0,
                         metrics.height() + 6.0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(12, 16, 24, 225));
        painter->drawRoundedRect(box, 6, 6);
        painter->setPen(color);
        painter->drawText(box, Qt::AlignCenter, label);
    }
    painter->restore();
}

// 点击反馈：三圈由内向外扩散，越外越淡。左键用 Agent 色，右键偏红——用户不用
// 看日志就知道 Agent 点的是哪个键。
void paintRipple(QPainter *painter, const QPointF &center, const QColor &color,
                 qreal progress) {
    painter->save();
    painter->setBrush(Qt::NoBrush);
    for (int i = 0; i < kPulseRings; ++i) {
        const qreal local = (progress - i * 0.14) / 0.62;
        if (local <= 0.0 || local >= 1.0) {
            continue;
        }
        QColor ring = color;
        ring.setAlphaF((1.0 - local) * 0.7);
        painter->setPen(QPen(ring, 2.0));
        const qreal radius = 9.0 + local * 42.0;
        painter->drawEllipse(center, radius, radius);
    }
    painter->restore();
}

// 每块屏一扇穿透窗口。没有 Q_OBJECT：它不需要信号槽，只需要一个重画回调——
// 少一个 moc 类，少一处和 AUTOMOC 打交道的理由。
class LayerWidget final : public QWidget {
public:
    using Painter = std::function<void(QPainter *, const QPoint &)>;

    LayerWidget(const QRect &logicalRect, Painter painter)
        : QWidget(nullptr), painter_(std::move(painter)) {
        setObjectName(QStringLiteral("agentCursorLayer"));
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                       Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                       Qt::NoDropShadowWindowHint |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(logicalRect);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter_(&painter, geometry().topLeft());
    }

private:
    Painter painter_;
};

}  // namespace

CursorOverlay::CursorOverlay(QObject *parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(kFrameMs);
    connect(timer_, &QTimer::timeout, this, &CursorOverlay::tick);

    holdTimer_ = new QTimer(this);
    holdTimer_->setSingleShot(true);
    connect(holdTimer_, &QTimer::timeout, this, [this] { hide(); });

    connect(qApp, &QGuiApplication::screenAdded, this,
            [this](QScreen *) { rebuild(); });
    connect(qApp, &QGuiApplication::screenRemoved, this,
            [this](QScreen *) { rebuild(); });
}

CursorOverlay::~CursorOverlay() {
    qDeleteAll(layers_);
    layers_.clear();
}

void CursorOverlay::ensureLayers() {
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (layers_.size() != screens.size()) {
        rebuild();
        return;
    }
    for (int i = 0; i < layers_.size(); ++i) {
        layers_.at(i)->setGeometry(screens.at(i)->geometry());
    }
}

void CursorOverlay::rebuild() {
    const bool wasVisible = visible_;
    for (QWidget *layer : layers_) {
        layer->hide();
        delete layer;
    }
    layers_.clear();
    for (QScreen *screen : QGuiApplication::screens()) {
        auto *layer = new LayerWidget(
            screen->geometry(), [this](QPainter *painter, const QPoint &origin) {
                paintLayer(painter, origin);
            });
        layers_.append(layer);
        if (wasVisible) {
            layer->show();
        }
    }
}

void CursorOverlay::moveTo(const QPoint &physical, int durationMs,
                           const QString &agent, int holdMs) {
    QPoint logical;
    if (!toLogical(physical, &logical)) {
        return;
    }
    if (!agent.trimmed().isEmpty()) {
        agent_ = agent.trimmed();
    }
    color_ = AgentCursor::agentColor(agent_);
    if (!visible_) {
        // 从目标位置淡入：不从上次停留的旧位置「飞」过来，那会让用户以为 Agent
        // 真的在屏幕上扫过一段。
        current_ = logical;
        opacity_ = 0.0;
    }
    from_ = current_;
    target_ = logical;
    moveStartedAt_ = QDateTime::currentMSecsSinceEpoch();
    moveDurationMs_ = qBound(0, durationMs, kMaxDurationMs);
    if (moveDurationMs_ == 0) {
        current_ = target_;
    }
    visible_ = true;
    holdMs_ = qMax(0, holdMs);
    if (holdMs_ > 0) {
        holdTimer_->start(holdMs_);
    } else {
        holdTimer_->stop();
    }
    ensureLayers();
    for (QWidget *layer : layers_) {
        if (!layer->isVisible()) {
            layer->show();
        }
    }
    if (!timer_->isActive()) {
        timer_->start();
    }
    tick();
}

void CursorOverlay::pulse(const QString &button) {
    if (!visible_) {
        return;  // 没有指针就没有涟漪
    }
    pulsing_ = true;
    pulseStartedAt_ = QDateTime::currentMSecsSinceEpoch();
    pulseButton_ = button;
    pulseCount_ += 1;
    if (!timer_->isActive()) {
        timer_->start();
    }
    tick();
}

void CursorOverlay::show() {
    if (visible_) {
        return;
    }
    visible_ = true;
    ensureLayers();
    for (QWidget *layer : layers_) {
        layer->show();
    }
    if (!timer_->isActive()) {
        timer_->start();
    }
}

void CursorOverlay::hide(int fadeMs) {
    holdTimer_->stop();
    if (!visible_) {
        return;
    }
    if (fadeMs <= 0) {
        visible_ = false;
        opacity_ = 0.0;
        pulsing_ = false;
        for (QWidget *layer : layers_) {
            layer->hide();
        }
        timer_->stop();
        return;
    }
    fadingOut_ = true;
    fadeStartedAt_ = QDateTime::currentMSecsSinceEpoch();
    fadeDurationMs_ = fadeMs;
    opacityAtFadeStart_ = opacity_;
    if (!timer_->isActive()) {
        timer_->start();
    }
}

void CursorOverlay::tick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool busy = false;

    if (moveDurationMs_ > 0) {
        const qreal t = qBound(0.0, qreal(now - moveStartedAt_) / moveDurationMs_,
                               1.0);
        const qreal eased = easeOutCubic(t);
        current_ = QPoint(
            from_.x() + qRound((target_.x() - from_.x()) * eased),
            from_.y() + qRound((target_.y() - from_.y()) * eased));
        if (t >= 1.0) {
            current_ = target_;
            moveDurationMs_ = 0;
        } else {
            busy = true;
        }
    }

    if (fadingOut_) {
        const qreal t = qBound(
            0.0, qreal(now - fadeStartedAt_) / qMax(1, fadeDurationMs_), 1.0);
        opacity_ = opacityAtFadeStart_ * (1.0 - t);
        if (t >= 1.0) {
            fadingOut_ = false;
            visible_ = false;
            opacity_ = 0.0;
            for (QWidget *layer : layers_) {
                layer->hide();
            }
        } else {
            busy = true;
        }
    } else if (visible_ && opacity_ < 1.0) {
        opacity_ = qMin(1.0, opacity_ + qreal(kFrameMs) / kFadeInMs);
        if (opacity_ < 1.0) {
            busy = true;
        }
    }

    if (pulsing_) {
        const qreal t = qreal(now - pulseStartedAt_) / kPulseMs;
        if (t >= 1.0) {
            pulsing_ = false;
        } else {
            busy = true;
        }
    }

    for (QWidget *layer : layers_) {
        layer->update();
    }
    if (!busy) {
        timer_->stop();
        for (QWidget *layer : layers_) {
            layer->update();  // 补最后一帧：把消散中的涟漪擦干净
        }
    }
}

void CursorOverlay::paintLayer(QPainter *painter, const QPoint &layerOrigin) {
    if (!visible_ || opacity_ <= 0.001) {
        return;
    }
    painter->setOpacity(opacity_);
    const QPointF tip(current_.x() - layerOrigin.x(),
                      current_.y() - layerOrigin.y());
    const QColor color(color_.isEmpty() ? QStringLiteral("#4da3ff") : color_);

    if (pulsing_) {
        const qreal t =
            qreal(QDateTime::currentMSecsSinceEpoch() - pulseStartedAt_) /
            kPulseMs;
        QColor pulseColor = color;
        if (pulseButton_ == QStringLiteral("right")) {
            pulseColor = QColor(255, 120, 120);
        } else if (pulseButton_ == QStringLiteral("middle")) {
            pulseColor = QColor(255, 214, 102);
        }
        paintRipple(painter, tip, pulseColor, qBound(0.0, t, 1.0));
    }
    paintPointer(painter, tip, color, agent_);
}

bool CursorOverlay::toLogical(const QPoint &physical, QPoint *logical) const {
    const QList<MonitorMap> maps = monitorMaps();
    if (maps.isEmpty()) {
        *logical = physical;  // 拿不到屏幕信息就照原样（单屏无缩放的常见情形）
        return true;
    }
    const MonitorMap *map = monitorFor(maps, physical);
    if (map == nullptr) {
        return false;
    }
    *logical = map->logical.topLeft() +
               QPoint(qRound((physical.x() - map->physical.x()) / map->dpr),
                      qRound((physical.y() - map->physical.y()) / map->dpr));
    return true;
}

void CursorOverlay::reportClick(bool ok, const QString &backend, bool verified,
                                const QString &target, const QString &error) {
    lastClickOk_ = ok;
    lastClickBackend_ = backend;
    lastClickVerified_ = verified;
    lastClickTarget_ = target;
    lastClickError_ = error;
    lastClickAt_ = QDateTime::currentMSecsSinceEpoch();
}

QJsonObject CursorOverlay::stateJson() const {
    QJsonObject out;
    out.insert(QStringLiteral("visible"), visible_);
    out.insert(QStringLiteral("agent"), agent_);
    out.insert(QStringLiteral("color"), color_);
    out.insert(QStringLiteral("opacity"), opacity_);
    out.insert(QStringLiteral("animating"), timer_ != nullptr && timer_->isActive());
    out.insert(QStringLiteral("clicks"), pulseCount_);
    // 异步点击的结果：调用方点完立刻拿到 200，真实结局回这里查。
    QJsonObject last;
    last.insert(QStringLiteral("ok"), lastClickOk_);
    last.insert(QStringLiteral("backend"), lastClickBackend_);
    last.insert(QStringLiteral("verified"), lastClickVerified_);
    last.insert(QStringLiteral("target"), lastClickTarget_);
    last.insert(QStringLiteral("error"), lastClickError_);
    last.insert(QStringLiteral("at_ms"), double(lastClickAt_));
    out.insert(QStringLiteral("last_click"), last);
    // 对外一律物理像素：调用方给的是物理像素，读回来也该是同一套数。
    const QList<MonitorMap> maps = monitorMaps();
    const MonitorMap *map = monitorFor(maps, QPoint(0, 0));
    QPoint physical = current_;
    for (const MonitorMap &candidate : maps) {
        if (candidate.logical.contains(current_)) {
            map = &candidate;
            break;
        }
    }
    if (map != nullptr && map->dpr > 0.0) {
        physical = QPoint(
            map->physical.x() + qRound((current_.x() - map->logical.x()) * map->dpr),
            map->physical.y() + qRound((current_.y() - map->logical.y()) * map->dpr));
    }
    out.insert(QStringLiteral("x"), physical.x());
    out.insert(QStringLiteral("y"), physical.y());
    return out;
}
