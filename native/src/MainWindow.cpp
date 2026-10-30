#include "MainWindow.h"

#include "ui/KeymapPage.h"
#include "ui/VoiceSettingsDialog.h"

#include "core/InputEngine.h"
#include "core/VoiceController.h"

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

class RemoteCanvas final : public QWidget {
public:
    explicit RemoteCanvas(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(300);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal bodyW = 110.0;
        const qreal bodyH = qMin<qreal>(height() - 12.0, 330.0);
        const QRectF body((width() - bodyW) / 2.0, (height() - bodyH) / 2.0,
                          bodyW, bodyH);
        p.setPen(QPen(QColor(255, 255, 255, 22), 1.0));
        p.setBrush(QColor(8, 12, 18));
        p.drawRoundedRect(body, 22, 22);

        const qreal cx = body.center().x();
        auto ring = [&](qreal y, qreal r, const QColor &color) {
            p.setPen(QPen(color, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(cx, y), r, r);
        };
        ring(body.top() + 22, 5.0, QColor(255, 255, 255, 55));
        ring(body.top() + 44, 4.0, QColor(255, 255, 255, 32));

        const QPointF ok(cx, body.top() + 108);
        p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
        p.setBrush(QColor(15, 22, 32));
        p.drawEllipse(ok, 31, 31);
        p.setPen(QPen(QColor(255, 255, 255, 90), 1.2));
        p.drawEllipse(ok, 16, 16);

        QFont f = font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(230, 237, 243));
        p.drawText(QRectF(ok.x() - 16, ok.y() - 8, 32, 16), Qt::AlignCenter, "OK");

        QFont g = font();
        g.setPixelSize(11);
        p.setFont(g);
        p.setPen(QColor(200, 210, 222));
        auto arrow = [&](qreal dx, qreal dy, const QString &glyph) {
            p.drawText(QRectF(ok.x() + dx - 10, ok.y() + dy - 8, 20, 16),
                       Qt::AlignCenter, glyph);
        };
        arrow(0, -47, QString::fromUtf8("↑"));
        arrow(0, 47, QString::fromUtf8("↓"));
        arrow(-47, 0, QString::fromUtf8("←"));
        arrow(47, 0, QString::fromUtf8("→"));

        const qreal chipsY = body.top() + 166;
        for (int i = 0; i < 3; ++i) {
            const QRectF chip(body.left() + 18 + i * 26, chipsY, 20, 18);
            p.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
            p.setBrush(QColor(14, 20, 29));
            p.drawRoundedRect(chip, 6, 6);
        }
        p.setPen(QColor(160, 172, 186));
        p.drawText(QRectF(body.left() + 18, chipsY, 20, 18), Qt::AlignCenter,
                   QString::fromUtf8("←"));
        p.drawText(QRectF(body.left() + 44, chipsY, 20, 18), Qt::AlignCenter,
                   QString::fromUtf8("≡"));
        p.drawText(QRectF(body.left() + 70, chipsY, 20, 18), Qt::AlignCenter,
                   QString::fromUtf8("⌂"));

        const QRectF volume(cx - 8, body.top() + 202, 16, bodyH - 226);
        p.setPen(QPen(QColor(255, 255, 255, 22), 1.0));
        p.setBrush(QColor(12, 17, 25));
        p.drawRoundedRect(volume, 8, 8);
    }
};

}  // namespace

MainWindow::MainWindow(bool withTray, const QString &pin, QWidget *parent)
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
                if (!voice_->busy()) {
                    refreshVoiceUi();
                }
            });
    connect(engine_, &InputEngine::voiceDown, voice_, &VoiceController::voiceDown);
    connect(engine_, &InputEngine::voiceUp, voice_, &VoiceController::voiceUp);
    connect(engine_, &InputEngine::mediaVoice, voice_,
            &VoiceController::mediaToggle);
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
    tabs->addTab(keymapPage_, QString::fromUtf8("按键映射"));
    tabs->addTab(buildGuidePage(), QString::fromUtf8("使用引导"));
    tabs->setCurrentIndex(1);
    grid->addWidget(tabs, 1);

    outer->addWidget(body, 1);

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
    auto *badge = new QLabel("M4");
    badge->setObjectName("badge");
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(badge);
    lay->addLayout(head);

    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    auto *canvas = new RemoteCanvas;
    canvas->setFixedWidth(126);
    row->addWidget(canvas, 0, Qt::AlignTop);

    auto *info = new QVBoxLayout;
    info->setSpacing(10);
    info->setContentsMargins(0, 6, 0, 0);
    const QStringList labels = {
        QString::fromUtf8("电源（红外）"),
        QString::fromUtf8("对焦 · 全屏"),
        QString::fromUtf8("语音 · 按住说话"),
        QString::fromUtf8("方向 · 滚动 / OK 发送"),
        QString::fromUtf8("返回 · 菜单 · 主页"),
        QString::fromUtf8("音量（红外）"),
    };
    for (const QString &text : labels) {
        auto *label = new QLabel(text);
        label->setObjectName("muted");
        label->setWordWrap(true);
        info->addWidget(label);
    }
    info->addStretch(1);
    row->addLayout(info, 1);
    lay->addLayout(row, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(7);
    auto *dot = new QLabel;
    dot->setObjectName("dot");
    dot->setFixedSize(6, 6);
    auto *state = new QLabel(QString::fromUtf8("等待迁移"));
    state->setObjectName("muted");
    auto *bt = new QPushButton(QString::fromUtf8("蓝牙设置"));
    bt->setObjectName("ghost");
    bt->setEnabled(false);
    foot->addWidget(dot, 0, Qt::AlignVCenter);
    foot->addWidget(state);
    foot->addStretch(1);
    foot->addWidget(bt);
    lay->addLayout(foot);
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
        QString::fromUtf8("里程碑 5（当前）· 语音：Qoder 热键 / SenseVoice / Qwen3-ASR"),
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

void MainWindow::openVoiceSettings() {
    auto *dialog = new VoiceSettingsDialog(store_.get(), this);
    connect(dialog, &VoiceSettingsDialog::saved, this, [this] {
        voice_->reload();
        refreshVoiceUi();
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
    appendEvent(kind, name, profile, detail, vk);
}

void MainWindow::onEngineRunning(bool running, const QString &message) {
    updateListenUi();
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
