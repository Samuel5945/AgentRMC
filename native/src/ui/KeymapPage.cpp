#include "KeymapPage.h"

#include "core/Presets.h"
#include "core/VoiceSettings.h"

#include "ui/ToggleSwitch.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const QHash<int, QString> &glyphs() {
    static const QHash<int, QString> table = {
        {38, QString::fromUtf8("↑")}, {40, QString::fromUtf8("↓")},
        {37, QString::fromUtf8("←")}, {39, QString::fromUtf8("→")},
        {13, "OK"}, {27, QString::fromUtf8("↩")},
        {93, QString::fromUtf8("☰")}, {36, QString::fromUtf8("⌂")},
        {116, QString::fromUtf8("◉")}, {122, QString::fromUtf8("◎")},
    };
    return table;
}

const QHash<int, QString> &names() {
    static const QHash<int, QString> table = {
        {38, QStringLiteral("上键")}, {40, QStringLiteral("下键")},
        {37, QStringLiteral("左键")}, {39, QStringLiteral("右键")},
        {13, QStringLiteral("确定键")}, {27, QStringLiteral("返回键")},
        {93, QStringLiteral("菜单键")}, {36, QStringLiteral("主页键")},
        {116, QStringLiteral("语音键")}, {122, QStringLiteral("对焦键")},
    };
    return table;
}

const QVector<QPair<QString, QVector<int>>> &mapGroups() {
    static const QVector<QPair<QString, QVector<int>>> table = {
        {QStringLiteral("方向键"), {38, 40, 37, 39}},
        {QStringLiteral("功能键"), {13, 27, 93, 36}},
        {QStringLiteral("语音与直通"), {116, 122}},
    };
    return table;
}

QString dash() {
    return QString::fromUtf8("—");
}

QString triggerText(const QJsonObject &config, const Profile &profile) {
    QString trigger = profile.voice.value("trigger").toString();
    if (trigger.isEmpty()) {
        trigger = config.value("voice").toObject().value("hotkey").toObject()
                      .value("trigger").toString();
    }
    return trigger.isEmpty() ? QStringLiteral("hold") : trigger.toLower();
}

class ComboCaptureDialog final : public QDialog {
public:
    explicit ComboCaptureDialog(QWidget *parent) : QDialog(parent) {
        setObjectName("comboCapture");
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setModal(true);
        setFixedSize(360, 150);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(9);

        auto *title = new QLabel(QStringLiteral("自定义组合键"));
        title->setObjectName("captureTitle");
        auto *hint = new QLabel(QStringLiteral("在键盘上按下要绑定的组合键；Esc 取消"));
        hint->setObjectName("captureHint");
        combo_ = new QLabel(QStringLiteral("按下组合键…"));
        combo_->setObjectName("captureCombo");
        combo_->setAlignment(Qt::AlignCenter);
        combo_->setMinimumHeight(42);

        layout->addWidget(title);
        layout->addWidget(combo_);
        layout->addWidget(hint);
        layout->addStretch(1);
        setFocusPolicy(Qt::StrongFocus);
    }

    QStringList mods() const { return mods_; }
    int vk() const { return vk_; }
    QString combo() const { return Presets::comboText(mods_, vk_); }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        const int key = event->key();
        if (key == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
            reject();
            return;
        }
        updateMods(event->modifiers());
        if (key == Qt::Key_Control || key == Qt::Key_Alt ||
            key == Qt::Key_Shift || key == Qt::Key_Meta ||
            key == Qt::Key_unknown) {
            return;
        }
        vk_ = int(event->nativeVirtualKey());
        if (vk_ == 0) {
            vk_ = key;
        }
        combo_->setText(Presets::comboText(mods_, vk_));
        accept();
    }

