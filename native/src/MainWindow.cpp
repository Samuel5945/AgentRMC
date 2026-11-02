#include "MainWindow.h"

#include "ui/AppPage.h"
#include "ui/GuideDialog.h"
#include "ui/KeymapPage.h"
#include "ui/RemotePanel.h"
#include "ui/ThemeDialog.h"
#include "ui/Toast.h"
#include "ui/VoicePet.h"
#include "ui/VoiceSettingsDialog.h"
#include "ui/VoiceWindow.h"

#include "core/AgentBridge.h"
#include "core/PluginUpgrade.h"
#include "core/AgentWindow.h"
#include "core/AppLauncher.h"
#include "core/InputEngine.h"
#include "core/ModelImport.h"
#include "core/Presets.h"
#include "core/ThemeManager.h"
#include "core/Updater.h"
#include "core/VoiceController.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QIcon>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QSettings>
#include <QDateTime>
#include <QDialog>
#include <QPointer>
#include <QPair>
#include <QThreadPool>
#include <QCursor>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QSignalBlocker>
#include <QTime>
#include <QTimer>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <QVector>
#include <QWindow>

#include <algorithm>

namespace {

// 「使用引导」里的一句话安装话术：插件源和 zip 随安装包落在 exe 旁的 plugins\ 下，
// 用户不用认路径——复制这一句发给正在干活的 Agent，它自己装好并启用。
// 让 Agent 走宿主自己的安装器（install_extension）而不是手改注册表：本地包的版本号
// 只在文件名和清单里，装错了没人看得出来。
QString agentBridgeInstallSentence() {
    const QString root = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/plugins"));
    const QString zip = root + QStringLiteral("\\agentrmc-bridge.zip");
    const QString version = pluginupgrade::bundledVersion(
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/plugins/agentrmc-bridge"));
    return QString::fromUtf8(
               "帮我装或升级本机的 AgentRMC 桌宠联动插件 agentrmc-bridge（内置版本 %3），装好并启用。"
               "Qoder / Qoder CN：用你手上的 Qoder 设置安装器装本地包 —— install_extension，"
               "参数 source=local、kind=plugin、filePath=%1；"
               "国际版和 Qoder CN 是两个独立 app，各装一次，装完重启那个 app 才真加载。"
               "没有安装器时才手工：侧边栏 扩展 → 插件 → 添加插件 → 上传插件，选同一个 zip。"
               "ZCode / WorkBuddy：在应用市场添加目录 %2（内含 marketplace.json）。"
               "装完把两边各自的版本报给我。之后在我等确认或任务结束时用桌宠提醒我。")
        .arg(zip, root, version.isEmpty() ? QStringLiteral("未知") : version);
}

class TitleBar final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            if (auto *handle = window()->windowHandle()) {
                handle->startSystemMove();
                event->accept();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }
};

// 全局语音热键（无遥控器触发用）的注册 ID。
constexpr UINT kVoiceHotkeyId = 0xB00B;

// 主界面底色：主题带背景图就铺满整窗再压一层暗色遮罩。
// 以前只有语音面板做了遮罩，主窗直接把 11px 灰字压在花纹上，正文基本读不了。
// 这里给一个下限——主窗是信息密集界面，主题作者写的 dim 只当「想更暗」的加成。
class BackdropRoot final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override {
        const Theme &theme = ThemeManager::instance().current();
        if (theme.mainBackground.isEmpty()) {
            return;  // classic：QSS 的纯色底就够
        }
        const QImage art = ThemeManager::cachedImage(theme.mainBackground);
        if (art.isNull()) {
            return;
        }
        QPainter painter(this);
        const QImage scaled = art.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
        painter.drawImage(QPoint((width() - scaled.width()) / 2,
                                 (height() - scaled.height()) / 2),
                          scaled);
        const qreal dim = ThemeManager::instance().backdropDim();
        painter.fillRect(rect(), QColor(11, 14, 20, int(255 * dim)));
    }
};

// 待办行：状态色条 + 一行「Agent · 阶段 —— 时间」+ 一行消息。
// 原来整行一个颜色、状态用方括号塞在正文里，扫十行找不出哪条在等自己。
class WorkItemDelegate final : public QStyledItemDelegate {
public:
    explicit WorkItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(0, 56);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setClipRect(option.rect);

        const QRectF row = QRectF(option.rect).adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(selected ? QPen(QColor(79, 140, 255, 150), 1.0)
                                     : QPen(Qt::NoPen));
            painter->setBrush(selected ? QColor(79, 140, 255, 44)
                                       : QColor(255, 255, 255, 10));
            painter->drawRoundedRect(row, 10, 10);
        }

        const QColor stage = index.data(Qt::UserRole + 2).value<QColor>();
        painter->setPen(Qt::NoPen);
        painter->setBrush(stage);
        painter->drawRoundedRect(
            QRectF(row.left() + 9, row.top() + 12, 3, row.height() - 24), 1.5,
            1.5);

        const qreal textLeft = row.left() + 21;
        const qreal textRight = row.right() - 12;

        const QString agent = index.data(Qt::UserRole + 4).toString();
        const QString stageText = index.data(Qt::UserRole + 3).toString();
        const QString when = index.data(Qt::UserRole + 5).toString();
        const QString message = index.data(Qt::DisplayRole).toString();

        QFont nameFont = painter->font();
        nameFont.setPixelSize(12);
        nameFont.setWeight(QFont::DemiBold);
        QFont metaFont = painter->font();
        metaFont.setPixelSize(10);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics metaMetrics(metaFont);

        const qreal timeWidth = metaMetrics.horizontalAdvance(when) + 12;
        const QRectF line1(textLeft, row.top() + 9,
                           textRight - textLeft - timeWidth, 16);

        painter->setFont(metaFont);
        painter->setPen(QColor(0x7c, 0x87, 0x98));
        painter->drawText(line1, Qt::AlignRight | Qt::AlignVCenter, when);

        const qreal stageWidth = metaMetrics.horizontalAdvance(stageText);
        const qreal nameRoom =
            qMax<qreal>(40, line1.width() - stageWidth - 14);
        painter->setFont(nameFont);
        painter->setPen(QColor(0xe6, 0xed, 0xf3));
        const QString shownAgent =
            nameMetrics.elidedText(agent, Qt::ElideRight, int(nameRoom));
        painter->drawText(line1, Qt::AlignLeft | Qt::AlignVCenter, shownAgent);
        const qreal agentWidth = nameMetrics.horizontalAdvance(shownAgent);

        painter->setFont(metaFont);
        painter->setPen(stage);
        painter->drawText(QRectF(line1.left() + agentWidth + 7, line1.top(),
                                 stageWidth + 4, line1.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, stageText);

        QFont bodyFont = painter->font();
        bodyFont.setPixelSize(11);
        painter->setFont(bodyFont);
        painter->setPen(QColor(0xa9, 0xb6, 0xc6));
        const QRectF line2(textLeft, row.top() + 29,
                           textRight - textLeft, 16);
        painter->drawText(
            line2, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(bodyFont).elidedText(message.simplified(),
                                              Qt::ElideRight,
                                              int(line2.width())));
        painter->restore();
    }
};

// 统计小胶囊：等你 / 待验收 / 进行中。
QLabel *makeChip(const QString &label, const QString &color,
                 const QString &border, const QString &fill) {
    auto *chip = new QLabel(label);
    chip->setStyleSheet(
        QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid %3;"
                       " border-radius: 9px; padding: 1px 8px; font-size: 10.5px;"
                       " font-weight: 600; }")
            .arg(color, fill, border));
    chip->hide();
    return chip;
}

}  // namespace

MainWindow::MainWindow(bool withTray, const QString &pin, bool autoListen,
                       QWidget *parent)
    : QMainWindow(parent) {
    store_ = std::make_unique<ProfileStore>(
        QCoreApplication::applicationDirPath() + "/profiles.json");
    store_->load();
    store_->loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
    // Work Item 落在用户数据目录，不跟安装目录走：升级安装不会覆盖，
    // 也不会被 installer 打进包里。
    QString workDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (workDir.isEmpty()) {
        workDir = QCoreApplication::applicationDirPath();
    }
    QDir().mkpath(workDir);
    workItems_ = std::make_unique<WorkItemStore>(workDir + "/work-items.json");
    workItems_->load();
    spoolPath_ = workDir + "/pending-events.jsonl";
    startedAt_ = QDateTime::currentDateTimeUtc();
    // 主题在窗口构建前应用：渲染 QSS + 载入 ui.theme 指向的主题。
    ThemeManager::instance().init(store_.get());
    // ui.custom_apps 里的自定义应用灌进启动目录（应用页/语音「打开 xx」共用）。
    AppLauncher::setCustomApps(
        AppLauncher::parseCustomApps(store_->uiArray("custom_apps")));
    if (!pin.isEmpty()) {
        store_->setPinned(pin);
    }
    engine_ = new InputEngine(store_.get(), this);
    voice_ = new VoiceController(store_.get(), this);
    connect(engine_, &InputEngine::eventReady, this, &MainWindow::onEngineEvent);
    connect(engine_, &InputEngine::runningChanged, this,
            &MainWindow::onEngineRunning);
    connect(engine_, &InputEngine::runningChanged, this,
            [this](bool running, const QString &) {
                voice_->setListening(running);
            });
    connect(engine_, &InputEngine::profileChanged, this,
            [this](const QString &name) {
                sceneName_ = name;
                updateTrayState();
                refreshRemoteCard();
                if (!voice_->busy()) {
                    refreshVoiceUi();
                }
                if (remoteState_ != nullptr && engine_->isRunning()) {
                    remoteState_->setText(
                        QString::fromUtf8("监听中 · ") +
                        voiceProfileLabel(store_->current(true).name));
                }
            });
    connect(engine_, &InputEngine::voiceDown, voice_, &VoiceController::voiceDown);
    connect(engine_, &InputEngine::voiceDown, this, [this] {
        if (remotePanel_ != nullptr) {
            remotePanel_->flash(116);
        }
        if (remoteState_ != nullptr && remotePanel_ != nullptr) {
            remoteState_->setText(QString::fromUtf8("语音 · ") +
                                  remotePanel_->actionFor(116));
        }
    });
    connect(engine_, &InputEngine::voiceUp, voice_, &VoiceController::voiceUp);
    connect(engine_, &InputEngine::mediaVoice, voice_,
            &VoiceController::mediaToggle);
    connect(engine_, &InputEngine::mediaVoice, this, [this] {
        if (remoteState_ != nullptr) {
            remoteState_->setText(QString::fromUtf8("耳机 · 语音开关"));
        }
    });
    connect(voice_, &VoiceController::stateChanged, this,
            &MainWindow::onVoiceState);
    connect(voice_, &VoiceController::transcribed, this,
            &MainWindow::onVoiceTranscribed);
    connect(voice_, &VoiceController::eventReady, this,
            [this](const QString &kind, const QString &text) {
                appendEvent(kind, QString(), QString(), text, 0);
            });
    setWindowTitle("AgentRMC Native");
    setWindowIcon(QIcon(":/icons/icon.ico"));
    setAcceptDrops(true);
    dropHint_ = new QLabel(QString::fromUtf8("松手，导入这个模型包"), this);
    dropHint_->setAlignment(Qt::AlignCenter);
    dropHint_->setStyleSheet(QStringLiteral(
        "background: rgba(11,14,20,225); color: #e6edf3; font-size: 18px;"
        " border: 2px dashed rgba(79,140,255,190); border-radius: 12px;"));
    dropHint_->hide();
    qApp->installEventFilter(this);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    // 再窄左栏就挤不下了：语音卡 + 遥控器卡是固定高度，列表也需要宽度放时间列。
    setMinimumSize(940, 620);

    auto *root = new BackdropRoot;
    root->setObjectName("root");
    // 主题背景图走 QSS background-image；plain QWidget 显式声明按样式表绘制。
    root->setAttribute(Qt::WA_StyledBackground, true);
    setCentralWidget(root);
    // 换主题时 QSS 会重刷，但背景是我们自己画的，得手动重绘一次。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, root,
            [root](const Theme &) { root->update(); });

    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(buildHeader());

    auto *body = new QWidget;
    auto *grid = new QHBoxLayout(body);
    grid->setContentsMargins(14, 12, 14, 14);
    grid->setSpacing(12);

    auto *left = new QWidget;
    left->setFixedWidth(286);
    auto *leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(12);
    // 语音在上面：主操作「开始监听」和识别方式是一打开就要用的；
    // 遥控器是可选的次要设备，放到下面并吃掉剩余高度。
    leftLay->addWidget(buildVoiceCard(), 0);
    leftLay->addWidget(buildRemoteCard(), 1);
    grid->addWidget(left);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(false);
    tabs_->addTab(buildWorkPage(), QString::fromUtf8("待我处理"));
    tabs_->addTab(buildLogPage(), QString::fromUtf8("事件流"));
    tabs_->addTab(buildSystemPage(), QString::fromUtf8("系统"));
    keymapPage_ = new KeymapPage(store_.get(), this);
    connect(keymapPage_, &KeymapPage::mappingsChanged, this,
            &MainWindow::refreshRemoteCard);
    connect(keymapPage_, &KeymapPage::viewChanged, this,
            &MainWindow::refreshRemoteCard);
    connect(keymapPage_, &KeymapPage::keySelected, this, [this](int vk) {
        if (remotePanel_ != nullptr) {
            remotePanel_->setLinkedVk(vk);
        }
    });
    connect(keymapPage_, &KeymapPage::keyHovered, this,
            [this](int vk, bool on) {
                if (remotePanel_ != nullptr) {
                    remotePanel_->setLinkedVk(on ? vk : keymapPage_->selectedVk());
                }
            });
    tabs_->addTab(keymapPage_, QString::fromUtf8("按键映射"));
    appPage_ = new AppPage(store_.get());
    connect(appPage_, &AppPage::launched, this,
            [this](const QString &name, bool ok, const QString &error) {
                if (ok) {
                    appendEvent(QStringLiteral("voice"), QString(), QString(),
                                QStringLiteral("已打开 ") + name, 0);
                    if (toast_ != nullptr) {
                        toast_->showText(QStringLiteral("已打开 ") + name, 1800);
                    }
                } else {
                    appendEvent(QStringLiteral("error"), QString(), QString(),
                                QStringLiteral("打不开 ") + name +
                                    (error.isEmpty() ? QString()
                                                     : QStringLiteral(" · ") + error),
                                0);
                    if (toast_ != nullptr) {
                        toast_->showText(QStringLiteral("打不开 ") + name, 2400);
                    }
                }
            });
    tabs_->addTab(appPage_, QString::fromUtf8("应用"));
    tabs_->addTab(buildGuidePage(), QString::fromUtf8("使用引导"));
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (remotePanel_ != nullptr) {
            const int keymapIndex =
                keymapPage_ != nullptr ? tabs_->indexOf(keymapPage_) : -1;
            remotePanel_->setLinkedVk(
                index == keymapIndex && keymapPage_ != nullptr
                    ? keymapPage_->selectedVk()
                    : 0);
        }
    });
    // 打开先看待办：有人等你确认才是需要你回来处理的事；没有待办时才落到按键映射。
    const bool hasAttention = workItems_ != nullptr &&
                              workItems_->attentionCount() > 0;
    tabs_->setCurrentIndex(hasAttention || keymapPage_ == nullptr
                               ? 0
                               : tabs_->indexOf(keymapPage_));
    refreshWorkItems();
    grid->addWidget(tabs_, 1);

    outer->addWidget(body, 1);

    toast_ = new Toast(this);
    // 语音桌宠：主窗/语音面板关掉之后留在屏幕上的小球，跟着监听状态变色。
    voicePet_ = new VoicePet(store_.get(), voice_);
    // 对话里问「任务状态」时直接给模型真实待办，别让它编造工具调用。
    voicePet_->setWorkSummaryProvider([this] {
        if (workItems_ == nullptr) {
            return QString();
        }
        int needs = 0;
        int review = 0;
        QString latest;
        for (const WorkItem &item : workItems_->items()) {
            if (item.done) {
                continue;
            }
            if (item.stage == QStringLiteral("needs-me")) {
                ++needs;
                if (latest.isEmpty()) {
                    latest = item.message;
                }
            } else if (item.stage == QStringLiteral("review")) {
                ++review;
                if (latest.isEmpty()) {
                    latest = item.message;
                }
            }
        }
        if (needs == 0 && review == 0) {
            return QString::fromUtf8("没有需要用户处理的待办");
        }
        QString summary =
            QString::fromUtf8("%1 件等你处理、%2 件待验收")
                .arg(needs)
                .arg(review);
        if (!latest.isEmpty()) {
            summary += QString::fromUtf8("；最近一条：%1").arg(latest);
        }
        return summary;
    });
    connect(voicePet_, &VoicePet::fullUiRequested, this, &MainWindow::wake);
    connect(voicePet_, &VoicePet::panelRequested, this,
            &MainWindow::showVoicePanel);
    connect(voicePet_, &VoicePet::listenToggleRequested, this,
            &MainWindow::toggleListening);
    // 语音派活成功：先落一条 working，后续 permission/stop 会合并到同一张卡。
    connect(voicePet_, &VoicePet::workHandedOff, this,
            [this](const QString &exe, const QString &text) {
                if (workItems_ == nullptr) {
                    return;
                }
                workItems_->upsert(QStringLiteral("voice"), exe, QString(),
                                   QString(), QStringLiteral("voice"),
                                   AgentWindow::displayName(exe), text, exe);
                refreshWorkItems();
            });
    // 桌宠收起时对话回复照常给出：语音面板转写框 + 事件流。
    connect(voicePet_, &VoicePet::chatReplied, this, [this](const QString &text) {
        if (voiceWindow_ != nullptr) {
            voiceWindow_->showChatReply(text);
        }
        appendEvent(QStringLiteral("voice"), QString(), QString(),
                    QString::fromUtf8("鲸鱼娘：%1").arg(text), 0);
    });
    // 点气泡回跳：提醒不再靠自动抢前台，用户点了才把宿主窗口拉回来。
    connect(voicePet_, &VoicePet::bubbleActivated, this,
            [](const QString &exe) { AgentWindow::raise(exe); });
    connect(voicePet_, &VoicePet::voiceFeedbackError, this,
            [this](const QString &error) {
                appendEvent(QStringLiteral("error"), QString(), QString(),
                            QString::fromUtf8("语音反馈失败：%1").arg(error), 0);
            });
    connect(voicePet_, &VoicePet::hiddenByUser, this, [this] {
        if (petAction_ != nullptr) {
            const QSignalBlocker block(petAction_);
            petAction_->setChecked(false);
        }
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QString::fromUtf8("语音桌宠已关闭；托盘菜单里可重新打开"), 0);
    });
    // 桌宠/语音面板里切了识别方式时，主窗自己的控件也要跟上。
    connect(voice_, &VoiceController::optionsChanged, this, [this] {
        refreshVoiceUi();
        refreshRemoteCard();
    });
    refreshRemoteCard();
    for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
        button->setFocusPolicy(Qt::NoFocus);
    }
    if (autoListen && store_->uiBool("auto_listen", true)) {
        QTimer::singleShot(0, this, &MainWindow::toggleListening);
    }
    if (withTray) {
        QTimer::singleShot(12000, this, [this] {
            QSettings settings;
            const qint64 last = settings
                                    .value(QStringLiteral("update/last_check"), 0)
                                    .toLongLong();
            if (QDateTime::currentSecsSinceEpoch() - last < 12 * 3600) {
                return;
            }
            checkUpdates(false);
        });
    }
    if (withTray) {
        buildTray();
    }
    if (withTray) {
        // ZCode 等 Agent 的进度通知与交互：桌宠在就冒气泡，不在就托盘弹一条；
        // speak 走气泡 + TTS（桌宠收起也照念）。截图模式不起（会跟正在跑的真实例抢端口）。
        auto *bridge = new AgentBridge(this);
        bridge_ = bridge;
        bridgeRequested_ = true;
        bridge->setStateProvider([this] {
            return voicePet_ != nullptr ? voicePet_->stateJson()
                                        : QStringLiteral("{}");
        });
        bridge->setHealthProvider([this] { return healthJson(); });
        bridge->setWorkItemsProvider([this] {
            return workItems_ != nullptr ? workItems_->toJson()
                                         : QStringLiteral("{}");
        });
        bridge->setWorkItemAckHandler([this](const QString &id) {
            if (workItems_ == nullptr || !workItems_->ack(id)) {
                return false;
            }
            refreshWorkItems();
            return true;
        });
        bridge->setWorkersProvider([this] { return workersJson(); });
        bridge->setWorkItemUpdateHandler(
            [this](const QJsonObject &payload) { return reportWorkItem(payload); });
        connect(bridge, &AgentBridge::notify, this,
                [this](const QString &kind, const QString &title,
                       const QString &message, const QString &raise,
                       const QString &sessionId, const QString &project,
                       const QString &eventId,
                       const QString &pluginVersion,
                       const QString &speech) {
                    handleNotify(kind, title, message, raise, sessionId,
                                 project, eventId, pluginVersion, speech,
                                 false);
                });
        if (!bridge->start()) {
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QString::fromUtf8(
                            "桥接端口 45915 被占用，5 秒后自动重试"),
                        0);
        }
    }
    // 离线 spool 补投 + 健康行；5 秒一拍，启动时先补一次。
    healthTimer_ = new QTimer(this);
    connect(healthTimer_, &QTimer::timeout, this, &MainWindow::watchdogTick);
    healthTimer_->start(5000);
    QTimer::singleShot(0, this, &MainWindow::watchdogTick);
}

