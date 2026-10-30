#include "MainWindow.h"

#include "ui/KeymapPage.h"
#include "ui/RemotePanel.h"
#include "ui/Toast.h"
#include "ui/VoiceSettingsDialog.h"

#include "core/InputEngine.h"
#include "core/Presets.h"
#include "core/VoiceController.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
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
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWindow>

namespace {

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

}  // namespace

MainWindow::MainWindow(bool withTray, const QString &pin, bool autoListen,
                       QWidget *parent)
    : QMainWindow(parent) {
    store_ = std::make_unique<ProfileStore>(
        QCoreApplication::applicationDirPath() + "/profiles.json");
    store_->load();
    store_->loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
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
            [this](const QString &) {
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
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(880, 560);

    auto *root = new QWidget;
    root->setObjectName("root");
    setCentralWidget(root);

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
    leftLay->addWidget(buildRemoteCard(), 1);
    leftLay->addWidget(buildVoiceCard(), 0);
    grid->addWidget(left);

    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(false);
    tabs->addTab(buildLogPage(), QString::fromUtf8("事件流"));
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
    tabs->addTab(keymapPage_, QString::fromUtf8("按键映射"));
    tabs->addTab(buildGuidePage(), QString::fromUtf8("使用引导"));
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (remotePanel_ != nullptr) {
            remotePanel_->setLinkedVk(
                index == 1 && keymapPage_ != nullptr ? keymapPage_->selectedVk()
                                                     : 0);
        }
    });
    tabs->setCurrentIndex(1);
    grid->addWidget(tabs, 1);

    outer->addWidget(body, 1);

    toast_ = new Toast(this);
    refreshRemoteCard();
    for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
        button->setFocusPolicy(Qt::NoFocus);
    }
    if (autoListen && store_->uiBool("auto_listen", true)) {
        QTimer::singleShot(0, this, &MainWindow::toggleListening);
    }
    if (withTray) {
        buildTray();
    }
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

    auto *tag = new QLabel("NATIVE · M5");
    tag->setObjectName("tag");

    lay->addWidget(logo);
    lay->addWidget(brand);
    lay->addWidget(tag);
    lay->addStretch(1);

    statusDot_ = new QLabel;
    statusDot_->setStyleSheet(
        QStringLiteral("background: #4a5568; border-radius: 3px;"));
    statusDot_->setFixedSize(6, 6);
    statusText_ = new QLabel(QString::fromUtf8("未监听"));
    statusText_->setObjectName("muted");
    lay->addWidget(statusDot_, 0, Qt::AlignVCenter);
    lay->addWidget(statusText_);
    lay->addSpacing(6);

    auto *guide = new QPushButton(QString::fromUtf8("使用引导"));
    guide->setObjectName("ghost");
    guide->setCursor(Qt::PointingHandCursor);
    connect(guide, &QPushButton::clicked, this, &MainWindow::showMilestones);
    lay->addWidget(guide);

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

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(8);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("遥控器"));
    title->setObjectName("cardTitle");
    remoteBadge_ = new QLabel("M5");
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
    bt->setObjectName("ghost");
    bt->setEnabled(false);
    bt->setToolTip(QString::fromUtf8("BLE 电量/配对在里程碑 6 接入"));
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
    card->setFixedHeight(250);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(7);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("语音"));
    title->setObjectName("cardTitle");
    auto *badge = new QLabel("M5");
    badge->setObjectName("badge");
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(badge);
    lay->addLayout(head);

    voiceBackend_ = new QComboBox;
    voiceBackend_->addItem(QString::fromUtf8("转发热键 · 0 模型"), "hotkey");
    voiceBackend_->addItem(QString::fromUtf8("本地 SenseVoice"), "sensevoice");
    voiceBackend_->addItem(QString::fromUtf8("本地 Qwen3-ASR"), "qwen3");
    voiceBackend_->setCursor(Qt::PointingHandCursor);
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
                appendEvent(QStringLiteral("status"), QString(),
                            QString(),
                            QString::fromUtf8("语音后端切换为 ") +
                                voiceBackend_->itemText(index),
                            0);
            });
    lay->addWidget(voiceBackend_);

    voiceState_ = new QLabel;
    voiceState_->setObjectName("voiceBox");
    voiceState_->setWordWrap(true);
    voiceState_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    voiceState_->setMinimumHeight(48);
    lay->addWidget(voiceState_, 1);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(6);
    voiceTest_ = new QPushButton(QString::fromUtf8("测试 3 秒"));
    voiceTest_->setObjectName("ghostSmall");
    voiceTest_->setCursor(Qt::PointingHandCursor);
    connect(voiceTest_, &QPushButton::clicked, voice_,
            &VoiceController::startTest);
    auto *settings = new QPushButton(QString::fromUtf8("模型设置"));
    settings->setObjectName("ghostSmall");
    settings->setCursor(Qt::PointingHandCursor);
    connect(settings, &QPushButton::clicked, this,
            &MainWindow::openVoiceSettings);
    tools->addWidget(voiceTest_);
    tools->addWidget(settings);
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
    logCount_->setObjectName("muted");
    auto *clear = new QPushButton(QString::fromUtf8("清空"));
    clear->setObjectName("ghostText");
    clear->setCursor(Qt::PointingHandCursor);
    connect(clear, &QPushButton::clicked, this, &MainWindow::clearLog);
    head->addWidget(logCount_);
    head->addStretch(1);
    head->addWidget(clear);
    cl->addLayout(head);

    logEmpty_ = new QLabel(QString::fromUtf8("等待按键…"));
    logEmpty_->setObjectName("muted");
    logEmpty_->setAlignment(Qt::AlignCenter);
    cl->addWidget(logEmpty_, 1);

    logList_ = new QListWidget;
    logList_->setObjectName("logList");
    logList_->setFrameShape(QFrame::NoFrame);
    logList_->setSelectionMode(QAbstractItemView::NoSelection);
    logList_->setFocusPolicy(Qt::NoFocus);
    logList_->setWordWrap(true);
    logList_->setSpacing(2);
    logList_->hide();
    cl->addWidget(logList_, 1);
    lay->addWidget(card);
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
    cl->setSpacing(10);

    auto *title = new QLabel(QString::fromUtf8("原生壳迁移"));
    title->setObjectName("secTitle");
    cl->addWidget(title);

    const QStringList lines = {
        QString::fromUtf8("里程碑 1 已完成 · 窗口 / 主题 / 托盘 / 构建"),
        QString::fromUtf8("里程碑 2 已完成 · config + profiles + 可编辑映射表"),
        QString::fromUtf8("里程碑 3 已完成 · Raw Input + KeyRouter + 动作注入"),
        QString::fromUtf8("里程碑 4 已完成 · 原生键接管 / 鼠标模式"),
        QString::fromUtf8("里程碑 5（当前）· 语音三后端 / 遥控器实时高亮与映射"),
        QString::fromUtf8("里程碑 6 · BLE 电量 / 跨屏 DPI / windeployqt 打包"),
        QString::fromUtf8("Qt 6.8 没有 Multimedia 模块，录音走 WinMM；转写走 sherpa-onnx C API。"),
        QString::fromUtf8("Python 版继续保留，作为行为参照；两版共用 config/profiles 格式。"),
    };
    for (const QString &line : lines) {
        auto *label = new QLabel(line);
        label->setObjectName("secBody");
        label->setWordWrap(true);
        cl->addWidget(label);
    }
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
    auto *show = menu->addAction(QString::fromUtf8("显示主界面"));
    connect(show, &QAction::triggered, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    menu->addSeparator();
    auto *quit = menu->addAction(QString::fromUtf8("退出 AgentRMC Native"));
    connect(quit, &QAction::triggered, this, [this] {
        quitting_ = true;
        QApplication::quit();
    });
    tray_->setContextMenu(menu);
    tray_->setToolTip("AgentRMC Native · 里程碑 5");
    tray_->show();

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    showNormal();
                    raise();
                    activateWindow();
                }
            });
}