private:
    void updateMods(Qt::KeyboardModifiers modifiers) {
        mods_.clear();
        if (modifiers & Qt::ControlModifier) mods_ << "ctrl";
        if (modifiers & Qt::AltModifier) mods_ << "alt";
        if (modifiers & Qt::ShiftModifier) mods_ << "shift";
        if (modifiers & Qt::MetaModifier) mods_ << "win";
        if (vk_ == 0) {
            QStringList parts;
            for (const QString &m : mods_) {
                parts << m.toUpper();
            }
            combo_->setText(parts.isEmpty()
                                ? QStringLiteral("按下组合键…")
                                : parts.join('+') + QString::fromUtf8("+…"));
        }
    }

    QStringList mods_;
    int vk_ = 0;
    QLabel *combo_ = nullptr;
};

}  // namespace

KeymapPage::KeymapPage(ProfileStore *store, QWidget *parent)
    : QFrame(parent), store_(store) {
    setObjectName("card");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 11, 12, 11);
    layout->setSpacing(8);
    buildUi();
    rebuildCombo();
    refreshAll();

    tick_ = new QTimer(this);
    tick_->setInterval(300);
    connect(tick_, &QTimer::timeout, this, [this] {
        if (store_->isPinned()) {
            return;
        }
        const QString name = store_->current(true).name;
        if (name != shownName_) {
            refreshAll();
        }
    });
    tick_->start();
}

void KeymapPage::buildUi() {
    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());

    auto *head = new QWidget;
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(6);

    auto *left = new QVBoxLayout;
    left->setSpacing(1);
    auto *state = new QHBoxLayout;
    state->setSpacing(6);
    sceneDot_ = new QLabel;
    sceneDot_->setFixedSize(6, 6);
    sceneName_ = new QLabel(QStringLiteral("—"));
    sceneName_->setObjectName("sceneName");
    sceneRule_ = new QLabel;
    sceneRule_->setObjectName("sceneRule");
    sceneRule_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    state->addWidget(sceneDot_, 0, Qt::AlignVCenter);
    state->addWidget(sceneName_);
    state->addSpacing(2);
    state->addWidget(sceneRule_, 1);
    left->addLayout(state);
    headLayout->addLayout(left, 1);

    combo_ = new QComboBox;
    combo_->setObjectName("combo");
    combo_->setCursor(Qt::PointingHandCursor);
    combo_->setFixedWidth(104);
    combo_->setToolTip(QStringLiteral(
        "跟随前台：切窗口自动换映射\n选具体场景：固定预览这个场景"));
    connect(combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                store_->setPinned(combo_->currentData().toString());
                refreshAll();
            });

    auto *advanced = new QPushButton(QStringLiteral("高级配置"));
    advanced->setObjectName("ghostSmall");
    advanced->setCursor(Qt::PointingHandCursor);
    advanced->setToolTip(QStringLiteral("直接编辑 profiles.json"));
    connect(advanced, &QPushButton::clicked, this, &KeymapPage::openProfiles);

    auto *reload = new QPushButton(QStringLiteral("重新加载"));
    reload->setObjectName("ghostSmall");
    reload->setCursor(Qt::PointingHandCursor);
    connect(reload, &QPushButton::clicked, this, &KeymapPage::reloadProfiles);

    headLayout->addWidget(combo_, 0, Qt::AlignTop);
    headLayout->addSpacing(2);
    headLayout->addWidget(advanced, 0, Qt::AlignTop);
    headLayout->addWidget(reload, 0, Qt::AlignTop);
    layout->addWidget(head);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    holder_ = new QWidget;
    holder_->setObjectName("mapHolder");
    holder_->setMinimumWidth(500);
    grid_ = new QVBoxLayout(holder_);
    grid_->setContentsMargins(0, 0, 4, 0);
    grid_->setSpacing(0);
    scroll_->setWidget(holder_);
    layout->addWidget(scroll_, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(8);
    footHint_ = new QLabel(QStringLiteral("点击手势格改键 · 自动保存"));
    footHint_->setObjectName("mapFoot");
    footHint_->setStyleSheet(
        "QLabel#mapFoot { color: #5b6472; font-size: 10px; }");
    const QString takeoverTip = QStringLiteral(
        "开关打开后，当前前台场景生效：吞掉映射键原生事件再按映射注入。"
        "实体键盘同名键也会按映射处理；双击键的单击延迟约 0.3 秒。");
    takeoverLabel_ = new QLabel(QStringLiteral("接管原生键"));
    takeoverLabel_->setObjectName("mapTakeover");
    takeoverLabel_->setFixedWidth(58);
    takeoverLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    takeoverLabel_->setToolTip(takeoverTip);
    takeoverSwitch_ = new ToggleSwitch(store_->uiBool("native_takeover", false));
    takeoverSwitch_->setToolTip(takeoverTip);
    connect(takeoverSwitch_, &ToggleSwitch::toggled, this, [this](bool on) {
        store_->setUiBool("native_takeover", on);
        syncTakeoverUi();
    });
    foot->addWidget(footHint_, 1);
    foot->addSpacing(6);
    foot->addWidget(takeoverLabel_);
    foot->addWidget(takeoverSwitch_);
    syncTakeoverUi();
    layout->addLayout(foot);

    hintTimer_ = new QTimer(this);
    hintTimer_->setSingleShot(true);
    connect(hintTimer_, &QTimer::timeout, this, &KeymapPage::resetHint);
}

