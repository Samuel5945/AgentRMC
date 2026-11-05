#include "VoiceWindow.h"

#include "DispatchPicker.h"
#include "TranscriptLayout.h"
#include "WaveformButton.h"

#include "Toast.h"
#include "TtsSettingsDialog.h"
#include "VoiceSettingsDialog.h"
#include "core/Profiles.h"
#include "core/ThemeManager.h"
#include "core/VoiceController.h"
#include "core/VoiceSettings.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
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
    hintSeen_ = store_->uiBool(QStringLiteral("voice_hint_seen"), false);
    setObjectName(embedded ? QStringLiteral("voiceWindowEmbedded")
                           : QStringLiteral("voiceWindow"));
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
    header->setObjectName(embedded_ ? QStringLiteral("voiceHeaderEmbedded")
                                    : QStringLiteral("voiceHeader"));
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
    // 主窗左栏精简后，「试一句」诊断收进语音界面页眉（两种形态都有）。
    testBtn_ = new QPushButton(QString::fromUtf8("试一句"));
    testBtn_->setObjectName("ghostText");
    testBtn_->setCursor(Qt::PointingHandCursor);
    testBtn_->setToolTip(QString::fromUtf8(
        "录 3 秒并转写一次，用来确认本地模型是否正常"));
    testBtn_->setEnabled(voice_->options().backend != VoiceBackend::Hotkey);
    connect(testBtn_, &QPushButton::clicked, voice_,
            &VoiceController::startTest);
    head->addWidget(testBtn_);
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

    // 整条长波形语音键：无文字，颜色表状态（绿录音/蓝待命/灰未监听）。
    mic_ = new WaveformButton;
    mic_->setFixedHeight(embedded_ ? 40 : 56);
    mic_->setToolTip(QString::fromUtf8("语音键：点击说话，再点结束"));
    connect(mic_, &QPushButton::clicked, this, [this] {
        if (!listening_) {
            toast_->showText(QString::fromUtf8(
                "还没开始监听：点主界面右上角「未监听」胶囊开启"));
            return;
        }
        voice_->mediaToggle();
    });
    body->addWidget(mic_);
    // 宿主/任务会话紧贴波形下面，中间不隔文字。
    picker_ = new DispatchPicker(store_);
    body->addWidget(picker_);
    auto *micText = new QVBoxLayout;
    micText->setSpacing(2);
    stateLabel_ = new QLabel(QString::fromUtf8("点击开始 · 再点结束"));
    stateLabel_->setObjectName("voiceStateLine");
    stateLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    stateLabel_->setVisible(!hintSeen_);
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
    body->addLayout(micText);

    // 对话块：最新转写 + 历史。整面板不滚动，只有这一块内部滚（滚轮）。
    transcript_ = new QLabel;
    transcript_->setObjectName("voiceTranscript");
    transcript_->setWordWrap(true);
    transcript_->setCursor(Qt::PointingHandCursor);
    transcript_->installEventFilter(this);
    body->addWidget(transcript_);
    setTranscript(QString::fromUtf8("说了话会显示在这里，点击可复制"));

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
    // 空态：嵌入态/无插画时中部别留死白；独立态有插画时中部是形象舞台，不放字。
    historyEmpty_ = new QLabel(QString::fromUtf8("说完的话会攒在这里 · 点击复制"),
                               history_->viewport());
    historyEmpty_->setObjectName("emptyHint");
    historyEmpty_->setAlignment(Qt::AlignCenter);
    auto *emptyLay = new QVBoxLayout(history_->viewport());
    emptyLay->setContentsMargins(0, 0, 0, 0);
    emptyLay->addWidget(historyEmpty_);

    // 底部控制区dock：独立态铺卡片底保住可读性，嵌入态透明（页面本身已是卡）。
    auto *dockW = new QWidget;
    dockW->setObjectName("voiceDock");
    dockW->setAttribute(Qt::WA_StyledBackground);
    auto *dock = new QVBoxLayout(dockW);
    dock->setContentsMargins(embedded_ ? 0 : 10, embedded_ ? 0 : 8,
                             embedded_ ? 0 : 10, embedded_ ? 0 : 8);
    dock->setSpacing(embedded_ ? 4 : 6);

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
    dock->addLayout(typedRow);

    auto *footW = new QWidget;
    auto *footLay = new QVBoxLayout(footW);
    footLay->setContentsMargins(0, 0, 0, 0);
    footLay->setSpacing(6);
    auto *foot = new QHBoxLayout;
    foot->setSpacing(6);
    footLay->addLayout(foot);
    backend_ = new QComboBox;
    // 与主窗语音卡同一套命名，两处别各叫各的。
    backend_->addItem(QString::fromUtf8("转发热键 · 0 模型"), "hotkey");
    backend_->addItem(QString::fromUtf8("本地 SenseVoice"), "sensevoice");
    backend_->addItem(QString::fromUtf8("本地 Qwen3-ASR"), "qwen3");
    backend_->addItem(QString::fromUtf8("云端 ASR（需联网）"), "api");
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
        "桌宠对话模式：说话发给模型聊，回复用 TTS 念出来\n"
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
    if (embedded_) {
        // 主窗左栏只剩波形键后，引擎/设置收进这里；286 宽一行放不下四控件，
        // 引擎独占一行，功能按钮第二行。
        auto *row2 = new QHBoxLayout;
        row2->setSpacing(6);
        row2->addWidget(chatMode);
        row2->addWidget(tts);
        row2->addWidget(settings);
        footLay->addLayout(row2);
    } else {
        foot->addWidget(chatMode);
        foot->addWidget(tts);
        foot->addWidget(settings);
    }
    dock->addWidget(footW);
    body->addWidget(dockW);

    root->addLayout(body, 1);
    refreshHistoryEmpty();

    toast_ = new Toast(this);

    levelTimer_ = new QTimer(this);
    connect(levelTimer_, &QTimer::timeout, this, [this] {
        mic_->setLevel(recording_ ? voice_->micLevel() : 0.0);
    });
    levelTimer_->start(50);

    connect(voice_, &VoiceController::stateChanged, this,
            [this](const QString &state, const QString &detail) {
                recording_ = state == QStringLiteral("recording");
                mic_->setRecording(recording_);
                refreshStatusDot();
                if (recording_ && !hintSeen_) {
                    hintSeen_ = true;
                    store_->setUiBool(QStringLiteral("voice_hint_seen"), true);
                    refreshHint();
                }
                if (state == QStringLiteral("idle")) {
                    const bool warn =
                        detail.contains(QString::fromUtf8("太短")) ||
                        detail.contains(QString::fromUtf8("未"));
                    stateLabel_->setText(detail == QStringLiteral("就绪")
                                             ? QStringLiteral("点击开始 · 再点结束")
                                             : detail);
                    // 就绪提醒只给首次使用的人；警告/实时状态照常上屏。
                    stateLabel_->setVisible(
                        !(hintSeen_ && detail == QStringLiteral("就绪")));
                    stateLabel_->setStyleSheet(warn
                                                   ? QStringLiteral("color:#d29922;")
                                                   : QString());
                } else {
                    stateLabel_->setVisible(true);
                    stateLabel_->setStyleSheet(QString());
                    // 这个界面没有「松手」：把主窗的文案换成贴切的说法。
                    stateLabel_->setText(
                        recording_ ? QStringLiteral("录音中 · 再点一下或按热键结束")
                                   : detail);
                }
            });
    connect(voice_, &VoiceController::transcribed, this,
            [this](const QString &text, int costMs) {
                setTranscript(text);
                pushHistory(text, costMs);
            });
    connect(voice_, &VoiceController::optionsChanged, this, [this] {
        // 桌宠/主界面那边也可能切了识别方式，把下拉框拉回来。
        const QSignalBlocker block(backend_);
        const int synced = backend_->findData(voiceBackendKey(voice_->options().backend));
        if (synced >= 0) {
            backend_->setCurrentIndex(synced);
        }
        testBtn_->setEnabled(voice_->options().backend != VoiceBackend::Hotkey);
        refreshHint();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &) {
                // 面板底/麦克风/Toast 是自绘的，不吃 QSS：换配色模式要自己重画。
                refreshStatusDot();
                refreshHistoryEmpty();
                // 字号倍率也走这条：转写框的行高跟着字体走，换主题要重量。
                layoutTranscript();
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
    mic_->setListening(listening);
    refreshHint();
}

void VoiceWindow::refreshHint() {
    const VoiceOptions options = voice_->options();
    QString hotkey;
    if (options.kbHotkeyEnabled) {
        hotkey = QStringLiteral("热键 %1 也可以随时开始").arg(
            voiceComboText(options.kbHotkeyMods, options.kbHotkeyVk));
    }
    // 说明常态收进悬浮提示兜底；页面上只留给首次使用的人。
    QString tip = QStringLiteral("语音键：点击说话，再点结束");
    if (!hotkey.isEmpty()) {
        tip += QStringLiteral("\n") + hotkey;
    }
    if (!listening_) {
        tip += QStringLiteral("\n先点右上角「未监听」开启监听");
    }
    mic_->setToolTip(tip);
    QString hint = hotkey;
    if (!listening_) {
        hint += hint.isEmpty() ? QString()
                               : QStringLiteral(" · ");
        hint += QStringLiteral("先点右上角「未监听」开启监听");
    }
    hintLabel_->setText(hint);
    hintLabel_->setVisible(!hintSeen_ && !hint.isEmpty() && !embedded_);
}

void VoiceWindow::refreshStatusDot() {
    statusDot_->setStyleSheet(
        QStringLiteral("background:%1;")
            .arg(recording_
                     ? QStringLiteral("#3fb950")
                     : ThemeManager::instance().css(QStringLiteral("accent"))));
}

void VoiceWindow::refreshDispatchMode() {
    picker_->refreshMode();
}

void VoiceWindow::copyText(const QString &text) {
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    toast_->showText(QString::fromUtf8("已复制到剪贴板"));
}

void VoiceWindow::setTranscript(const QString &text) {
    transcriptFull_ = text;
    layoutTranscript();
}

void VoiceWindow::layoutTranscript() {
    if (transcript_ == nullptr) {
        return;
    }
    transcript_->ensurePolished();  // font() 要拿到 QSS（含字号倍率）再量
    const QFontMetrics fm(transcript_->font());
    // 箱体厚度优先按当前几何量（QSS 的 padding 会落到 contentsRect 上），
    // 量不到就退回 style.qss 里写死的 padding+border。
    const int chrome = qMax(transcript::kChromePx,
                            transcript_->width() -
                                transcript_->contentsRect().width());
    const int textWidth = transcript_->width() - chrome;
    if (textWidth <= 0) {
        transcript_->setText(transcriptFull_);  // 还没排版，show/resize 会重量
        return;
    }
    const QString shown = transcript::elideToLines(transcriptFull_, fm,
                                                   textWidth);
    transcript_->setText(shown);
    transcript_->setToolTip(shown == transcriptFull_ ? QString()
                                                     : transcriptFull_);
    const int height = transcript::boxHeight(shown, fm, textWidth, chrome);
    if (transcript_->minimumHeight() != height) {
        transcript_->setFixedHeight(height);
    }
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
    refreshHistoryEmpty();
}

void VoiceWindow::refreshHistoryEmpty() {
    // 独立态铺了插画时中部留给形象；其余空列表给一句空态提示。
    const bool stage =
        !embedded_ &&
        !ThemeManager::instance().current().panelBackground.isEmpty();
    historyEmpty_->setVisible(history_->count() == 0 && !stage);
}

bool VoiceWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == transcript_ && event->type() == QEvent::MouseButtonPress) {
        copyText(transcriptFull_);
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
    const QString name = ThemeManager::instance().current().characterName;
    setTranscript(name.isEmpty()
                      ? text
                      : QStringLiteral("%1：%2").arg(name, text));
    pushHistory(text, 0);
}

