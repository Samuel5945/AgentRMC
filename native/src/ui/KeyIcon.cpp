#include "KeyIcon.h"

#include <QPainter>

namespace {

// 麦克风在 9.2 x 13.6 的逻辑框里绘制，调用方只给目标框。
void drawMic(QPainter &painter, const QPointF &center, qreal scale,
             const QColor &color, qreal strokeWidth) {
    QPen pen(color, qMax(qreal(1.0), strokeWidth * scale));
    pen.setCapStyle(Qt::RoundCap);
    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const qreal cy = center.y() - 1.4 * scale;
    painter.drawRoundedRect(
        QRectF(center.x() - 2.3 * scale, cy - 5.4 * scale, 4.6 * scale,
               7.0 * scale),
        2.3 * scale, 2.3 * scale);
    painter.drawArc(
        QRectF(center.x() - 4.6 * scale, cy - 1.8 * scale, 9.2 * scale,
               8.4 * scale),
        180 * 16, 180 * 16);
    painter.drawLine(QPointF(center.x(), cy + 6.6 * scale),
                     QPointF(center.x(), cy + 8.2 * scale));
    painter.restore();
}

}  // namespace

namespace KeyIcon {

void paintMic(QPainter &painter, const QRectF &box, const QColor &color,
              qreal strokeWidth) {
    const qreal scale =
        qMin(box.width() / 9.2, box.height() / 13.6);
    drawMic(painter, box.center(), scale, color, strokeWidth);
}

QPixmap micPixmap(int size, qreal devicePixelRatio, const QColor &color) {
    const qreal dpr = qMax(qreal(1.0), devicePixelRatio);
    QPixmap pixmap(QSize(int(size * dpr), int(size * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintMic(painter, QRectF(0, 0, size, size), color, 1.15);
    return pixmap;
}

}  // namespace KeyIcon
