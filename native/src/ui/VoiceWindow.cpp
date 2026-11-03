#include "VoiceWindow.h"

#include "Toast.h"
#include "TtsSettingsDialog.h"
#include "VoiceSettingsDialog.h"
#include "core/AgentWindow.h"
#include "core/Profiles.h"
#include "core/ThemeManager.h"
#include "core/VoiceController.h"
#include "core/VoiceSettings.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace {

// 自绘控件也必须走调色板：写死深色的话，classic 浅色下会露出黑底，
// 而子控件（QSS 令牌）已经翻成浅底深字，两边叠在一起谁都读不出。
// token 缺失/解析不了才回落到深色原值，别画成透明。
QColor paletteColor(const QString &token, const QColor &fallback) {
    const QColor color = ThemeManager::instance().color(token);
    return color.isValid() ? color : fallback;
}

// 大圆麦克风按钮：Idle 蓝环 + 麦克风图形，录音变绿环（随电平加粗）+ 停止方块。
class MicButton final : public QPushButton {
public:
    explicit MicButton(QWidget *parent = nullptr) : QPushButton(parent) {
        setFixedSize(124, 124);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFlat(true);
        setObjectName("micButton");
        setAccessibleName(QString::fromUtf8("麦克风按钮"));
    }

    void setLevel(double level) {
        level_ = qBound(0.0, level, 1.0);
        update();
    }

    void setRecording(bool on) {
        if (recording_ == on) {
            return;
        }
        recording_ = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF area = rect();
        const QPointF center = area.center();
        // 几何随控件尺寸缩放（嵌入 64 / 独立 124）：写死 124 的几何在小尺寸
        // 上会把内盘裁成方块（2026-11-03 真机）。
        const double side = qMin(area.width(), area.height());
        // 环宽随尺寸缩放；内盘贴环内缘，不留背景色缝——留缝会看起来是
        // 两个分离的圆而不是一个按钮（2026-11-03 用户反馈）。
        const double ringW =
            qMax(2.0, side * (recording_ ? 0.024 + level_ * 0.04 : 0.032));
        QPen ring(recording_
                      ? QColor(0x3f, 0xb9, 0x50)
                      : paletteColor(QStringLiteral("accent"),
                                     QColor(79, 140, 255)));
        ring.setWidthF(ringW);
        painter.setPen(ring);
        painter.setBrush(Qt::NoBrush);
        const double radius = side / 2.0 - ringW / 2.0;
        painter.drawEllipse(center, radius, radius);

        painter.setPen(Qt::NoPen);
        painter.setBrush(paletteColor(QStringLiteral("card.bg"),
                                      QColor(0x12, 0x17, 0x22)));
        const double disc = radius - ringW / 2.0;
        painter.drawEllipse(center, disc, disc);
        // glyph 随内盘缩放，保持与盘面的比例不变。
        const double k = disc / 44.0;

        if (recording_) {
            painter.setBrush(QColor(0xf8, 0x51, 0x49));
            painter.drawRoundedRect(QRectF(center.x() - 13 * k, center.y() - 13 * k,
                                           26 * k, 26 * k), 5 * k, 5 * k);
            return;
        }

        // 麦克风：胶囊 + 拾音弧 + 底座。
        const QColor glyph =
            paletteColor(QStringLiteral("text.dim"), QColor(0xc9, 0xd1, 0xd9));
        QPen pen(glyph);
        pen.setWidthF(qMax(1.5, 3 * k));
        painter.setPen(pen);
        painter.setBrush(glyph);
        painter.drawRoundedRect(QRectF(center.x() - 9 * k, center.y() - 22 * k,
                                       18 * k, 30 * k), 9 * k, 9 * k);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(center.x() - 17 * k, center.y() - 12 * k, 34 * k,
                               26 * k), 180 * 16, 180 * 16);
        painter.drawLine(QPointF(center.x(), center.y() + 8 * k),
                         QPointF(center.x(), center.y() + 15 * k));
        painter.drawLine(QPointF(center.x() - 9 * k, center.y() + 15 * k),
                         QPointF(center.x() + 9 * k, center.y() + 15 * k));
    }

