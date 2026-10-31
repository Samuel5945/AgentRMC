#include "ui/VoicePet.h"

#include "core/LlmClient.h"
#include "core/Profiles.h"
#include "core/TtsClient.h"
#include "core/VoiceController.h"
#include "core/VoiceSettings.h"
#include "ui/LlmSettingsDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

#include <cmath>

namespace {

constexpr int kPetSize = 84;    // 桌宠球体直径
constexpr int kPeek = 20;       // 贴边藏起来后露出的边条宽度
constexpr int kSnapDist = 80;   // 拖到离屏幕边多近算贴边
constexpr int kCardWidth = 244;
// 对话模式的默认人设：回复要被念出来，所以要求口语化、短。
constexpr const char *kDefaultChatSystem =
    "你是 AgentRMC 的语音桌宠，一个待在屏幕边上的小助手。用户在用语音和你聊天："
    "回复要口语化、简短（一般不超过两句话、50 字以内），不要用列表、markdown、"
    "表情或颜文字，因为你的回复会被朗读出来。默认用中文回答。";

}  // namespace

// 悬停卡片：状态行 + 识别方式切换 + 最近转写 + 快捷按钮。
// 不用 Q_OBJECT：没有自己的信号，按钮全部 lambda 连回 VoicePet；
// 成员公开给 VoicePet 直接读写（同一个 cpp 里的实现细节类）。
class VoicePet::Card final : public QWidget {
public:
    explicit Card(VoicePet *pet) : QWidget(pet), pet_(pet) {
        setObjectName("petCard");
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                       Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_StyledBackground);  // plain QWidget：不设这个 QSS 背景不画
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        setFixedWidth(kCardWidth);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 11, 12, 11);
        root->setSpacing(7);

        status_ = new QLabel(this);
        status_->setObjectName("petStatus");
        root->addWidget(status_);

        detail_ = new QLabel(this);
        detail_->setObjectName("petDetail");
        detail_->setWordWrap(true);
        root->addWidget(detail_);

        auto *modes = new QHBoxLayout;
        modes->setSpacing(4);
        const QStringList names = {QString::fromUtf8("转发"),
                                   QStringLiteral("SenseVoice"),
                                   QStringLiteral("Qwen3"),
                                   QString::fromUtf8("云端")};
        for (int i = 0; i < names.size(); ++i) {
            auto *button = new QPushButton(names.at(i), this);
            button->setObjectName("petMode");
            button->setCheckable(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            connect(button, &QPushButton::clicked, button,
                    [this, i] { pet_->setBackend(i); });
            modeButtons_.append(button);
            modes->addWidget(button, 1);
        }
        root->addLayout(modes);

        transcript_ = new QLabel(QString::fromUtf8("说了话会显示在这里，点击复制"), this);
        transcript_->setObjectName("petTranscript");
        transcript_->setWordWrap(true);
        transcript_->setMinimumHeight(44);
        transcript_->setMaximumHeight(60);
        transcript_->setCursor(Qt::PointingHandCursor);
        transcript_->installEventFilter(this);
        root->addWidget(transcript_);

        hint_ = new QLabel(this);
        hint_->setObjectName("petDetail");
        hint_->setWordWrap(true);
        hint_->hide();
        root->addWidget(hint_);

        auto *tools = new QHBoxLayout;
        tools->setSpacing(5);
        const auto addButton = [this, tools](const QString &text, auto slot) {
            auto *button = new QPushButton(text, this);
            button->setObjectName("ghostSmall");
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            connect(button, &QPushButton::clicked, button, std::move(slot));
            tools->addWidget(button, 1);
        };
        addButton(QString::fromUtf8("面板"), [this] { pet_->openPanel(); });
        addButton(QString::fromUtf8("主界面"), [this] { pet_->openFullUi(); });
        addButton(QString::fromUtf8("贴边"), [this] { pet_->dockAndPeek(); });
        addButton(QString::fromUtf8("✕"), [this] { pet_->closeFromUi(); });
        root->addLayout(tools);
    }

    void setStatus(const QString &state, const QString &color,
                   const QString &backend) {
        status_->setText(
            QStringLiteral("<span style=\"color:%1;\">●</span> %2 · "
                           "<span style=\"color:#8b949e;\">%3</span>")
                .arg(color, state, backend));
    }

    void setDetail(const QString &text) { detail_->setText(text); }

    void setBackendChecked(int backend) {
        for (int i = 0; i < modeButtons_.size(); ++i) {
            modeButtons_.at(i)->setChecked(i == backend);
        }
    }

    void setTranscript(const QString &text, int costMs) {
        text_ = text;
        transcript_->setText(text);
        transcript_->setToolTip(
            QStringLiteral("%1 · %2s")
                .arg(text, QString::number(costMs / 1000.0, 'f', 1)));
    }

    void setHint(const QString &text) {
        hint_->setText(text);
        hint_->setVisible(!text.isEmpty());
    }

    QLabel *status_ = nullptr;
    QLabel *detail_ = nullptr;
    QLabel *transcript_ = nullptr;
    QLabel *hint_ = nullptr;
    QVector<QPushButton *> modeButtons_;
    QString text_;

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        // 顶层透明窗口的 QSS 背景不可靠，照 Toast 的办法自己画。
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
        painter.setBrush(QColor(15, 20, 29));
        painter.drawRoundedRect(box, 12, 12);
    }

    void enterEvent(QEnterEvent *event) override {
        QWidget::enterEvent(event);
        pet_->collapseTimer_->stop();
    }

    void leaveEvent(QEvent *event) override {
        QWidget::leaveEvent(event);
        pet_->collapseSoon();
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == transcript_ && event->type() == QEvent::MouseButtonPress &&
            !text_.isEmpty()) {
            QApplication::clipboard()->setText(text_);
            pet_->speak(QString::fromUtf8("已复制到剪贴板"));
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    VoicePet *pet_ = nullptr;
};