QWidget *MainWindow::buildHeader() {
    auto *bar = new TitleBar;
    bar->setObjectName("header");
    bar->setFixedHeight(44);

    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(16, 8, 10, 4);
    lay->setSpacing(10);

    auto *logo = new QLabel("A");
    logo->setObjectName("brandLogo");
    logo->setAlignment(Qt::AlignCenter);
    logo->setFixedSize(22, 22);

    auto *brand = new QLabel("AgentRMC");
    brand->setObjectName("brand");

    lay->addWidget(logo);
    lay->addWidget(brand);
    lay->addStretch(1);

    // 监听状态做成一颗胶囊：整窗最该一眼看到的状态，不该是裸灰字。
    auto *pill = new QWidget;
    pill->setObjectName("statusPill");
    pill->setAttribute(Qt::WA_StyledBackground, true);
    auto *pillLay = new QHBoxLayout(pill);
    pillLay->setContentsMargins(9, 3, 11, 3);
    pillLay->setSpacing(6);
    statusDot_ = new QLabel;
    statusDot_->setStyleSheet(
        QStringLiteral("background: #4a5568; border-radius: 3px;"));
    statusDot_->setFixedSize(6, 6);
    statusText_ = new QLabel(QString::fromUtf8("未监听"));
    statusText_->setObjectName("statusText");
    pillLay->addWidget(statusDot_, 0, Qt::AlignVCenter);
    pillLay->addWidget(statusText_);
    lay->addWidget(pill, 0, Qt::AlignVCenter);
    lay->addSpacing(4);

    updateButton_ = new QPushButton(QString::fromUtf8("检查更新"));
    updateButton_->setObjectName("ghostText");
    updateButton_->setCursor(Qt::PointingHandCursor);
    updateButton_->setToolTip(QString::fromUtf8("检查是否有新版本"));
    connect(updateButton_, &QPushButton::clicked, this,
            [this] { checkUpdates(true); });
    lay->addWidget(updateButton_);

    // 换肤 + 调卡片透明度 / 背景压暗。以前只在「语音设置」里有个下拉，
    // 用户找不到，也调不了「字看不清」这件事。
    auto *themeButton = new QPushButton(QString::fromUtf8("主题"));
    themeButton->setObjectName("ghostText");
    themeButton->setCursor(Qt::PointingHandCursor);
    themeButton->setToolTip(
        QString::fromUtf8("切换主题、调卡片不透明度与背景压暗"));
    connect(themeButton, &QPushButton::clicked, this,
            &MainWindow::openThemeDialog);
    lay->addWidget(themeButton);

    // 分步图文引导（配对遥控器 / 选识别方式 / 接入 Agent）。
    // 「使用引导」那一页是纯文字速查表，两个入口靠名字区分开。
    auto *guideButton = new QPushButton(QString::fromUtf8("新手引导"));
    guideButton->setObjectName("ghost");
    guideButton->setCursor(Qt::PointingHandCursor);
    guideButton->setToolTip(
        QString::fromUtf8("分步引导：没有遥控器怎么用、怎么配对、怎么接入 Agent"));
    connect(guideButton, &QPushButton::clicked, this, &MainWindow::showGuide);
    lay->addWidget(guideButton);

    auto *min = new QToolButton;
    min->setObjectName("winBtn");
    min->setText(QString::fromUtf8("—"));
    min->setFixedSize(28, 24);
    min->setCursor(Qt::PointingHandCursor);

    auto *close = new QToolButton;
    close->setObjectName("closeBtn");
    close->setText(QString::fromUtf8("✕"));
    close->setFixedSize(28, 24);
    close->setCursor(Qt::PointingHandCursor);

    connect(min, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(close, &QToolButton::clicked, this, &QWidget::close);
    lay->addWidget(min);
    lay->addWidget(close);
    return bar;
}

QWidget *MainWindow::buildRemoteCard() {
    auto *card = new QFrame;
    card->setObjectName("card");
    card->setFixedWidth(286);
    // 遥控器是可选设备：卡片别跟着窗口一起长高，否则中间一大片空。
    card->setMaximumHeight(392);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(8);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("遥控器"));
    title->setObjectName("cardTitle");
    remoteBadge_ = new QLabel(QString::fromUtf8("待机"));
    remoteBadge_->setObjectName("badge");
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(remoteBadge_);
    lay->addLayout(head);

    remotePanel_ = new RemotePanel;
    lay->addWidget(remotePanel_, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(7);
    remoteDot_ = new QLabel;
    remoteDot_->setObjectName("dot");
    remoteDot_->setFixedSize(6, 6);
    remoteState_ = new QLabel(QString::fromUtf8("未监听"));
    remoteState_->setObjectName("muted");
    auto *bt = new QPushButton(QString::fromUtf8("蓝牙设置"));
    bt->setObjectName("ghostSmall");
    bt->setCursor(Qt::PointingHandCursor);
    bt->setToolTip(QString::fromUtf8("打开 Windows 蓝牙设置"));
    connect(bt, &QPushButton::clicked, this, [this] {
        if (!QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:bluetooth")))) {
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QString::fromUtf8("打不开系统蓝牙设置"), 0);
        }
    });
    foot->addWidget(remoteDot_, 0, Qt::AlignVCenter);
    foot->addWidget(remoteState_);
    foot->addStretch(1);
    foot->addWidget(bt);
    lay->addLayout(foot);

    refreshRemoteCard();
    return card;
}

QWidget *MainWindow::buildVoiceCard() {
    auto *card = new QFrame;
    card->setObjectName("card");

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(8);

    // 标题行右侧直接放识别方式：少一行、少一个「选什么」的疑问。
    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    auto *title = new QLabel(QString::fromUtf8("语音"));
    title->setObjectName("cardTitle");
    head->addWidget(title);
    head->addStretch(1);

    voiceBackend_ = new QComboBox;
    voiceBackend_->addItem(QString::fromUtf8("转发热键 · 0 模型"), "hotkey");
    voiceBackend_->addItem(QString::fromUtf8("本地 SenseVoice"), "sensevoice");
    voiceBackend_->addItem(QString::fromUtf8("本地 Qwen3-ASR"), "qwen3");
    voiceBackend_->addItem(QString::fromUtf8("云端 ASR（需联网）"), "api");
    voiceBackend_->setCursor(Qt::PointingHandCursor);
    voiceBackend_->setToolTip(
        QString::fromUtf8("语音怎么识别：转发当前应用的快捷键，或本地 / 云端模型"));
    const int backendIndex = voiceBackend_->findData(
        voiceBackendKey(voice_->options().backend));
    voiceBackend_->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
    connect(voiceBackend_, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                const QString key = voiceBackend_->itemData(index).toString();
                store_->updateVoice(QJsonObject{{"backend", key}});
                voice_->reload();
                lastTranscript_.clear();
                refreshVoiceUi();
                refreshRemoteCard();
                if (key == QStringLiteral("api") && !voice_->modelReady()) {
                    toast_->showText(
                        QString::fromUtf8(
                            "云端 ASR 还没配好：语音设置 → 云端 ASR 填模型和 Key"),
                        3600);
                }
                appendEvent(QStringLiteral("status"), QString(),
                            QString(),
                            QString::fromUtf8("语音后端切换为 ") +
                                voiceBackend_->itemText(index),
                            0);
            });
    head->addWidget(voiceBackend_, 0);
    lay->addLayout(head);

    voiceState_ = new QLabel;
    voiceState_->setObjectName("voiceBox");
    voiceState_->setWordWrap(true);
    voiceState_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    voiceState_->setMinimumHeight(48);
    lay->addWidget(voiceState_, 1);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(6);
    voiceTest_ = new QPushButton(QString::fromUtf8("试一句"));
    voiceTest_->setToolTip(QString::fromUtf8(
        "录 3 秒并转写一次，用来确认本地模型是否正常"));
    voiceTest_->setObjectName("ghostSmall");
    voiceTest_->setCursor(Qt::PointingHandCursor);
    connect(voiceTest_, &QPushButton::clicked, voice_,
            &VoiceController::startTest);
    auto *settings = new QPushButton(QString::fromUtf8("模型设置"));
    settings->setObjectName("ghostSmall");
    settings->setCursor(Qt::PointingHandCursor);
    connect(settings, &QPushButton::clicked, this,
            &MainWindow::openVoiceSettings);
    auto *voicePanel = new QPushButton(QString::fromUtf8("语音主界面"));
    voicePanel->setObjectName("ghostSmall");
    voicePanel->setCursor(Qt::PointingHandCursor);
    voicePanel->setToolTip(QString::fromUtf8(
        "切到无遥控器的简洁语音界面：大按钮一键说话"));
    connect(voicePanel, &QPushButton::clicked, this,
            &MainWindow::showVoicePanel);
    tools->addWidget(voiceTest_);
    tools->addWidget(settings);
    tools->addWidget(voicePanel);
    tools->addStretch(1);
    lay->addLayout(tools);

    listenButton_ = new QPushButton(QString::fromUtf8("开始监听"));
    listenButton_->setObjectName("primary");
    listenButton_->setCursor(Qt::PointingHandCursor);
    connect(listenButton_, &QPushButton::clicked, this,
            &MainWindow::toggleListening);
    lay->addWidget(listenButton_);

    auto *foot = new QHBoxLayout;
    voiceFoot_ = new QLabel;
    voiceFoot_->setObjectName("muted");
    foot->addWidget(voiceFoot_);
    foot->addStretch(1);
    lay->addLayout(foot);


    refreshVoiceUi();
    return card;
}