private:
    double level_ = 0.0;
    bool recording_ = false;
};

// 顶部拖拽条（同主窗 TitleBar 的做法）。
class DragStrip final : public QWidget {
public:
    explicit DragStrip(QWidget *parent, bool movable = true)
        : QWidget(parent), movable_(movable) {}

protected:
    void mousePressEvent(QMouseEvent *event) override {
        // 嵌入主窗时拖_header_会拖走整个主窗：嵌入态不启动系统移动。
        if (!movable_) {
            QWidget::mousePressEvent(event);
            return;
        }
        // 本控件是子控件，自己的 windowHandle() 是空的；必须从顶层窗口拿。
        if (event->button() == Qt::LeftButton) {
            if (auto *handle = window()->windowHandle()) {
                handle->startSystemMove();
                event->accept();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }

private:
    bool movable_ = true;
};

}  // namespace

VoiceWindow::VoiceWindow(VoiceController *voice, ProfileStore *store,
                         QWidget *parent, bool embedded)
    : QWidget(parent), voice_(voice), store_(store), embedded_(embedded) {
    setObjectName("voiceWindow");
    setWindowTitle(QString::fromUtf8("AgentRMC 语音"));
    if (embedded_) {
        // 主窗左栏的一页：普通子控件、随栏宽伸缩，不要顶层窗口那套。
        setWindowFlags(Qt::Widget);
        setAttribute(Qt::WA_StyledBackground);
        // 不设显式 minimum：让布局 minimumSizeHint 传给外层 QScrollArea，
        // 空间不够时出滚动条而不是把控件压到重叠（真机踩坑）。
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    } else {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        // qss 里给了 12px 圆角，透明背景才能圆出效果；plain QWidget 要显式开 StyledBackground。
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_StyledBackground);
        setFixedSize(380, 560);
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *header = new DragStrip(this, !embedded_);
    header->setObjectName("voiceHeader");
    header->setFixedHeight(embedded_ ? 36 : 44);
    auto *head = new QHBoxLayout(header);
    head->setContentsMargins(14, 0, 10, 0);
    head->setSpacing(8);
    statusDot_ = new QLabel;
    statusDot_->setFixedSize(6, 6);
    refreshStatusDot();
    auto *title = new QLabel(QString::fromUtf8("语音"));
    title->setObjectName("brand");
    auto *full = new QPushButton(QString::fromUtf8("完整界面"));
    full->setObjectName("ghostText");
    full->setCursor(Qt::PointingHandCursor);
    full->setToolTip(QString::fromUtf8("切回完整主界面"));
    connect(full, &QPushButton::clicked, this, &VoiceWindow::fullUiRequested);
    auto *minBtn = new QToolButton;
    minBtn->setText(QString::fromUtf8("—"));
    minBtn->setObjectName("winBtn");
    minBtn->setFixedSize(28, 24);
    minBtn->setCursor(Qt::PointingHandCursor);
    connect(minBtn, &QToolButton::clicked, this, &QWidget::showMinimized);
    auto *closeBtn = new QToolButton;
    closeBtn->setText(QString::fromUtf8("✕"));
    closeBtn->setObjectName("closeBtn");
    closeBtn->setFixedSize(28, 24);
    closeBtn->setCursor(Qt::PointingHandCursor);
    // 走 close()：收面板 = 回托盘并把桌宠叫出来，和标题栏 ✕ 语义一致。
    connect(closeBtn, &QToolButton::clicked, this, &QWidget::close);
    head->addWidget(statusDot_);
    head->addWidget(title);
    head->addStretch(1);
    // 「朗读最近」收进页眉：不再单独占一行（能一行的不拆两行）。
    readLast_ = new QPushButton(QString::fromUtf8("朗读最近"));
    readLast_->setObjectName("ghostText");
    readLast_->setCursor(Qt::PointingHandCursor);
    readLast_->setToolTip(QString::fromUtf8("用桌宠的 TTS 念出最近一条转写"));
    readLast_->setEnabled(false);
    connect(readLast_, &QPushButton::clicked, this, [this] {
        if (lastText_.isEmpty()) {
            toast_->showText(QString::fromUtf8("还没有可朗读的内容"));
            return;
        }
        emit speakRequested(lastText_);
    });
    head->addWidget(readLast_);
    if (embedded_) {
        // 嵌入态：窗口按钮没意义；给一个切回遥控器卡的入口。
        full->hide();
        minBtn->hide();
        closeBtn->hide();
        auto *back = new QPushButton(QString::fromUtf8("遥控器"));
        back->setObjectName("ghostText");
        back->setCursor(Qt::PointingHandCursor);
        back->setToolTip(QString::fromUtf8("把左栏这块切回遥控器卡"));
        connect(back, &QPushButton::clicked, this,
                &VoiceWindow::embedExitRequested);
        head->addWidget(back);
    } else {
        head->addWidget(full);
        head->addWidget(minBtn);
        head->addWidget(closeBtn);
    }
    root->addWidget(header);

    auto *body = new QVBoxLayout;
    body->setContentsMargins(embedded_ ? 10 : 20, embedded_ ? 6 : 10,
                             embedded_ ? 10 : 20, embedded_ ? 8 : 14);
    body->setSpacing(embedded_ ? 4 : 8);

    mic_ = new MicButton;
    // 与状态/提示同行后不再需要居中巨 mic：嵌入 56 / 独立 96。
    mic_->setFixedSize(embedded_ ? 56 : 96, embedded_ ? 56 : 96);
    connect(mic_, &QPushButton::clicked, this, [this] {
        if (!listening_) {
            toast_->showText(QString::fromUtf8(
                "还没开始监听：托盘菜单或主界面里点「开始监听」"));
            return;
        }
        voice_->mediaToggle();
    });
    // 麦克风与状态/提示同一行：能一行的不拆两行（用户 2026-11-03 19:32）。
    auto *micRow = new QHBoxLayout;
    micRow->setSpacing(embedded_ ? 8 : 12);
    micRow->addWidget(mic_);
    auto *micText = new QVBoxLayout;
    micText->setSpacing(2);
    stateLabel_ = new QLabel(QString::fromUtf8("点击开始 · 再点结束"));
    stateLabel_->setObjectName("voiceStateLine");
    stateLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    micText->addWidget(stateLabel_);
    hintLabel_ = new QLabel;
    hintLabel_->setObjectName("muted");
    hintLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hintLabel_->setWordWrap(true);
    refreshHint();
    // 嵌入态高度金贵：热键提示在引导/语音卡 tooltip 里有，这里不占行。
    if (embedded_) {
        hintLabel_->hide();
    }
    micText->addWidget(hintLabel_);
    micRow->addLayout(micText, 1);
    // 文本块与麦克风垂直居中：顶对齐会在右侧留一大块空，失衡。
    micRow->setAlignment(micText, Qt::AlignVCenter);
    body->addLayout(micRow);

    // 对话块：最新转写 + 历史。整面板不滚动，只有这一块内部滚（滚轮）。
    transcript_ = new QLabel(QString::fromUtf8("说了话会显示在这里，点击可复制"));
    transcript_->setObjectName("voiceTranscript");
    transcript_->setWordWrap(true);
    transcript_->setMinimumHeight(embedded_ ? 32 : 56);
    transcript_->setMaximumHeight(embedded_ ? 48 : 76);
    transcript_->setCursor(Qt::PointingHandCursor);
    transcript_->installEventFilter(this);
    body->addWidget(transcript_);

    history_ = new QListWidget;
    history_->setObjectName("voiceHistory");
    history_->setFrameShape(QFrame::NoFrame);
    history_->setSelectionMode(QAbstractItemView::NoSelection);
    history_->setFocusPolicy(Qt::NoFocus);
    history_->setWordWrap(true);
    history_->setSpacing(2);
    // 原生滚动条又粗又突兀：藏掉，滚轮照常滚（全局 QSS 也已宽/高 0）。
    history_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    history_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    history_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    history_->setTextElideMode(Qt::ElideNone);
    connect(history_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { copyText(item->data(Qt::UserRole).toString()); });
    body->addWidget(history_, 1);

    // 打字对话：不想说话/环境吵时敲一句，路由与说话完全相同。
    auto *typedRow = new QHBoxLayout;
    typedRow->setSpacing(6);
    typed_ = new QLineEdit;
    typed_->setObjectName("voiceTyped");
    typed_->setPlaceholderText(QString::fromUtf8("打字说一句话，Enter 发送"));
    typed_->setClearButtonEnabled(true);
    // placeholder 颜色不吃 QSS：跟主题手动同步，深色下别黑字黑底。
    const auto syncPlaceholder = [this](const Theme &) {
        QPalette pal = typed_->palette();
        pal.setColor(QPalette::PlaceholderText,
                     paletteColor(QStringLiteral("text.dim"),
                                  QColor(0x8b, 0x94, 0x9e)));
        typed_->setPalette(pal);
    };
    syncPlaceholder(ThemeManager::instance().current());
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            syncPlaceholder);
    connect(typed_, &QLineEdit::returnPressed, this, &VoiceWindow::submitTyped);
    auto *send = new QPushButton(QString::fromUtf8("发送"));
    send->setObjectName("ghostSmall");
    send->setCursor(Qt::PointingHandCursor);
    connect(send, &QPushButton::clicked, this, &VoiceWindow::submitTyped);
    typedRow->addWidget(typed_, 1);
    typedRow->addWidget(send);
    body->addLayout(typedRow);

