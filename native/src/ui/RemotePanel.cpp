#include "RemotePanel.h"

#include "KeyIcon.h"

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

void RemotePanel::setModeKeys(const QSet<int> &keys) {
    if (modeKeys_ == keys) {
        return;
    }
    modeKeys_ = keys;
    update();
}

void RemotePanel::setLinkedVk(int vk) {
    if (linkedVk_ == vk) {
        return;
    }
    linkedVk_ = vk;
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

        const bool linked = key.vk != 0 && key.vk == linkedVk_;
        const bool ringed = key.vk != 0 && modeKeys_.contains(key.vk);
        if (!lit && linked) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(79, 140, 255, 210), 1.5));
            painter.drawEllipse(circle.adjusted(-3, -3, 3, 3));
        }
        if (!lit && ringed) {
            const double extra = linked ? 6.0 : 3.0;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(124, 92, 255, 200), 1.6));
            painter.drawEllipse(circle.adjusted(-extra, -extra, extra, extra));
        }

        const QColor iconColor =
            lit ? QColor(255, 255, 255)
                : (key.infrared ? QColor(90, 100, 114) : QColor(230, 237, 243));
        KeyIcon::paintGlyph(
            painter, QRectF(cx - 5.25, key.y - 5.25, 10.5, 10.5),
            KeyIcon::glyphForVk(key.vk), iconColor);
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
        const QColor rowColor = entry.infrared
                                    ? QColor(90, 100, 114)
                                    : (lit ? QColor(230, 237, 243)
                                           : QColor(160, 172, 186));
        KeyIcon::Glyph glyph = KeyIcon::glyphForVk(entry.vk);
        if (entry.label == QString::fromUtf8("电源")) {
            glyph = KeyIcon::Glyph::Power;
        } else if (entry.label == QString::fromUtf8("音量")) {
            glyph = KeyIcon::Glyph::Volume;
        }
        KeyIcon::paintGlyph(painter, QRectF(120.75, rowY + 3.75, 10.5, 10.5),
                            glyph, lit ? QColor(230, 237, 243) : rowColor);
        painter.setPen(rowColor);
        const QString text = metrics.elidedText(
            entry.action, Qt::ElideRight, 110);
        painter.drawText(QRectF(137, rowY, 110, 18),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
        rowY += 20;
    }
}