void VoiceWindow::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    // 顶层透明窗口的 QSS 背景不可靠，照 Toast 的办法自己画圆角底。
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    if (embedded_) {
        // 嵌入页表面完全交给 QSS（与 QFrame#card 同一条渐变+细边+圆角）：
        // 美术主题的 card.bg 是半透明 rgba，自绘或 #voiceWindow 的 @bg@
        // 兜底都会把它变成死黑板（2026-11-03 鲸鱼娘主题真机）。
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
        // 缓存按 (路径, 尺寸) 判失效：4K 源图每帧 Smooth 现缩太贵。
        if (artScaledPath_ != theme.panelBackground ||
            artScaledSize_ != size()) {
            artScaled_ = art.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                    Qt::SmoothTransformation);
            artScaledPath_ = theme.panelBackground;
            artScaledSize_ = size();
        }
        const QImage &scaled = artScaled_;
        painter.drawImage(
            QRect((width() - scaled.width()) / 2,
                  (height() - scaled.height()) / 2, scaled.width(),
                  scaled.height()),
            scaled);
        // 遮罩跟主窗同一套 backdropDim：深色模式压暗、浅色模式蒙白纱；
        // 深色有可读性下限（鲸鱼娘 0.1 太浅），浅色白纱由主题 dim.light 自定。
        painter.setOpacity(ThemeManager::instance().backdropDim());
        painter.setBrush(ThemeManager::instance().isLightActive()
                             ? QColor(255, 255, 255)
                             : QColor(0x0b, 0x0e, 0x14));
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
    picker_->refresh();
    QWidget::showEvent(event);
    layoutTranscript();
}

void VoiceWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // 框宽变了换行就变了，高度要跟着重量（独立态可拉伸，嵌入态随栏宽）。
    layoutTranscript();
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
    setTranscript(text);
    pushHistory(text, 0);
    emit typedSubmitted(text);
}