void KeymapPage::rebuildCombo() {
    QSignalBlocker blocker(combo_);
    combo_->clear();
    combo_->addItem(QStringLiteral("跟随前台"), QString());
    for (const QString &name : store_->names()) {
        combo_->addItem(name, name);
    }
    const int index = combo_->findData(store_->pinned());
    combo_->setCurrentIndex(index < 0 ? 0 : index);
}

void KeymapPage::refreshAll() {
    const Profile &profile = store_->current(true);
    const bool changed = shownName_ != profile.name;
    shownName_ = profile.name;
    if (changed) {
        emit viewChanged();
    }
    refreshScene();
    rebuildTable();
    const int index = combo_->findData(store_->pinned());
    QSignalBlocker blocker(combo_);
    combo_->setCurrentIndex(index < 0 ? 0 : index);
}

void KeymapPage::refreshScene() {
    const Profile &profile = store_->current(false);
    const bool pinned = store_->isPinned();
    sceneDot_->setStyleSheet(QStringLiteral("background: %1; border-radius: 3px;")
                                 .arg(pinned ? "#d29922" : "#3fb950"));
    sceneName_->setText(profile.name);
    sceneName_->setMinimumWidth(
        sceneName_->fontMetrics().horizontalAdvance(profile.name));
    const auto summary = ProfileStore::matchSummary(profile);
    sceneRule_->setText(QStringLiteral("· ") + summary.first);
    sceneRule_->setToolTip(summary.second);
}

QWidget *KeymapPage::buildHeaderRow() {
    auto *header = new QWidget;
    auto *outer = new QVBoxLayout(header);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(2);
    auto *row = new QHBoxLayout;
    row->setContentsMargins(7, 1, 11, 0);
    row->setSpacing(8);
    const QVector<QPair<QString, int>> columns = {
        {QStringLiteral("键位"), 88},
        {QStringLiteral("单击"), 0},
        {QStringLiteral("双击"), 0},
        {QStringLiteral("长按"), 0},
    };
    for (const auto &column : columns) {
        auto *label = new QLabel(column.first);
        label->setObjectName("mapHead");
        if (column.second > 0) {
            label->setFixedWidth(column.second);
            row->addWidget(label);
        } else {
            label->setContentsMargins(8, 0, 0, 0);
            row->addWidget(label, 1);
        }
    }
    outer->addLayout(row);
    auto *hair = new QFrame;
    hair->setObjectName("mapHair");
    hair->setFixedHeight(1);
    outer->addWidget(hair);
    return header;
}

QWidget *KeymapPage::buildSection(const QString &title) {
    auto *section = new QWidget;
    section->setFixedHeight(22);
    auto *row = new QHBoxLayout(section);
    row->setContentsMargins(2, 5, 0, 0);
    row->setSpacing(8);
    auto *label = new QLabel(title);
    label->setObjectName("mapGroup");
    auto *hair = new QFrame;
    hair->setObjectName("mapHair");
    hair->setFixedHeight(1);
    hair->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->addWidget(label);
    row->addWidget(hair, 1, Qt::AlignVCenter);
    return section;
}