QWidget *MainWindow::buildLogPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *card = new QFrame;
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(14, 11, 12, 12);
    cl->setSpacing(10);

    auto *head = new QHBoxLayout;
    logCount_ = new QLabel(QString::fromUtf8("0 条"));
    logCount_->setObjectName("workCount");
    auto *clear = new QPushButton(QString::fromUtf8("清空"));
    clear->setObjectName("ghostText");
    clear->setCursor(Qt::PointingHandCursor);
    connect(clear, &QPushButton::clicked, this, &MainWindow::clearLog);
    head->addWidget(logCount_);
    head->addStretch(1);
    head->addWidget(clear);
    cl->addLayout(head);

    // 空状态给一句「这里会有什么」，比一个居中的「等待按键…」有用。
    auto *emptyBox = new QWidget;
    auto *emptyLay = new QVBoxLayout(emptyBox);
    emptyLay->setContentsMargins(0, 0, 0, 0);
    emptyLay->setSpacing(6);
    emptyLay->addStretch(1);
    logEmpty_ = new QLabel(QString::fromUtf8("还没有事件"));
    logEmpty_->setObjectName("emptyTitle");
    logEmpty_->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(logEmpty_);
    auto *emptyHint = new QLabel(QString::fromUtf8(
        "遥控器按键、语音转写、Agent 通知都会按时间记在这里，\n"
        "排查「按了没反应」时先看这一页。"));
    emptyHint->setObjectName("emptyHint");
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyLay->addWidget(emptyHint);
    emptyLay->addStretch(1);
    cl->addWidget(emptyBox, 1);

    logList_ = new QListWidget;
    logList_->setObjectName("logList");
    logList_->setFrameShape(QFrame::NoFrame);
    logList_->setSelectionMode(QAbstractItemView::NoSelection);
    logList_->setFocusPolicy(Qt::NoFocus);
    logList_->setWordWrap(true);
    logList_->setSpacing(2);
    logList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    logList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    logList_->hide();
    cl->addWidget(logList_, 1);
    lay->addWidget(card);
    return page;
}

QWidget *MainWindow::buildWorkPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *card = new QFrame;
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(16, 13, 14, 12);
    cl->setSpacing(9);

    // 一行说清「有多少、卡在哪」：总数 + 三个分类胶囊 + Worker 过滤 + 清理。
    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    workCount_ = new QLabel(QString::fromUtf8("没有待办"));
    workCount_->setObjectName("workCount");
    head->addWidget(workCount_);

    chipNeeds_ = makeChip(QString(), QStringLiteral("#f0883e"),
                          QStringLiteral("rgba(240,136,62,95)"),
                          QStringLiteral("rgba(240,136,62,26)"));
    chipReview_ = makeChip(QString(), QStringLiteral("#3fb950"),
                           QStringLiteral("rgba(63,185,80,95)"),
                           QStringLiteral("rgba(63,185,80,24)"));
    chipWorking_ = makeChip(QString(), QStringLiteral("#58a6ff"),
                            QStringLiteral("rgba(88,166,255,95)"),
                            QStringLiteral("rgba(88,166,255,24)"));
    head->addWidget(chipNeeds_);
    head->addWidget(chipReview_);
    head->addWidget(chipWorking_);
    head->addStretch(1);

    workFilter_ = new QComboBox;
    workFilter_->setMinimumWidth(132);
    workFilter_->setToolTip(QString::fromUtf8("只看某个 Agent 的待办"));
    connect(workFilter_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshWorkItems(); });
    head->addWidget(workFilter_);

    auto *clear = new QPushButton(QString::fromUtf8("清空已完成"));
    clear->setObjectName("ghostText");
    clear->setCursor(Qt::PointingHandCursor);
    clear->setToolTip(QString::fromUtf8("把已经处理完的记录从列表里删掉"));
    connect(clear, &QPushButton::clicked, this, [this] {
        if (workItems_ != nullptr) {
            workItems_->clearFinished();
        }
        refreshWorkItems();
    });
    head->addWidget(clear);
    cl->addLayout(head);

    // 系统健康压成一行：桥接 / Worker / 最近事件，异常才变色。
    workHealth_ = new QLabel;
    workHealth_->setObjectName("workHealth");
    workHealth_->setTextFormat(Qt::RichText);
    cl->addWidget(workHealth_);

    workList_ = new QListWidget;
    workList_->setObjectName("workList");
    workList_->setFrameShape(QFrame::NoFrame);
    workList_->setWordWrap(false);
    workList_->setSpacing(2);
    workList_->setMouseTracking(true);
    workList_->setUniformItemSizes(true);
    workList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    workList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    workList_->setItemDelegate(new WorkItemDelegate(workList_));
    workList_->setSelectionMode(QAbstractItemView::SingleSelection);
    workList_->hide();
    connect(workList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                if (item != nullptr) {
                    raiseWorkItem(item->data(Qt::UserRole).toString());
                }
            });
    connect(workList_, &QListWidget::itemSelectionChanged, this, [this] {
        const bool has = !selectedWorkId().isEmpty();
        if (workRaise_ != nullptr) {
            workRaise_->setEnabled(has);
        }
        if (workDone_ != nullptr) {
            workDone_->setEnabled(has);
        }
        if (workHint_ != nullptr) {
            workHint_->setText(
                has ? QString::fromUtf8("Enter 回到 Agent · Delete 标为已处理")
                    : QString::fromUtf8(
                          "选中一条待办后可操作 · 双击直接回到对应 Agent"));
        }
    });
    cl->addWidget(workList_, 1);

    // 空状态：说清楚这里会出现什么，并给出下一步入口，而不是一句「没有」。
    workEmptyBox_ = new QWidget;
    auto *emptyLay = new QVBoxLayout(workEmptyBox_);
    emptyLay->setContentsMargins(0, 0, 0, 0);
    emptyLay->setSpacing(6);
    emptyLay->addStretch(1);
    workEmpty_ = new QLabel(QString::fromUtf8("没有需要你处理的事"));
    workEmpty_->setObjectName("emptyTitle");
    workEmpty_->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(workEmpty_);
    auto *emptyHint = new QLabel(QString::fromUtf8(
        "Agent 等你确认、或任务做完时，这里会自动出现一条。\n"
        "先给 Qoder / ZCode / WorkBuddy 装上 agentrmc-bridge 插件。"));
    emptyHint->setObjectName("emptyHint");
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyLay->addWidget(emptyHint);
    auto *emptyRow = new QHBoxLayout;
    auto *emptyBtn = new QPushButton(QString::fromUtf8("看怎么接入 Agent"));
    emptyBtn->setObjectName("ghostSmall");
    emptyBtn->setCursor(Qt::PointingHandCursor);
    connect(emptyBtn, &QPushButton::clicked, this, [this] {
        if (tabs_ != nullptr) {
            for (int i = 0; i < tabs_->count(); ++i) {
                if (tabs_->tabText(i).startsWith(QString::fromUtf8("使用引导"))) {
                    tabs_->setCurrentIndex(i);
                    return;
                }
            }
        }
    });
    emptyRow->addStretch(1);
    emptyRow->addWidget(emptyBtn);
    emptyRow->addStretch(1);
    emptyLay->addLayout(emptyRow);
    emptyLay->addStretch(1);
    cl->addWidget(workEmptyBox_, 1);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    workHint_ = new QLabel(
        QString::fromUtf8("选中一条待办后可操作 · 双击直接回到对应 Agent"));
    workHint_->setObjectName("workHint");
    actions->addWidget(workHint_, 1);
    workRaise_ = new QPushButton(QString::fromUtf8("回到 Agent"));
    workRaise_->setObjectName("ghostSmall");
    workRaise_->setCursor(Qt::PointingHandCursor);
    workRaise_->setToolTip(QString::fromUtf8("把发这条待办的 Agent 窗口切到前台"));
    workRaise_->setEnabled(false);
    connect(workRaise_, &QPushButton::clicked, this,
            &MainWindow::raiseSelectedWork);
    workDone_ = new QPushButton(QString::fromUtf8("标为已处理"));
    workDone_->setObjectName("primarySmall");
    workDone_->setCursor(Qt::PointingHandCursor);
    workDone_->setToolTip(QString::fromUtf8("处理完了，从待办里移走（Delete）"));
    workDone_->setEnabled(false);
    connect(workDone_, &QPushButton::clicked, this,
            &MainWindow::ackSelectedWork);
    actions->addWidget(workRaise_);
    actions->addWidget(workDone_);
    cl->addLayout(actions);

    // 键盘也能干活：Enter 回去，Delete 结掉。
    auto *enterKey = new QShortcut(QKeySequence(Qt::Key_Return), workList_);
    enterKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(enterKey, &QShortcut::activated, this,
            &MainWindow::raiseSelectedWork);
    auto *delKey = new QShortcut(QKeySequence::Delete, workList_);
    delKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delKey, &QShortcut::activated, this, &MainWindow::ackSelectedWork);

    lay->addWidget(card);
    refreshWorkItems();
    return page;
}

void MainWindow::refreshWorkItems() {
    if (workList_ == nullptr || workItems_ == nullptr) {
        return;
    }
    const QSignalBlocker block(workList_);
    const QString selected = selectedWorkId();
    workList_->clear();

    QStringList aliveWorkers;
    for (const QString &exe : AgentWindow::knownExes()) {
        if (AgentWindow::alive(exe)) {
            aliveWorkers.append(exe);
        }
    }

    const QString filterExe =
        workFilter_ != nullptr ? workFilter_->currentData().toString()
                               : QString();
    if (workFilter_ != nullptr) {
        const QSignalBlocker filterBlock(workFilter_);
        workFilter_->clear();
        workFilter_->addItem(QString::fromUtf8("全部 Worker"), QString());
        for (const QString &exe : aliveWorkers) {
            workFilter_->addItem(AgentWindow::displayName(exe), exe);
        }
        const int index = workFilter_->findData(filterExe);
        workFilter_->setCurrentIndex(index >= 0 ? index : 0);
    }

    QVector<WorkItem> items = workItems_->items();
    std::sort(items.begin(), items.end(),
              [](const WorkItem &a, const WorkItem &b) {
                  const auto rank = [](const QString &stage) {
                      if (stage == QStringLiteral("needs-me")) {
                          return 0;
                      }
                      if (stage == QStringLiteral("review")) {
                          return 1;
                      }
                      if (stage == QStringLiteral("working")) {
                          return 2;
                      }
                      return 3;
                  };
                  const int ra = rank(a.stage);
                  const int rb = rank(b.stage);
                  if (ra != rb) {
                      return ra < rb;
                  }
                  return a.updatedAt > b.updatedAt;
              });

    int needs = 0;
    int review = 0;
    int working = 0;
    int open = 0;
    for (const WorkItem &item : items) {
        if (item.done) {
            continue;
        }
        if (!filterExe.isEmpty()) {
            const bool match =
                item.agentExe.compare(filterExe, Qt::CaseInsensitive) == 0 ||
                item.title.compare(AgentWindow::displayName(filterExe),
                                   Qt::CaseInsensitive) == 0;
            if (!match) {
                continue;
            }
        }
        ++open;
        QString stage = QString::fromUtf8("待办");
        QColor color("#8b949e");
        if (item.stage == QStringLiteral("needs-me")) {
            stage = QString::fromUtf8("等你处理");
            color = QColor("#f0883e");
            ++needs;
        } else if (item.stage == QStringLiteral("review")) {
            stage = QString::fromUtf8("待验收");
            color = QColor("#3fb950");
            ++review;
        } else if (item.stage == QStringLiteral("working")) {
            stage = QString::fromUtf8("进行中");
            color = QColor("#58a6ff");
            ++working;
        }

        QString agent = item.title;
        if (agent.isEmpty()) {
            agent = AgentWindow::displayName(item.agentExe);
        }
        const QDateTime when = item.updatedAt.isValid() ? item.updatedAt
                                                        : item.createdAt;

        // 行内不再拼「[待验收] Xxx」这种字符串：阶段色、Agent、时间各占各的位置，
        // 由 WorkItemDelegate 分栏画，扫一眼就能分出「谁在等我」。
        auto *row = new QListWidgetItem(workList_);
        row->setData(Qt::DisplayRole, item.message);
        row->setData(Qt::UserRole, item.id);
        row->setData(Qt::UserRole + 1, item.raiseExe);
        row->setData(Qt::UserRole + 2, color);
        row->setData(Qt::UserRole + 3, stage);
        row->setData(Qt::UserRole + 4, agent);
        row->setData(Qt::UserRole + 5,
                     when.isValid()
                         ? when.toLocalTime().toString(QStringLiteral("HH:mm"))
                         : QString());
        row->setSizeHint(QSize(0, 56));
        row->setToolTip(
            QString::fromUtf8("%1 · %2  %3\n%4\n%5")
                .arg(stage, agent,
                     when.isValid()
                         ? when.toLocalTime().toString(
                               QStringLiteral("MM-dd HH:mm"))
                         : QString(),
                     item.message,
                     item.project.isEmpty()
                         ? QStringLiteral("（无项目路径）")
                         : QString::fromUtf8("项目：%1").arg(item.project)));
        if (item.id == selected) {
            workList_->setCurrentItem(row);
        }
    }

    if (open == 0) {
        workList_->hide();
        if (workEmptyBox_ != nullptr) {
            workEmptyBox_->show();
        }
        const bool filtered = !filterExe.isEmpty();
        workEmpty_->setText(
            filtered ? QString::fromUtf8("这个 Worker 没有待办")
                     : QString::fromUtf8("没有需要你处理的事"));
        workCount_->setText(
            filtered ? QString::fromUtf8("这个 Worker 没有待办")
                     : QString::fromUtf8("没有待办"));
    } else {
        if (workEmptyBox_ != nullptr) {
            workEmptyBox_->hide();
        }
        workList_->show();
        workCount_->setText(QString::fromUtf8("%1 件待办").arg(open));
    }

    // 分类胶囊只显示非 0 的那几类，0 值不占版面。
    const auto setChip = [](QLabel *chip, int count, const char *format) {
        if (chip == nullptr) {
            return;
        }
        chip->setVisible(count > 0);
        if (count > 0) {
            chip->setText(QString::fromUtf8(format).arg(count));
        }
    };
    setChip(chipNeeds_, needs, "等你 %1");
    setChip(chipReview_, review, "待验收 %1");
    setChip(chipWorking_, working, "进行中 %1");

    if (tabs_ != nullptr && tabs_->count() > 0) {
        const int attention = workItems_->attentionCount();
        tabs_->setTabText(
            0, attention > 0
                   ? QString::fromUtf8("待我处理 (%1)").arg(attention)
                   : QString::fromUtf8("待我处理"));
    }
    const bool has = !selectedWorkId().isEmpty();
    if (workRaise_ != nullptr) {
        workRaise_->setEnabled(has);
    }
    if (workDone_ != nullptr) {
        workDone_->setEnabled(has);
    }
    if (workHint_ != nullptr) {
        workHint_->setText(
            has ? QString::fromUtf8("Enter 回到 Agent · Delete 标为已处理")
                : QString::fromUtf8(
                      "选中一条待办后可操作 · 双击直接回到对应 Agent"));
    }
    refreshHealth();
}

