#include "ThemeDialog.h"

#include "core/Profiles.h"
#include "core/ThemeManager.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kPreviewW = 84;
constexpr int kPreviewH = 56;

// 没有 preview.png 的主题（含内置 classic）用调色板画一块色卡，
// 保证列表里每一项都有东西看，不会出现「空白方块」。
QPixmap swatch(const Theme &theme) {
    QPixmap pm(kPreviewW, kPreviewH);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box(0.5, 0.5, kPreviewW - 1.0, kPreviewH - 1.0);

    const QColor bg =
        ThemeManager::parsePaletteColor(theme.palette.value(QStringLiteral("bg")));
    const QColor card = ThemeManager::parsePaletteColor(
        theme.palette.value(QStringLiteral("card.bg")));
    const QColor accent = ThemeManager::parsePaletteColor(
        theme.palette.value(QStringLiteral("accent")));

    painter.setPen(QPen(QColor(255, 255, 255, 30), 1.0));
    painter.setBrush(bg.isValid() ? bg : QColor(0x0b, 0x0e, 0x14));
    painter.drawRoundedRect(box, 7, 7);

    painter.setPen(Qt::NoPen);
    painter.setBrush(card.isValid() ? card : QColor(0x12, 0x17, 0x22));
    painter.drawRoundedRect(QRectF(9, 9, kPreviewW - 18, kPreviewH - 18), 5, 5);
    painter.setBrush(accent.isValid() ? accent : QColor(0x4f, 0x8c, 0xff));
    painter.drawRoundedRect(QRectF(14, kPreviewH - 18, 26, 4), 2, 2);
    return pm;
}

QPixmap roundedThumb(const QPixmap &raw,
                     Qt::AspectRatioMode mode = Qt::KeepAspectRatioByExpanding) {
    QPixmap pm(kPreviewW, kPreviewH);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, kPreviewW, kPreviewH), 7, 7);
    painter.setClipPath(clip);
    const QPixmap scaled = raw.scaled(kPreviewW, kPreviewH, mode,
                                      Qt::SmoothTransformation);
    painter.drawPixmap((kPreviewW - scaled.width()) / 2,
                       (kPreviewH - scaled.height()) / 2, scaled);
    painter.setClipping(false);
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, kPreviewW - 1.0, kPreviewH - 1.0),
                            7, 7);
    return pm;
}

QPixmap themePixmap(const ThemeInfo &info) {
    if (!info.preview.isEmpty()) {
        const QPixmap raw(info.preview);
        if (!raw.isNull()) {
            return roundedThumb(raw);
        }
    }
    return swatch(ThemeManager::instance().themeById(info.id));
}

// 形象缩略图优先用 idle 帧（角色本体），完整 fit 不裁头脚。
QPixmap characterThumb(const ThemeInfo &info, const Theme &art) {
    QString path;
    const auto idle = art.petStates.constFind(QStringLiteral("idle"));
    if (idle != art.petStates.constEnd() && !idle->frames.isEmpty()) {
        path = idle->frames.first();
    }
    if (path.isEmpty()) {
        path = info.preview;
    }
    if (!path.isEmpty()) {
        const QPixmap raw(path);
        if (!raw.isNull()) {
            return roundedThumb(raw, Qt::KeepAspectRatio);
        }
    }
    return swatch(art);
}

}  // namespace

