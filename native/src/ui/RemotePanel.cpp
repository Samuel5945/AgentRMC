#include "RemotePanel.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>
#include <QTimer>

namespace {

constexpr double kLogicalWidth = 254.0;
constexpr double kLogicalHeight = 310.0;
constexpr qint64 kFlashMs = 360;

struct KeySpec {
    int vk;
    double dx;
    double y;
    double radius;
    bool infrared;
};

const QVector<KeySpec> &keys() {
    static const QVector<KeySpec> table = {
        {0, 0, 22, 7.0, true},     // 电源（红外）
        {122, 0, 48, 8.0, false},  // 对焦 / 全屏
        {116, 0, 74, 8.5, false},  // 语音
        {38, 0, 102, 8.5, false},  // 上
        {37, -25, 130, 8.5, false},
        {13, 0, 130, 12.0, false},
        {39, 25, 130, 8.5, false},
        {40, 0, 158, 8.5, false},  // 下
        {27, -25, 190, 8.0, false},
        {93, 0, 190, 8.0, false},
        {36, 25, 190, 8.0, false},
    };
    return table;
}

QString glyphFor(int vk) {
    switch (vk) {
    case 116: return QStringLiteral("F5");
    case 122: return QStringLiteral("◎");
    case 38: return QStringLiteral("↑");
    case 40: return QStringLiteral("↓");
    case 37: return QStringLiteral("←");
    case 39: return QStringLiteral("→");
    case 13: return QStringLiteral("OK");
    case 27: return QStringLiteral("↩");
    case 93: return QStringLiteral("☰");
    case 36: return QStringLiteral("⌂");
    default: return {};
    }
}

bool isLit(int vk, int litVk, qint64 litUntil) {
    return vk != 0 && vk == litVk &&
           QDateTime::currentMSecsSinceEpoch() < litUntil;
}

}  // namespace

RemotePanel::RemotePanel(QWidget *parent) : QWidget(parent) {
    setMinimumSize(238, 250);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void RemotePanel::setEntries(const QVector<Entry> &entries) {
    entries_ = entries;
    update();
}

void RemotePanel::flash(int vk) {
    if (vk == 0) {
        return;
    }
    litVk_ = vk;
    litUntil_ = QDateTime::currentMSecsSinceEpoch() + kFlashMs;
    update();
    QTimer::singleShot(int(kFlashMs) + 20, this, [this] { update(); });
}

QString RemotePanel::actionFor(int vk) const {
    for (const Entry &entry : entries_) {
        if (entry.vk == vk) {
            return entry.action;
        }
    }
    return {};
}

QString RemotePanel::labelFor(int vk) const {
    for (const Entry &entry : entries_) {
        if (entry.vk == vk) {
            return entry.label;
        }
    }
    return {};
}

void RemotePanel::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale =
        qMin(width() / kLogicalWidth, height() / kLogicalHeight);
    painter.translate((width() - kLogicalWidth * scale) / 2.0,
                      (height() - kLogicalHeight * scale) / 2.0);
    painter.scale(scale, scale);

    const QRectF body(6, 4, 100, 302);
    const double cx = body.center().x();
    painter.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
    painter.setBrush(QColor(8, 12, 18));
    painter.drawRoundedRect(body, 22, 22);

    QFont glyphFont = font();
    glyphFont.setPixelSize(8);
    glyphFont.setBold(true);

    for (const KeySpec &key : keys()) {
        const bool lit = isLit(key.vk, litVk_, litUntil_);
        if (lit) {
            painter.setBrush(QColor(79, 140, 255));
            painter.setPen(QPen(QColor(120, 170, 255), 1.0));
        } else if (key.infrared) {
            painter.setBrush(QColor(255, 255, 255, 6));
            painter.setPen(QPen(QColor(255, 255, 255, 20), 1.0, Qt::DashLine));
        } else {
            painter.setBrush(QColor(255, 255, 255, 16));
            painter.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
        }
        const QRectF circle(cx + key.dx - key.radius, key.y - key.radius,
                            key.radius * 2, key.radius * 2);
        painter.drawEllipse(circle);

        const QString glyph = glyphFor(key.vk);
        if (!glyph.isEmpty()) {
            painter.setFont(glyphFont);
            painter.setPen(lit ? QColor(255, 255, 255)
                               : (key.infrared ? QColor(90, 100, 114)
                                               : QColor(230, 237, 243)));
            painter.drawText(circle.adjusted(-5, 0, 5, 0), Qt::AlignCenter,
                             glyph);
        }
    }

    // 音量条（红外，不参与映射）
    painter.setPen(QPen(QColor(255, 255, 255, 22), 1.0, Qt::DashLine));
    painter.setBrush(QColor(255, 255, 255, 8));
    painter.drawRoundedRect(QRectF(cx - 8, 218, 16, 50), 8, 8);

    QFont rowFont = font();
    rowFont.setPixelSize(9);
    painter.setFont(rowFont);
    const QFontMetrics metrics(rowFont);
    double rowY = 10;
    for (const Entry &entry : entries_) {
        const QRectF row(114, rowY, 136, 18);
        const bool lit = isLit(entry.vk, litVk_, litUntil_);
        if (lit) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(79, 140, 255, 42));
            painter.drawRoundedRect(row, 6, 6);
        }
        painter.setPen(entry.infrared ? QColor(90, 100, 114)
                                     : (lit ? QColor(230, 237, 243)
                                            : QColor(160, 172, 186)));
        painter.drawText(QRectF(116, rowY, 20, 18), Qt::AlignCenter,
                         entry.glyph);
        const QString text = metrics.elidedText(
            entry.action, Qt::ElideRight, 110);
        painter.drawText(QRectF(137, rowY, 110, 18),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
        rowY += 20;
    }
}