    // 交给 + 派活合并一行（能一行的不拆两行）。
    auto *pickRow = new QHBoxLayout;
    pickRow->setSpacing(6);
    auto *agentLabel = new QLabel(QString::fromUtf8("交给"));
    agentLabel->setObjectName("muted");
    agentTarget_ = new QComboBox;
    agentTarget_->setToolTip(QString::fromUtf8(
        "语音派活交给哪个 Agent；选定后说的话直接进它的会话（桌宠不再判断任务/"
        "闲聊，也不走自带对话模型）。自动判断 = 前台优先 → 最近发通知的宿主 → "
        "最近派活的宿主"));
    connect(agentTarget_, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (index < 0) {
                    return;
                }
                store_->setUiString(QStringLiteral("agent_target"),
                                    agentTarget_->itemData(index).toString());
                refreshDispatchMode();
            });
    auto *modeLabel = new QLabel(QString::fromUtf8("派活"));
    modeLabel->setObjectName("muted");
    dispatchMode_ = new QComboBox;
    dispatchMode_->addItem(QString::fromUtf8("续当前会话（默认）"),
                           QString());
    dispatchMode_->addItem(QString::fromUtf8("下次派活开新任务"),
                           QStringLiteral("new"));
    dispatchMode_->setToolTip(QString::fromUtf8(
        "续当前会话 = 粘进当前打开的会话输入框，读回验证；"
        "下次派活开新任务 = 一次性，交付后自动回到续当前会话"));
    connect(dispatchMode_, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (index < 0 || agentTarget_ == nullptr) {
                    return;
                }
                const QString exe = agentTarget_->currentData().toString();
                if (exe.isEmpty()) {
                    return;
                }
                store_->setUiString(QStringLiteral("dispatch_mode_") + exe,
                                    dispatchMode_->itemData(index).toString());
            });
    pickRow->addWidget(agentLabel);
    pickRow->addWidget(agentTarget_, 1);
    pickRow->addWidget(modeLabel);
    pickRow->addWidget(dispatchMode_, 1);
    body->addLayout(pickRow);
    refreshAgentTarget();

    auto *footW = new QWidget;
    auto *foot = new QHBoxLayout(footW);
    foot->setContentsMargins(0, 0, 0, 0);
    foot->setSpacing(6);
    backend_ = new QComboBox;
    backend_->addItem(QString::fromUtf8("转发热键 · 0 模型"), "hotkey");
    backend_->addItem(QString::fromUtf8("SenseVoice"), "sensevoice");
    backend_->addItem(QString::fromUtf8("Qwen3-ASR"), "qwen3");
    backend_->addItem(QString::fromUtf8("云端 ASR"), "api");
    const QString backendKey = [&] {
        const QString value = store_->voiceConfig().value("backend").toString();
        return value.isEmpty() ? QStringLiteral("hotkey") : value;
    }();
    const int index = backend_->findData(backendKey);
    backend_->setCurrentIndex(index >= 0 ? index : 0);
    connect(backend_, &QComboBox::currentIndexChanged, this,
            [this](int comboIndex) {
                const QString key = backend_->itemData(comboIndex).toString();
                store_->updateVoice(QJsonObject{{"backend", key}});
                voice_->reload();
                if (key == QStringLiteral("api") && !voice_->modelReady()) {
                    toast_->showText(
                        QString::fromUtf8(
                            "云端 ASR 还没配好：模型设置 → 云端 ASR 填模型和 Key"),
                        3600);
                    return;
                }
                toast_->showText(QString::fromUtf8("识别引擎已切换"));
            });
    auto *settings = new QPushButton(QString::fromUtf8("模型设置"));
    settings->setObjectName("ghostSmall");
    settings->setCursor(Qt::PointingHandCursor);
    connect(settings, &QPushButton::clicked, this, [this] {
        auto *dialog = new VoiceSettingsDialog(store_, this);
        const auto apply = [this] {
            voice_->reload();
            refreshHint();
        };
        connect(dialog, &VoiceSettingsDialog::saved, this, apply);
        // 子对话框里保存的配置立即生效，不等外层「保存」。
        connect(dialog, &VoiceSettingsDialog::optionsApplied, this, apply);
        dialog->exec();
        dialog->deleteLater();
    });
    // 语音主界面里直接自填 TTS 的 Base URL / API Key / 模型。
    auto *tts = new QPushButton(QString::fromUtf8("语音反馈"));
    tts->setObjectName("ghostSmall");
    tts->setCursor(Qt::PointingHandCursor);
    tts->setToolTip(
        QString::fromUtf8("桌宠语音反馈：Base URL / API Key / TTS 模型自填"));
    connect(tts, &QPushButton::clicked, this, [this] {
        auto *dialog = new TtsSettingsDialog(store_, this);
        connect(dialog, &TtsSettingsDialog::saved, this, [this] {
            voice_->reload();
            refreshHint();
        });
        dialog->exec();
        dialog->deleteLater();
    });
    // 对话模式开关：说话 → 转写 → LLM → TTS 念回复，不再粘贴。
    auto *chatMode = new QPushButton(QString::fromUtf8("对话"));
    chatMode->setObjectName("ghostSmall");
    chatMode->setCheckable(true);
    chatMode->setChecked(store_->uiBool("pet_chat", false));
    chatMode->setCursor(Qt::PointingHandCursor);
    chatMode->setToolTip(QString::fromUtf8(
        "桌宠对话模式：说话发给模型聊，回复用 TTS 念出来；"
        "模型在「模型设置 → 桌宠对话」里配"));
    connect(chatMode, &QPushButton::toggled, this, [this](bool on) {
        store_->setUiBool(QStringLiteral("pet_chat"), on);
        voice_->setChatMode(on);
        toast_->showText(on ? QString::fromUtf8(
                                  "对话模式开：说话会得到模型的语音回复，不再粘贴")
                            : QString::fromUtf8("已退出对话模式"),
                          3000);
    });
    // 桌宠右键那边也可能开关对话模式：按钮勾选态跟着控制器的广播走，
    // 挡住 toggled 信号防止又写回一遍配置。
    connect(voice_, &VoiceController::chatModeChanged, this, [this, chatMode](bool on) {
        const QSignalBlocker block(chatMode);
        chatMode->setChecked(on);
    });
    foot->addWidget(backend_, 1);
    foot->addWidget(chatMode);
    foot->addWidget(tts);
    foot->addWidget(settings);
    // 嵌入态：引擎/模型设置语音卡上已有，对话/语音反馈桌宠右键有，
    // 这行藏掉省高度，也避免 286 宽里四个控件挤到截断。
    if (embedded_) {
        footW->hide();
    }
    body->addWidget(footW);

    root->addLayout(body, 1);

    toast_ = new Toast(this);

    levelTimer_ = new QTimer(this);
    connect(levelTimer_, &QTimer::timeout, this, [this] {
        static_cast<MicButton *>(mic_)->setLevel(recording_ ? voice_->micLevel()
                                                            : 0.0);
    });
    levelTimer_->start(50);

    connect(voice_, &VoiceController::stateChanged, this,
            [this](const QString &state, const QString &detail) {
                recording_ = state == QStringLiteral("recording");
                static_cast<MicButton *>(mic_)->setRecording(recording_);
                refreshStatusDot();
                if (state == QStringLiteral("idle")) {
                    const bool warn =
                        detail.contains(QString::fromUtf8("太短")) ||
                        detail.contains(QString::fromUtf8("未"));
                    stateLabel_->setText(detail == QStringLiteral("就绪")
                                             ? QStringLiteral("点击开始 · 再点结束")
                                             : detail);
                    stateLabel_->setStyleSheet(warn
                                                   ? QStringLiteral("color:#d29922;")
                                                   : QString());
                } else {
                    stateLabel_->setStyleSheet(QString());
                    // 这个界面没有「松手」：把主窗的文案换成贴切的说法。
                    stateLabel_->setText(
                        recording_ ? QStringLiteral("录音中 · 再点一下或按热键结束")
                                   : detail);
                }
            });
    connect(voice_, &VoiceController::transcribed, this,
            [this](const QString &text, int costMs) {
                transcript_->setText(text);
                pushHistory(text, costMs);
            });
    connect(voice_, &VoiceController::optionsChanged, this, [this] {
        // 桌宠/主界面那边也可能切了识别方式，把下拉框拉回来。
        const QSignalBlocker block(backend_);
        const int synced = backend_->findData(voiceBackendKey(voice_->options().backend));
        if (synced >= 0) {
            backend_->setCurrentIndex(synced);
        }
        refreshHint();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &) {
                // 面板底/麦克风/Toast 是自绘的，不吃 QSS：换配色模式要自己重画。
                refreshStatusDot();
                mic_->update();
                toast_->update();
                update();
            });
    connect(voice_, &VoiceController::eventReady, this,
            [this](const QString &kind, const QString &text) {
                if (kind == QStringLiteral("error")) {
                    toast_->showText(text);
                }
            });
}