void KeymapPage::rebuildTable() {
    while (QLayoutItem *item = grid_->takeAt(0)) {
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    rows_.clear();

    const Profile &profile = store_->current(false);
    grid_->addWidget(buildHeaderRow());

    for (const auto &group : mapGroups()) {
        grid_->addWidget(buildSection(group.first));
        for (int vk : group.second) {
            auto *row = new MapKeyRow(vk, glyphs().value(vk),
                                      names().value(vk), singleCell(vk, profile),
                                      doubleCell(vk, profile),
                                      holdCell(vk, profile), vk == selectedVk_);
            connect(row, &MapKeyRow::picked, this, &KeymapPage::selectRow);
            connect(row, &MapKeyRow::hovered, this, &KeymapPage::keyHovered);
            connect(row, &MapKeyRow::cellClicked, this,
                    [this](int value, const QString &gesture) {
                        selectRow(value);
                        openMenu(value, gesture);
                    });
            rows_[vk] = row;
            grid_->addWidget(row);
        }
    }
    grid_->addStretch(1);
    grid_->activate();
    holder_->updateGeometry();
}

void KeymapPage::selectRow(int vk) {
    selectedVk_ = vk;
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        it.value()->setSelected(it.key() == vk);
    }
    emit keySelected(vk);
}

bool KeymapPage::hasNav(const Profile &profile) const {
    return !profile.nav.value("keys").toObject().isEmpty();
}

MapCellData KeymapPage::singleCell(int vk, const Profile &profile) const {
    if (vk == 116) {
        const VoiceOptions voice = resolveVoiceOptions(
            store_->config(), profile.voice,
            QCoreApplication::applicationDirPath());
        if (voice.backend == VoiceBackend::Hotkey) {
            const QString combo = ProfileStore::voiceCombo(store_->config(),
                                                           profile);
            const QString trigger = triggerText(store_->config(), profile);
            const QString target = voiceProfileLabel(profile.name);
            const QString tip =
                trigger == "toggle"
                    ? QStringLiteral("开关式：按下开始、松手结束，转给 %1").arg(target)
                    : QStringLiteral("按住式：按住说话、松手结束，转给 %1").arg(target);
            return {QStringLiteral("语音输入"), combo, tip, false};
        }
        const bool qwen = voice.backend == VoiceBackend::Qwen3Asr;
        return {QStringLiteral("语音输入"),
                qwen ? QStringLiteral("本地 Qwen3-ASR")
                     : QStringLiteral("本地 SenseVoice"),
                QStringLiteral("按住遥控器语音键说话，松手后本地转写并粘贴到当前应用"),
                false};
    }

    const QString sem =
        profile.keys.value(QString::number(vk)).toString();
    const QJsonObject act =
        sem.isEmpty() ? QJsonObject{}
                      : profile.actions.value(sem).toObject();
    const auto cells = Presets::actionCells(act, vk, sem);
    if (vk == 122) {
        return {cells.first, cells.second,
                QStringLiteral("F11 原生直通，不能改为其它动作"), false};
    }
    QString tip = cells.first;
    if (!cells.second.isEmpty()) {
        tip += QStringLiteral(" · ") + cells.second;
    }
    return {cells.first, cells.second, tip, true};
}

