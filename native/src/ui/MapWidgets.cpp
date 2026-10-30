#include "MapWidgets.h"

#include "ui/KeyIcon.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QVBoxLayout>

MapCell::MapCell(int vk, QString gesture, MapCellData data, QWidget *parent)
    : QFrame(parent),
      vk_(vk),
      gesture_(std::move(gesture)),
      title_(data.title),
      effect_(data.effect),
      editable_(data.editable) {
    setObjectName("mapCell");
    setProperty("ro", editable_ ? "false" : "true");
    setFixedHeight(33);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(editable_ ? Qt::PointingHandCursor : Qt::ArrowCursor);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(7, 1, 5, 1);
    layout->setSpacing(4);

    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addStretch(1);

    auto *title = new QLabel(title_, this);
    title->setObjectName(title_ == QString::fromUtf8("—") ? "mapCellDash"
                                                          : "mapCellTitle");
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    title->setAttribute(Qt::WA_TransparentForMouseEvents);
    column->addWidget(title);

    auto *effect = new QLabel(effect_, this);
    effect->setObjectName("mapCellEffect");
    effect->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    effect->setAttribute(Qt::WA_TransparentForMouseEvents);
    column->addWidget(effect);
    column->addStretch(1);

    auto *chevron = new QLabel(QString::fromUtf8("⌄"), this);
    chevron->setObjectName("mapCellChev");
    chevron->setAttribute(Qt::WA_TransparentForMouseEvents);

    layout->addLayout(column, 1);
    layout->addWidget(chevron, 0, Qt::AlignVCenter);
    chevron->setVisible(editable_);

    QString tip = data.tip;
    if (tip.isEmpty()) {
        tip = title_;
        if (!effect_.isEmpty()) {
            tip += QStringLiteral(" · ") + effect_;
        }
    }
    if (editable_) {
        tip += QStringLiteral("\n点击选择常用动作");
    }
    setToolTip(tip);
}

QSize MapCell::sizeHint() const {
    return {120, 33};
}

QSize MapCell::minimumSizeHint() const {
    return {0, 33};
}

void MapCell::mouseReleaseEvent(QMouseEvent *event) {
    if (editable_ && event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint())) {
        emit clicked(vk_, gesture_);
    }
    QFrame::mouseReleaseEvent(event);
}

void MapCell::enterEvent(QEnterEvent *event) {
    emit hovered(vk_, true);
    QFrame::enterEvent(event);
}

void MapCell::leaveEvent(QEvent *event) {
    emit hovered(vk_, false);
    QFrame::leaveEvent(event);
}

MapKeyRow::MapKeyRow(int vk, QString glyph, QString name, MapCellData single,
                     MapCellData doubleCell, MapCellData hold, bool selected,
                     QWidget *parent)
    : QFrame(parent), vk_(vk) {
    setObjectName("mapKeyRow");
    setFixedHeight(35);
    setCursor(Qt::PointingHandCursor);
    setProperty("sel", selected ? "true" : "false");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 1, 10, 1);
    layout->setSpacing(8);

    auto *keyBox = new QWidget(this);
    keyBox->setFixedWidth(88);
    keyBox->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *keyLayout = new QHBoxLayout(keyBox);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(8);

    auto *chip = new QLabel(keyBox);
    chip->setObjectName("mapChip");
    chip->setFixedSize(24, 24);
    chip->setAlignment(Qt::AlignCenter);
    chip->setAttribute(Qt::WA_TransparentForMouseEvents);
    if (vk == 116) {
        chip->setPixmap(KeyIcon::micPixmap(
            13, devicePixelRatioF(), QColor(230, 237, 243)));
    } else {
        chip->setText(glyph);
    }

    auto *label = new QLabel(name, keyBox);
    label->setObjectName("mapKeyName");
    label->setFixedWidth(50);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);

    keyLayout->addWidget(chip);
    keyLayout->addWidget(label);
    layout->addWidget(keyBox);

    const QVector<QPair<QString, MapCellData>> cells = {
        {"single", single}, {"double", doubleCell}, {"hold", hold}};
    for (const auto &entry : cells) {
        auto *cell = new MapCell(vk, entry.first, entry.second, this);
        connect(cell, &MapCell::clicked, this, &MapKeyRow::cellClicked);
        connect(cell, &MapCell::hovered, this, &MapKeyRow::hovered);
        layout->addWidget(cell, 1);
    }
    setToolTip(name + QStringLiteral(" · 点键帽选中，点手势格改键"));
}

void MapKeyRow::setSelected(bool on) {
    setProperty("sel", on ? "true" : "false");
    style()->unpolish(this);
    style()->polish(this);
}

void MapKeyRow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint())) {
        emit picked(vk_);
    }
    QFrame::mouseReleaseEvent(event);
}

void MapKeyRow::enterEvent(QEnterEvent *event) {
    emit hovered(vk_, true);
    QFrame::enterEvent(event);
}

void MapKeyRow::leaveEvent(QEvent *event) {
    emit hovered(vk_, false);
    QFrame::leaveEvent(event);
}