// 说话气泡：Toast 同款画法（自定义 paintEvent），保证半透明圆角底稳定渲染。
class PetBubble final : public QLabel {
public:
    explicit PetBubble(QWidget *parent) : QLabel(parent) {
        setObjectName("petBubble");
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                       Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setFixedWidth(220);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(QColor(255, 255, 255, 30), 1.0));
        painter.setBrush(QColor(15, 20, 29, 242));
        painter.drawRoundedRect(box, 10, 10);
        QLabel::paintEvent(event);
    }
};

VoicePet::VoicePet(ProfileStore *store, VoiceController *voice, QWidget *parent)
    : QWidget(parent), store_(store), voice_(voice) {
    setObjectName("voicePet");
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kPetSize, kPetSize);

    animTimer_ = new QTimer(this);
    connect(animTimer_, &QTimer::timeout, this, &VoicePet::tick);

    tts_ = new TtsClient(this);
    applyTtsOptions();
    connect(tts_, &TtsClient::failed, this, [this](const QString &error) {
        // 余额不足这类错误每次会话只提示一次，别在每次反馈时刷气泡。
        if (ttsErrorShown_) {
            return;
        }
        ttsErrorShown_ = true;
        speak(QString::fromUtf8("语音反馈失败：%1").arg(error), 5000);
    });

    // 对话闭环：转写进来 → LLM → 回复用 TTS 念出来。
    llm_ = new LlmClient(this);
    applyLlmOptions();
    connect(llm_, &LlmClient::replied, this,
            [this](const QString &text, qint64) {
                llmBusy_ = false;
                chatHistory_.append(
                    QJsonObject{{"role", "assistant"}, {"content", text}});
                while (chatHistory_.size() > 8) {
                    chatHistory_.removeFirst();
                }
                speak(text);
            });
    connect(llm_, &LlmClient::failed, this,
            [this](const QString &error, qint64) {
                llmBusy_ = false;
                bubbleOnly(QString::fromUtf8("对话失败：%1").arg(error), 4200);
            });
    connect(voice_, &VoiceController::chatRequested, this,
            [this](const QString &text) {
                bubbleOnly(QString::fromUtf8("你：%1").arg(text), 2400);
                askLlm(text);
            });
    chatMode_ = store_->uiBool(QStringLiteral("pet_chat"), false);
    voice_->setChatMode(chatMode_);

    collapseTimer_ = new QTimer(this);
    collapseTimer_->setSingleShot(true);
    connect(collapseTimer_, &QTimer::timeout, this, [this] {
        if (underMouse() || (card_ != nullptr && card_->underMouse())) {
            return;
        }
        collapseCard();
        if (dock_ != Dock::None && !peeked_) {
            peeked_ = true;
            applyDockPos(true);
        }
    });

    bubbleTimer_ = new QTimer(this);
    bubbleTimer_->setSingleShot(true);
    connect(bubbleTimer_, &QTimer::timeout, this,
            [this] { if (bubble_ != nullptr) bubble_->hide(); });

    connect(voice_, &VoiceController::stateChanged, this,
            [this](const QString &state, const QString &detail) {
                recording_ = state == QStringLiteral("recording");
                transcribing_ =
                    !recording_ && detail.contains(QString::fromUtf8("转写"));
                detail_ = detail;
                refreshState();
                update();
                // 「已打开 xx」这类结果趁桌宠可见时念出来，算一问一答的反馈。
                if (detail.startsWith(QString::fromUtf8("已打开"))) {
                    speak(detail);
                }
            });
    connect(voice_, &VoiceController::transcribed, this,
            [this](const QString &text, int costMs) {
                lastText_ = text;
                if (card_ != nullptr) {
                    card_->setTranscript(text, costMs);
                }
            });
    connect(voice_, &VoiceController::optionsChanged, this, [this] {
        applyTtsOptions();
        refreshState();
    });
    refreshState();
}