MapCellData KeymapPage::doubleCell(int vk, const Profile &profile) const {
    if (vk == 116) {
        return {dash(), {}, QStringLiteral("语音键不支持双击"), false};
    }
    if (vk == 122) {
        return {dash(), {}, QStringLiteral("对焦键不支持双击"), false};
    }
    const QString spec =
        profile.doubleMap.value(QString::number(vk)).toString();
    if (spec == "none") {
        return {dash(), QStringLiteral("已关闭"),
                QStringLiteral("双击已关闭；单击不再等双击窗口"), true};
    }
    if (!spec.isEmpty()) {
        const QJsonObject act = profile.actions.value(spec).toObject();
        QString title = act.value("label").toString();
        if (title.isEmpty()) {
            title = Presets::actionCn(spec);
        }
        if (title.isEmpty()) {
            title = spec;
        }
        QString effect = Presets::actionEffectText(act);
        if (effect == title) {
            effect.clear();
        }
        QString tip = QStringLiteral("双击 ") + title;
        if (!effect.isEmpty()) {
            tip += QStringLiteral(" · ") + effect;
        }
        return {title, effect, tip, true};
    }
    if (vk == 27) {
        return {QStringLiteral("音量/亮度模式"), QStringLiteral("双击进入"),
                QStringLiteral("双击进入音量 / 亮度模式：↑↓ 音量、←→ 屏幕亮度；"
                               "再双击返回或 12 秒无操作退出"),
                true};
    }
    if (vk == 93) {
        return {QStringLiteral("鼠标模式"), QStringLiteral("双击进入"),
                QStringLiteral("双击进入鼠标模式：方向键移动、OK 左键；单击菜单退出"),
                true};
    }
    if (vk == 36 && hasNav(profile)) {
        return {QStringLiteral("导航模式"), QStringLiteral("双击切换"),
                QStringLiteral("双击切换导航模式：↑↓ 翻页、←→ 切换控件；单击主页退出"),
                true};
    }
    return {dash(), {}, QStringLiteral("双击无动作，点击选择常用动作"), true};
}

MapCellData KeymapPage::holdCell(int vk, const Profile &profile) const {
    if (vk == 116) {
        return {QStringLiteral("按住说话"), {},
                QStringLiteral("按住遥控器语音键说话，松手结束"), false};
    }
    if (vk == 122) {
        return {dash(), {}, QStringLiteral("对焦键不支持长按"), false};
    }
    const QString sem =
        profile.keys.value(QString::number(vk)).toString();
    const QJsonObject act =
        sem.isEmpty() ? QJsonObject{}
                      : profile.actions.value(sem).toObject();
    const QString spec =
        profile.hold.value(QString::number(vk)).toString();
    if (spec == "none") {
        return {dash(), QStringLiteral("已关闭"), QStringLiteral("长按已关闭"),
                true};
    }
    if (spec == "repeat" || spec == "repeat_accel" ||
        (spec.isEmpty() && act.value("hold_repeat").toInt() > 0)) {
        const bool accel =
            spec == "repeat_accel" ||
            (spec.isEmpty() && act.value("hold_accel").toBool());
        if (act.value("type").toString() == "scroll") {
            return {accel ? QStringLiteral("连滚加速")
                          : QStringLiteral("连续滚动"),
                    accel ? QStringLiteral("越按越快")
                          : QStringLiteral("按住重复"),
                    QStringLiteral("按住重复当前滚动") +
                        (accel ? QStringLiteral("，越按越快") : QString()),
                    true};
        }
        return {accel ? QStringLiteral("连发加速")
                      : QStringLiteral("连续触发"),
                accel ? QStringLiteral("越按越快")
                      : QStringLiteral("按住重复"),
                QStringLiteral("按住重复当前动作") +
                    (accel ? QStringLiteral("，越按越快") : QString()),
                true};
    }
    if (!spec.isEmpty()) {
        const QJsonObject holdAct = profile.actions.value(spec).toObject();
        QString title = holdAct.value("label").toString();
        if (title.isEmpty()) {
            title = Presets::actionCn(spec);
        }
        if (title.isEmpty()) {
            title = spec;
        }
        return {title, QStringLiteral("按住触发"),
                QStringLiteral("长按 ") + title +
                    QStringLiteral(" · 按住 0.6 秒后触发"),
                true};
    }
    if (act.value("hold_action").isObject()) {
        const QJsonObject holdAct = act.value("hold_action").toObject();
        QString title = holdAct.value("label").toString();
        if (title.isEmpty()) {
            title = Presets::describeAction(holdAct, false);
        }
        return {title, QStringLiteral("按住触发"),
                QStringLiteral("长按 ") + title +
                    QStringLiteral(" · 按住 0.6 秒后触发"),
                true};
    }
    return {dash(), {}, QStringLiteral("长按无动作，点击选择常用动作"), true};
}