ThemeDialog::ThemeDialog(ProfileStore *store, QWidget *parent)
    : QDialog(parent), store_(store) {
    setObjectName("themeDialog");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(470);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("主题与外观"));
    title->setObjectName("captureTitle");
    auto *close = new QPushButton(QString::fromUtf8("✕"));
    close->setObjectName("ghostText");
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedWidth(24);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(close);
    root->addLayout(head);

    auto *themeLabel = new QLabel(QString::fromUtf8("主题"));
    themeLabel->setObjectName("mapHead");
    root->addWidget(themeLabel);

    // 图标墙：预览图 + 名字，点一下立即换肤（选择写回 ui.theme）。
    themes_ = new QListWidget;
    themes_->setObjectName("themeList");
    themes_->setViewMode(QListView::IconMode);
    themes_->setIconSize(QSize(kPreviewW, kPreviewH));
    themes_->setGridSize(QSize(kPreviewW + 20, kPreviewH + 34));
    themes_->setResizeMode(QListView::Adjust);
    themes_->setMovement(QListView::Static);
    themes_->setWrapping(true);
    themes_->setUniformItemSizes(true);
    themes_->setSelectionMode(QAbstractItemView::SingleSelection);
    themes_->setFrameShape(QFrame::NoFrame);
    themes_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    themes_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    themes_->setCursor(Qt::PointingHandCursor);
    connect(themes_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
                if (current == nullptr) {
                    return;
                }
                const QString id = current->data(Qt::UserRole).toString();
                if (id.isEmpty() ||
                    id == ThemeManager::instance().currentId()) {
                    return;
                }
                ThemeManager::instance().setTheme(id);
                reloadSliders();  // 新主题的默认透明度要跟上
            });
    root->addWidget(themes_);

    // 形象与主题分离：主题管配色，形象管插画/桌宠帧/音色。
    auto *charLabel = new QLabel(QString::fromUtf8("形象"));
    charLabel->setObjectName("mapHead");
    root->addWidget(charLabel);
    characters_ = new QListWidget;
    characters_->setObjectName("characterList");
    characters_->setViewMode(QListView::IconMode);
    characters_->setIconSize(QSize(kPreviewW, kPreviewH));
    characters_->setGridSize(QSize(kPreviewW + 20, kPreviewH + 34));
    characters_->setResizeMode(QListView::Adjust);
    characters_->setMovement(QListView::Static);
    characters_->setWrapping(true);
    characters_->setUniformItemSizes(true);
    characters_->setSelectionMode(QAbstractItemView::SingleSelection);
    characters_->setFrameShape(QFrame::NoFrame);
    characters_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    characters_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    characters_->setCursor(Qt::PointingHandCursor);
    connect(characters_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
                if (current == nullptr) {
                    return;
                }
                const QString id = current->data(Qt::UserRole).toString();
                if (id == ThemeManager::instance().characterId()) {
                    return;
                }
                ThemeManager::instance().setCharacter(id);
            });
    root->addWidget(characters_);
    auto *charNote = new QLabel(QString::fromUtf8(
        "形象管桌宠帧和音色；配色与两张插画背景跟主题，两者可自由搭配。"));
    charNote->setObjectName("emptyHint");
    charNote->setWordWrap(true);
    root->addWidget(charNote);

    // 配色模式：浅色 / 深色（夜间）/ 跟随系统。作用于内置 classic 和
    // 带浅色变体的主题；没带变体的自定义主题选中时这三颗置灰。
    auto *schemeLabel = new QLabel(QString::fromUtf8("配色模式"));
    schemeLabel->setObjectName("mapHead");
    root->addWidget(schemeLabel);
    auto *schemeRow = new QHBoxLayout;
    schemeRow->setSpacing(8);
    const auto makeSchemeBtn = [this, schemeRow](const QString &text,
                                                 QPushButton **out) {
        auto *btn = new QPushButton(text);
        btn->setObjectName("schemeOption");
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        schemeRow->addWidget(btn, 1);
        *out = btn;
    };
    makeSchemeBtn(QString::fromUtf8("浅色"), &schemeLight_);
    makeSchemeBtn(QString::fromUtf8("深色 · 夜间"), &schemeDark_);
    makeSchemeBtn(QString::fromUtf8("跟随系统"), &schemeAuto_);
    root->addLayout(schemeRow);
    schemeAuto_->setToolTip(QString::fromUtf8(
        "跟随 Windows 的深浅色设置，系统切换时自动跟上"));
    connect(schemeLight_, &QPushButton::clicked, this,
            [this] { ThemeManager::instance().setScheme(QStringLiteral("light")); });
    connect(schemeDark_, &QPushButton::clicked, this,
            [this] { ThemeManager::instance().setScheme(QStringLiteral("dark")); });
    connect(schemeAuto_, &QPushButton::clicked, this,
            [this] { ThemeManager::instance().setScheme(QStringLiteral("auto")); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &) {
                reloadScheme();
                reloadCharacters();  // 「跟随主题」的缩略图随当前主题变
            });

    auto *sep = new QFrame;
    sep->setObjectName("mapHair");
    sep->setFixedHeight(1);
    root->addWidget(sep);

    auto *lookLabel = new QLabel(QString::fromUtf8("外观"));
    lookLabel->setObjectName("mapHead");
    root->addWidget(lookLabel);

    const auto makeRow = [this, root](const QString &name, QSlider **outSlider,
                                      QLabel **outValue) {
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        auto *label = new QLabel(name);
        label->setObjectName("muted");
        // 固定宽是给三条滑块对齐用的；随字号倍率长，倍率大时标签不吃字。
        label->setFixedWidth(ThemeManager::scaledPx(88));
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setCursor(Qt::PointingHandCursor);
        auto *value = new QLabel;
        value->setObjectName("muted");
        value->setFixedWidth(ThemeManager::scaledPx(40));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(label);
        row->addWidget(slider, 1);
        row->addWidget(value);
        root->addLayout(row);
        *outSlider = slider;
        *outValue = value;
    };
    makeRow(QString::fromUtf8("卡片不透明度"), &alpha_, &alphaValue_);
    makeRow(QString::fromUtf8("背景压暗"), &dim_, &dimValue_);
    makeRow(QString::fromUtf8("字体大小"), &fontScale_, &fontScaleValue_);
    // 字号滑块的刻度是百分比（85-140），和上面两条 0-100 的不同。
    fontScale_->setRange(85, 140);
    fontScale_->setPageStep(5);

    alpha_->setToolTip(
        QString::fromUtf8("往右拉：正文更清楚；往左拉：更多背景插画透出来"));
    dim_->setToolTip(QString::fromUtf8("背景图整体压暗程度，和卡片透明度独立"));
    fontScale_->setToolTip(
        QString::fromUtf8("整窗文字大小，立即生效；太大可能挤到个别固定高度的行"));

    auto *note = new QLabel(QString::fromUtf8(
        "卡片越透明，背景插画越明显，正文也越难读；拖到看不清就收一点。"));
    note->setObjectName("emptyHint");
    note->setWordWrap(true);
    root->addWidget(note);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *reset = new QPushButton(QString::fromUtf8("恢复主题默认"));
    reset->setObjectName("ghostSmall");
    reset->setCursor(Qt::PointingHandCursor);
    connect(reset, &QPushButton::clicked, this, [this] {
        ThemeManager::instance().resetAppearance();
        reloadSliders();
    });
    auto *done = new QPushButton(QString::fromUtf8("完成"));
    done->setObjectName("primarySmall");
    done->setCursor(Qt::PointingHandCursor);
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    actions->addWidget(reset);
    actions->addStretch(1);
    actions->addWidget(done);
    root->addLayout(actions);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(60);
    connect(debounce_, &QTimer::timeout, this, [this] {
        ThemeManager::instance().setCardAlpha(alpha_->value() * 255 / 100);
        ThemeManager::instance().setBackdropDim(dim_->value() / 100.0);
        ThemeManager::instance().setFontScale(fontScale_->value() / 100.0);
    });
    connect(alpha_, &QSlider::valueChanged, this, [this](int) {
        syncLabels();
        scheduleApply();
    });
    connect(dim_, &QSlider::valueChanged, this, [this](int) {
        syncLabels();
        scheduleApply();
    });
    connect(fontScale_, &QSlider::valueChanged, this, [this](int) {
        syncLabels();
        scheduleApply();
    });

    reloadThemes();
    reloadCharacters();
    reloadScheme();
    reloadSliders();
}

