#include "WaveformButton.h"

#include "core/ThemeManager.h"

#include <QDateTime>
#include <QPainter>
#include <QtMath>

namespace {

QColor token(const QString &name, const QColor &fallback) {
    const QColor color = ThemeManager::instance().color(name);
    return color.isValid() ? color : fallback;
}

}  // namespace

void paintWaveBars(QPainter &painter, const QRectF &area, double level,
                   bool active, const QColor &color) {
    const qreal t = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    const int n = qMax(12, int(area.width() / 7));
    const qreal bw = area.width() / (2.0 * n - 1);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (int i = 0; i < n; ++i) {
        qreal h;
        if (active) {
            h = (0.25 + 0.75 * qBound(0.0, level, 1.0)) *
                (0.35 + 0.65 * qAbs(qSin(t * 9 + i * 0.55)));
            h = qMax<qreal>(h, 0.08);
        } else {
            // 静态声波轮廓：中间高两端低的包络 + 每根条的细小起伏。
            const qreal x = i / qreal(n - 1);
            const qreal env = qSin(x * 3.141592653589793);
            h = 0.18 + 0.72 * env * env * (0.75 + 0.25 * qSin(i * 0.9));
        }
        const qreal bh = h * area.height();
        painter.drawRoundedRect(
            QRectF(area.left() + i * 2 * bw, area.center().y() - bh / 2, bw, bh),
            bw / 2, bw / 2);
    }
}

WaveformButton::WaveformButton(QWidget *parent) : QPushButton(parent) {
    setObjectName("voiceKey");
    setFixedHeight(40);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(QString::fromUtf8("语音键"));
}

void WaveformButton::setListening(bool on) {
    if (listening_ == on) {
        return;
    }
    listening_ = on;
    update();
}

void WaveformButton::setRecording(bool on) {
    if (recording_ == on) {
        return;
    }
    recording_ = on;
    update();
}

void WaveformButton::setLevel(double level) {
    level_ = qBound(0.0, level, 1.0);
    if (recording_) {
        update();
    }
}

void WaveformButton::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    // 整条按钮就是波形，无文字；状态全靠颜色：录音绿 / 待命蓝 / 未监听灰。
    QColor bars;
    QColor bg;
    QColor edge;
    if (recording_) {
        bars = QColor(0x3f, 0xb9, 0x50);
        bg = QColor(63, 185, 80, 26);
        edge = QColor(63, 185, 80, 90);
    } else if (listening_) {
        bars = token(QStringLiteral("accent"), QColor(79, 140, 255));
        bg = token(QStringLiteral("w8"), QColor(255, 255, 255, 8));
        edge = token(QStringLiteral("w14"), QColor(255, 255, 255, 14));
    } else {
        bars = token(QStringLiteral("faint"), QColor(0x46, 0x50, 0x5e));
        bg = token(QStringLiteral("w6"), QColor(255, 255, 255, 6));
        edge = token(QStringLiteral("w10"), QColor(255, 255, 255, 10));
    }
    painter.setPen(QPen(edge, 1));
    painter.setBrush(bg);
    painter.drawRoundedRect(box, 10, 10);
    painter.setPen(Qt::NoPen);
    paintWaveBars(painter,
                  box.adjusted(12, box.height() * 0.22, -12, -box.height() * 0.22),
                  level_, recording_, bars);
}