void VoiceWindow::setListeningUi(bool listening) {
    listening_ = listening;
    refreshHint();
}

void VoiceWindow::refreshHint() {
    const VoiceOptions options = voice_->options();
    QString hint;
    if (options.kbHotkeyEnabled) {
        hint = QStringLiteral("热键 %1 也可以随时开始").arg(
            voiceComboText(options.kbHotkeyMods, options.kbHotkeyVk));
    }
    if (!listening_) {
        hint += hint.isEmpty() ? QString()
                               : QStringLiteral(" · ");
        hint += QStringLiteral("先在托盘或主界面开始监听");
    }
    hintLabel_->setText(hint);
    hintLabel_->setVisible(!hint.isEmpty());
}

void VoiceWindow::refreshStatusDot() {
    statusDot_->setStyleSheet(
        QStringLiteral("background:%1;")
            .arg(recording_
                     ? QStringLiteral("#3fb950")
                     : ThemeManager::instance().css(QStringLiteral("accent"))));
}

void VoiceWindow::refreshAgentTarget() {
    if (agentTarget_ == nullptr) {
        return;
    }
    const QSignalBlocker block(agentTarget_);
    const QString pinned = store_->uiString(QStringLiteral("agent_target"));
    agentTarget_->clear();
    agentTarget_->addItem(QString::fromUtf8("自动判断"), QString());
    bool anyAlive = false;
    for (const QString &exe : AgentWindow::knownExes()) {
        // 进程在跑就列出；窗口收进托盘的标注「窗口隐藏」——选它语音派活
        // 不会硬注入（没有可交互窗口，粘了也是粘到别人窗口上）。
        if (!AgentWindow::processAlive(exe)) {
            continue;
        }
        anyAlive = true;
        if (AgentWindow::alive(exe)) {
            agentTarget_->addItem(AgentWindow::displayName(exe), exe);
        } else {
            agentTarget_->addItem(
                QString::fromUtf8("%1（窗口隐藏）")
                    .arg(AgentWindow::displayName(exe)),
                exe);
        }
    }
    if (!pinned.isEmpty() && !AgentWindow::processAlive(pinned)) {
        agentTarget_->addItem(
            QString::fromUtf8("%1（未运行）")
                .arg(AgentWindow::displayName(pinned)),
            pinned);
    } else if (!pinned.isEmpty() && !AgentWindow::alive(pinned)) {
        // 钉住的宿主进程在但窗口收起来了：也标出来，别让用户以为能直接派。
        const int existing = agentTarget_->findData(pinned);
        if (existing >= 0) {
            agentTarget_->setItemText(
                existing, QString::fromUtf8("%1（窗口隐藏）")
                              .arg(AgentWindow::displayName(pinned)));
        }
    }
    int index = agentTarget_->findData(pinned);
    if (index < 0) {
        index = 0;
    }
    agentTarget_->setCurrentIndex(index);
    agentTarget_->setToolTip(
        anyAlive || !pinned.isEmpty()
            ? QString::fromUtf8("语音派活交给哪个 Agent；"
                                "自动判断 = 前台优先 → 最近发通知的宿主")
            : QString::fromUtf8("没检测到在跑的 Agent；"
                                "语音会走自带 LLM 或提示先打开 Agent"));
    refreshDispatchMode();
}