void VoicePet::setListening(bool listening) {
    if (listening_ == listening) {
        return;
    }
    listening_ = listening;
    refreshState();
    update();
}

bool VoicePet::petEnabled() const {
    return store_->uiBool(QStringLiteral("pet_enabled"), true);
}

void VoicePet::setPetEnabled(bool on) {
    if (!store_->setUiBool(QStringLiteral("pet_enabled"), on)) {
        return;
    }
    if (on) {
        return;
    }
    hide();
    if (bubble_ != nullptr) {
        bubble_->hide();
    }
    collapseCard();
}

void VoicePet::present() {
    if (isVisible()) {
        if (peeked_) {
            popOut();
        }
        raise();
        return;
    }
    if (dock_ == Dock::None) {
        QScreen *scr = QGuiApplication::screenAt(QCursor::pos());
        if (scr == nullptr) {
            scr = QGuiApplication::primaryScreen();
        }
        if (scr != nullptr) {
            const QRect available = scr->availableGeometry();
            move(available.right() - width() - 28,
                 available.bottom() - height() - 28);
        }
    }
    show();
    raise();
}

void VoicePet::presentIfEnabled() {
    if (petEnabled()) {
        present();
    }
}

void VoicePet::speak(const QString &text, int durationMs) {
    if (text.isEmpty() || !isVisible()) {
        return;
    }
    bubbleOnly(text, durationMs);
    // TTS 异步合成，好了直接喇叭出声；没配 key 或关了开关就只有气泡。
    if (tts_ != nullptr && tts_->ready()) {
        tts_->speak(text);
    }
}

void VoicePet::bubbleOnly(const QString &text, int durationMs) {
    if (text.isEmpty() || !isVisible()) {
        return;
    }
    ensureBubble();
    bubble_->setText(text);
    bubble_->adjustSize();
    QScreen *scr = screen();
    if (scr == nullptr) {
        scr = QGuiApplication::primaryScreen();
    }
    const QRect available = scr != nullptr ? scr->availableGeometry() : QRect();
    int x = this->x() + width() / 2 - bubble_->width() / 2;
    if (!available.isNull()) {
        x = qBound(available.left() + 4, x,
                   available.right() - bubble_->width() - 4);
    }
    // 贴上边时就冒在球下面，其它情况冒在球上面。
    int y = dock_ == Dock::Top ? this->y() + height() + 8
                               : this->y() - bubble_->height() - 8;
    if (!available.isNull() && y < available.top() + 4) {
        y = available.top() + 4;
    }
    bubble_->move(x, y);
    bubble_->show();
    bubble_->raise();
    bubbleTimer_->start(durationMs > 0
                            ? durationMs
                            : qMin(7000, 1800 + 90 * text.size()));
}

void VoicePet::applyLlmOptions() {
    if (llm_ == nullptr) {
        return;
    }
    const VoiceOptions options = voice_->options();
    llm_->setOptions(options.llmApiBaseUrl, options.llmApiKey,
                     options.llmApiModel, options.llmApiSystem);
}