void ThemeDialog::reloadCharacters() {
    const QSignalBlocker block(characters_);
    characters_->clear();
    const QString current = ThemeManager::instance().characterId();
    const auto add = [this, current](const QString &id, const QString &text,
                                     const QPixmap &pm, const QString &tip) {
        auto *item = new QListWidgetItem(characters_);
        item->setIcon(QIcon(pm));
        item->setText(text);
        item->setData(Qt::UserRole, id);
        item->setTextAlignment(Qt::AlignHCenter);
        item->setToolTip(tip);
        if (id == current) {
            characters_->setCurrentItem(item);
        }
    };

    const QString themeId = ThemeManager::instance().currentId();
    QPixmap followPm;
    for (const ThemeInfo &info : ThemeManager::instance().themes()) {
        if (info.id == themeId) {
            followPm = themePixmap(info);
            break;
        }
    }
    if (followPm.isNull()) {
        followPm = swatch(ThemeManager::instance().current());
    }
    add(QString(), QString::fromUtf8("跟随主题"), followPm,
        QString::fromUtf8("桌宠帧和音色随当前主题包"));

    QPixmap nonePm(kPreviewW, kPreviewH);
    nonePm.fill(Qt::transparent);
    {
        QPainter painter(&nonePm);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 255, 255, 30), 1.0));
        painter.setBrush(QColor(0x12, 0x17, 0x22));
        painter.drawRoundedRect(QRectF(0.5, 0.5, kPreviewW - 1.0, kPreviewH - 1.0),
                                7, 7);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x8b, 0x94, 0x9e));
        painter.drawEllipse(QPointF(kPreviewW / 2.0, kPreviewH / 2.0), 12, 12);
    }
    add(QStringLiteral("none"), QString::fromUtf8("无形象"), nonePm,
        QString::fromUtf8("圆球桌宠 + 默认音色；插画背景仍跟主题"));

    for (const ThemeInfo &info : ThemeManager::instance().themes()) {
        if (!info.valid || info.id == QStringLiteral("classic")) {
            continue;
        }
        const Theme art = ThemeManager::instance().themeById(info.id);
        if (!art.hasPetSprite() && art.voiceName.isEmpty()) {
            continue;
        }
        add(info.id,
            art.characterName.isEmpty() ? info.displayName : art.characterName,
            characterThumb(info, art),
            QString::fromUtf8("只用它的桌宠帧和音色，配色与插画背景仍跟主题"));
    }
    const int count = characters_->count();
    const int rows = qMax(1, (count + 3) / 4);
    characters_->setFixedHeight(rows * (kPreviewH + 34) + 8);
}