void VoiceWindow::refreshDispatchMode() {
    if (dispatchMode_ == nullptr || agentTarget_ == nullptr) {
        return;
    }
    const QSignalBlocker block(dispatchMode_);
    const QString exe = agentTarget_->currentData().toString();
    dispatchMode_->setEnabled(!exe.isEmpty());
    const QString mode =
        exe.isEmpty() ? QString()
                      : store_->uiString(QStringLiteral("dispatch_mode_") + exe);
    const int index = dispatchMode_->findData(mode);
    dispatchMode_->setCurrentIndex(index >= 0 ? index : 0);
    dispatchMode_->setToolTip(
        exe.isEmpty()
            ? QString::fromUtf8("先在「交给」里选定一个 Agent，再设它的派活模式")
            : QString::fromUtf8(
                  "续当前会话（默认）= 粘进当前打开的会话输入框，读回验证；"
                  "下次派活开新任务 = 一次性：该句开新任务（Qoder 系 Ctrl+N、"
                  "WorkBuddy 系 UIA），交付后自动回到续当前会话；"
                  "没登记键位的宿主两种模式都粘当前焦点"));
}

void VoiceWindow::copyText(const QString &text) {
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    toast_->showText(QString::fromUtf8("已复制到剪贴板"));
}

void VoiceWindow::pushHistory(const QString &text, int costMs) {
    auto *item = new QListWidgetItem(history_);
    item->setText(QStringLiteral("%1  ·  %2s")
                      .arg(text,
                           QString::number(costMs / 1000.0, 'f', 1)));
    item->setData(Qt::UserRole, text);
    item->setToolTip(text);
    lastText_ = text;
    if (readLast_ != nullptr) {
        readLast_->setEnabled(true);
    }
    history_->insertItem(0, item);
    while (history_->count() > 30) {
        delete history_->takeItem(history_->count() - 1);
    }
}

