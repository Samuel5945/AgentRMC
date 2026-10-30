#include "Toast.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QTimer>

Toast::Toast(QWidget *parent) : QLabel(parent) {
    setObjectName("toast");
    setAlignment(Qt::AlignCenter);
    setWordWrap(false);
    setMaximumWidth(900);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();

    effect_ = new QGraphicsOpacityEffect(this);
    effect_->setOpacity(1.0);
    setGraphicsEffect(effect_);

    fade_ = new QPropertyAnimation(effect_, "opacity", this);
    fade_->setDuration(220);
    fade_->setStartValue(1.0);
    fade_->setEndValue(0.0);
    connect(fade_, &QPropertyAnimation::finished, this, [this] {
        if (effect_->opacity() <= 0.01) {
            hide();
        }
    });

    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, this, [this] {
        fade_->stop();
        fade_->start();
    });

    if (parent != nullptr) {
        parent->installEventFilter(this);
    }
}

bool Toast::eventFilter(QObject *watched, QEvent *event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize &&
        isVisible()) {
        reposition();
    }
    return QLabel::eventFilter(watched, event);
}

void Toast::showText(const QString &text, int durationMs) {
    fade_->stop();
    setText(text);
    adjustSize();
    reposition();
    effect_->setOpacity(1.0);
    show();
    raise();
    hideTimer_->start(durationMs);
}

void Toast::reposition() {
    QWidget *parent = parentWidget();
    if (parent == nullptr) {
        return;
    }
    adjustSize();
    const int x = (parent->width() - width()) / 2;
    const int y = parent->height() - height() - 44;
    move(qMax(8, x), qMax(8, y));
}