QString MainWindow::selectedWorkId() const {
    if (workList_ == nullptr) {
        return QString();
    }
    if (const QListWidgetItem *item = workList_->currentItem()) {
        return item->data(Qt::UserRole).toString();
    }
    // 辅助技术（UIA）可能只设置 selected 而不设 current：退回第一项选中。
    const QList<QListWidgetItem *> selected = workList_->selectedItems();
    return selected.isEmpty()
               ? QString()
               : selected.first()->data(Qt::UserRole).toString();
}

QJsonArray MainWindow::workersArray() const {
    QJsonArray workers;
    const QString foreground = AgentWindow::foregroundAgentExe();
    for (const QString &exe : AgentWindow::knownExes()) {
        if (!AgentWindow::alive(exe)) {
            continue;
        }
        int open = 0;
        int needs = 0;
        int review = 0;
        int working = 0;
        QDateTime last;
        const QString name = AgentWindow::displayName(exe);
        if (workItems_ != nullptr) {
            for (const WorkItem &item : workItems_->items()) {
                if (item.done) {
                    continue;
                }
                const bool match =
                    item.agentExe.compare(exe, Qt::CaseInsensitive) == 0 ||
                    item.title.compare(name, Qt::CaseInsensitive) == 0;
                if (!match) {
                    continue;
                }
                ++open;
                if (item.stage == QStringLiteral("needs-me")) {
                    ++needs;
                } else if (item.stage == QStringLiteral("review")) {
                    ++review;
                } else if (item.stage == QStringLiteral("working")) {
                    ++working;
                }
                if (!last.isValid() || item.updatedAt > last) {
                    last = item.updatedAt;
                }
            }
        }
        workers.append(QJsonObject{
            {QStringLiteral("exe"), exe},
            {QStringLiteral("name"), name},
            {QStringLiteral("alive"), true},
            {QStringLiteral("foreground"),
             foreground.compare(exe, Qt::CaseInsensitive) == 0},
            {QStringLiteral("open_items"), open},
            {QStringLiteral("needs_me"), needs},
            {QStringLiteral("review"), review},
            {QStringLiteral("working"), working},
            {QStringLiteral("last_activity"),
             last.isValid()
                 ? last.toUTC().toString(Qt::ISODateWithMs)
                 : QString()},
        });
    }
    return workers;
}

QString MainWindow::workersJson() const {
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("workers"), workersArray()}})
            .toJson(QJsonDocument::Compact));
}

namespace {

// 联动插件随安装包落在 exe 旁边：源目录用来读内置版本，zip 用来升级各家宿主。
QString pluginSourceDir() {
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/plugins/agentrmc-bridge");
}

QString pluginZipPath() {
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/plugins/agentrmc-bridge.zip");
}

}  // namespace

QJsonObject MainWindow::pluginHealth(QStringList *problems) const {
    QJsonArray rows;
    const QString bundled = pluginupgrade::bundledVersion(pluginSourceDir());
    const QList<pluginupgrade::Host> hosts =
        pluginupgrade::scanHomes(QDir::homePath());
    for (const pluginupgrade::Host &host : hosts) {
        QString state;
        if (!host.problem.isEmpty()) {
            state = QString::fromUtf8("读不出");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("%1 插件状态读不出：%2")
                                     .arg(host.name, host.problem));
            }
        } else if (!host.installed) {
            // 没装过是一种状态，不是故障：首次安装走「使用引导」那句话，
            // 不该每 5 秒在问题行里刷一条。
            state = QString::fromUtf8("未装");
        } else if (bundled.isEmpty()) {
            state = QString::fromUtf8("安装包没带版本");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("安装包里没有 plugins/agentrmc-bridge/VERSION"));
            }
        } else if (versionIsNewer(bundled, host.version)) {
            state = QString::fromUtf8("落后");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("%1 插件 %2 → 内置 %3")
                                     .arg(host.name, host.version, bundled));
            }
        } else {
            state = QString::fromUtf8("已装齐");
        }
        // 盘上装齐了但进程还在跑旧版，是「升级完没重启」，和「没升级」两回事。
        const QString loaded = pluginLoaded_.value(host.name);
        const bool staleProcess =
            state == QString::fromUtf8("已装齐") && !loaded.isEmpty() &&
            loaded != bundled;
        if (staleProcess && problems != nullptr) {
            problems->append(QString::fromUtf8("%1 盘上 %2，进程里还是 %3，重启宿主才生效")
                                 .arg(host.name, bundled, loaded));
        }
        // 升级后一条 hook 都没收到：已开着的会话把命令指向上一版目录，而那个目录
        // 常被安装器删掉，于是提醒静默断掉——不报旧版本号，是什么都不报。
        const bool awaiting = pluginAwaitingReport_.contains(host.name);
        if (awaiting && problems != nullptr) {
            problems->append(
                QString::fromUtf8("%1 升级后还没收到 hook：开着的会话仍指向旧版本目录，"
                                  "新开会话或重启宿主才恢复提醒").arg(host.name));
        }
        const QDateTime lastReport = pluginLastReport_.value(host.name);
        rows.append(QJsonObject{
            {QStringLiteral("name"), host.name},
            {QStringLiteral("installed"), host.version},
            {QStringLiteral("loaded"), loaded},
            {QStringLiteral("enabled"), host.enabled},
            {QStringLiteral("running"), AgentWindow::alive(host.exe)},
            {QStringLiteral("state"), state},
            {QStringLiteral("last_report"),
             lastReport.isValid()
                 ? lastReport.toUTC().toString(Qt::ISODateWithMs)
                 : QString()},
            {QStringLiteral("awaiting_report"), awaiting},
        });
    }
    return QJsonObject{
        {QStringLiteral("bundled"), bundled},
        {QStringLiteral("hosts"), rows},
    };
}

void MainWindow::upgradePlugins() {
    const QString bundled = pluginupgrade::bundledVersion(pluginSourceDir());
    const QString zip = pluginZipPath();
    const QString backupDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/plugin-backups");
    QStringList lines;
    int upgraded = 0;
    for (const pluginupgrade::Host &host :
         pluginupgrade::scanHomes(QDir::homePath())) {
        if (!host.problem.isEmpty()) {
            lines << QString::fromUtf8("%1 读不出：%2").arg(host.name, host.problem);
            continue;
        }
        if (!host.installed) {
            lines << QString::fromUtf8("%1 没装过，先在宿主里装一次").arg(host.name);
            continue;
        }
        if (!versionIsNewer(bundled, host.version)) {
            lines << QString::fromUtf8("%1 已 %2").arg(host.name, host.version);
            continue;
        }
        const pluginupgrade::Result result =
            pluginupgrade::upgrade(host, zip, bundled, backupDir);
        if (result.ok) {
            ++upgraded;
            lines << QString::fromUtf8("%1 %2→%3%4")
                         .arg(host.name, result.from, result.to,
                              AgentWindow::alive(host.exe)
                                  ? QString::fromUtf8("（重启该宿主才加载）")
                                  : QString());
        } else {
            lines << QString::fromUtf8("%1 没成：%2").arg(host.name, result.detail);
        }
    }
    // 只列 Qoder 家族，点完容易让人以为「WorkBuddy 怎么没动」，把边界一起说清。
    const QString detail =
        lines.join(QString::fromUtf8(" · ")) +
        QString::fromUtf8(" · WorkBuddy / ZCode 走市场目录源自动更新，不用点这里");
    appendEvent(upgraded > 0 ? QStringLiteral("status") : QStringLiteral("error"),
                QString(), QString(),
                QString::fromUtf8("联动插件升级：%1").arg(detail), 0);
    if (toast_ != nullptr) {
        toast_->showText(detail, 4200);
    }
    refreshSystemPage();
}

void MainWindow::trackPluginTransitions() {
    for (const pluginupgrade::Host &host :
         pluginupgrade::scanHomes(QDir::homePath())) {
        if (!host.installed) {
            continue;
        }
        const QString seen = pluginSeenInstalled_.value(host.name);
        if (!seen.isEmpty() && seen != host.version &&
            AgentWindow::alive(host.exe)) {
            // 版本是在运行期间换的，而这家宿主还开着：它已经起来的会话里 hook 命令
            // 仍指向上一版目录。那一版目录要是被安装器删了，提醒就静默没了。
            pluginAwaitingReport_.insert(host.name, true);
        }
        pluginSeenInstalled_.insert(host.name, host.version);
    }
}

QString MainWindow::healthJson() const {
    const int spool = spoolPendingCount();
    QJsonArray problems;
    if (bridge_ == nullptr || !bridge_->isRunning()) {
        problems.append(QString::fromUtf8("桥接未启动"));
    }
    if (spool > 0) {
        problems.append(QString::fromUtf8("离线待补 %1 条").arg(spool));
    }
    QStringList pluginProblems;
    const QJsonObject plugin = pluginHealth(&pluginProblems);
    for (const QString &problem : pluginProblems) {
        problems.append(problem);
    }
    QJsonObject root{
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("bridge"), bridge_ != nullptr && bridge_->isRunning()},
        {QStringLiteral("uptime_seconds"),
         static_cast<double>(
             startedAt_.secsTo(QDateTime::currentDateTimeUtc()))},
        {QStringLiteral("spool_pending"), spool},
        {QStringLiteral("plugin"), plugin},
        {QStringLiteral("last_event_at"),
         lastEventAt_.isValid()
             ? lastEventAt_.toUTC().toString(Qt::ISODateWithMs)
             : QString()},
        {QStringLiteral("work_items"),
         QJsonObject{{QStringLiteral("attention"),
                      workItems_ != nullptr ? workItems_->attentionCount() : 0},
                     {QStringLiteral("open"),
                      workItems_ != nullptr ? workItems_->openCount() : 0}}},
        {QStringLiteral("workers"), workersArray()},
        {QStringLiteral("problems"), problems},
    };
    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MainWindow::refreshHealth() {
    if (workHealth_ == nullptr) {
        return;
    }
    int workers = 0;
    for (const QString &exe : AgentWindow::knownExes()) {
        if (AgentWindow::alive(exe)) {
            ++workers;
        }
    }
    const bool bridgeOk = bridge_ != nullptr && bridge_->isRunning();
    const int spool = spoolPendingCount();
    QStringList problems;
    if (!bridgeOk) {
        problems.append(QString::fromUtf8("桥接未启动"));
    }
    if (spool > 0) {
        problems.append(QString::fromUtf8("离线待补 %1 条").arg(spool));
    }
    const QString last =
        lastEventAt_.isValid()
            ? lastEventAt_.toLocalTime().toString(QStringLiteral("HH:mm"))
            : QStringLiteral("—");
    // 一行富文本：正常时只有一个绿点 + 灰字；有问题就直接列问题，不重复报「异常」。
    const QString dot = problems.isEmpty() ? QStringLiteral("#3fb950")
                                           : QStringLiteral("#f0883e");
    const QString body =
        problems.isEmpty()
            ? QString::fromUtf8("桥接正常 · %1 个 Worker · 最近 %2")
                  .arg(workers)
                  .arg(last)
            : QString::fromUtf8("%1 · %2 个 Worker")
                  .arg(problems.join(QString::fromUtf8(" · ")))
                  .arg(workers);
    workHealth_->setText(
        QStringLiteral("<span style=\"color:%1;\">●</span>&nbsp;%2")
            .arg(dot, body));
    refreshSystemPage();
}

