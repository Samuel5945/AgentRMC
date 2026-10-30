#pragma once

#include <QColor>
#include <QPixmap>
#include <QRectF>

class QPainter;

namespace KeyIcon {

enum class Glyph {
    Unknown = 0,
    Up,
    Down,
    Left,
    Right,
    Ok,
    Back,
    Menu,
    Home,
    Mic,
    Focus,
    Power,
    Volume,
};

Glyph glyphForVk(int vk);
void paintGlyph(QPainter &painter, const QRectF &box, Glyph glyph,
                const QColor &color);
QPixmap glyphPixmap(Glyph glyph, int size, qreal devicePixelRatio,
                    const QColor &color);

}  // namespace KeyIcon
