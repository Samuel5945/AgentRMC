#include "ToggleSwitch.h"

#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

ToggleSwitch::ToggleSwitch(bool checked, QWidget *parent)
    : QWidget(parent), checked_(checked) {
    setFixedSize(36, 21);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
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
        track.setColorAt(0, QColor("#4f8cff"));
        track.setColorAt(1, QColor("#7c5cff"));
    } else {
        track.setColorAt(0, QColor(hover_ ? "#313b4a" : "#2a3240"));
        track.setColorAt(1, QColor(hover_ ? "#2b3442" : "#232b37"));
    }
    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(QRectF(0, 1.5, 36, 18), 9, 9);

    const qreal knobX = checked_ ? 27 : 9;
    p.setBrush(QColor("#ffffff"));
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