void KeymapPage::openMenu(int vk, const QString &gesture) {
    const Profile &profile = currentProfile();
    const MapCellData current = gesture == "single" ? singleCell(vk, profile)
                                : gesture == "double"
                                    ? doubleCell(vk, profile)
                                    : holdCell(vk, profile);
    QMenu menu(this);
    auto *head = menu.addAction(QStringLiteral("当前：") + current.title +
                                (current.effect.isEmpty()
                                     ? QString()
                                     : QStringLiteral(" · ") + current.effect));
    head->setEnabled(false);
    menu.addSeparator();

    if (gesture == "single") {
        auto *custom = menu.addAction(QStringLiteral("自定义组合键…"));
        connect(custom, &QAction::triggered, this,
                [this, vk, gesture] { captureCombo(vk, gesture); });
        auto *pass = menu.addAction(QStringLiteral("原生直通"));
        connect(pass, &QAction::triggered, this,
                [this, vk, gesture] { applyPreset(vk, gesture, "pass"); });
        menu.addSeparator();
        for (const auto &group : Presets::groups()) {
            auto *sub = menu.addMenu(group.first);
            for (const QString &id : group.second) {
                auto *action = sub->addAction(Presets::actionLabel(id));
                connect(action, &QAction::triggered, this,
                        [this, vk, gesture, id] {
                            applyPreset(vk, gesture, id);
                        });
            }
        }
    } else if (gesture == "double") {
        auto *custom = menu.addAction(QStringLiteral("自定义组合键…"));
        connect(custom, &QAction::triggered, this,
                [this, vk, gesture] { captureCombo(vk, gesture); });
        auto *close = menu.addAction(QStringLiteral("关闭双击"));
        connect(close, &QAction::triggered, this,
                [this, vk, gesture] { applyPreset(vk, gesture, "none"); });
        menu.addSeparator();
        auto *modes = menu.addMenu(QStringLiteral("双击模式"));
        for (const QString &id : Presets::doubleModeIds()) {
            auto *action = modes->addAction(Presets::actionLabel(id));
            connect(action, &QAction::triggered, this,
                    [this, vk, gesture, id] { applyPreset(vk, gesture, id); });
        }
        menu.addSeparator();
        for (const auto &group : Presets::groups()) {
            auto *sub = menu.addMenu(group.first);
            for (const QString &id : group.second) {
                auto *action = sub->addAction(Presets::actionLabel(id));
                connect(action, &QAction::triggered, this,
                        [this, vk, gesture, id] {
                            applyPreset(vk, gesture, id);
                        });
            }
        }
    } else {
        auto *custom = menu.addAction(QStringLiteral("自定义组合键…"));
        connect(custom, &QAction::triggered, this,
                [this, vk, gesture] { captureCombo(vk, gesture); });
        auto *close = menu.addAction(QStringLiteral("关闭长按"));
        connect(close, &QAction::triggered, this,
                [this, vk, gesture] { applyPreset(vk, gesture, "none"); });
        auto *repeat = menu.addAction(QStringLiteral("连续触发（按住重复）"));
        connect(repeat, &QAction::triggered, this,
                [this, vk, gesture] { applyPreset(vk, gesture, "repeat"); });
        auto *accel =
            menu.addAction(QStringLiteral("连发加速（按住越来越快）"));
        connect(accel, &QAction::triggered, this,
                [this, vk, gesture] { applyPreset(vk, gesture, "repeat_accel"); });
        menu.addSeparator();
        auto *hold = menu.addMenu(QStringLiteral("按住动作"));
        for (const QString &id : Presets::holdIds()) {
            auto *action = hold->addAction(Presets::actionLabel(id));
            connect(action, &QAction::triggered, this,
                    [this, vk, gesture, id] { applyPreset(vk, gesture, id); });
        }
        auto *apps = menu.addMenu(QStringLiteral("启动应用"));
        for (const QString &id : Presets::appKinds()) {
            auto *action = apps->addAction(Presets::actionLabel(id));
            connect(action, &QAction::triggered, this,
                    [this, vk, gesture, id] { applyPreset(vk, gesture, id); });
        }
    }
    menu.exec(QCursor::pos());
}