void MainWindow::refreshSystemPage() {
    if (sysBridge_ == nullptr) {
        return;
    }
    const bool bridgeOk = bridge_ != nullptr && bridge_->isRunning();
    sysBridge_->setText(
        QString::fromUtf8("桥接：%1（127.0.0.1:45915）")
            .arg(bridgeOk ? QString::fromUtf8("正常")
                          : QString::fromUtf8("异常")));
    const qint64 secs = startedAt_.secsTo(QDateTime::currentDateTimeUtc());
    sysUptime_->setText(QString::fromUtf8("运行时长：%1 分 %2 秒")
                            .arg(secs / 60)
                            .arg(secs % 60));
    sysLast_->setText(
        QString::fromUtf8("最近事件：%1")
            .arg(lastEventAt_.isValid()
                     ? lastEventAt_.toLocalTime().toString(
                           QStringLiteral("MM-dd HH:mm:ss"))
                     : QStringLiteral("—")));
    const int spool = spoolPendingCount();
    sysSpool_->setText(QString::fromUtf8("离线待补：%1 条").arg(spool));

    QStringList pluginProblems;
    const QJsonObject plugin = pluginHealth(&pluginProblems);
    if (sysPlugin_ != nullptr) {
        QString text = QString::fromUtf8("联动插件：内置 %1")
                           .arg(plugin.value(QStringLiteral("bundled"))
                                        .toString()
                                        .isEmpty()
                                    ? QString::fromUtf8("读不到")
                                    : plugin.value(QStringLiteral("bundled")).toString());
        const QJsonArray rows = plugin.value(QStringLiteral("hosts")).toArray();
        if (rows.isEmpty()) {
            text += QString::fromUtf8(" · 本机没有 Qoder 配置家");
        }
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            text += QString::fromUtf8(" · %1 %2（%3%4）")
                        .arg(row.value(QStringLiteral("name")).toString(),
                             row.value(QStringLiteral("installed"))
                                 .toString()
                                 .isEmpty()
                             ? QString::fromUtf8("未装")
                             : row.value(QStringLiteral("installed")).toString(),
                             row.value(QStringLiteral("state")).toString(),
                             row.value(QStringLiteral("enabled")).toBool()
                                 ? QString()
                                 : QString::fromUtf8("·未启用"));
            // 盘上已装齐但进程还跑旧版：只有 hook 报上来的 loaded 能看出来。
            const QString loaded = row.value(QStringLiteral("loaded")).toString();
            if (!loaded.isEmpty() &&
                loaded != row.value(QStringLiteral("installed")).toString()) {
                text += QString::fromUtf8("·进程 %1").arg(loaded);
            }
            if (row.value(QStringLiteral("awaiting_report")).toBool()) {
                text += QString::fromUtf8("·等新版上报");
            }
        }
        // 这行只列 Qoder 家族，WorkBuddy / ZCode 不在里面是设计如此，不是读漏——
        // 不写一句，用户只会以为插件状态没被读出来。
        text += QString::fromUtf8(" · WorkBuddy / ZCode 走市场目录源，自动更新");
        sysPlugin_->setText(text);
        sysPlugin_->setToolTip(QString::fromUtf8(
            "这一行只对账 Qoder / Qoder CN：它们把上传的 zip 记成本地插件，"
            "插件页的「更新」按钮不会去拉新包，所以要这里代劳。\n"
            "WorkBuddy / ZCode 的市场源是安装目录下的 plugins 目录（目录源 + "
            "自动更新），宿主自己就会拉到新版，不用点「升级联动插件」——"
            "AgentRMC 插手反而会和宿主抢插件注册表。\n"
            "改完插件重启对应宿主即可；行里的「进程」是此刻真在跑的版本。"));
    }

    QStringList problems = pluginProblems;
    if (!bridgeOk) {
        problems.append(QString::fromUtf8("桥接未启动（端口可能被占用）"));
    }
    if (spool > 0) {
        problems.append(
            QString::fromUtf8("有 %1 条离线通知待补投").arg(spool));
    }
    sysProblems_->setText(
        problems.isEmpty()
            ? QString::fromUtf8("问题：无")
            : QString::fromUtf8("问题：%1").arg(problems.join(QString::fromUtf8("；"))));
    sysProblems_->setStyleSheet(
        problems.isEmpty() ? QStringLiteral("color:#8b949e;")
                           : QStringLiteral("color:#f0883e;"));

    const QJsonArray workers = workersArray();
    sysWorkers_->setRowCount(workers.size());
    for (int i = 0; i < workers.size(); ++i) {
        const QJsonObject worker = workers.at(i).toObject();
        const QString name = worker.value(QStringLiteral("name")).toString();
        const bool foreground =
            worker.value(QStringLiteral("foreground")).toBool();
        const QString last =
            worker.value(QStringLiteral("last_activity")).toString();
        QDateTime lastAt = QDateTime::fromString(last, Qt::ISODateWithMs);
        if (!lastAt.isValid()) {
            lastAt = QDateTime::fromString(last, Qt::ISODate);
        }
        const QString lastText =
            lastAt.isValid()
                ? lastAt.toLocalTime().toString(QStringLiteral("HH:mm"))
                : QStringLiteral("—");
        const QVector<QPair<QString, QString>> columns = {
            {name, foreground ? QString::fromUtf8("前台")
                              : QString::fromUtf8("在线")},
            {QString::number(worker.value(QStringLiteral("needs_me")).toInt()),
             QString::number(worker.value(QStringLiteral("review")).toInt())},
            {QString::number(worker.value(QStringLiteral("working")).toInt()),
             lastText},
        };
        int column = 0;
        for (const auto &pair : columns) {
            auto *first = new QTableWidgetItem(pair.first);
            auto *second = new QTableWidgetItem(pair.second);
            if (foreground && column == 0) {
                first->setForeground(QColor("#58a6ff"));
            }
            sysWorkers_->setItem(i, column, first);
            sysWorkers_->setItem(i, column + 1, second);
            column += 2;
        }
    }
}

void MainWindow::watchdogTick() {
    if (bridgeRequested_ && bridge_ != nullptr && !bridge_->isRunning()) {
        if (bridge_->start()) {
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("桥接已恢复"), 0);
        }
    }
    importSpool();
    trackPluginTransitions();
    refreshHealth();
}

int MainWindow::spoolPendingCount() const {
    const auto lines = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return 0;
        }
        int count = 0;
        while (!file.atEnd()) {
            if (!file.readLine().trimmed().isEmpty()) {
                ++count;
            }
        }
        return count;
    };
    return lines(spoolPath_) + lines(spoolPath_ + QStringLiteral(".processing"));
}

int MainWindow::importSpool() {
    if (spoolPath_.isEmpty()) {
        return 0;
    }
    int imported = 0;
    const QString processing = spoolPath_ + QStringLiteral(".processing");
    for (const QString &path : {spoolPath_, processing}) {
        QFile file(path);
        if (!file.exists()) {
            continue;
        }
        QString readPath = path;
        if (path == spoolPath_) {
            QFile::remove(processing);
            if (!file.rename(processing)) {
                continue;
            }
            readPath = processing;
        }
        QFile input(readPath);
        if (!input.open(QIODevice::ReadOnly)) {
            continue;
        }
        while (!input.atEnd()) {
            const QByteArray line = input.readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            QJsonParseError error{};
            const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                continue;
            }
            const QJsonObject obj = doc.object();
            handleNotify(obj.value(QStringLiteral("event")).toString(),
                         obj.value(QStringLiteral("title")).toString(),
                         obj.value(QStringLiteral("message")).toString(),
                         obj.value(QStringLiteral("raise")).toString(),
                         obj.value(QStringLiteral("session_id")).toString(),
                         obj.value(QStringLiteral("project")).toString(),
                         obj.value(QStringLiteral("event_id")).toString(),
                         obj.value(QStringLiteral("plugin_version")).toString(),
                         obj.value(QStringLiteral("speech")).toString(),
                         true);
            ++imported;
        }
        input.close();
        QFile::remove(readPath);
    }
    if (imported > 0) {
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QString::fromUtf8("补投离线通知 %1 条").arg(imported), 0);
    }
    return imported;
}

QJsonObject MainWindow::reportWorkItem(const QJsonObject &payload) {
    const QString stage =
        payload.value(QStringLiteral("stage")).toString().trimmed();
    if (WorkItemStore::kindForReport(stage).isEmpty()) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QString::fromUtf8("stage 只支持 progress / blocked / ready")}};
    }
    if (workItems_ == nullptr) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("work item store not ready")}};
    }
    const QString title =
        payload.value(QStringLiteral("title")).toString().trimmed();
    QString agentExe =
        payload.value(QStringLiteral("agent_exe")).toString().trimmed();
    if (agentExe.isEmpty()) {
        // MCP 只知道自己叫什么产品名，进程映像名在这里反查——Worker 行按 exe
        // 归堆，认错了待办就会挂到另一个宿主头上（本机有两份 WorkBuddy）。
        for (const QString &exe : AgentWindow::knownExes()) {
            if (!title.isEmpty() &&
                AgentWindow::displayName(exe).compare(title,
                                                      Qt::CaseInsensitive) == 0) {
                agentExe = exe;
                break;
            }
        }
    }
    QString message =
        payload.value(QStringLiteral("message")).toString().trimmed();
    if (message.isEmpty()) {
        // 没给文案也要有话说，否则列表里是一行空白。
        const QString s = stage.toLower();
        message = s == QStringLiteral("progress")
                      ? QString::fromUtf8("正在处理")
                      : s == QStringLiteral("blocked")
                            ? QString::fromUtf8("卡住了，等你")
                            : QString::fromUtf8("做完了，等你验收");
    }
    const WorkItem item = workItems_->report(
        agentExe, payload.value(QStringLiteral("raise")).toString().trimmed(),
        payload.value(QStringLiteral("session_id")).toString().trimmed(),
        payload.value(QStringLiteral("project")).toString().trimmed(), stage,
        title, message);
    if (item.id.isEmpty()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("stage rejected")}};
    }
    lastEventAt_ = QDateTime::currentDateTimeUtc();
    refreshWorkItems();
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("id"), item.id},
                       {QStringLiteral("stage"), item.stage},
                       {QStringLiteral("title"), item.title}};
}

void MainWindow::handleNotify(const QString &kind, const QString &title,
                              const QString &message, const QString &raise,
                              const QString &sessionId,
                              const QString &project, const QString &eventId,
                              const QString &pluginVersion,
                              const QString &speech, bool replay) {
    lastEventAt_ = QDateTime::currentDateTimeUtc();
    // 念的和显示的不是一句：speech 是插件给的短语音文案（带宿主名、不含工具名和
    // 路径），没带就退回念 message。气泡和工单始终用 message。
    const QString spoken = speech.isEmpty() ? message : speech;
    // 记「谁在跑哪一版」要在判事件类型之前：turn-start 每轮都发，是最常到的读数。
    if (!title.isEmpty()) {
        pluginLastReport_.insert(title, QDateTime::currentDateTimeUtc());
        pluginAwaitingReport_.remove(title);  // 收到上报就说明这条路径还活着
    }
    if (!pluginVersion.isEmpty() && !title.isEmpty()) {
        pluginLoaded_.insert(title, pluginVersion);
    }
    if (kind == QStringLiteral("turn-start")) {
        // 同一 session 已经开始新对话：上一轮的 review 视为已看到，直接归档。
        if (workItems_ != nullptr && !sessionId.isEmpty() &&
            workItems_->resolveReviews(sessionId)) {
            refreshWorkItems();
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("新对话开始，旧验收已归档"), 0);
        }
        return;
    }
    if (workItems_ != nullptr &&
        (kind == QStringLiteral("permission") ||
         kind == QStringLiteral("question") ||
         kind == QStringLiteral("progress") || kind == QStringLiteral("stop"))) {
        workItems_->upsert(QStringLiteral("hook"), raise, sessionId, project,
                           kind, title, message, raise, eventId);
        refreshWorkItems();
    }
    if (replay) {
        // 离线补投只落工单，不补喊/抢前台，避免启动时一次念一串旧提醒。
        return;
    }
    appendEvent(QStringLiteral("status"), QString(), QString(),
                speech.isEmpty() || speech == message
                    ? title + QStringLiteral(" · ") + message
                    : title + QStringLiteral(" · ") + message +
                          QStringLiteral("（念：") + spoken + QStringLiteral("）"),
                0);
    // 记住发通知的宿主，桌宠分流时要靠它判断「Agent 在不在」，
    // 冷启动也能直接用上次的值，不用重新猜。只在变了才写盘。
    if (!raise.isEmpty() &&
        store_->uiString(QStringLiteral("agent_exe")) != raise) {
        store_->setUiString(QStringLiteral("agent_exe"), raise);
    }
    if (kind == QStringLiteral("stop")) {
        // 任务完成：念出来 + 气泡停满 7s。**不自动抢前台**——
        // 活儿干完就把焦点拽走很烦，气泡可点，点了再回去。
        // 是否前台不影响是否朗读：用户可能没看屏幕，语音是主通道。
        if (voicePet_ != nullptr) {
            voicePet_->speak(spoken, 7000, raise);
        }
        return;
    }
    if (kind == QStringLiteral("permission")) {
        // 等确认要把人喊回来：气泡停久一点 + 语音，桌宠收起也照念；
        // 收起又没 TTS 时退回托盘，别让提醒凭空消失。
        if (voicePet_ != nullptr) {
            voicePet_->speak(spoken, 7000, raise);
            if (!voicePet_->isVisible() && !voicePet_->ttsReady() &&
                tray_ != nullptr) {
                tray_->showMessage(title, message,
                                   QSystemTrayIcon::Information, 6000);
            }
        } else if (tray_ != nullptr) {
            tray_->showMessage(title, message,
                               QSystemTrayIcon::Information, 6000);
        }
        AgentWindow::raise(raise);
        return;
    }
    if (kind == QStringLiteral("speak")) {
        // MCP 主动让桌宠说话：只念，不抢前台。
        if (voicePet_ != nullptr) {
            voicePet_->speak(message);
        }
        return;
    }
    if (voicePet_ != nullptr && voicePet_->isVisible() &&
        voicePet_->petEnabled()) {
        voicePet_->notify(message, raise);
    } else if (tray_ != nullptr) {
        tray_->showMessage(title, message, QSystemTrayIcon::Information, 6000);
    }
}

void MainWindow::raiseWorkItem(const QString &id) {
    if (workItems_ == nullptr || id.isEmpty()) {
        return;
    }
    for (const WorkItem &item : workItems_->items()) {
        if (item.id != id) {
            continue;
        }
        if (!item.raiseExe.isEmpty()) {
            AgentWindow::raise(item.raiseExe);
        } else if (!item.agentExe.isEmpty()) {
            AgentWindow::raise(item.agentExe);
        } else {
            AgentWindow::raise(QString());  // 缺省找 ZCode
        }
        return;
    }
}

void MainWindow::raiseSelectedWork() {
    raiseWorkItem(selectedWorkId());
}

void MainWindow::ackSelectedWork() {
    const QString id = selectedWorkId();
    if (id.isEmpty() || workItems_ == nullptr) {
        return;
    }
    if (workItems_->ack(id)) {
        refreshWorkItems();
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QString::fromUtf8("待办已标记完成"), 0);
    }
}

QWidget *MainWindow::buildSystemPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *card = new QFrame;
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(14, 11, 12, 12);
    cl->setSpacing(8);

    auto *title = new QLabel(QString::fromUtf8("系统健康"));
    title->setObjectName("cardTitle");
    cl->addWidget(title);

    sysBridge_ = new QLabel;
    sysBridge_->setObjectName("muted");
    cl->addWidget(sysBridge_);
    sysUptime_ = new QLabel;
    sysUptime_->setObjectName("muted");
    cl->addWidget(sysUptime_);
    sysLast_ = new QLabel;
    sysLast_->setObjectName("muted");
    cl->addWidget(sysLast_);
    sysSpool_ = new QLabel;
    sysSpool_->setObjectName("muted");
    cl->addWidget(sysSpool_);
    sysPlugin_ = new QLabel;
    sysPlugin_->setObjectName("muted");
    sysPlugin_->setWordWrap(true);
    cl->addWidget(sysPlugin_);
    sysProblems_ = new QLabel;
    sysProblems_->setObjectName("muted");
    sysProblems_->setWordWrap(true);
    cl->addWidget(sysProblems_);

    sysWorkers_ = new QTableWidget(0, 6);
    sysWorkers_->setHorizontalHeaderLabels(
        {QString::fromUtf8("Worker"), QString::fromUtf8("状态"),
         QString::fromUtf8("待你"), QString::fromUtf8("待验收"),
         QString::fromUtf8("进行中"), QString::fromUtf8("最近")});
    sysWorkers_->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Stretch);
    sysWorkers_->verticalHeader()->setVisible(false);
    sysWorkers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sysWorkers_->setSelectionMode(QAbstractItemView::NoSelection);
    sysWorkers_->setFocusPolicy(Qt::NoFocus);
    sysWorkers_->setShowGrid(false);
    sysWorkers_->setAlternatingRowColors(true);
    cl->addWidget(sysWorkers_, 1);

    auto *actions = new QHBoxLayout;
    auto *refresh = new QPushButton(QString::fromUtf8("刷新"));
    refresh->setObjectName("ghostSmall");
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::watchdogTick);
    auto *openDir = new QPushButton(QString::fromUtf8("打开数据目录"));
    openDir->setObjectName("ghostSmall");
    openDir->setCursor(Qt::PointingHandCursor);
    connect(openDir, &QPushButton::clicked, this, [this] {
        const QString dir = QFileInfo(spoolPath_).absolutePath();
        if (!dir.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });
    pluginUpgrade_ = new QPushButton(QString::fromUtf8("升级联动插件"));
    pluginUpgrade_->setObjectName("ghostSmall");
    pluginUpgrade_->setCursor(Qt::PointingHandCursor);
    pluginUpgrade_->setToolTip(QString::fromUtf8(
        "只升级 Qoder / Qoder CN 两家已装的插件。\n"
        "WorkBuddy / ZCode 不用点这里：它们的市场源是安装目录 plugins "
        "（目录源、自动更新），宿主自己拉新版，重启宿主生效。"));
    connect(pluginUpgrade_, &QPushButton::clicked, this,
            &MainWindow::upgradePlugins);
    actions->addWidget(refresh);
    actions->addWidget(pluginUpgrade_);
    actions->addWidget(openDir);
    actions->addStretch(1);
    cl->addLayout(actions);

    lay->addWidget(card);
    refreshSystemPage();
    return page;
}

