#include "ToggleSwitch.h"

#include "core/ThemeManager.h"

#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace {

// 自绘控件吃主题 token：写死深色盘的值在浅色模式/美术主题里会脱节。
QColor token(const char *name, const QColor &fallback) {
    const QColor color =
        ThemeManager::instance().color(QString::fromLatin1(name));
    return color.isValid() ? color : fallback;
}

}  // namespace

ToggleSwitch::ToggleSwitch(bool checked, QWidget *parent)
    : QWidget(parent), checked_(checked) {
    setFixedSize(36, 21);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    // 换主题/切浅色时重绘（轨道和强调色都来自 token）。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &) { update(); });
}

void ToggleSwitch::setChecked(bool on) {
    on = bool(on);
    if (on == checked_) {
        return;
    }
    checked_ = on;
    update();
}

void ToggleSwitch::flip() {
    checked_ = !checked_;
    update();
    emit toggled(checked_);
}

void ToggleSwitch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient track(0, 0, width(), 0);
    if (checked_) {
        track.setColorAt(0, token("accent", QColor("#4f8cff")));
        track.setColorAt(1, token("accent.2", QColor("#7c5cff")));
    } else {
        // 关态轨道用主题的表面透明阶梯：浅色盘里是深灰透明，深色盘里是白色透明。
        // w24/w30 与旧深色轨道亮度接近，悬停再亮一档。
        const QColor base = token(hover_ ? "w30" : "w24",
                                  QColor(255, 255, 255, hover_ ? 30 : 24));
        track.setColorAt(0, base);
        track.setColorAt(1, base);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(QRectF(0, 1.5, 36, 18), 9, 9);

    const qreal knobX = checked_ ? 27 : 9;
    // 投影 + 白钮：浅色底上白钮和浅轨道靠这层影分开（和 ThemeSlider 同手法）。
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawEllipse(QPointF(knobX, 11.2), 7.5, 7.5);
    p.setBrush(QColor("#ffffff"));
    p.setPen(QPen(token("w40", QColor(255, 255, 255, 40)), 1.0));
    p.drawEllipse(QPointF(knobX, 10.5), 7.5, 7.5);
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint())) {
        flip();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ToggleSwitch::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
        event->key() == Qt::Key_Enter) {
        flip();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ToggleSwitch::enterEvent(QEnterEvent *event) {
    hover_ = true;
    update();
    QWidget::enterEvent(event);
}

void ToggleSwitch::leaveEvent(QEvent *event) {
    hover_ = false;
    update();
    QWidget::leaveEvent(event);
}