void VoicePet::setChatMode(bool on) {
    if (chatMode_ == on) {
        return;
    }
    chatMode_ = on;
    store_->setUiBool(QStringLiteral("pet_chat"), on);
    voice_->setChatMode(on);
    chatHistory_ = QJsonArray{};
    llmBusy_ = false;
    if (on) {
        bubbleOnly(llm_ != nullptr && llm_->ready()
                       ? QString::fromUtf8("对话模式开：直接说话吧")
                       : QString::fromUtf8(
                             "对话模式开，但还没配模型：右键 → 对话设置"),
                   3600);
    } else {
        bubbleOnly(QString::fromUtf8("已退出对话模式"), 2000);
    }
}

void VoicePet::askLlm(const QString &text) {
    if (!llm_->ready()) {
        bubbleOnly(QString::fromUtf8("对话还没配置：右键 → 对话设置 填模型和 Key"),
                   4200);
        return;
    }
    if (llmBusy_) {
        bubbleOnly(QString::fromUtf8("上一条还在想…"), 1600);
        return;
    }
    const VoiceOptions options = voice_->options();
    chatHistory_.append(QJsonObject{{"role", "user"}, {"content", text}});
    while (chatHistory_.size() > 8) {
        chatHistory_.removeFirst();
    }
    QJsonArray messages;
    messages.append(QJsonObject{
        {"role", "system"},
        {"content", options.llmApiSystem.isEmpty()
                        ? QString::fromUtf8(kDefaultChatSystem)
                        : options.llmApiSystem}});
    for (const QJsonValue &entry : chatHistory_) {
        messages.append(entry);
    }
    llmBusy_ = true;
    llm_->chat(messages);
}

void VoicePet::openLlmSettings() {
    auto *dialog = new LlmSettingsDialog(store_, this);
    connect(dialog, &LlmSettingsDialog::saved, this,
            [this] { applyLlmOptions(); });
    dialog->exec();
    dialog->deleteLater();
}

void VoicePet::applyTtsOptions() {
    if (tts_ == nullptr) {
        return;
    }
    const VoiceOptions options = voice_->options();
    tts_->setOptions(options.ttsEnabled, options.ttsBaseUrl, options.ttsApiKey,
                     options.ttsModel, options.ttsVoice, options.ttsStyle);
}

void VoicePet::setVoiceFeedbackMuted(bool muted) {
    if (tts_ == nullptr) {
        return;
    }
    if (muted) {
        tts_->setOptions(false, QString(), QString(), QString(), QString(),
                         QString());
        tts_->stop();
    } else {
        applyTtsOptions();
    }
}

void VoicePet::setTtsEnabled(bool on) {
    // updateVoice 是浅合并：整个 tts 对象读出来改完再写回，才不会丢 api_key。
    QJsonObject tts = store_->voiceConfig().value("tts").toObject();
    tts["enabled"] = on;
    if (!store_->updateVoice(QJsonObject{{"tts", tts}})) {
        return;
    }
    applyTtsOptions();
    if (!on) {
        tts_->stop();
    }
}