QWidget *MainWindow::buildGuidePage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *holder = new QWidget;
    holder->setObjectName("guideHolder");
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    auto *card = new QFrame;
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(18, 16, 18, 16);
    cl->setSpacing(16);

    struct Line {
        const char *key;
        const char *text;
    };
    const auto addSectionQ = [cl](const QString &name,
                                  const QVector<QPair<QString, QString>> &lines) {
        auto *box = new QWidget;
        auto *boxLay = new QVBoxLayout(box);
        boxLay->setContentsMargins(0, 0, 0, 0);
        boxLay->setSpacing(9);
        boxLay->addWidget(guide::heading(name));
        for (const auto &line : lines) {
            boxLay->addWidget(guide::row(line.first, line.second, 92));
        }
        cl->addWidget(box);
    };
    const auto addSection = [addSectionQ](const QString &name,
                                          const QVector<Line> &lines) {
        QVector<QPair<QString, QString>> text;
        for (const Line &line : lines) {
            text.append({QString::fromUtf8(line.key),
                         QString::fromUtf8(line.text)});
        }
        addSectionQ(name, text);
    };

    auto *intro = new QLabel(QString::fromUtf8(
        "遥控器是可选的：有遥控器就按下面配对；没有也能用语音——先看「没有遥控器」"
        "这一段。桌宠右键菜单里也能完成同样的操作。"));
    intro->setObjectName("secBody");
    intro->setWordWrap(true);
    cl->addWidget(intro);

    addSection(QString::fromUtf8("没有遥控器也能用"),
               {{"触发", "Ctrl+Alt+V 全局热键 / 语音主界面大麦克风 / "
                         "耳机播放键 / 单击桌宠"},
                {"识别", "转发热键（0 模型，交给 Qoder / WorkBuddy）或本地模型"
                         "（任何输入框都能出字）"},
                {"对话", "右键桌宠 → 对话模式：闲聊走 LLM，"
                         "干活的话交给前台 Agent"},
                {"提醒", "装 agentrmc-bridge 插件：等授权、等选方案、任务完成时"
                         "桌宠气泡 + 语音"},
                {"入口", "主界面左栏「语音主界面」/ 托盘「语音面板」/ 桌宠右键"},
                {"模型", "把模型 zip 拖进窗口，或「语音面板 → 模型设置 → "
                         "导入模型包…」"}});
    addSection(QString::fromUtf8("有遥控器：第一次配对"),
               {{"长按 3 秒", "同时按住「返回」+「主页」，直到遥控器指示灯快速闪烁"},
                {"蓝牙设置", "点遥控器卡片右侧的「蓝牙设置」，在列表里连接遥控器"},
                {"看状态", "按一下任意键，卡片上对应键位亮起就说明连上了"},
                {"以后", "配对过一次后无需再配对：按一下任意键就会自动回连"}});
    addSection(QString::fromUtf8("手势速查"),
               {{"语音键", "按住说话、松手结束（Qoder / WorkBuddy 自动切换接法）"},
                {"方向键", "浏览内容；导航模式内改为翻页"},
                {"OK", "发送消息；鼠标模式内 = 鼠标左键"},
                {"返回", "停止 / 返回；双击进入音量·亮度模式"},
                {"菜单", "换行；双击进入鼠标模式"},
                {"主页", "回到顶部；双击进入导航模式"},
                {"对焦", "全屏（走系统原生 F11）"}});
    addSection(QString::fromUtf8("模式怎么用"),
               {{"进入", "双击对应键，屏幕底部提示「xx模式 开」，并有一声提示音"},
                {"操作", "音量/亮度：↑↓ 音量、←→ 屏幕亮度；鼠标：方向键移动、OK 点击；"
                         "导航：←→ 切换控件、↑↓ 翻页"},
                {"退出", "音量/亮度与鼠标模式单击「返回」或「菜单」退出；"
                         "导航模式单击「主页」退出"},
                {"自动退出", "音量/亮度、鼠标模式 12 秒不操作自动退出"},
                {"接管原生键", "映射页右下角打开后，当前场景中映射键的原生事件被吞；"
                               "实体键盘同名键也会按遥控器映射处理，"
                               "双击键的单击会有一段判定延迟"}});
    addSection(QString::fromUtf8("语音怎么用（有/无遥控器都适用）"),
               {{"触发", "无遥控器：Ctrl+Alt+V / 语音主界面大麦克风 / "
                         "耳机播放键 / 单击桌宠；有遥控器：按住语音键"},
                {"转发", "默认，0 模型：把语音键转给当前应用自己的快捷键"
                         "（Qoder Ctrl+Shift+V / WorkBuddy Ctrl+D）"},
                {"本地", "SenseVoice ~230MB / Qwen3-ASR ~1GB：任何输入框都能出字，"
                         "可离线；模型 zip 拖进窗口或「模型设置」导入"},
                {"云端", "不装模型；音频上传到你配置的 OpenAI 兼容 ASR 服务"},
                {"对话", "右键桌宠 → 对话模式：闲聊走 LLM，"
                         "干活的话交给前台 Agent"},
                {"耳机", "电脑没在放媒体时，播放/暂停 = 同一个语音开关"}});
    addSection(QString::fromUtf8("桌宠右键菜单（只留高频）"),
               {{"一级", "开始/停止监听、说一句/结束说话"},
                {"窗口", "语音面板、完整界面、隐藏桌宠"},
                {"其它", "识别方式、对话、主题、交给哪个 Agent、朗读最近，"
                         "都在语音面板 / 完整界面里操作"}});
    addSection(QString::fromUtf8("打开应用"),
               {{"怎么说", "按住语音键说「打开剪映」「打开 Qoder」「打开浏览器」，"
                          "松手后直接启动，不会再往输入框里贴这几个字"},
                {"内置", "浏览器 / Chrome / Edge / Qoder / Qoder CN / WorkBuddy / "
                         "DeepSeek / Codex / ZCode / 剪映，说「帮我打开…吧」也算"},
                {"其它应用", "开始菜单里有的，按名字原样说就能开，例如「打开 WorkBuddy AI」"},
                {"应用页", "右侧「应用」页能看到本机装没装、来源在哪，也可以直接点「打开」"},
                {"遥控器", "「按键映射」的单击 / 双击 / 长按菜单里有「启动应用」，"
                           "把常用的绑到键上，不用说话"},
                {"前提", "语音启动只在本地识别（SenseVoice / Qwen3）下生效；"
                         "转发热键时语音是整个交给前台应用的"},
                {"不想被抢", "想把「打开剪映」这句话原样打给 Agent，就在「应用」页右下角"
                            "关掉「语音启动」（默认开）"}});
    addSectionQ(QString::fromUtf8("接入 AI Agent（桌宠提醒）"),
               {{QString::fromUtf8("自动提醒"),
                 QString::fromUtf8("在你干活的那个 Agent（Qoder / ZCode / WorkBuddy）里装上 "
                                   "agentrmc-bridge 插件：等你确认、任务结束时，桌宠冒气泡念一句")},
                {QString::fromUtf8("怎么装"),
                 QString::fromUtf8("点下面「复制安装话术」，把那句话发给正在干活的 Agent，"
                                   "它自己装好并启用——话术里已经带你本机上的位置")},
                {QString::fromUtf8("自己动手"),
                 QString::fromUtf8("Qoder：侧边栏 扩展 → 插件 → 添加插件 → 上传插件，选安装目录 "
                                   "plugins 里的 agentrmc-bridge.zip；ZCode / WorkBuddy："
                                   "在应用市场添加同一个 plugins 目录")},
                {QString::fromUtf8("怎么升级"),
                 QString::fromUtf8("Qoder 把上传的包记成本地插件，插件页那个「更新」按钮不拉新包，"
                                   "而且 Qoder 和 Qoder CN 是两个独立 app、各装各的。"
                                   "升级去「系统」页看「联动插件」那一行：落后的话点"
                                   "「升级联动插件」，两家一起指到内置版本；改完要重启对应的 "
                                   "app 才真加载新代码（那行里的「进程」就是此刻真在跑的版本）")},
                {QString::fromUtf8("WorkBuddy 怎么升"),
                 QString::fromUtf8("不用点「升级联动插件」——那个按钮只认 Qoder 家族。"
                                   "WorkBuddy / ZCode 的市场源是安装目录下的 plugins 目录"
                                   "（目录源、自动更新），宿主自己会拉到新版；"
                                   "改完重启宿主就生效。注意 WorkBuddy 和 WorkBuddy AI "
                                   "是两份独立安装，各读各的配置家（.workbuddy / .workbuddy-ai），"
                                   "要升就得两家各自刷新")}});
    addSection(QString::fromUtf8("已知限制"),
               {{"电源 / 音量", "这两个键走红外，电脑收不到，无法映射"},
                {"未开接管", "遥控器按键仍会原生送达前台，映射动作和原生动作可能同时生效"},
                {"双击判定", "两次按下间隔 0.45 秒以内才算双击，慢速连点会当成两次单击"},
                {"任务栏闪", "开始/结束录音时任务栏闪一下，是 Windows 麦克风指示器"}});

    auto *links = new QHBoxLayout;
    auto *site = new QPushButton(QString::fromUtf8("官网  ↗"));
    site->setObjectName("ghostSmall");
    site->setCursor(Qt::PointingHandCursor);
    connect(site, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://ligdesign.win/agentrmc")));
    });
    auto *github = new QPushButton(QString::fromUtf8("GitHub  ↗"));
    github->setObjectName("ghostSmall");
    github->setCursor(Qt::PointingHandCursor);
    connect(github, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/Samuel5945/AgentRMC")));
    });
    auto *version = new QLabel(
        QString::fromUtf8("当前版本 v%1").arg(appVersion()));
    version->setObjectName("muted");
    const QString installSentence = agentBridgeInstallSentence();
    auto *copySentence = new QPushButton(QString::fromUtf8("复制安装话术"));
    copySentence->setObjectName("ghostSmall");
    copySentence->setCursor(Qt::PointingHandCursor);
    connect(copySentence, &QPushButton::clicked, copySentence,
            [copySentence, installSentence] {
                QApplication::clipboard()->setText(installSentence);
                copySentence->setText(QString::fromUtf8("已复制 ✓ 去粘给 Agent"));
                QTimer::singleShot(2600, copySentence, [copySentence] {
                    copySentence->setText(QString::fromUtf8("复制安装话术"));
                });
            });
    links->addWidget(site);
    links->addWidget(github);
    links->addWidget(copySentence);
    links->addSpacing(8);
    links->addWidget(version);
    links->addStretch(1);
    cl->addLayout(links);
    cl->addStretch(1);

    hl->addWidget(card);
    scroll->setWidget(holder);
    lay->addWidget(scroll);
    return page;
}

void MainWindow::buildTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }
    QIcon icon(":/icons/icon.ico");
    if (icon.isNull()) {
        icon = windowIcon();
    }
    tray_ = new QSystemTrayIcon(icon, this);

    auto *menu = new QMenu(this);
    showAction_ = menu->addAction(QString::fromUtf8("显示主界面"));
    connect(showAction_, &QAction::triggered, this, &MainWindow::wake);
    auto *voicePanelAction = menu->addAction(QString::fromUtf8("语音面板"));
    connect(voicePanelAction, &QAction::triggered, this,
            &MainWindow::showVoicePanel);
    petAction_ = menu->addAction(QString::fromUtf8("语音桌宠"));
    petAction_->setCheckable(true);
    petAction_->setChecked(voicePet_->petEnabled());
    connect(petAction_, &QAction::toggled, this, [this](bool on) {
        voicePet_->setPetEnabled(on);
        if (on) {
            voicePet_->present();
        }
    });
    listenAction_ = menu->addAction(QString::fromUtf8("开始监听"));
    connect(listenAction_, &QAction::triggered, this, [this] {
        toggleListening();
        updateListenUi();
    });
    auto *update = menu->addAction(QString::fromUtf8("检查更新"));
    connect(update, &QAction::triggered, this, [this] { checkUpdates(true); });
    auto *site = menu->addAction(QString::fromUtf8("官网"));
    connect(site, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://ligdesign.win/agentrmc")));
    });
    auto *github = menu->addAction(QString::fromUtf8("GitHub 仓库"));
    connect(github, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/Samuel5945/AgentRMC")));
    });
    menu->addSeparator();
    autoStartAction_ = menu->addAction(QString::fromUtf8("开机自启"));
    autoStartAction_->setCheckable(true);
    autoStartAction_->setChecked(autoStartEnabled());
    connect(autoStartAction_, &QAction::toggled, this, [this](bool on) {
        setAutoStart(on);
        if (autoStartEnabled() != on) {
            QSignalBlocker block(autoStartAction_);
            autoStartAction_->setChecked(!on);
        }
    });
    menu->addSeparator();
    auto *quit = menu->addAction(QString::fromUtf8("退出 AgentRMC Native"));
    connect(quit, &QAction::triggered, this, [this] {
        quitting_ = true;
        QApplication::quit();
    });
    tray_->setContextMenu(menu);
    tray_->show();

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    wake();
                }
            });
    updateTrayState();
}

void MainWindow::updateTrayState() {
    if (tray_ == nullptr) {
        return;
    }
    const bool running = engine_ != nullptr && engine_->isRunning();
    const QString scene =
        sceneName_.isEmpty() ? store_->current(true).name : sceneName_;
    tray_->setToolTip(QString::fromUtf8("AgentRMC · %1 · %2")
                          .arg(running ? QString::fromUtf8("监听中")
                                       : QString::fromUtf8("未监听"),
                               voiceProfileLabel(scene)));
    if (listenAction_ != nullptr) {
        listenAction_->setText(running ? QString::fromUtf8("停止监听")
                                       : QString::fromUtf8("开始监听"));
    }
}

