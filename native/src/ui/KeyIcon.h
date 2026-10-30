#pragma once

#include <QColor>
#include <QPixmap>
#include <QRectF>

class QPainter;

namespace KeyIcon {

void paintMic(QPainter &painter, const QRectF &box, const QColor &color,
              qreal strokeWidth = 1.2);
QPixmap micPixmap(int size, qreal devicePixelRatio, const QColor &color);

}  // namespace KeyIcon