void VoicePet::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF center = rect().center();
    const double radius = 38;

    QColor ring;
    double ringWidth = 3;
    if (recording_) {
        ring = QColor(0x3f, 0xb9, 0x50);
        ringWidth = 4 + level_ * 5;
    } else if (transcribing_) {
        ring = QColor(0xd2, 0x99, 0x22);
    } else if (listening_) {
        ring = QColor(79, 140, 255);
        ring.setAlpha(int(140 + 70 * std::sin(phase_ / 9.0)));
    } else {
        ring = QColor(0x4a, 0x55, 0x68);
    }
    QPen pen(ring);
    pen.setWidthF(ringWidth);
    painter.setPen(pen);
    painter.setBrush(QColor(0x12, 0x17, 0x22));
    painter.drawEllipse(center, radius, radius);

    if (transcribing_) {
        // 外圈一段旋转弧：转写进行中的感觉。
        QPen arc(QColor(0xd2, 0x99, 0x22));
        arc.setWidthF(4);
        arc.setCapStyle(Qt::RoundCap);
        painter.setPen(arc);
        painter.setBrush(Qt::NoBrush);
        const double r = radius + 5;
        painter.drawArc(QRectF(center.x() - r, center.y() - r, r * 2, r * 2),
                        -(phase_ % 360) * 16, 100 * 16);
    }

    painter.setPen(Qt::NoPen);
    if (recording_) {
        painter.setBrush(QColor(0xf8, 0x51, 0x49));
        painter.drawRoundedRect(QRectF(center.x() - 11, center.y() - 11, 22, 22),
                                5, 5);
        return;
    }
    // 麦克风：胶囊 + 拾音弧 + 底座（同 MicButton 的画法，等比缩小）。
    const QColor glyph = listening_ ? QColor(0xc9, 0xd1, 0xd9)
                                    : QColor(0x8b, 0x94, 0x9e);
    QPen glyphPen(glyph);
    glyphPen.setWidth(3);
    painter.setPen(glyphPen);
    painter.setBrush(glyph);
    painter.drawRoundedRect(QRectF(center.x() - 7, center.y() - 18, 14, 24),
                            7, 7);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(center.x() - 13, center.y() - 9, 26, 20),
                    180 * 16, 180 * 16);
    painter.drawLine(QPointF(center.x(), center.y() + 7),
                     QPointF(center.x(), center.y() + 13));
    painter.drawLine(QPointF(center.x() - 7, center.y() + 13),
                     QPointF(center.x() + 7, center.y() + 13));
}

void VoicePet::enterEvent(QEnterEvent *event) {
    QWidget::enterEvent(event);
    collapseTimer_->stop();
    if (peeked_) {
        popOut();
    }
    expandCard();
}

void VoicePet::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    collapseSoon();
}

void VoicePet::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    animTimer_->start(50);
}

void VoicePet::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    animTimer_->stop();
    if (bubble_ != nullptr) {
        bubble_->hide();
    }
    if (tts_ != nullptr) {
        tts_->stop();  // 桌宠收起时别让声音悬在半空
    }
    if (chatMode_) {
        setChatMode(false);  // 对话跟着桌宠走，收起即退出（会持久化）
    }
    collapseCard();
}

void VoicePet::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        dragGlobal_ = event->globalPosition().toPoint();
        dragOffset_ = dragGlobal_ - frameGeometry().topLeft();
        pressMoved_ = false;
    }
    QWidget::mousePressEvent(event);
}

