#include "MainWindow.h"

#include "ui/AppPage.h"
#include "ui/GuideDialog.h"
#include "ui/KeymapPage.h"
#include "ui/RemotePanel.h"
#include "ui/Toast.h"
#include "ui/VoicePet.h"
#include "ui/VoiceSettingsDialog.h"
#include "ui/VoiceWindow.h"

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
#include <QMimeData>
#include <QSettings>
#include <QDateTime>
#include <QDialog>
#include <QPointer>
#include <QThreadPool>
#include <QCursor>
#include <QDesktopServices>
#include <QGuiApplication>
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

// 全局语音热键（无遥控器触发用）的注册 ID。
constexpr UINT kVoiceHotkeyId = 0xB00B;

}  // namespace

MainWindow::MainWindow(bool withTray, const QString &pin, bool autoListen,
                       QWidget *parent)
    : QMainWindow(parent) {
    store_ = std::make_unique<ProfileStore>(
        QCoreApplication::applicationDirPath() + "/profiles.json");
    store_->load();
    store_->loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
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
    setMinimumSize(880, 560);

    auto *root = new QWidget;
    root->setObjectName("root");
    // 主题背景图走 QSS background-image；plain QWidget 显式声明按样式表绘制。
    root->setAttribute(Qt::WA_StyledBackground, true);
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
    tabs->addTab(appPage_, QString::fromUtf8("应用"));
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
    // 语音桌宠：主窗/语音面板关掉之后留在屏幕上的小球，跟着监听状态变色。
    voicePet_ = new VoicePet(store_.get(), voice_);
    connect(voicePet_, &VoicePet::fullUiRequested, this, &MainWindow::wake);
    connect(voicePet_, &VoicePet::panelRequested, this,
            &MainWindow::showVoicePanel);
    connect(voicePet_, &VoicePet::listenToggleRequested, this,
            &MainWindow::toggleListening);
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

    statusDot_ = new QLabel;
    statusDot_->setStyleSheet(
        QStringLiteral("background: #4a5568; border-radius: 3px;"));
    statusDot_->setFixedSize(6, 6);
    statusText_ = new QLabel(QString::fromUtf8("未监听"));
    statusText_->setObjectName("muted");
    lay->addWidget(statusDot_, 0, Qt::AlignVCenter);
    lay->addWidget(statusText_);
    lay->addSpacing(6);

    updateButton_ = new QPushButton(QString::fromUtf8("检查更新"));
    updateButton_->setObjectName("ghostText");
    updateButton_->setCursor(Qt::PointingHandCursor);
    connect(updateButton_, &QPushButton::clicked, this,
            [this] { checkUpdates(true); });
    lay->addWidget(updateButton_);

    auto *guideButton = new QPushButton(QString::fromUtf8("使用引导"));
    guideButton->setObjectName("ghost");
    guideButton->setCursor(Qt::PointingHandCursor);
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
    card->setFixedHeight(250);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(7);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("语音"));
    title->setObjectName("cardTitle");
    head->addWidget(title);
    head->addStretch(1);
    lay->addLayout(head);

    voiceBackend_ = new QComboBox;
    voiceBackend_->addItem(QString::fromUtf8("转发热键 · 0 模型"), "hotkey");
    voiceBackend_->addItem(QString::fromUtf8("本地 SenseVoice"), "sensevoice");
    voiceBackend_->addItem(QString::fromUtf8("本地 Qwen3-ASR"), "qwen3");
    voiceBackend_->addItem(QString::fromUtf8("云端 ASR（需联网）"), "api");
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
    lay->addWidget(voiceBackend_);

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
    cl->setSpacing(16);

    struct Line {
        const char *key;
        const char *text;
    };
    const auto addSection = [cl](const QString &name,
                                 const QVector<Line> &lines) {
        auto *box = new QWidget;
        auto *boxLay = new QVBoxLayout(box);
        boxLay->setContentsMargins(0, 0, 0, 0);
        boxLay->setSpacing(9);
        boxLay->addWidget(guide::heading(name));
        for (const Line &line : lines) {
            boxLay->addWidget(guide::row(QString::fromUtf8(line.key),
                                         QString::fromUtf8(line.text), 92));
        }
        cl->addWidget(box);
    };

    addSection(QString::fromUtf8("第一次使用：配对遥控器"),
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
    addSection(QString::fromUtf8("语音怎么用"),
               {{"默认", "遥控器语音键转发给当前应用自己的语音快捷键"
                         "（Qoder Ctrl+Shift+V / WorkBuddy Ctrl+D），不用装模型"},
                {"装本地模型", "把模型包（.zip）拖进窗口，或点「模型设置 → 导入模型包…」；"
                               "装好后在「识别方式」里切到本地才会用上"},
                {"什么时候用本地", "本地转写是通用输入：任何软件的输入框里都能直接出字；"
                                   "应用没有语音快捷键、或想离线识别时用它。SenseVoice 约 230MB"},
                {"耳机", "电脑上没有媒体在播放时，耳机播放/暂停 = 同一个语音开关"}});
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
    links->addWidget(site);
    links->addWidget(github);
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