void ThemeDialog::reloadThemes() {
    const QSignalBlocker block(themes_);
    themes_->clear();
    const QString currentId = ThemeManager::instance().currentId();
    for (const ThemeInfo &info : ThemeManager::instance().themes()) {
        if (!info.valid) {
            continue;
        }
        auto *item = new QListWidgetItem(themes_);
        item->setIcon(QIcon(themePixmap(info)));
        item->setText(info.displayName);
        item->setData(Qt::UserRole, info.id);
        item->setTextAlignment(Qt::AlignHCenter);
        item->setToolTip(
            info.author.isEmpty()
                ? info.displayName
                : QString::fromUtf8("%1 · %2").arg(info.displayName, info.author));
        if (info.id == currentId) {
            themes_->setCurrentItem(item);
        }
    }
    // 一行 4 个；超出就多给一行高度，别让列表自己滚。
    const int count = themes_->count();
    const int rows = qMax(1, (count + 3) / 4);
    themes_->setFixedHeight(rows * (kPreviewH + 34) + 8);
}

void ThemeDialog::reloadScheme() {
    const Theme &cur = ThemeManager::instance().current();
    const bool supports = cur.id == QStringLiteral("classic") ||
                          cur.hasLightVariant();
    const QString scheme = ThemeManager::instance().scheme();
    schemeLight_->setChecked(scheme == QStringLiteral("light"));
    schemeDark_->setChecked(scheme == QStringLiteral("dark"));
    schemeAuto_->setChecked(scheme == QStringLiteral("auto"));
    // 没浅色变体的主题不吃配色模式：置灰但保留选中态，切回支持的主题时生效。
    schemeLight_->setEnabled(supports);
    schemeDark_->setEnabled(supports);
    schemeAuto_->setEnabled(supports);
}

void ThemeDialog::reloadSliders() {
    const QSignalBlocker blockA(alpha_);
    const QSignalBlocker blockD(dim_);
    const QSignalBlocker blockF(fontScale_);
    alpha_->setValue(qRound(ThemeManager::instance().cardAlpha() * 100 / 255.0));
    dim_->setValue(qRound(ThemeManager::instance().backdropDim() * 100));
    fontScale_->setValue(qRound(ThemeManager::instance().fontScale() * 100));
    syncLabels();
}

void ThemeDialog::syncLabels() {
    alphaValue_->setText(QStringLiteral("%1%").arg(alpha_->value()));
    dimValue_->setText(QStringLiteral("%1%").arg(dim_->value()));
    fontScaleValue_->setText(QStringLiteral("%1%").arg(fontScale_->value()));
}

void ThemeDialog::scheduleApply() {
    debounce_->start();
}
