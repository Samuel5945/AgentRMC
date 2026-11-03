#include "Toast.h"

#include "core/ThemeManager.h"

#include <QCursor>
#include <QGuiApplication>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>

Toast::Toast(QWidget *parent) : QLabel(parent) {
    setObjectName("toast");
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAlignment(Qt::AlignCenter);
    setWordWrap(false);
    setMaximumWidth(900);
    hide();

    fade_ = new QPropertyAnimation(this, "windowOpacity", this);
    fade_->setDuration(220);
    fade_->setStartValue(1.0);
    fade_->setEndValue(0.0);
    connect(fade_, &QPropertyAnimation::finished, this, [this] {
        if (windowOpacity() <= 0.01) {
            hide();
        }
    });

    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, this, [this] {
        fade_->stop();
        fade_->start();
    });
}

void Toast::showText(const QString &text, int durationMs) {
    fade_->stop();
    setText(text);
    adjustSize();
    reposition();
    setWindowOpacity(1.0);
    show();
    raise();
    hideTimer_->start(durationMs);
}

void Toast::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // 气泡底与描边走调色板：正文颜色由 QSS 给（浅色模式是深字），
    // 气泡再写死深色就没人读得出。
    const QColor bubble =
        ThemeManager::instance().color(QStringLiteral("bubble.bg"));
    const QColor edge = ThemeManager::instance().color(QStringLiteral("w30"));
    painter.setPen(QPen(edge.isValid() ? edge : QColor(255, 255, 255, 32), 1.0));
    painter.setBrush(bubble.isValid() ? bubble : QColor(15, 20, 29, 238));
    painter.drawRoundedRect(box, 12, 12);
    QLabel::paintEvent(event);
}

void Toast::reposition() {
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    adjustSize();
    const QRect available = screen->availableGeometry();
    const int x = available.x() + (available.width() - width()) / 2;
    const int y = available.y() + available.height() - height() - 80;
    move(x, y);
}