void MainWindow::showOnStartup(bool startHidden) {
    // 首次运行按「这次安装」判断：标记文件放在 exe 同目录，重装/升级不会误判。
    const QString marker =
        QCoreApplication::applicationDirPath() + QStringLiteral("/.guide_seen");
    const bool firstRun = !QFile::exists(marker);
    if (startHidden && !store_->uiBool("start_minimized", true)) {
        startHidden = false;
    }
    if (startHidden) {
        // 只留托盘时也要先把 HWND 建出来，第二次启动才能 FindWindow 叫回窗口。
        winId();
        // 开机自启只留托盘：桌宠就是这时的“脸面”。
        QTimer::singleShot(0, this, [this] { voicePet_->presentIfEnabled(); });
        return;
    }
    // 上次用的语音主界面就直接开它，不闪一下完整界面。
    if (store_->uiBool("voice_mode", false)) {
        winId();  // 热键注册和二次启动唤醒都要 HWND
        showVoicePanel();
    } else {
        wake();
    }
    // 桌宠常驻：界面开着也露头。以前只有「关窗进托盘」「收起语音面板」「开机只留
    // 托盘」这三处才把桌宠叫出来，所以手动启动时 pet_visible 一直是 false，看起来
    // 像桌宠没开。present() 自己兼顾已可见（只 raise）和贴边收纳（popOut）两种态；
    // 关掉桌宠开关时 presentIfEnabled 什么都不做，尊重用户设置。
    QTimer::singleShot(0, this, [this] { voicePet_->presentIfEnabled(); });
    if (firstRun) {
        QFile file(marker);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("1");
        }
        QTimer::singleShot(0, this, &MainWindow::showGuide);
    }
}

void MainWindow::wake() {
    showNormal();
    raise();
    activateWindow();
}

bool MainWindow::autoStartEnabled() const {
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\"
                                 "Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    return !run.value(QStringLiteral("AgentRMCNative")).toString().isEmpty();
}

void MainWindow::setAutoStart(bool on) {
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\"
                                 "Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    if (!on) {
        run.remove(QStringLiteral("AgentRMCNative"));
        return;
    }
    const QString exe =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    run.setValue(QStringLiteral("AgentRMCNative"),
                 QStringLiteral("\"%1\" --tray").arg(exe));
}

void MainWindow::showGuide() {
    if (guide_ == nullptr) {
        guide_ = new GuideDialog(this);
        connect(guide_, &GuideDialog::useHotkeyRequested, this, [this] {
            store_->updateVoice(QJsonObject{{QStringLiteral("backend"),
                                             QStringLiteral("hotkey")}});
            voice_->reload();
            refreshVoiceUi();
            guide_->setBackend(QStringLiteral("hotkey"));
            if (toast_ != nullptr) {
                toast_->showText(
                    QString::fromUtf8("已切到转发模式（0 模型）"), 2400);
            }
        });
        connect(guide_, &GuideDialog::modelPackageDropped, this,
                [this](const QString &file) {
                    guide_->hide();
                    importDroppedModel(file);
                    guide_->show();
                    guide_->raise();
                });
        connect(guide_, &GuideDialog::voiceModeRequested, this, [this] {
            store_->setUiBool("voice_mode", true);
            if (guide_ != nullptr) {
                guide_->close();
            }
            showVoicePanel();
        });
        connect(guide_, &GuideDialog::openVoiceSettingsRequested, this, [this] {
            // 引导是置顶窗口，直接开设置会被它挡住：先收起引导，设置关掉再放回来。
            guide_->hide();
            openVoiceSettings();
            guide_->setBackend(voiceBackendKey(voice_->options().backend));
            guide_->show();
            guide_->raise();
        });
    }
    guide_->setBackend(voiceBackendKey(voice_->options().backend));
    guide_->showStep(0);
    const QSize size = guide_->sizeHint();
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect area = screen->availableGeometry();
        guide_->move(area.x() + (area.width() - size.width()) / 2,
                     area.y() + (area.height() - size.height()) / 2);
    }
    guide_->show();
    guide_->raise();
}

void MainWindow::toggleListening() {
    if (engine_ == nullptr) {
        return;
    }
    if (engine_->isRunning()) {
        engine_->stop();
        return;
    }
    QString error;
    if (!engine_->start(&error)) {
        appendEvent(QStringLiteral("error"), QStringLiteral("启动失败"),
                    QString(), error, 0);
    }
    updateListenUi();
}

void MainWindow::updateListenUi() {
    const bool running = engine_ != nullptr && engine_->isRunning();
    if (statusDot_ != nullptr) {
        statusDot_->setStyleSheet(
            QStringLiteral("background: %1; border-radius: 3px;")
                .arg(running ? "#3fb950" : "#4a5568"));
    }
    if (statusText_ != nullptr) {
        statusText_->setText(running ? QStringLiteral("监听中")
                                     : QStringLiteral("未监听"));
        statusText_->setStyleSheet(
            running ? QStringLiteral("color:#3fb950; font-size:11px;"
                                     " font-weight:600;")
                    : QStringLiteral("color:#8b949e; font-size:11px;"));
    }
    if (listenButton_ != nullptr) {
        listenButton_->setText(running ? QStringLiteral("停止监听")
                                       : QStringLiteral("开始监听"));
    }
    // 无遥控器的语音触发：监听开着才注册全局热键，停了就还给别人。
    if (running) {
        registerVoiceHotkey();
    } else {
        unregisterVoiceHotkey();
    }
    if (voiceWindow_ != nullptr) {
        voiceWindow_->setListeningUi(running);
    }
    if (voicePet_ != nullptr) {
        voicePet_->setListening(running);
    }
    updateTrayState();
}

void MainWindow::registerVoiceHotkey() {
    unregisterVoiceHotkey();
    const VoiceOptions options = voice_->options();
    if (!options.kbHotkeyEnabled) {
        return;
    }
    UINT mods = 0;
    for (const QString &mod : options.kbHotkeyMods) {
        if (mod == QStringLiteral("ctrl")) {
            mods |= MOD_CONTROL;
        } else if (mod == QStringLiteral("alt")) {
            mods |= MOD_ALT;
        } else if (mod == QStringLiteral("shift")) {
            mods |= MOD_SHIFT;
        } else if (mod == QStringLiteral("win")) {
            mods |= MOD_WIN;
        }
    }
    if (RegisterHotKey(reinterpret_cast<HWND>(winId()), kVoiceHotkeyId, mods,
                       UINT(options.kbHotkeyVk))) {
        voiceHotkeyRegistered_ = true;
        return;
    }
    appendEvent(QStringLiteral("error"), QString(), QString(),
                QString::fromUtf8("全局语音热键注册失败（组合键被别的程序占用），"
                                  "可在「模型设置」里换一个"),
                0);
}

void MainWindow::unregisterVoiceHotkey() {
    if (!voiceHotkeyRegistered_) {
        return;
    }
    UnregisterHotKey(reinterpret_cast<HWND>(winId()), kVoiceHotkeyId);
    voiceHotkeyRegistered_ = false;
}

void MainWindow::showVoicePanel() {
    if (voiceWindow_ == nullptr) {
        voiceWindow_ = new VoiceWindow(voice_, store_.get());
        voiceWindow_->setListeningUi(engine_ != nullptr && engine_->isRunning());
        connect(voiceWindow_, &VoiceWindow::fullUiRequested, this, [this] {
            store_->setUiBool("voice_mode", false);
            voiceWindow_->hide();
            wake();
        });
        // 面板收起（✕）时把桌宠叫出来，监听状态不丢。
        connect(voiceWindow_, &VoiceWindow::panelHidden, this, [this] {
            voicePet_->presentIfEnabled();
        });
        // 面板里的「朗读最近」交给桌宠的 TTS，面板本身不持有 TTS。
        connect(voiceWindow_, &VoiceWindow::speakRequested, this,
                [this](const QString &text) {
                    if (voicePet_ != nullptr) {
                        voicePet_->speak(text);
                    }
                });
    }
    store_->setUiBool("voice_mode", true);
    if (const QPoint cursor = QCursor::pos();
        QGuiApplication::screenAt(cursor) != nullptr) {
        const QRect screen = QGuiApplication::screenAt(cursor)->geometry();
        voiceWindow_->move(screen.center() - QRect(QPoint(0, 0),
                                                   voiceWindow_->size())
                                                  .center());
    }
    hide();
    voiceWindow_->show();
    voiceWindow_->raise();
    voiceWindow_->activateWindow();
}

void MainWindow::refreshVoiceUi() {
    if (voice_ == nullptr || voiceState_ == nullptr) {
        return;
    }
    const VoiceOptions options = voice_->options();
    if (voiceBackend_ != nullptr) {
        const int index = voiceBackend_->findData(voiceBackendKey(options.backend));
        if (index >= 0 && index != voiceBackend_->currentIndex()) {
            const QSignalBlocker blocker(voiceBackend_);
            voiceBackend_->setCurrentIndex(index);
        }
    }
    if (options.backend == VoiceBackend::Hotkey) {
        setVoiceStatus(voice_->statusText(), QStringLiteral("#9ec1ff"), true);
        if (voiceFoot_ != nullptr) {
            voiceFoot_->setText(QString::fromUtf8("0 模型 · 跟随前台场景转发"));
        }
    } else if (options.backend == VoiceBackend::CloudAsr) {
        // 云端路线没有「拖模型包」这个动作，配置入口走语音设置。
        const bool ready = voice_->modelReady();
        setVoiceStatus(voice_->statusText(),
                       ready ? QStringLiteral("#c9d1d9")
                             : QStringLiteral("#d29922"),
                       true);
        if (voiceFoot_ != nullptr) {
            voiceFoot_->setText(
                QString::fromUtf8("云端转写 · 音频将上传到所填服务"));
        }
    } else {
        const bool ready = voice_->modelReady();
        if (ready) {
            voiceState_->setStyleSheet(QString());
            setVoiceStatus(voice_->statusText(), QStringLiteral("#c9d1d9"), true);
            if (voiceFoot_ != nullptr) {
                voiceFoot_->setText(QString::fromUtf8("本地转写 · 音频不出本机"));
            }
        } else {
            // 没装模型时，状态框本身就是拖拽区。
            voiceState_->setTextFormat(Qt::PlainText);
            voiceState_->setStyleSheet(QStringLiteral(
                "QLabel#voiceBox { border: 1px dashed rgba(120,170,255,150);"
                " border-radius: 10px; background: rgba(79,140,255,18);"
                " color: #9ec1ff; padding: 8px; }"));
            voiceState_->setText(
                QString::fromUtf8("⇣  把模型包（.zip）拖到这里导入"));
            voiceState_->setToolTip(
                QString::fromUtf8("也可以点「模型设置 → 导入模型包…」选择文件"));
            if (voiceFoot_ != nullptr) {
                voiceFoot_->setText(
                    QString::fromUtf8("未安装模型 · 拖入模型包，或点「模型设置」"));
            }
        }
    }
    if (voiceTest_ != nullptr) {
        // 只随识别方式变；录音中不再闪烁变灰（那会让人以为按钮被按了）。
        voiceTest_->setEnabled(options.backend != VoiceBackend::Hotkey);
    }
}

void MainWindow::refreshRemoteCard() {
    if (remotePanel_ == nullptr) {
        return;
    }
    const Profile &profile = store_->current(true);
    auto actionFor = [&](int vk) {
        const QString semantic =
            profile.keys.value(QString::number(vk)).toString();
        if (semantic.isEmpty()) {
            return QString::fromUtf8("未映射");
        }
        const QJsonObject action =
            profile.actions.value(semantic).toObject();
        if (action.isEmpty()) {
            return QString::fromUtf8("无动作");
        }
        return Presets::shortDesc(action, vk);
    };

    const VoiceOptions voice = voice_->options();
    QString voiceText;
    if (voice.backend == VoiceBackend::Hotkey) {
        voiceText = voiceComboText(voice.hotkeyMods, voice.hotkeyVk);
        voiceText += voice.hotkeyTrigger == QStringLiteral("toggle")
                         ? QString::fromUtf8(" · 开关")
                         : QString::fromUtf8(" · 按住");
    } else {
        voiceText = voiceBackendLabel(voice.backend);
    }

    const QVector<RemotePanel::Entry> entries = {
        {0, QStringLiteral("○"), QString::fromUtf8("电源"),
         QString::fromUtf8("红外"), true},
        {122, QStringLiteral("◎"), QString::fromUtf8("对焦"), actionFor(122),
         false},
        {116, QString(), QString::fromUtf8("语音"), voiceText, false},
        {38, QStringLiteral("↑"), QString::fromUtf8("上键"), actionFor(38), false},
        {40, QStringLiteral("↓"), QString::fromUtf8("下键"), actionFor(40), false},
        {37, QStringLiteral("←"), QString::fromUtf8("左键"), actionFor(37), false},
        {39, QStringLiteral("→"), QString::fromUtf8("右键"), actionFor(39), false},
        {13, QStringLiteral("OK"), QString::fromUtf8("OK"), actionFor(13), false},
        {27, QStringLiteral("↩"), QString::fromUtf8("返回"), actionFor(27), false},
        {93, QStringLiteral("☰"), QString::fromUtf8("菜单"), actionFor(93), false},
        {36, QStringLiteral("⌂"), QString::fromUtf8("主页"), actionFor(36), false},
        {0, QStringLiteral("VOL"), QString::fromUtf8("音量"),
         QString::fromUtf8("红外"), true},
    };
    remotePanel_->setEntries(entries);
}

void MainWindow::applyRemoteMode(const QString &badge, const QSet<int> &keys,
                                 bool on) {
    if (remoteBadge_ != nullptr) {
        remoteBadge_->setText(on ? badge : QString::fromUtf8("待机"));
    }
    if (remotePanel_ != nullptr) {
        remotePanel_->setModeKeys(on ? keys : QSet<int>());
    }
}

void MainWindow::openThemeDialog() {
    ThemeDialog dialog(store_.get(), this);
    const QSize size = dialog.sizeHint();
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect area = screen->availableGeometry();
        dialog.move(area.x() + (area.width() - size.width()) / 2,
                    area.y() + (area.height() - size.height()) / 2);
    }
    dialog.exec();
}

