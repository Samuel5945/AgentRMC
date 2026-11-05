#include "ThemeSlider.h"

#include "core/ThemeManager.h"

#include <QEnterEvent>
#include <QFocusEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

namespace {

constexpr qreal kTrackH = 6.0;
constexpr qreal kHandleD = 16.0;
// 手柄中心到控件边缘的余量：半径 + 悬停光环，保证拖到两端也不越界。
constexpr qreal kInset = kHandleD / 2.0 + 5.0;
constexpr int kHeight = 30;

// 自绘控件也得吃主题 token：写死深色的话 classic 浅色下白球贴白底、
// 轨道和阴影全糊在一起。token 缺失才回落到原来的深色值。
QColor token(const char *name, const QColor &fallback) {
    const QColor color =
        ThemeManager::instance().color(QString::fromLatin1(name));
    return color.isValid() ? color : fallback;
}

}  // namespace

ThemeSlider::ThemeSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &) { update(); });
}

QRectF ThemeSlider::trackRect() const {
    return QRectF(kInset, (height() - kTrackH) / 2.0,
                  qMax(0.0, width() - 2.0 * kInset), kTrackH);
}

qreal ThemeSlider::handleCenterX() const {
    const QRectF track = trackRect();
    const int span = qMax(1, qRound(track.width()));
    const int offset = QStyle::sliderPositionFromValue(
        minimum(), maximum(), value(), span, invertedAppearance());
    return track.left() + offset;
}

int ThemeSlider::valueFromX(qreal x) const {
    const QRectF track = trackRect();
    const int span = qMax(1, qRound(track.width()));
    return QStyle::sliderValueFromPosition(minimum(), maximum(),
                                           qRound(x - track.left()), span,
                                           invertedAppearance());
}

void ThemeSlider::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF track = trackRect();
    const QColor accent = token("accent", QColor(0x4f, 0x8c, 0xff));
    const QColor accent2 = token("accent.2", QColor(0x7c, 0x5c, 0xff));

    // 轨道：整条淡色，完成段用主题渐变；圆角半径取轨道高度一半。
    p.setPen(Qt::NoPen);
    p.setBrush(token("w14", QColor(255, 255, 255, 14)));
    p.drawRoundedRect(track, kTrackH / 2.0, kTrackH / 2.0);

    const qreal centerX = handleCenterX();
    const qreal fillW = qBound(0.0, centerX - track.left(), track.width());
    if (fillW > 0.5) {
        QLinearGradient fill(track.left(), 0.0, track.right(), 0.0);
        fill.setColorAt(0.0, accent);
        fill.setColorAt(1.0, accent2);
        p.setBrush(fill);
        p.drawRoundedRect(
            QRectF(track.left(), track.top(), fillW, track.height()),
            kTrackH / 2.0, kTrackH / 2.0);
    }

    const QPointF center(centerX, height() / 2.0);
    const bool active = isSliderDown();

    // 悬停 / 拖动时给手柄一圈主题色光环，静态时只有很淡的一层。
    const qreal haloR =
        kHandleD / 2.0 + (active ? 5.0 : (hover_ ? 4.0 : 1.5));
    QColor halo = accent;
    halo.setAlpha(active ? 64 : (hover_ ? 44 : 18));
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawEllipse(center, haloR, haloR);

    // 投影：先画一层往下偏 1px 的暗圆，再盖上白球；浅色主题下白球不会
    // 和浅轨道黏成一片（深色底上这层几乎看不见，正好）。
    QColor shadow(0, 0, 0, 70);
    p.setBrush(shadow);
    p.drawEllipse(center + QPointF(0.0, 1.0), kHandleD / 2.0,
                  kHandleD / 2.0);

    p.setBrush(QColor(0xff, 0xff, 0xff));
    p.setPen(QPen(token("w40", QColor(255, 255, 255, 40)), 1.0));
    p.drawEllipse(center, kHandleD / 2.0 - 0.5, kHandleD / 2.0 - 0.5);

    // 键盘焦点：细一圈主题色，鼠标点过不留框（QSlider 默认是 StrongFocus）。
    if (keyboardFocus_) {
        QColor ring = accent;
        ring.setAlpha(150);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ring, 1.5));
        p.drawEllipse(center, kHandleD / 2.0 + 2.5,
                      kHandleD / 2.0 + 2.5);
    }
}

void ThemeSlider::mousePressEvent(QMouseEvent *event) {
    // 现代滑块的惯例：按哪儿跳哪儿，再按住拖。QSlider 默认是翻页，
    // 在 286px 宽的轨道上要连点好几下才到位。
    if (event->button() == Qt::LeftButton && isEnabled() &&
        maximum() > minimum()) {
        setSliderDown(true);
        setValue(valueFromX(event->position().x()));
        update();
        event->accept();
        return;
    }
    QSlider::mousePressEvent(event);
}

void ThemeSlider::mouseMoveEvent(QMouseEvent *event) {
    if (isSliderDown() && (event->buttons() & Qt::LeftButton)) {
        setValue(valueFromX(event->position().x()));
        update();
        event->accept();
        return;
    }
    QSlider::mouseMoveEvent(event);
}

void ThemeSlider::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && isSliderDown()) {
        setValue(valueFromX(event->position().x()));
        setSliderDown(false);
        update();
        event->accept();
        return;
    }
    QSlider::mouseReleaseEvent(event);
}

void ThemeSlider::enterEvent(QEnterEvent *event) {
    hover_ = true;
    update();
    QSlider::enterEvent(event);
}

void ThemeSlider::leaveEvent(QEvent *event) {
    hover_ = false;
    update();
    QSlider::leaveEvent(event);
}

void ThemeSlider::focusInEvent(QFocusEvent *event) {
    keyboardFocus_ = event->reason() != Qt::MouseFocusReason;
    update();
    QSlider::focusInEvent(event);
}

void ThemeSlider::focusOutEvent(QFocusEvent *event) {
    keyboardFocus_ = false;
    update();
    QSlider::focusOutEvent(event);
}