bool VoiceWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == transcript_ && event->type() == QEvent::MouseButtonPress) {
        copyText(transcript_->text());
    }
    return QWidget::eventFilter(watched, event);
}

void VoiceWindow::mousePressEvent(QMouseEvent *event) {
    // 面板空白处也能拖走：子控件（按钮/列表）各自吃掉自己的按下事件，
    // 落到这里的就是背景。顶层窗口自己有 windowHandle()。
    if (event->button() == Qt::LeftButton) {
        if (auto *handle = windowHandle()) {
            handle->startSystemMove();
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void VoiceWindow::showChatReply(const QString &text) {
    transcript_->setText(QString::fromUtf8("鲸鱼娘：%1").arg(text));
    pushHistory(text, 0);
}

void VoiceWindow::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    // 顶层透明窗口的 QSS 背景不可靠，照 Toast 的办法自己画圆角底。
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    if (embedded_) {
        // 嵌入页跟卡片体系走：card.bg + 细边圆角，与上方语音卡/遥控器卡
        // 同一视觉语言，不再是一块孤立的平底（2026-11-03 用户反馈）。
        painter.setPen(QPen(paletteColor(QStringLiteral("w18"),
                                         QColor(255, 255, 255, 28)), 1.0));
        painter.setBrush(paletteColor(QStringLiteral("card.bg"),
                                      QColor(0x12, 0x17, 0x22)));
        painter.drawRoundedRect(box, 12, 12);
        return;
    }

    // 主题带面板背景就先铺图再压暗，保住文字可读；classic 走纯色底。
    const Theme &theme = ThemeManager::instance().current();
    const QImage art = ThemeManager::cachedImage(theme.panelBackground);
    const QColor base =
        paletteColor(QStringLiteral("bg"), QColor(0x0b, 0x0e, 0x14));
    const QColor edge =
        paletteColor(QStringLiteral("w22"), QColor(255, 255, 255, 22));
    if (!art.isNull()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(base);
        painter.drawRoundedRect(box, 12, 12);
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(rect(), 12, 12);
        painter.setClipPath(clip);
        QImage scaled = art.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                   Qt::SmoothTransformation);
        painter.drawImage(
            QRect((width() - scaled.width()) / 2,
                  (height() - scaled.height()) / 2, scaled.width(),
                  scaled.height()),
            scaled);
        // 压暗遮罩照主窗 backdropDim 一样恒深：它的职责就是把背景图压到能读字，
        // 跟着浅色主题的 bg 走会把背景图提亮，反而糊掉文字。
        painter.setOpacity(theme.dim);
        painter.setBrush(QColor(0x0b, 0x0e, 0x14));
        painter.drawRoundedRect(box, 12, 12);
        painter.restore();
        painter.setOpacity(1.0);
    } else {
        painter.setPen(QPen(edge, 1.0));
        painter.setBrush(base);
        painter.drawRoundedRect(box, 12, 12);
        return;
    }
    painter.setPen(QPen(edge, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(box, 12, 12);
}

void VoiceWindow::showEvent(QShowEvent *event) {
    refreshAgentTarget();
    QWidget::showEvent(event);
}

void VoiceWindow::closeEvent(QCloseEvent *event) {
    if (embedded_) {
        // 嵌入页没有「回托盘」语义：交回主窗切回遥控器卡。
        event->ignore();
        emit embedExitRequested();
        return;
    }
    // ✕ 的语义是回托盘，不是退出；真退出走托盘菜单。
    event->ignore();
    hide();
    emit panelHidden();
}

void VoiceWindow::submitTyped() {
    if (typed_ == nullptr) {
        return;
    }
    const QString text = typed_->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    typed_->clear();
    transcript_->setText(text);
    pushHistory(text, 0);
    emit typedSubmitted(text);
}