void MainWindow::openVoiceSettings() {
    auto *dialog = new VoiceSettingsDialog(store_.get(), this);
    connect(dialog, &VoiceSettingsDialog::saved, this, [this] {
        voice_->reload();
        refreshVoiceUi();
        refreshRemoteCard();
        // 键盘热键可能改了：注销再按新配置注册。
        if (engine_ != nullptr && engine_->isRunning()) {
            registerVoiceHotkey();
        }
        appendEvent(QStringLiteral("status"), QString(),
                    QString(), QString::fromUtf8("语音设置已保存"), 0);
        if (toast_ != nullptr) {
            const bool local =
                voice_->options().backend != VoiceBackend::Hotkey;
            toast_->showText(
                local ? QString::fromUtf8(
                            "已切到本地识别：语音键按住说话，松手转写并粘贴")
                      : QString::fromUtf8("已切回转发：语音键转发当前应用自己的语音快捷键"),
                3200);
        }
    });
    // 子对话框保存的 TTS/云端 ASR/桌宠对话配置立即生效，不等外层「保存」。
    connect(dialog, &VoiceSettingsDialog::optionsApplied, this, [this] {
        voice_->reload();
        refreshVoiceUi();
    });
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::setVoiceStatus(const QString &detail, const QString &color,
                                 bool showTranscript) {
    if (voiceState_ == nullptr) {
        return;
    }
    QString html = QStringLiteral("<span style=\"color:%1;\">%2</span>")
                       .arg(color, detail.toHtmlEscaped());
    if (showTranscript && !lastTranscript_.isEmpty()) {
        QString text = lastTranscript_;
        if (text.size() > 40) {
            text = text.left(40) + QString::fromUtf8("…");
        }
        html += QStringLiteral("<br><span style=\"color:#e6edf3;\">%1</span>")
                    .arg(text.toHtmlEscaped());
        voiceState_->setToolTip(lastTranscript_);
    } else {
        voiceState_->setToolTip(QString());
    }
    voiceState_->setTextFormat(Qt::RichText);
    voiceState_->setText(html);
}

void MainWindow::onVoiceState(const QString &state, const QString &detail) {
    if (state == QStringLiteral("idle")) {
        const bool warn = detail.contains(QString::fromUtf8("太短")) ||
                          detail.contains(QString::fromUtf8("未"));
        const QString shown =
            detail == QStringLiteral("就绪") ||
                    detail == QStringLiteral("语音快捷键已结束")
                ? voice_->statusText()
                : detail;
        setVoiceStatus(shown,
                       warn ? QStringLiteral("#d29922")
                            : QStringLiteral("#c9d1d9"),
                       true);
    } else {
        lastTranscript_.clear();
        setVoiceStatus(detail, QStringLiteral("#7c9cff"), false);
    }
    if (voiceTest_ != nullptr) {
        voiceTest_->setEnabled(voice_->options().backend != VoiceBackend::Hotkey);
    }
}

void MainWindow::onVoiceTranscribed(const QString &text, int costMs) {
    lastTranscript_ = text;
    appendEvent(QStringLiteral("voice"), QString(), QString(),
                QStringLiteral("✓ %1s · %2")
                    .arg(costMs / 1000.0, 0, 'f', 1)
                    .arg(text),
                0);
    refreshVoiceUi();
}

void MainWindow::onEngineEvent(const QString &kind, const QString &name,
                               const QString &profile, const QString &detail,
                               int vk) {
    if (kind == QStringLiteral("status")) {
        const bool on = detail.contains(QString::fromUtf8(" 开"));
        if (name == QString::fromUtf8("音量/亮度模式")) {
            applyRemoteMode(QString::fromUtf8("音量 · 亮度"),
                            QSet<int>{37, 38, 39, 40}, on);
            if (toast_ != nullptr) {
                toast_->showText(
                    on ? QString::fromUtf8(
                             "音量 / 亮度模式 · ↑↓ 音量，←→ 屏幕亮度；单击返回/菜单退出")
                       : QString::fromUtf8("已退出音量 / 亮度模式"));
            }
        } else if (name == QString::fromUtf8("鼠标模式")) {
            applyRemoteMode(QString::fromUtf8("鼠标模式"),
                            QSet<int>{37, 38, 39, 40, 13}, on);
            if (toast_ != nullptr) {
                toast_->showText(
                    on ? QString::fromUtf8(
                             "鼠标模式 · 方向键移动，OK 左键；单击菜单退出")
                       : QString::fromUtf8("已退出鼠标模式"));
            }
        } else if (name == QString::fromUtf8("导航模式")) {
            applyRemoteMode(QString::fromUtf8("导航模式"),
                            QSet<int>{37, 38, 39, 40}, on);
            if (toast_ != nullptr) {
                toast_->showText(
                    on ? QString::fromUtf8(
                             "导航模式 · ←→ 切换控件，↑↓ 翻页；单击主页退出")
                       : QString::fromUtf8("已退出导航模式"));
            }
        }
    }
    if (kind == QStringLiteral("key") && vk != 0) {
        if (remotePanel_ != nullptr) {
            remotePanel_->flash(vk);
        }
        if (remoteState_ != nullptr && remotePanel_ != nullptr) {
            const QString text = name.isEmpty() ? detail : name;
            const QString label = remotePanel_->labelFor(vk);
            if (!text.isEmpty()) {
                remoteState_->setText(label.isEmpty()
                                          ? text
                                          : label + QStringLiteral(" · ") + text);
            }
        }
    }
    if (guide_ != nullptr && guide_->isVisible() &&
        kind == QStringLiteral("key") && vk != 0) {
        guide_->noteKey(Presets::remoteKeyName(vk));
    }
    appendEvent(kind, name, profile, detail, vk);
}

void MainWindow::onEngineRunning(bool running, const QString &message) {
    updateListenUi();
    refreshRemoteCard();
    if (remoteDot_ != nullptr) {
        remoteDot_->setStyleSheet(
            running ? QStringLiteral("background:#3fb950; border-radius:3px;")
                    : QStringLiteral("background:#4a5568; border-radius:3px;"));
    }
    if (remoteState_ != nullptr) {
        remoteState_->setText(
            running ? QString::fromUtf8("监听中 · ") +
                          voiceProfileLabel(store_->current(true).name)
                    : QString::fromUtf8("未监听"));
    }
    if (!running) {
        applyRemoteMode(QString(), QSet<int>(), false);
    }
    appendEvent(QStringLiteral("status"),
                running ? QStringLiteral("监听") : QStringLiteral("停止"),
                QString(), message, 0);
}

void MainWindow::appendEvent(const QString &kind, const QString &name,
                             const QString &profile, const QString &detail,
                             int vk) {
    Q_UNUSED(vk);
    if (logList_ == nullptr) {
        return;
    }
    const QString stamp = QTime::currentTime().toString("HH:mm:ss");
    QString text = stamp + "  ";
    if (kind == "key") {
        text += profile + QStringLiteral(" · ") + name;
        if (!detail.isEmpty()) {
            text += QStringLiteral(" · ") + detail;
        }
    } else {
        text += detail.isEmpty() ? name : detail;
    }
    auto *item = new QListWidgetItem(text, logList_);
    QColor color("#c9d1d9");
    if (kind == "error") {
        color = QColor("#f85149");
    } else if (kind == "voice") {
        color = QColor("#3fb950");
    } else if (kind == "status") {
        color = QColor("#8b949e");
    }
    item->setForeground(color);
    logList_->addItem(item);
    logList_->scrollToBottom();
    // 空状态是一整块（标题 + 说明），藏在 logEmpty_ 的父控件上。
    if (QWidget *emptyBox = logEmpty_->parentWidget()) {
        emptyBox->hide();
    }
    logList_->show();
    logCount_->setText(QStringLiteral("%1 条").arg(logList_->count()));
}

void MainWindow::clearLog() {
    if (logList_ == nullptr) {
        return;
    }
    logList_->clear();
    logList_->hide();
    if (QWidget *emptyBox = logEmpty_->parentWidget()) {
        emptyBox->show();
    }
    logCount_->setText(QStringLiteral("0 条"));
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message,
                             qintptr *result) {
    static const UINT wakeMessage =
        RegisterWindowMessageW(L"AgentRMC.Native.Wake");
    auto *msg = static_cast<MSG *>(message);
    if (msg != nullptr && wakeMessage != 0 && msg->message == wakeMessage) {
        wake();
        *result = 0;
        return true;
    }
    // 全局语音热键（无遥控器触发）：一键开始/结束，复用耳机键的状态机。
    if (msg != nullptr && msg->message == WM_HOTKEY &&
        msg->wParam == kVoiceHotkeyId) {
        voice_->mediaToggle();
        *result = 0;
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    const QEvent::Type type = event->type();
    if (type == QEvent::DragEnter) {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (!modelPackageFromMime(drag->mimeData()).isEmpty()) {
            drag->acceptProposedAction();
            showDropHint(true);
            return true;
        }
    } else if (type == QEvent::DragLeave) {
        showDropHint(false);
    } else if (type == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event);
        const QString file = modelPackageFromMime(drop->mimeData());
        showDropHint(false);
        if (!file.isEmpty()) {
            drop->acceptProposedAction();
            importDroppedModel(file);
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::showDropHint(bool on) {
    if (dropHint_ == nullptr) {
        return;
    }
    if (!on) {
        dropHint_->hide();
        return;
    }
    dropHint_->setGeometry(rect());
    dropHint_->raise();
    dropHint_->show();
}

void MainWindow::importDroppedModel(const QString &file) {
    const ModelImportResult result = importModelPackage(this, file);
    if (!result.ok) {
        appendEvent(QStringLiteral("error"), QString::fromUtf8("模型导入失败"),
                    QString(), result.error, 0);
        if (toast_ != nullptr) {
            toast_->showText(QString::fromUtf8("模型导入失败：%1").arg(result.error),
                             3200);
        }
        return;
    }
    const QString name = result.backend == QStringLiteral("qwen3")
                             ? QStringLiteral("Qwen3-ASR")
                             : QStringLiteral("SenseVoice");
    // 不擅自改识别方式：正在用的路线（默认转发）照旧，要用本地得自己切。
    appendEvent(QStringLiteral("voice"), QString::fromUtf8("模型已导入"),
                QString(),
                QString::fromUtf8("%1 已装好；要用本地识别，在「模型设置 → "
                                  "识别方式」里切过去")
                    .arg(name),
                0);
    refreshVoiceUi();
    if (toast_ != nullptr) {
        toast_->showText(
            QString::fromUtf8("%1 已装好，可在「模型设置」里切到本地识别").arg(name),
            3200);
    }
}

void MainWindow::checkUpdates(bool manual) {
    if (updating_) {
        return;
    }
    updating_ = true;
    if (updateButton_ != nullptr) {
        updateButton_->setText(QString::fromUtf8("检查中…"));
    }
    QString url = store_->config()
                      .value(QStringLiteral("ui"))
                      .toObject()
                      .value(QStringLiteral("update_url"))
                      .toString();
    if (url.trimmed().isEmpty()) {
        url = QStringLiteral("https://ligdesign.win/agentrmc-update.json");
    }
    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start([self, url, manual] {
        const UpdateInfo info = fetchUpdateInfo(url);
        if (self == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            self, [self, info, manual] { self->onUpdateChecked(info, manual); },
            Qt::QueuedConnection);
    });
}

void MainWindow::onUpdateChecked(const UpdateInfo &info, bool manual) {
    updating_ = false;
    if (updateButton_ != nullptr) {
        updateButton_->setText(QString::fromUtf8("检查更新"));
    }
    QSettings settings;
    if (!info.ok) {
        if (manual && toast_ != nullptr) {
            toast_->showText(QString::fromUtf8("检查更新失败：%1").arg(info.error),
                             3200);
        }
        return;
    }
    settings.setValue(QStringLiteral("update/last_check"),
                      QDateTime::currentSecsSinceEpoch());
    if (!versionIsNewer(info.version, appVersion())) {
        if (manual && toast_ != nullptr) {
            toast_->showText(
                QString::fromUtf8("已是最新版本 v%1").arg(appVersion()), 2400);
        }
        return;
    }
    if (!manual &&
        settings.value(QStringLiteral("update/skip_version")).toString() ==
            info.version) {
        return;
    }
    showUpdateDialog(info);
}

void MainWindow::showUpdateDialog(const UpdateInfo &info) {
    QDialog dialog(this);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setFixedWidth(560);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #0f141d; border: 1px solid rgba(255,255,255,24);"
        " border-radius: 12px; }"
        "QLabel { color: #c9d1d9; font-size: 12px; }"
        "QLabel#newVer { color: #e6edf3; font-size: 15px; font-weight: 600; }"
        "QCheckBox { color: #8b949e; font-size: 11px; }"
        "QPushButton { color: #c9d1d9; background: rgba(255,255,255,8);"
        " border: 1px solid rgba(255,255,255,20); border-radius: 8px;"
        " padding: 5px 12px; }"
        "QPushButton:hover { background: rgba(255,255,255,14); }"
        "QPushButton#primary { background: #4f8cff; color: #ffffff;"
        " border: none; }"
        "QPushButton#primary:hover { background: #6ba0ff; }"));

    auto *lay = new QVBoxLayout(&dialog);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(10);

    auto *title =
        new QLabel(QString::fromUtf8("发现新版本 v%1").arg(info.version));
    title->setObjectName("newVer");
    lay->addWidget(title);

    QString meta = QString::fromUtf8("当前 v%1").arg(appVersion());
    if (!info.date.isEmpty()) {
        meta = QString::fromUtf8("%1 · 发布于 %2").arg(meta, info.date);
    }
    lay->addWidget(new QLabel(meta));

    if (!info.title.isEmpty()) {
        auto *headline = new QLabel(info.title);
        headline->setStyleSheet(QStringLiteral("color:#e6edf3;"));
        headline->setWordWrap(true);
        lay->addWidget(headline);
    }
    for (const QString &note : info.notes) {
        auto *line = new QLabel(QString::fromUtf8("· ") + note);
        line->setWordWrap(true);
        lay->addWidget(line);
    }

    auto *skip = new QCheckBox(QString::fromUtf8("跳过这个版本"));
    lay->addWidget(skip);

    auto *buttons = new QHBoxLayout;
    auto *later = new QPushButton(QString::fromUtf8("稍后"));
    connect(later, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto *quark = new QPushButton(QString::fromUtf8("夸克网盘"));
    quark->setVisible(!info.quark.isEmpty());
    connect(quark, &QPushButton::clicked, &dialog, [&dialog, info] {
        QDesktopServices::openUrl(QUrl(info.quark));
        dialog.accept();
    });
    auto *download = new QPushButton(QString::fromUtf8("去下载"));
    download->setObjectName("primary");
    connect(download, &QPushButton::clicked, &dialog, [&dialog, info] {
        const QString target =
            !info.baidu.isEmpty() ? info.baidu
                                  : (!info.site.isEmpty() ? info.site : info.quark);
        if (!target.isEmpty()) {
            QDesktopServices::openUrl(QUrl(target));
        }
        dialog.accept();
    });
    buttons->addWidget(skip);
    buttons->addStretch(1);
    buttons->addWidget(later);
    buttons->addWidget(quark);
    buttons->addWidget(download);
    lay->addLayout(buttons);

    dialog.exec();
    if (skip->isChecked()) {
        QSettings settings;
        settings.setValue(QStringLiteral("update/skip_version"), info.version);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (tray_ && !quitting_ && store_->uiBool("close_to_tray", true)) {
        hide();
        // 关窗进托盘：语音桌宠顶上，监听/录音状态一眼可见。
        voicePet_->presentIfEnabled();
        if (!trayHintShown_) {
            trayHintShown_ = true;
            tray_->showMessage(
                QString::fromUtf8("AgentRMC 仍在后台"),
                QString::fromUtf8("监听继续；点托盘图标叫回主界面"),
                QSystemTrayIcon::Information, 3000);
        }
        event->ignore();
        return;
    }
    quitting_ = true;
    QMainWindow::closeEvent(event);
    QApplication::quit();
}