void KeymapPage::applyPreset(int vk, const QString &gesture,
                             const QString &kind) {
    const QString profileName = currentProfile().name;
    if (!store_->apply(profileName, vk, gesture, kind)) {
        flashHint(QStringLiteral("保存失败"), QStringLiteral("#f85149"));
        return;
    }
    refreshAll();
    emit mappingsChanged();
    QString label;
    if (kind == "none") {
        label = gesture == "double" ? QStringLiteral("已关闭")
                                    : QStringLiteral("已关闭");
    } else if (kind == "repeat") {
        label = QStringLiteral("连续触发");
    } else if (kind == "repeat_accel") {
        label = QStringLiteral("连发加速");
    } else {
        label = Presets::actionLabel(kind);
    }
    flashHint(QStringLiteral("已保存 · %1 · %2 %3 → %4")
                  .arg(profileName, keyName(vk), gestureLabel(gesture), label),
              QStringLiteral("#7ee2a8"));
}

void KeymapPage::captureCombo(int vk, const QString &gesture) {
    ComboCaptureDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted || dialog.vk() == 0) {
        return;
    }
    QJsonObject action{
        {"type", "key"},
        {"mods", QJsonArray::fromStringList(dialog.mods())},
        {"vk", dialog.vk()},
        {"label", dialog.combo()},
    };
    const QString profileName = currentProfile().name;
    if (!store_->apply(profileName, vk, gesture, "custom", action)) {
        flashHint(QStringLiteral("保存失败"), QStringLiteral("#f85149"));
        return;
    }
    refreshAll();
    emit mappingsChanged();
    flashHint(QStringLiteral("已保存 · %1 · %2 %3 → %4")
                  .arg(profileName, keyName(vk), gestureLabel(gesture),
                       dialog.combo()),
              QStringLiteral("#7ee2a8"));
}

void KeymapPage::syncTakeoverUi() {
    if (takeoverSwitch_ == nullptr) {
        return;
    }
    const bool on = store_->uiBool("native_takeover", false);
    takeoverSwitch_->setChecked(on);
    takeoverLabel_->setStyleSheet(
        QStringLiteral("QLabel#mapTakeover { color: %1; font-size: 10px; }")
            .arg(on ? QStringLiteral("#9ec1ff")
                    : QStringLiteral("#6b7684")));
}

void KeymapPage::flashHint(const QString &text, const QString &color) {
    footHint_->setText(text);
    footHint_->setStyleSheet(
        QStringLiteral("QLabel#mapFoot { color: %1; font-size: 10px; }")
            .arg(color));
    hintTimer_->start(2600);
}

void KeymapPage::resetHint() {
    footHint_->setText(QStringLiteral("点击手势格改键 · 自动保存"));
    footHint_->setStyleSheet(
        QStringLiteral("QLabel#mapFoot { color: #5b6472; font-size: 10px; }"));
}

void KeymapPage::openProfiles() {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(store_->path()))) {
        QMessageBox::warning(this, QStringLiteral("打开失败"),
                             QStringLiteral("打不开 profiles.json"));
    }
}

void KeymapPage::reloadProfiles() {
    QString error;
    if (!store_->load(&error)) {
        flashHint(QStringLiteral("重载失败：") + error,
                  QStringLiteral("#f85149"));
        return;
    }
    rebuildCombo();
    refreshAll();
    flashHint(QStringLiteral("按键映射已重新加载"),
              QStringLiteral("#7ee2a8"));
}

const Profile &KeymapPage::currentProfile() {
    return store_->current(true);
}

QString KeymapPage::keyName(int vk) const {
    return names().value(vk, QString("0x%1").arg(vk, 2, 16, QChar('0')));
}

QString KeymapPage::gestureLabel(const QString &gesture) const {
    if (gesture == "single") return QStringLiteral("单击");
    if (gesture == "double") return QStringLiteral("双击");
    return QStringLiteral("长按");
}
