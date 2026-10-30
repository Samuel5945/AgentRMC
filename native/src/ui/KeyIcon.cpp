#include "KeyIcon.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace KeyIcon {
namespace {

constexpr double kPi = 3.14159265358979323846;

void drawMic(QPainter &painter, const QPointF &center, qreal scale,
             const QColor &color) {
    QPen pen(color, qMax(qreal(1.0), 1.5 * scale));
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

void drawArrow(QPainter &painter, Glyph glyph) {
    // 16x16 逻辑框，箭头方向按 y 轴向下。
    QPointF tail;
    QPointF tip;
    switch (glyph) {
    case Glyph::Up: tail = QPointF(0, 6); tip = QPointF(0, -6); break;
    case Glyph::Down: tail = QPointF(0, -6); tip = QPointF(0, 6); break;
    case Glyph::Left: tail = QPointF(6, 0); tip = QPointF(-6, 0); break;
    case Glyph::Right: tail = QPointF(-6, 0); tip = QPointF(6, 0); break;
    default: return;
    }
    painter.drawLine(tail, tip);
    const QPointF d = tip - tail;
    const qreal len = std::hypot(d.x(), d.y());
    const QPointF u = d / len;
    const QPointF n(-u.y(), u.x());
    const QPointF base = tip - u * 4.5;
    painter.drawLine(tip, base + n * 3.0);
    painter.drawLine(tip, base - n * 3.0);
}

void paintNormalized(QPainter &painter, const QRectF &box, Glyph glyph,
                     const QColor &color) {
    const qreal scale = qMin(box.width(), box.height()) / 16.0;
    painter.save();
    painter.translate(box.center());
    painter.scale(scale, scale);
    QPen pen(color, 1.15 / scale);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (glyph) {
    case Glyph::Up:
    case Glyph::Down:
    case Glyph::Left:
    case Glyph::Right:
        drawArrow(painter, glyph);
        break;
    case Glyph::Back: {
        QPainterPath path;
        path.moveTo(5.0, 4.5);
        path.cubicTo(5.0, -2.0, 0.0, -5.0, -4.2, -4.0);
        painter.drawPath(path);
        painter.drawLine(QPointF(-4.2, -4.0), QPointF(-1.4, -6.0));
        painter.drawLine(QPointF(-4.2, -4.0), QPointF(-6.0, -1.2));
        break;
    }
    case Glyph::Menu:
        for (qreal y : {-5.2, 0.0, 5.2}) {
            painter.drawLine(QPointF(-5.5, y), QPointF(5.5, y));
        }
        break;
    case Glyph::Home: {
        QPainterPath path;
        path.moveTo(-6.0, 0.0);
        path.lineTo(0.0, -6.0);
        path.lineTo(6.0, 0.0);
        path.moveTo(-4.4, -0.8);
        path.lineTo(-4.4, 6.0);
        path.lineTo(4.4, 6.0);
        path.lineTo(4.4, -0.8);
        painter.drawPath(path);
        break;
    }
    case Glyph::Focus:
        painter.drawEllipse(QPointF(0, 0), 4.8, 4.8);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(0, 0), 1.3, 1.3);
        painter.setBrush(Qt::NoBrush);
        for (qreal angle : {0.0, 90.0, 180.0, 270.0}) {
            const qreal rad = angle * kPi / 180.0;
            painter.drawLine(QPointF(std::cos(rad) * 5.6, std::sin(rad) * 5.6),
                             QPointF(std::cos(rad) * 6.6, std::sin(rad) * 6.6));
        }
        break;
    case Glyph::Power:
        painter.drawArc(QRectF(-5.6, -5.6, 11.2, 11.2), 120 * 16, 300 * 16);
        painter.drawLine(QPointF(0, -7.4), QPointF(0, -1.0));
        break;
    case Glyph::Volume: {
        QPainterPath path;
        path.moveTo(-6.0, -2.0);
        path.lineTo(-3.0, -2.0);
        path.lineTo(1.0, -5.5);
        path.lineTo(1.0, 5.5);
        path.lineTo(-3.0, 2.0);
        path.lineTo(-6.0, 2.0);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawArc(QRectF(0.0, -4.0, 8.0, 8.0), -60 * 16, 120 * 16);
        break;
    }
    default:
        break;
    }
    painter.restore();
}

}  // namespace

Glyph glyphForVk(int vk) {
    switch (vk) {
    case 38: return Glyph::Up;
    case 40: return Glyph::Down;
    case 37: return Glyph::Left;
    case 39: return Glyph::Right;
    case 13: return Glyph::Ok;
    case 27: return Glyph::Back;
    case 93: return Glyph::Menu;
    case 36: return Glyph::Home;
    case 116: return Glyph::Mic;
    case 122: return Glyph::Focus;
    case 0: return Glyph::Power;
    default: return Glyph::Unknown;
    }
}

void paintGlyph(QPainter &painter, const QRectF &box, Glyph glyph,
                const QColor &color) {
    if (glyph == Glyph::Unknown || box.width() <= 0 || box.height() <= 0) {
        return;
    }
    if (glyph == Glyph::Ok) {
        QFont font = painter.font();
        font.setPixelSize(qMax(8, int(box.height() * 0.80)));
        font.setBold(true);
        painter.save();
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(box, Qt::AlignCenter, QStringLiteral("OK"));
        painter.restore();
        return;
    }
    if (glyph == Glyph::Mic) {
        const qreal scale =
            qMin(box.width() / 9.2, box.height() / 13.6) * 0.78;
        drawMic(painter, box.center(), scale, color);
        return;
    }
    paintNormalized(painter, box, glyph, color);
}

QPixmap glyphPixmap(Glyph glyph, int size, qreal devicePixelRatio,
                    const QColor &color) {
    const qreal dpr = qMax(qreal(1.0), devicePixelRatio);
    QPixmap pixmap(QSize(int(size * dpr), int(size * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintGlyph(painter, QRectF(0, 0, size, size), glyph, color);
    return pixmap;
}

}  // namespace KeyIcon