void MainWindow::showMilestones() {
    QMessageBox::information(
        this, "AgentRMC Native",
        QString::fromUtf8(
            "C++/Qt 原生迁移\n\n"
            "里程碑 1：窗口 / 主题 / 托盘 / 构建（已完成）\n"
            "2：config + profiles + 可编辑映射表（已完成）\n"
            "3：Raw Input + Router + 注入（已完成）\n"
            "4：原生键接管 / 鼠标模式（已完成）\n"
            "5：语音（当前）· Qoder 热键 / SenseVoice / Qwen3-ASR\n"
            "6：BLE / DPI / 打包"));
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
    }
    if (listenButton_ != nullptr) {
        listenButton_->setText(running ? QStringLiteral("停止监听")
                                       : QStringLiteral("开始监听"));
    }
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
    } else {
        const bool ready = voice_->modelReady();
        setVoiceStatus(voice_->statusText(),
                       ready ? QStringLiteral("#c9d1d9")
                             : QStringLiteral("#d29922"),
                       true);
        if (voiceFoot_ != nullptr) {
            voiceFoot_->setText(ready
                                    ? QString::fromUtf8("本地转写 · 音频不出本机")
                                    : QString::fromUtf8("模型未安装 · 打开“模型设置”"));
        }
    }
    if (voiceTest_ != nullptr) {
        voiceTest_->setEnabled(options.backend != VoiceBackend::Hotkey &&
                              !voice_->busy());
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
        {116, QStringLiteral("F5"), QString::fromUtf8("语音"), voiceText, false},
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
        remoteBadge_->setText(on ? badge : QStringLiteral("M5"));
    }
    if (remotePanel_ != nullptr) {
        remotePanel_->setModeKeys(on ? keys : QSet<int>());
    }
}

void MainWindow::openVoiceSettings() {
    auto *dialog = new VoiceSettingsDialog(store_.get(), this);
    connect(dialog, &VoiceSettingsDialog::saved, this, [this] {
        voice_->reload();
        refreshVoiceUi();
        refreshRemoteCard();
        appendEvent(QStringLiteral("status"), QString(),
                    QString(), QString::fromUtf8("语音设置已保存"), 0);
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
        voiceTest_->setEnabled(voice_->options().backend != VoiceBackend::Hotkey &&
                              !voice_->busy());
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
    logEmpty_->hide();
    logList_->show();
    logCount_->setText(QStringLiteral("%1 条").arg(logList_->count()));
}

void MainWindow::clearLog() {
    if (logList_ == nullptr) {
        return;
    }
    logList_->clear();
    logList_->hide();
    logEmpty_->show();
    logCount_->setText(QStringLiteral("0 条"));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (tray_ && !quitting_) {
        hide();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}