void VoicePet::mouseMoveEvent(QMouseEvent *event) {
    if (!(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (!pressMoved_) {
        if ((event->globalPosition().toPoint() - dragGlobal_).manhattanLength() <
            8) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        pressMoved_ = true;
        dock_ = Dock::None;
        peeked_ = false;
        collapseCard();
    }
    move(event->globalPosition().toPoint() - dragOffset_);
}

void VoicePet::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (!pressMoved_) {
            // 单击：没监听先开监听；监听中就是开始/结束说话。
            if (listening_) {
                voice_->mediaToggle();
            } else {
                emit listenToggleRequested();
            }
        } else {
            maybeSnap();
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void VoicePet::contextMenuEvent(QContextMenuEvent *event) {
    QMenu menu(this);
    QAction *listen = menu.addAction(listening_ ? QString::fromUtf8("停止监听")
                                                : QString::fromUtf8("开始监听"));
    connect(listen, &QAction::triggered, this,
            [this] { emit listenToggleRequested(); });
    QAction *talk = menu.addAction(recording_ ? QString::fromUtf8("结束说话")
                                              : QString::fromUtf8("说一句"));
    talk->setEnabled(listening_);
    connect(talk, &QAction::triggered, this,
            [this] { voice_->mediaToggle(); });

    QMenu *mode = menu.addMenu(QString::fromUtf8("识别方式"));
    const QVector<std::pair<int, QString>> backends = {
        {int(VoiceBackend::Hotkey), QString::fromUtf8("转发热键 · 0 模型")},
        {int(VoiceBackend::SenseVoice), QStringLiteral("SenseVoice")},
        {int(VoiceBackend::Qwen3Asr), QStringLiteral("Qwen3-ASR")},
        {int(VoiceBackend::CloudAsr), QString::fromUtf8("云端 ASR（联网）")}};
    for (const auto &entry : backends) {
        QAction *action = mode->addAction(entry.second);
        action->setCheckable(true);
        action->setChecked(int(voice_->options().backend) == entry.first);
        connect(action, &QAction::triggered, this,
                [this, entry] { setBackend(entry.first); });
    }

    menu.addSeparator();
    QAction *chat = menu.addAction(QString::fromUtf8("对话模式"));
    chat->setCheckable(true);
    chat->setChecked(chatMode_);
    connect(chat, &QAction::triggered, this,
            [this] { setChatMode(!chatMode_); });
    QAction *chatSettings = menu.addAction(QString::fromUtf8("对话设置…"));
    connect(chatSettings, &QAction::triggered, this,
            [this] { openLlmSettings(); });
    QAction *read = menu.addAction(QString::fromUtf8("朗读最近一句"));
    read->setEnabled(!lastText_.isEmpty());
    connect(read, &QAction::triggered, this, [this] { speak(lastText_); });
    QAction *tts = menu.addAction(QString::fromUtf8("语音反馈"));
    tts->setCheckable(true);
    tts->setChecked(tts_ != nullptr && tts_->ready());
    tts->setEnabled(!voice_->options().ttsApiKey.isEmpty());
    connect(tts, &QAction::toggled, this,
            [this](bool on) { setTtsEnabled(on); });
    menu.addSeparator();
    QAction *full = menu.addAction(QString::fromUtf8("显示主界面"));
    connect(full, &QAction::triggered, this, [this] { openFullUi(); });
    QAction *panel = menu.addAction(QString::fromUtf8("语音面板"));
    connect(panel, &QAction::triggered, this, [this] { openPanel(); });
    QAction *dock = menu.addAction(QString::fromUtf8("贴边隐藏"));
    connect(dock, &QAction::triggered, this, [this] { dockAndPeek(); });
    QAction *hide = menu.addAction(QString::fromUtf8("隐藏桌宠"));
    connect(hide, &QAction::triggered, this, [this] { closeFromUi(); });
    menu.exec(event->globalPos());
}

void VoicePet::ensureCard() {
    if (card_ == nullptr) {
        card_ = new Card(this);
        refreshState();
    }
}

void VoicePet::ensureBubble() {
    if (bubble_ != nullptr) {
        return;
    }
    bubble_ = new PetBubble(this);
    bubble_->hide();
}

void VoicePet::expandCard() {
    ensureCard();
    refreshState();
    card_->adjustSize();
    QScreen *scr = screen();
    if (scr == nullptr) {
        scr = QGuiApplication::primaryScreen();
    }
    const QRect available = scr != nullptr ? scr->availableGeometry() : QRect();
    // 球在屏幕右半边就往左开卡片，避免伸出屏幕。
    int x = geometry().center().x() > available.center().x()
                ? this->x() - card_->width() - 6
                : this->x() + width() + 6;
    int y = this->y() + height() / 2 - card_->height() / 2;
    if (!available.isNull()) {
        x = qBound(available.left() + 4, x,
                   available.right() - card_->width() - 4);
        y = qBound(available.top() + 4, y,
                   available.bottom() - card_->height() - 4);
    }
    card_->move(x, y);
    card_->show();
    card_->raise();
    raise();
}

void VoicePet::collapseCard() {
    if (card_ != nullptr) {
        card_->hide();
    }
}

void VoicePet::collapseSoon() { collapseTimer_->start(1600); }

void VoicePet::setBackend(int backend) {
    const auto target = static_cast<VoiceBackend>(backend);
    if (target == voice_->options().backend) {
        return;
    }
    store_->updateVoice(
        QJsonObject{{QStringLiteral("backend"), voiceBackendKey(target)}});
    voice_->reload();
    // reload 会发 optionsChanged，卡片勾选态跟着刷新；气泡给句确认。
    speak(QString::fromUtf8("已切换到 %1").arg(voiceBackendLabel(target)));
}

void VoicePet::openPanel() { emit panelRequested(); }

void VoicePet::openFullUi() { emit fullUiRequested(); }

void VoicePet::closeFromUi() {
    setPetEnabled(false);
    emit hiddenByUser();
}

void VoicePet::dockAndPeek() {
    if (dock_ == Dock::None) {
        QScreen *scr = screen();
        if (scr == nullptr) {
            scr = QGuiApplication::primaryScreen();
        }
        if (scr != nullptr) {
            const QRect available = scr->availableGeometry();
            const QPointF center = geometry().center();
            const int distances[] = {
                int(center.x()) - available.left(),   // 左
                available.right() - int(center.x()),  // 右
                int(center.y()) - available.top(),    // 上
                available.bottom() - int(center.y())  // 下
            };
            int nearest = 0;
            for (int i = 1; i < 4; ++i) {
                if (distances[i] < distances[nearest]) {
                    nearest = i;
                }
            }
            dock_ = static_cast<Dock>(nearest + 1);
        }
    }
    peeked_ = true;
    applyDockPos(true);
    collapseCard();
}

void VoicePet::popOut() {
    if (dock_ == Dock::None) {
        return;
    }
    peeked_ = false;
    applyDockPos(false);
    raise();
}

void VoicePet::applyDockPos(bool peek) {
    QScreen *scr = screen();
    if (scr == nullptr) {
        scr = QGuiApplication::primaryScreen();
    }
    if (scr == nullptr || dock_ == Dock::None) {
        return;
    }
    const QRect available = scr->availableGeometry();
    const int size = width();
    int x = pos().x();
    int y = pos().y();
    switch (dock_) {
    case Dock::Right:
        x = available.right() + 1 - (peek ? kPeek : size);
        y = qBound(available.top(), y, available.bottom() - size + 1);
        break;
    case Dock::Left:
        x = available.left() - (peek ? size - kPeek : 0);
        y = qBound(available.top(), y, available.bottom() - size + 1);
        break;
    case Dock::Top:
        y = available.top() - (peek ? size - kPeek : 0);
        x = qBound(available.left(), x, available.right() - size + 1);
        break;
    case Dock::Bottom:
        y = available.bottom() + 1 - (peek ? kPeek : size);
        x = qBound(available.left(), x, available.right() - size + 1);
        break;
    case Dock::None:
        return;
    }
    move(x, y);
}

void VoicePet::maybeSnap() {
    QScreen *scr = screen();
    if (scr == nullptr) {
        scr = QGuiApplication::primaryScreen();
    }
    if (scr == nullptr) {
        return;
    }
    const QRect available = scr->availableGeometry();
    const QPointF center = geometry().center();
    if (center.x() - available.left() < kSnapDist) {
        dock_ = Dock::Left;
    } else if (available.right() - center.x() < kSnapDist) {
        dock_ = Dock::Right;
    } else if (center.y() - available.top() < kSnapDist) {
        dock_ = Dock::Top;
    } else if (available.bottom() - center.y() < kSnapDist) {
        dock_ = Dock::Bottom;
    } else {
        return;
    }
    dockAndPeek();
}

void VoicePet::refreshState() {
    const VoiceOptions options = voice_->options();
    QString state;
    QString color;
    if (recording_) {
        state = QString::fromUtf8("录音中");
        color = QStringLiteral("#3fb950");
    } else if (transcribing_) {
        state = QString::fromUtf8("转写中…");
        color = QStringLiteral("#d29922");
    } else if (listening_) {
        state = QString::fromUtf8("监听中");
        color = QStringLiteral("#4f8cff");
    } else {
        state = QString::fromUtf8("未监听");
        color = QStringLiteral("#8b949e");
    }
    const QString backend = voiceBackendLabel(options.backend);
    setToolTip(QStringLiteral("%1 · %2\n悬停展开 · 右键菜单 · 拖到屏幕边缘贴边")
                   .arg(state, backend));
    if (card_ != nullptr) {
        card_->setStatus(state, color, backend);
        card_->setDetail(detail_);
        card_->setBackendChecked(int(options.backend));
        card_->setHint(options.kbHotkeyEnabled
                           ? QString::fromUtf8("热键 %1 也可以开始")
                                 .arg(voiceComboText(options.kbHotkeyMods,
                                                     options.kbHotkeyVk))
                           : QString());
    }
}

void VoicePet::tick() {
    if (!isVisible()) {
        return;
    }
    phase_ = (phase_ + 1) % 100000;
    level_ = recording_ ? voice_->micLevel() : qMax(0.0, level_ - 0.08);
    update();
}
