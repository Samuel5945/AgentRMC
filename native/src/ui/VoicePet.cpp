#include "ui/VoicePet.h"

#include "core/AgentWindow.h"
#include "core/ChatText.h"
#include "core/HostNewTask.h"
#include "core/LlmClient.h"
#include "core/Profiles.h"
#include "core/SpeechText.h"
#include "core/SystemCommand.h"
#include "core/TaskCue.h"
#include "core/ThemeManager.h"
#include "core/TtsClient.h"
#include "core/VoiceController.h"
#include "core/VoiceSettings.h"
#include "ui/LlmSettingsDialog.h"
#include "ui/DispatchPicker.h"
#include "ui/PetSprite.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStringList>
#include <QTimer>
#include <QThread>
#include <QVBoxLayout>
#include <QVector>

#include <cmath>
#include <functional>

namespace {

constexpr int kPetSize = 84;    // 桌宠球体直径
// 透明外扩：转写外圈弧（r=43 + 半笔宽 2 = 45）和录音满级粗环（外缘 42.5）
// 都超出球体半径 42，窗口不留边就会被裁成平口；顺带容下精灵浮动/摆动。
constexpr int kPetPad = 4;
constexpr int kPeek = 20 + kPetPad;  // 贴边露出的边条宽（含透明边，实际内容仍露 20）
constexpr int kSnapDist = 80;   // 拖到离屏幕边多近算贴边
// 卡片宽度：比旧按钮排时代宽一点，给「下次派活开新任务」这类长选项和
// 左右留白留余地（宽度是呼吸感的一半，另一半是行距）。
constexpr int kCardWidth = 300;
// 转写区：空闲只占一行（不留一整块空箱子），有字才长到正文高度。
constexpr int kTranscriptMinH = 64;   // 有正文时的最矮高度（约三行）
constexpr int kTranscriptMaxH = 120;  // 再长就在框里滚/截，别把卡片顶出屏幕
constexpr int kBubbleWidth = 220;  // 说话气泡箱体固定宽
constexpr int kTailSpace = 12;     // 气泡尾巴侧预留高度（尾巴画在这里面）
constexpr int kTextPadX = 12;      // 文字离箱体左右边的内边距
constexpr int kTextPadY = 9;       // 文字离箱体上下边的内边距
constexpr qreal kDecorSize = 34.0;  // 装饰贴纸绘制边长（固定，不随箱体高度缩）
constexpr int kDecorPadLeft = 16;   // 有贴纸时箱体左侧的预留（贴纸坐箱左下角）
constexpr int kDecorPadBottom = 22; // 有贴纸时箱体下方的预留（=贴纸边长-咬进深度）
constexpr int kDecorInset = 8;      // 贴纸离窗左缘的内缩（重心压向箱角）
constexpr int kDecorOverlap = 12;   // 贴纸顶边压进箱底的深度（文字下边距会让它，不碰字）
// 对话模式的默认人设：回复要被念出来，所以要求口语化、短。
constexpr const char *kDefaultChatSystem =
    "你是 AgentRMC 的语音桌宠，一个待在屏幕边上的小助手。用户在用语音和你聊天："
    "回复要口语化、简短（最多两句话、50 字以内），不要用列表、markdown、加粗、"
    "表情或颜文字，因为你的回复会被朗读出来；用户明确要清单或步骤时可以逐条说。"
    "默认用中文回答。"
    "用户纠正听写或名词时，直接用一句话确认并继续，不要反问一串问题；不确定时"
    "最多问一个最短的澄清问题。"
    "你没有工具或函数调用能力，不要输出 tool call、<function>、mcp 函数名或 JSON "
    "tool_calls；用户问任务状态时，直接使用系统提供的「当前待办」摘要回答。";

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
        // 留白优先：卡片里元素已经只剩四行，行距比压缩更重要的是呼吸感。
        root->setContentsMargins(15, 14, 15, 13);
        root->setSpacing(10);

        // 顶行：监听状态 + 识别模型下拉。模型选择移到最顶部（同语音页），
        // 不再占中部一整排按钮。
        // 状态是胶囊不是裸字：给个底色框住「● 监听中」，眼睛先落在这里。
        auto *top = new QHBoxLayout;
        top->setSpacing(8);
        status_ = new QLabel(this);
        status_->setObjectName("petStatus");
        status_->setTextFormat(Qt::RichText);
        status_->setAlignment(Qt::AlignCenter);
        status_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        top->addWidget(status_, 0);
        // 模型选择当次要设置放右上角，别让它跟胶囊平分注意力。
        top->addStretch(1);
        backendCombo_ = new QComboBox(this);
        backendCombo_->setObjectName("petBackend");
        backendCombo_->setFocusPolicy(Qt::NoFocus);
        backendCombo_->setCursor(Qt::PointingHandCursor);
        backendCombo_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        backendCombo_->addItem(QString::fromUtf8("转发"));
        backendCombo_->addItem(QString::fromUtf8("Sense"));
        backendCombo_->addItem(QString::fromUtf8("Qwen"));
        backendCombo_->addItem(QString::fromUtf8("云端"));
        connect(backendCombo_, &QComboBox::currentIndexChanged, this,
                [this](int index) { pet_->setBackend(index); });
        top->addWidget(backendCombo_, 0);
        root->addLayout(top);

        // 中部原识别模型按钮排改为「宿主派任务」（宿主+会话两下拉），
        // 与语音页底部那行共用 DispatchPicker，选择同样持久化。
        dispatch_ = new DispatchPicker(pet_->store_, this);
        root->addWidget(dispatch_);

        // 两行设置和正文之间多留一点：不加分割线，靠留白分区。
        root->addSpacing(4);

        // 桌宠卡没有历史列表，这块转写就是唯一的正文显示，保留点击复制。
        // 空态只占一行（不留一整块空箱子），但那行要给够上下内边距，
        // 别贴着框边——挤就挤在这一行上。
        transcript_ = new QLabel(this);
        transcript_->setObjectName("petTranscript");
        transcript_->setWordWrap(true);
        transcript_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        transcript_->setCursor(Qt::PointingHandCursor);
        transcript_->installEventFilter(this);
        const int emptyHeight = transcriptEmptyHeight();
        transcript_->setMinimumHeight(emptyHeight);
        transcript_->setMaximumHeight(emptyHeight);
        root->addWidget(transcript_);

        // 提示行：只在这一轮真有要说的（出错 / 名单）时才出现，空态不占位。
        hint_ = new QLabel(this);
        hint_->setObjectName("petDetail");
        hint_->setWordWrap(false);  // 单行 + elide：折行会把下面的按钮排挤走
        hint_->setTextFormat(Qt::PlainText);
        hint_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        hint_->hide();
        root->addWidget(hint_);

        // 动作区和正文之间多留一点：按钮是「下一件事」，不该跟正文贴着。
        root->addSpacing(2);

        auto *tools = new QHBoxLayout;
        tools->setSpacing(7);
        // 快捷按钮：文字动作等宽；贴边/隐藏收成跟下拉同款的小箭头，
        // 不跟文字动作抢宽度。
        const auto addButton = [this, tools](const QString &text, int stretch,
                                             auto slot) {
            auto *button = new QPushButton(text, this);
            button->setObjectName("ghostSmall");
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            connect(button, &QPushButton::clicked, button, std::move(slot));
            tools->addWidget(button, stretch);
            return button;
        };
        addButton(QString::fromUtf8("面板"), 1, [this] { pet_->openPanel(); });
        addButton(QString::fromUtf8("主界面"), 1, [this] { pet_->openFullUi(); });
        // 对话模式占原来「贴边」的位置：可勾选 ghostSmall，开态主题色底
        // （ghostSmall:checked）。勾选态由 chatModeChanged 广播同步。
        chatBtn_ = new QPushButton(QString::fromUtf8("对话"), this);
        chatBtn_->setObjectName("ghostSmall");
        chatBtn_->setCheckable(true);
        chatBtn_->setChecked(pet_->chatMode());
        chatBtn_->setCursor(Qt::PointingHandCursor);
        chatBtn_->setFocusPolicy(Qt::NoFocus);
        chatBtn_->setToolTip(QString::fromUtf8(
            "桌宠对话模式：说话发给模型聊，回复用 TTS 念出来\n"
            "模型在「模型设置 → 桌宠对话」里配"));
        connect(chatBtn_, &QPushButton::toggled, this,
                [this](bool on) { pet_->setChatMode(on); });
        tools->addWidget(chatBtn_, 1);
        const auto addArrow = [this, tools](const QString &icon,
                                            const QString &tip, auto slot) {
            auto *button = new QPushButton(this);
            button->setObjectName("petArrow");
            button->setIcon(QIcon(icon));
            button->setIconSize(QSize(10, 10));
            button->setFixedWidth(20);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            button->setToolTip(tip);
            connect(button, &QPushButton::clicked, button, std::move(slot));
            tools->addWidget(button, 0);
            return button;
        };
        addArrow(QStringLiteral(":/icons/chevron-right.svg"),
                 QString::fromUtf8("贴边：藏到屏幕边露一条边，鼠标碰一下回来"),
                 [this] { pet_->dockAndPeek(); });
        // 隐藏用 ✕ 图标，跟贴边箭头同款同尺寸（都是裸 chevron 小按钮）。
        addArrow(QStringLiteral(":/icons/close.svg"),
                 QString::fromUtf8("隐藏桌宠（右键菜单里也能找回）"),
                 [this] { pet_->closeFromUi(); });
        root->addLayout(tools);
        setPlaceholder(QString::fromUtf8("说了话会显示在这里，点击复制"),
                       QString::fromUtf8("点击复制最近一句"));
    }

    void setStatus(const QString &state, const QString &color,
                   const QString &backend) {
        Q_UNUSED(backend);  // 模型名改由顶行下拉显示，状态行只留监听态
        status_->setText(QStringLiteral("<span style=\"color:%1;\">●</span> %2")
                             .arg(color, state));
    }

    void setHint(const QString &text) {
        // 空文案不占位（卡片高度跟着变，见 refreshState）：常驻一行
        // 「就绪」跟顶行「监听中」说的是一件事。
        hintFull_ = text;
        refitHint();
    }

    // QLabel 不会自己省略，超长按单行宽度算一遍：宁可少看几个字，
    // 也不能让这一行折成两行把下面的控件挤没。
    void refitHint() {
        if (hintFull_.isEmpty()) {
            hint_->clear();
            hint_->hide();
            return;
        }
        const int width = hint_->width() > 0
                              ? hint_->width()
                              : kCardWidth - 30;  // 卡片左右 contentsMargins 各 15
        hint_->setText(QFontMetrics(hint_->font())
                           .elidedText(hintFull_, Qt::ElideRight, width));
        hint_->setToolTip(hintFull_);
        hint_->show();
    }

    // 空转写框那行不是装饰：它是「还没听到话」时唯一该说点什么的地方。
    // 监听态和热键都换了，这行跟着换，热键值不再单独占一行。
    void setPlaceholder(const QString &text, const QString &tooltip) {
        placeholder_ = text;
        placeholderTooltip_ = tooltip;
        if (text_.isEmpty()) {
            // 空态高度跟着字号走：换主题/改字号时这份 fixed 高度要重算，
            // 否则文字会被上一套字体量出来的框切掉。
            const int height = transcriptEmptyHeight();
            transcript_->setMinimumHeight(height);
            transcript_->setMaximumHeight(height);
            refitTranscript();
        }
    }

    // 一行的净高：QSS 上下 padding 9+9、边框 1+1，再加一点余量。
    int transcriptEmptyHeight() {
        transcript_->ensurePolished();  // font() 要拿到 QSS 里的字号再量
        return transcript_->fontMetrics().height() + 26;
    }

    // 空态那行按框宽省略（换行会被 fixedHeight 切掉半截），正文照常换行。
    void refitTranscript() {
        if (!text_.isEmpty()) {
            return;
        }
        transcript_->ensurePolished();  // font() 要拿到 QSS 里的字号再量
        const int width = transcript_->width() > 0
                              ? transcript_->width() - 22  // QSS padding 左右 11
                              : kCardWidth - 52;           // 卡片 margins 30 + padding 22
        transcript_->setText(QFontMetrics(transcript_->font())
                                 .elidedText(placeholder_, Qt::ElideRight, width));
        transcript_->setToolTip(placeholderTooltip_);
        setTranscriptEmpty(true);
    }

    // 空态那句用次要色（QSS 里 [empty="true"]）：提示别和正文一个分量。
    void setTranscriptEmpty(bool empty) {
        if (transcript_->property("empty").toBool() == empty) {
            return;
        }
        transcript_->setProperty("empty", empty);
        transcript_->style()->unpolish(transcript_);
        transcript_->style()->polish(transcript_);
    }

    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        refitHint();
        refitTranscript();
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        refitHint();
        refitTranscript();
    }

    void setBackendChecked(int backend) {
        const QSignalBlocker block(backendCombo_);
        backendCombo_->setCurrentIndex(backend);
    }

    // QPushButton::setChecked 会发 toggled：同步勾选态时挡住信号，
    // 防止广播→setChecked→toggled→setChatMode 绕一圈。
    void setChatMode(bool on) {
        const QSignalBlocker block(chatBtn_);
        chatBtn_->setChecked(on);
    }

    void refreshDispatch() { dispatch_->refresh(); }

    void setTranscript(const QString &text, int costMs) {
        const QString previous = text_;
        text_ = text;
        if (text.isEmpty()) {
            // 回到空态：收成一行，再把上下文提示写回去。
            transcript_->setMinimumHeight(transcriptEmptyHeight());
            transcript_->setMaximumHeight(transcriptEmptyHeight());
            refitTranscript();
        } else {
            transcript_->setMinimumHeight(kTranscriptMinH);
            transcript_->setMaximumHeight(kTranscriptMaxH);
            transcript_->setText(text);
            setTranscriptEmpty(false);
            transcript_->setToolTip(
                QStringLiteral("%1 · %2s")
                    .arg(text, QString::number(costMs / 1000.0, 'f', 1)));
        }
        // 空↔有字的切换会改卡片高度：位置跟着重算，别留空档也别被屏幕切掉。
        if (isVisible() && previous.isEmpty() != text.isEmpty()) {
            pet_->placeCard();
        }
    }

    QLabel *status_ = nullptr;
    QLabel *transcript_ = nullptr;
    QLabel *hint_ = nullptr;
    QComboBox *backendCombo_ = nullptr;
    QPushButton *chatBtn_ = nullptr;
    DispatchPicker *dispatch_ = nullptr;
    QString text_;
    QString hintFull_;       // 提示行全文（显示的是按宽度 elide 后的版本）
    QString placeholder_;    // 空转写框那行（监听态 + 热键）
    QString placeholderTooltip_;

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        // 顶层透明窗口的 QSS 背景不可靠，照 Toast/PetBubble 的办法自己画。
        // 表面与说话气泡同组（bubble.bg→bubble.bg2 渐变）：浮窗和气泡一样漂在
        // 任意桌面上，得用近不透明的鲸鱼渐变才读得清；面板那组 card.bg 在美术
        // 主题下只有 ~22% 不透明（坐在固定星空底上才成立），浮窗用它会透成玻璃。
        // 旧版 dialog.bg 不透明但无色族，美术主题下是死黑板。
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const auto &tm = ThemeManager::instance();
        const QColor bg = tm.color(QStringLiteral("bubble.bg"));
        const QColor bg2 = tm.color(QStringLiteral("bubble.bg2"));
        QLinearGradient gradient(box.topLeft(), box.bottomLeft());
        gradient.setColorAt(0.0, bg);
        gradient.setColorAt(1.0, bg2.isValid() ? bg2 : bg);
        painter.setPen(QPen(tm.color(QStringLiteral("bubble.border")), 1.0));
        painter.setBrush(gradient);
        painter.drawRoundedRect(box, 16, 16);
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

// 说话气泡：自定义 paintEvent 画鲸鱼风外观（渐变底 + 鲸鱼尾 + 主题装饰贴纸），
// 保证半透明圆角底稳定渲染；classic 的 token 取旧值，观感与旧版一致只多尾巴。
// 带 raise 目标时气泡可点：点了把宿主窗口拉回前台，描边换 accent 色提示。
class PetBubble final : public QLabel {
public:
    // 尾巴指向桌宠：气泡在桌宠上方时朝下，贴顶停靠时朝上。
    enum class Tail { Down, Up };

    explicit PetBubble(QWidget *parent) : QLabel(parent) {
        setObjectName("petBubble");
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                       Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedWidth(kBubbleWidth);
        hide();
    }

    // 在 setText() 之后、show() 之前调用。尺寸全部自己算：QLabel 挂上 QSS 后
    // contentsMargins 不再参与 sizeHint 和文字布局（QSS box model 接管），
    // 靠 adjustSize/margins 留边会被忽略——换行文字会溢出箱体。
    // 窗口宽 = 箱体宽 + 左右文字边距 + 贴纸左侧预留；窗口高 = 换行文字高 +
    // 上下文字边距 + 尾巴/贴纸预留（同侧时取大者，碗和尾巴一左一右不挤）。
    // 文字由 paintEvent 画进箱体内部。
    void setTailDirection(Tail tail) {
        tail_ = tail;
        const bool decor =
            !ThemeManager::instance().current().bubbleDecor.isEmpty();
        setFixedWidth(kBubbleWidth + kTextPadX * 2 +
                      (decor ? kDecorPadLeft : 0));
        ensurePolished();  // font() 要拿到 QSS 里的字号再量文字
        const QFontMetrics fm(font());
        const QRect textBounds = fm.boundingRect(
            QRect(0, 0, kBubbleWidth, 100000),
            Qt::AlignCenter | Qt::TextWordWrap, text());
        const int top = kTextPadY + (tail == Tail::Up ? kTailSpace : 0);
        // 碗咬进箱底比文字下边距深时，下边距让到碗顶，字不会压到碗上。
        const int padBottom = decor ? qMax(kTextPadY, kDecorOverlap) : kTextPadY;
        const int bottom =
            padBottom + qMax(tail == Tail::Down ? kTailSpace : 0,
                             decor ? kDecorPadBottom : 0);
        setFixedHeight(textBounds.height() + top + bottom);
    }

    // 空 handler = 气泡只展示不响应（没 raise 目标时的默认状态）。
    void setClickHandler(std::function<void()> handler) {
        handler_ = std::move(handler);
        setCursor(handler_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();  // 可点击态描边换 accent 色，handler 变化要重绘
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const ThemeManager &theme = ThemeManager::instance();

        // 箱体 = 整个 rect 去掉贴纸预留角（左下）与尾巴侧预留；条件与
        // setTailDirection 同一套。文字内边距在箱体内部另收。
        const bool decor =
            !theme.current().bubbleDecor.isEmpty();
        QRectF box = QRectF(rect());
        if (decor) {
            box.setLeft(box.left() + kDecorPadLeft);
        }
        const int topReserve = tail_ == Tail::Up ? kTailSpace : 0;
        const int bottomReserve = qMax(tail_ == Tail::Down ? kTailSpace : 0,
                                       decor ? kDecorPadBottom : 0);
        box.setTop(box.top() + topReserve);
        box.setBottom(box.bottom() - bottomReserve);
        box.adjust(0.5, 0.5, -0.5, -0.5);

        const QColor bg = theme.color(QStringLiteral("bubble.bg"));
        const QColor bg2 = theme.color(QStringLiteral("bubble.bg2"));
        QColor border = theme.color(QStringLiteral("bubble.border"));
        if (handler_ != nullptr) {
            border = theme.color(QStringLiteral("bubble.accent"));
        }

        // 箱体和尾巴并成一条路径再填色描边：接缝处不会留下一道横线，
        // 渐变也自然延续到尾巴尖。
        QPainterPath boxPath;
        boxPath.addRoundedRect(box, 14, 14);
        const QPainterPath shape = boxPath.united(tailPath(box));

        QLinearGradient gradient(box.topLeft(), box.bottomLeft());
        gradient.setColorAt(0.0, bg);
        gradient.setColorAt(1.0, bg2.isValid() ? bg2 : bg);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(gradient);
        painter.drawPath(shape);

        drawDecor(painter, box);

        // 文字自己画进箱体内部（QLabel 自带布局不认 contentsMargins，见
        // setTailDirection 注释）：textRect = 箱体四边各收 kTextPad。
        const int padBottom = decor ? qMax(kTextPadY, kDecorOverlap) : kTextPadY;
        const QRectF textRect =
            box.adjusted(kTextPadX, kTextPadY, -kTextPadX, -padBottom);
        painter.setPen(theme.color(QStringLiteral("text")));
        painter.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, text());
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && handler_) {
            handler_();
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    // 尾巴路径：与箱体 united 后一次描边。主题 bubble.tail == "whale" 画
    // 鲸鱼双鳍（收窄再分叉两瓣），否则画普通对话气泡的圆尖尾。
    QPainterPath tailPath(const QRectF &box) const {
        // 尾巴统一靠右（箱体 75% 宽处）：和左下角的碗对角呼应，
        // classic 与美术主题位置一致。
        const qreal cx = box.left() + box.width() * 0.75;
        const qreal base = tail_ == Tail::Down ? box.bottom() : box.top();
        const int dir = tail_ == Tail::Down ? 1 : -1;
        const qreal tip = base + dir * (kTailSpace - 1.5);

        QPainterPath tail;
        if (theme().current().bubbleTail == QStringLiteral("whale")) {
            // 根部埋进箱体 1px，united 才能把接缝横线消掉。
            tail.moveTo(cx - 3.0, base - dir * 1.0);
            tail.cubicTo(cx - 5.0, base + dir * 4.0,
                         cx - 9.5, base + dir * 6.0,
                         cx - 9.0, tip);
            tail.cubicTo(cx - 6.0, base + dir * 6.5,
                         cx - 4.0, base + dir * 5.0,
                         cx, base + dir * 4.0);
            tail.cubicTo(cx + 4.0, base + dir * 5.0,
                         cx + 6.0, base + dir * 6.5,
                         cx + 9.0, tip);
            tail.cubicTo(cx + 9.5, base + dir * 6.0,
                         cx + 5.0, base + dir * 4.0,
                         cx + 3.0, base - dir * 1.0);
        } else {
            // 经典尖尾：两侧弧线收到一个圆点，底边埋进箱体保证并集连通。
            tail.moveTo(cx - 8.0, base - dir * 1.0);
            tail.cubicTo(cx - 4.0, base + dir * 3.0,
                         cx - 2.0, base + dir * 7.0,
                         cx, tip);
            tail.cubicTo(cx + 2.0, base + dir * 7.0,
                         cx + 4.0, base + dir * 3.0,
                         cx + 8.0, base - dir * 1.0);
        }
        tail.closeSubpath();
        return tail;
    }

    // 主题装饰贴纸（鲸鱼娘 = 一碗大米饭）：固定 kDecorSize 见方、全不透明，
    // 坐在箱体左下角——顶边压进箱底 kDecorOverlap，像把碗搁在气泡角上。
    // 预留边距由 setTailDirection/paintEvent 同步留出，不遮文字。
    void drawDecor(QPainter &painter, const QRectF &box) {
        const QString decor = ThemeManager::instance().current().bubbleDecor;
        if (decor.isEmpty()) {
            return;
        }
        const QImage image = ThemeManager::cachedImage(decor);
        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
            return;
        }
        // 等比装进 kDecorSize 见方（主题素材非正方形也不拉变形）。
        const qreal scale = qMin(kDecorSize / image.width(),
                                 kDecorSize / image.height());
        const qreal w = image.width() * scale;
        const qreal h = image.height() * scale;
        painter.drawImage(
            QRectF(kDecorInset, box.bottom() + kDecorPadBottom - h, w, h),
            image);
    }

    static ThemeManager &theme() { return ThemeManager::instance(); }

    std::function<void()> handler_;
    Tail tail_ = Tail::Down;
};

VoicePet::VoicePet(ProfileStore *store, VoiceController *voice, QWidget *parent)
    : QWidget(parent), store_(store), voice_(voice) {
    setObjectName("voicePet");
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kPetSize + 2 * kPetPad, kPetSize + 2 * kPetPad);

    animTimer_ = new QTimer(this);
    connect(animTimer_, &QTimer::timeout, this, &VoicePet::tick);

    tts_ = new TtsClient(this);
    applyTtsOptions();
    connect(tts_, &TtsClient::failed, this, [this](const QString &error) {
        // 余额不足这类错误每次会话只提示一次，别在每次反馈时刷气泡。
        emit voiceFeedbackError(error);
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
                QString reply = ChatText::cleanReply(text);
                if (ChatText::looksLikeToolCall(text)) {
                    // 没有工具权限，别把调用协议念出来；直接用真实待办兜底。
                    QString summary = workSummaryProvider_
                                          ? workSummaryProvider_()
                                          : QString();
                    if (summary.isEmpty()) {
                        summary = QString::fromUtf8(
                            "没有需要用户处理的待办");
                    }
                    reply = QString::fromUtf8("我不能调用工具。当前待办：%1。")
                                .arg(summary);
                }
                chatHistory_.append(
                    QJsonObject{{"role", "assistant"}, {"content", reply}});
                while (chatHistory_.size() > 8) {
                    chatHistory_.removeFirst();
                }
                // 对话回复可单独选音色（tts.chat_voice），空 = 跟随语音反馈。
                speakAs(reply);
            });
    connect(llm_, &LlmClient::failed, this,
            [this](const QString &error, qint64) {
                llmBusy_ = false;
                bubbleOnly(QString::fromUtf8("对话失败：%1").arg(error), 4200);
            });
    connect(voice_, &VoiceController::chatRequested, this,
            [this](const QString &text) { handleUtterance(text); });
    // 对话模式可能在语音面板那边开关：控制器广播过来，桌宠缓存必须跟进，
    // 不然 hideEvent 判断的还是旧值，「收起即退出对话模式」会失效。
    // 卡片上对话胶囊的勾选态也跟着广播走（setChatMode 里挡住 toggled）。
    connect(voice_, &VoiceController::chatModeChanged, this, [this](bool on) {
        chatMode_ = on;
        chatHistory_ = QJsonArray{};
        llmBusy_ = false;
        if (card_ != nullptr) {
            card_->setChatMode(on);
        }
    });
    chatMode_ = store_->uiBool(QStringLiteral("pet_chat"), false);
    voice_->setChatMode(chatMode_);

    // 精灵形象：主题有帧就播帧+程序动画，没有就画原来的圆球。
    sprite_ = new PetSprite(this);
    sprite_->applyTheme(ThemeManager::instance().current());
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &theme) {
                sprite_->applyTheme(theme);
                sprite_->setState(spriteState());
                refreshState();
                update();
                applyTtsOptions();  // 音色跟随主题：轮播/切主题立即跟着换
            });

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
                // 「已打开 xx」这类结果趁桌宠可见时念出来（对话模式下
                // 启动确认也走这条），失败的话术（打不开…）同样念。
                if (detail.startsWith(QString::fromUtf8("已打开")) ||
                    detail.startsWith(QString::fromUtf8("打不开"))) {
                    speak(detail);
                }
                sprite_->setState(spriteState());
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
        // LLM 配置（模型/Key/地址）也可能是在主界面或语音面板里改的，
        // 不在这里重读的话，对话会一直打到桌宠构造时的旧配置上。
        applyLlmOptions();
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

void VoicePet::speak(const QString &text, int durationMs,
                     const QString &raiseExe) {
    if (text.isEmpty()) {
        return;
    }
    // 气泡只在桌宠可见时出；语音反馈是声音，桌宠收起也照说。
    if (isVisible()) {
        bubbleOnly(text, durationMs, raiseExe);
    }
    if (tts_ != nullptr && tts_->ready()) {
        // 气泡保留原文（路径/标点可看），念出口转成口语（D:\foo.cpp → 「D盘 foo」）。
        const QString spoken = SpeechText::friendly(text);
        if (!spoken.isEmpty()) {
            tts_->speak(spoken);
        }
    }
}

void VoicePet::speakAs(const QString &text) {
    // 对话回复专用：tts.chat_voice 非空时用独立音色，空 = 跟随语音反馈。
    if (text.isEmpty()) {
        return;
    }
    if (isVisible()) {
        bubbleOnly(text, 0);
    }
    emit chatReplied(text);  // 桌宠收起时回复文本落语音面板/事件流
    if (tts_ != nullptr && tts_->ready()) {
        // 对话音色哨兵（@theme 等）归一成空 = 跟随全局已解析的音色，
        // 别把字面量发给服务端。
        const QString chatVoice = voice_->options().ttsChatVoice;
        tts_->speak(text, isFollowThemeVoice(chatVoice) ? QString()
                                                        : chatVoice);
    }
}

void VoicePet::notify(const QString &text, const QString &raiseExe) {
    if (text.isEmpty() || !isVisible()) {
        return;
    }
    bubbleOnly(text, 6000, raiseExe);
}

void VoicePet::setWorkSummaryProvider(std::function<QString()> provider) {
    workSummaryProvider_ = std::move(provider);
}

void VoicePet::hideBubble() {
    if (bubbleTimer_ != nullptr) {
        bubbleTimer_->stop();
    }
    pendingRaise_.clear();
    if (bubble_ != nullptr) {
        bubble_->hide();
        if (auto *pet = static_cast<PetBubble *>(bubble_)) {
            pet->setClickHandler(nullptr);
        }
    }
}

bool VoicePet::ttsReady() const {
    return tts_ != nullptr && tts_->ready();
}

QString VoicePet::stateJson() const {
    // agents：已知宿主逐个报「进程在不在 / 有没有可交互窗口」。用户排查
    // 「为什么没送过去」时不用再猜——收进托盘的宿主是 process=1 window=0，
    // 语音派活这时候会故意不注入（粘了也是粘到别的窗口上）。
    QJsonArray agents;
    for (const QString &exe : AgentWindow::knownExes()) {
        agents.append(QJsonObject{
            {QStringLiteral("exe"), exe},
            {QStringLiteral("name"), AgentWindow::displayName(exe)},
            {QStringLiteral("process"), AgentWindow::processAlive(exe)},
            {QStringLiteral("window"), AgentWindow::alive(exe)},
        });
    }
    const QJsonObject obj{
        {QStringLiteral("pet_visible"), isVisible()},
        {QStringLiteral("pet_enabled"), petEnabled()},
        {QStringLiteral("docked"), dock_ != Dock::None},
        {QStringLiteral("listening"), listening_},
        {QStringLiteral("recording"), recording_},
        {QStringLiteral("transcribing"), transcribing_},
        {QStringLiteral("chat_mode"), chatMode_},
        // 分流读数（排查「这句为什么没交给 Agent」先看这里）：cue=这句命中了
        // 哪条判据（点名/交接/回指/干活/长句/定住/回落），空=判据没命中、
        // 桌宠自己接；agent=这一刻真交出去的那台宿主，空=没交。
        {QStringLiteral("handoff"),
         QJsonObject{{QStringLiteral("cue"), handoffCue_},
                     {QStringLiteral("agent"), handoffAgent_}}},
        {QStringLiteral("tts_ready"), tts_ != nullptr && tts_->ready()},
        // 麦克风读数：排查「说话没录到」时先看这里——config=配置点名的设备、
        // source=这次真用哪一档（pinned/comms/mapper）、note=降级原因。
        {QStringLiteral("mic"),
         voice_ ? voice_->micStatus() : QJsonObject{}},
        {QStringLiteral("agents"), agents},
    };
    return QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void VoicePet::bubbleOnly(const QString &text, int durationMs,
                          const QString &raiseExe) {
    if (text.isEmpty() || !isVisible()) {
        return;
    }
    ensureBubble();
    bubble_->setText(text);
    // 尾巴方向决定哪侧留预留，setTailDirection 里按文字量好宽高：
    // 贴上边时气泡冒在球下面（尾朝上），其它情况冒在球上面（尾朝下）。
    if (auto *pet = static_cast<PetBubble *>(bubble_)) {
        pet->setTailDirection(dock_ == Dock::Top ? PetBubble::Tail::Up
                                                 : PetBubble::Tail::Down);
    }
    // 有 raise 目标才可点：点了收起气泡并把宿主窗口拉回前台。
    pendingRaise_ = raiseExe;
    if (auto *pet = static_cast<PetBubble *>(bubble_)) {
        if (raiseExe.isEmpty()) {
            pet->setClickHandler(nullptr);
        } else {
            pet->setClickHandler([this] {
                const QString target = pendingRaise_;
                hideBubble();
                if (!target.isEmpty()) {
                    emit bubbleActivated(target);
                }
            });
        }
    }
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

QString VoicePet::onlineAgentExe() const {
    // 交给谁，按可信度从高到低判，**不靠候选顺序瞎猜**：
    // 1) 右键菜单里显式指定过，且它真在跑 → 就用它（用户说了算）；
    // 2) 说话那一刻前台就是某个 Agent → 用户在哪儿说就回哪儿，最准；
    // 3) 最近发过通知的那个宿主 → 说明它正在干活；
    // 4) 都没有才按候选顺序探测（退路，可能不是用户想的那一个）。
    const QString pinned =
        store_ != nullptr ? store_->uiString(QStringLiteral("agent_target"))
                          : QString();
    if (!pinned.isEmpty() && AgentWindow::alive(pinned)) {
        return pinned;
    }
    const QString fg = AgentWindow::foregroundAgentExe();
    if (!fg.isEmpty()) {
        return fg;
    }
    const QString remembered =
        store_ != nullptr ? store_->uiString(QStringLiteral("agent_exe"))
                          : QString();
    if (!remembered.isEmpty() && AgentWindow::alive(remembered)) {
        return remembered;
    }
    // 信息都缺时也别固定偏向某个宿主：优先用户上次语音派活的目标，
    // 它还活着就接着用；真没有历史才落到 knownExes() 顺序。
    const QString lastUsed =
        store_ != nullptr ? store_->uiString(QStringLiteral("agent_last_used"))
                          : QString();
    return AgentWindow::detectAgentExe(lastUsed);
}

bool VoicePet::handOffToAgent(const QString &text, const QString &exe) {
    const QString shortName = AgentWindow::displayName(exe);
    // 派活模式（ui.dispatch_mode_<exe>，语音面板「派活」下拉）：
    // 默认（空 / 旧值 current）= 续当前会话，粘进宿主正在跑的对话；
    // new = 一次性「下次派活开新任务」：该句走新任务机器，真交付后由
    // handOffDelivered 消费回默认（开新任务后也应续当前会话）。
    const QString mode =
        store_ != nullptr
            ? store_->uiString(QStringLiteral("dispatch_mode_") + exe)
            : QString();
    const bool newTask = mode.compare(QStringLiteral("new"),
                                      Qt::CaseInsensitive) == 0;
    // 用户覆盖 ui.chat_focus_<exe>："none" = 不碰焦点直粘（两种模式都尊重）。
    const QString override =
        store_ != nullptr
            ? store_->uiString(QStringLiteral("chat_focus_") + exe)
            : QString();
    const bool noFocus = override.trimmed() == QStringLiteral("none");
    QString combo = override.trimmed().isEmpty()
                        ? AgentWindow::chatFocusCombo(exe)
                        : override;
    QStringList focusMods;
    int focusVk = 0;
    const bool hasFocus =
        !noFocus && newTask && voiceParseCombo(combo, &focusMods, &focusVk);
    // 没有聚焦键的宿主（WorkBuddy 系：Ctrl+N 是「新建文件」）改走 UIA 按名
    // 激活「新建任务」；只在 new 模式启用。
    const QStringList newTaskNames =
        (!newTask || noFocus || hasFocus) ? QStringList()
                                          : HostNewTask::controlNames(exe);
    // 续当前会话只对实测过聊天输入框的宿主走 UIA 定位 + 读回验证；
    // 未实测的宿主保持直粘当前焦点的老行为——别拿猜的输入框糊上去。
    const bool currentSession =
        !newTask && !noFocus && HostNewTask::measuredChatHost(exe);
    // 目标窗口已经在前台就别折腾 raise：省掉抢前台的等待，稍等焦点稳定即注入。
    if (AgentWindow::foregroundAgentExe().compare(exe, Qt::CaseInsensitive) == 0) {
        handOffPollForeground(text, exe, shortName, focusMods, focusVk,
                              hasFocus, newTaskNames, currentSession, 1);
        return true;
    }
    // 先把宿主窗口拉到前台：直接粘会落进用户原本待着的窗口里。
    AgentWindow::raise(exe);
    // 焦点切换不是瞬时的：轮询等前台真的切过去（60ms × 30 ≈ 1.8s），
    // 而不是赌一个固定延时——慢机器上 Electron 恢复焦点可能要几百毫秒。
    handOffPollForeground(text, exe, shortName, focusMods, focusVk, hasFocus,
                          newTaskNames, currentSession, 30);
    return true;
}

void VoicePet::handOffPollForeground(const QString &text, const QString &exe,
                                     const QString &shortName,
                                     const QStringList &focusMods, int focusVk,
                                     bool hasFocus,
                                     const QStringList &newTaskNames,
                                     bool currentSession, int attempts) {
    if (AgentWindow::foregroundAgentExe().compare(exe, Qt::CaseInsensitive) == 0) {
        // 前台已到位：再给 Electron 一拍恢复内部焦点，然后聚焦输入框 + 粘贴。
        QTimer::singleShot(150, this, [this, text, exe, shortName, focusMods,
                                       focusVk, hasFocus, newTaskNames,
                                       currentSession] {
            if (AgentWindow::foregroundAgentExe().compare(
                    exe, Qt::CaseInsensitive) != 0) {
                QApplication::clipboard()->setText(text);
                bubbleOnly(QString::fromUtf8("%1 没抢到前台，已复制，切过去粘贴")
                               .arg(shortName),
                           4600, exe);
                return;
            }
            if (currentSession) {
                // 续聊：焦点交给当前会话输入框并复查落稳，再走两段读回粘贴。
                // raise 后的激活过渡里单次 SetFocus 可能落不稳（真机 15:45 轮
                // 败过一次）：重试几拍再判失败。落不稳不赌：回落剪贴板。
                bool focused = false;
                for (int i = 0; i < 6 && !focused; ++i) {
                    focused = HostNewTask::focusComposer(exe);
                    if (!focused) {
                        QThread::msleep(150);
                    }
                }
                if (focused) {
                    handOffVerifiedPaste(text, exe, shortName);
                } else {
                    QApplication::clipboard()->setText(text);
                    bubbleOnly(
                        QString::fromUtf8(
                            "%1 当前会话输入框没找到/焦点没落稳，已复制")
                            .arg(shortName),
                        5200, exe);
                }
                return;
            }
            if (!newTaskNames.isEmpty()) {
                handOffNewTask(text, exe, shortName, newTaskNames);
                return;
            }
            if (hasFocus && voice_ != nullptr &&
                voice_->sendHotkey(focusMods, focusVk)) {
                // 等聊天输入页打开并聚焦再粘（Qoder 的 Ctrl+N 开新任务输入页）。
                // 走读回验证：一次性新任务的消费要有真交付证明，与 WorkBuddy 路同口径。
                QTimer::singleShot(300, this, [this, text, exe, shortName] {
                    handOffVerifiedPaste(text, exe, shortName);
                });
                return;
            }
            handOffPaste(text, exe, shortName);
        });
        return;
    }
    if (attempts <= 0) {
        // raise 找不到窗口 / 宿主收在托盘：硬注入会打进用户正待着的窗口，
        // 只写剪贴板兜底，至少这句话不丢。
        QApplication::clipboard()->setText(text);
        bubbleOnly(QString::fromUtf8("%1 没抢到前台，已复制，切过去粘贴")
                       .arg(shortName),
                   4600, exe);
        return;
    }
    QTimer::singleShot(60, this, [this, text, exe, shortName, focusMods, focusVk,
                                  hasFocus, newTaskNames, currentSession,
                                  attempts] {
        handOffPollForeground(text, exe, shortName, focusMods, focusVk,
                              hasFocus, newTaskNames, currentSession,
                              attempts - 1);
    });
}

void VoicePet::handOffNewTask(const QString &text, const QString &exe,
                              const QString &shortName,
                              const QStringList &names) {
    // 焦点交给「新建任务」→ 发回车 → 验焦点是否落进输入框。实测这条链在
    // WorkBuddy 两版各 5 轮全通过且不动鼠标（SetFocus + VK_RETURN）。
    const HostNewTask::Attempt att =
        HostNewTask::focusControl(exe, names, 1500);
    constexpr int kVkReturn = 0x0D;  // 不引 windows.h，值就是 VK_RETURN
    if (!att.focused || voice_ == nullptr ||
        !voice_->sendHotkey(QStringList(), kVkReturn)) {
        // 新任务意图失败不降级成续聊：粘进用户当前会话等于替用户决定语义
        // （真机 16:29 泄漏）。回落剪贴板喊人手动发，成因写进气泡。
        QApplication::clipboard()->setText(text);
        bubbleOnly(QString::fromUtf8("%1 新建任务没成（%2），已复制，切过去手动发")
                       .arg(shortName, att.fail.isEmpty()
                                           ? QStringLiteral("回车注入失败")
                                           : att.fail),
                   5200, exe);
        return;
    }
    QTimer::singleShot(350, this, [this, text, exe, shortName] {
        if (!HostNewTask::focusIsEditable(exe)) {
            // 回车后焦点不在该宿主进程的可编辑控件上：不粘，交给剪贴板，
            // 免得把一句话丢进宿主的某个按钮/编辑器里。
            QApplication::clipboard()->setText(text);
            bubbleOnly(QString::fromUtf8("%1 新建任务后焦点不在输入框，已复制")
                           .arg(shortName),
                       5200, exe);
            return;
        }
        handOffVerifiedPaste(text, exe, shortName);
    });
}

void VoicePet::handOffVerifiedPaste(const QString &text, const QString &exe,
                                    const QString &shortName) {
    // WorkBuddy 系：协议唤醒/新建任务导航后视图可能还在切换，paste+Enter 会被
    // 整段吞掉（2026-11-03 真机：最小化唤醒轮字丢、工单空挂；主窗态同 timing
    // 却成功）。改两段读回：先粘不带 Enter，证明字进了输入框；再 Enter，证明
    // 输入框不再含这句话且它出现在消息树里。任一段证明不了就重试一轮，仍不行
    // 回落剪贴板——宁可喊人手动发，不报假成功。
    constexpr int kVkReturn = 0x0D;  // 不引 windows.h，值就是 VK_RETURN
    for (int round = 0; round < 2; ++round) {
        if (voice_ == nullptr || !voice_->injectText(text, false)) {
            break;
        }
        bool landed = false;
        for (int i = 0; i < 9 && !landed; ++i) {
            QThread::msleep(100);
            landed = HostNewTask::composerValue(exe).contains(text);
        }
        if (!landed) {
            continue;  // 字被导航抹掉：下一轮重粘
        }
        if (!voice_->sendHotkey(QStringList(), kVkReturn)) {
            break;
        }
        for (int i = 0; i < 12; ++i) {
            QThread::msleep(100);
            if (!HostNewTask::composerValue(exe).contains(text) &&
                HostNewTask::treeHasText(exe, text)) {
                handOffDelivered(text, exe, shortName);
                return;
            }
        }
        // Enter 被吞或粘后又被抹：再来一轮（最多两轮）
    }
    QApplication::clipboard()->setText(text);
    bubbleOnly(QString::fromUtf8("%1 没证明发送成功，已复制，切过去手动发")
                   .arg(shortName),
               5200, exe);
}

void VoicePet::handOffDelivered(const QString &text, const QString &exe,
                                const QString &shortName) {
    // 「下次派活开新任务」是一次性意图：真交付后消费掉，后续续当前会话
    // （用户 16:25：开新任务后也应继续当前会话，而不是继续开新任务）。
    // 交付失败走剪贴板兜底时不消费，下一句仍重试新任务。
    if (store_ != nullptr &&
        store_->uiString(QStringLiteral("dispatch_mode_") + exe)
            .compare(QStringLiteral("new"), Qt::CaseInsensitive) == 0) {
        store_->setUiString(QStringLiteral("dispatch_mode_") + exe, QString());
    }
    bubbleOnly(QString::fromUtf8("已交给 %1").arg(shortName), 4200, exe);
    // 记下这次派活目标：下次没有前台/通知线索时优先还给它，而不是
    // 按白名单顺序固定偏向某个宿主。
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("agent_last_used"), exe);
    }
    emit workHandedOff(exe, text);
}

void VoicePet::handOffPaste(const QString &text, const QString &exe,
                            const QString &shortName) {
    // 粘贴前最后验一次前台：聚焦等待这几百毫秒里窗口可能被切走。
    if (AgentWindow::foregroundAgentExe().compare(exe, Qt::CaseInsensitive) != 0) {
        QApplication::clipboard()->setText(text);
        bubbleOnly(QString::fromUtf8("%1 不在前台了，已复制，切过去粘贴")
                       .arg(shortName),
                   4600, exe);
        return;
    }
    if (voice_ == nullptr || !voice_->injectText(text, true)) {
        // 注入被拦（UIPI/宿主不吃模拟按键）：退回复制，至少这句话不丢。
        QApplication::clipboard()->setText(text);
        bubbleOnly(QString::fromUtf8("没贴上，已复制，手动发一下"), 4200, exe);
        return;
    }
    handOffDelivered(text, exe, shortName);
}

void VoicePet::handleUtterance(const QString &text) {
    if (text.isEmpty()) {
        return;
    }
    bubbleOnly(QString::fromUtf8("你：%1").arg(text), 2400);
    const QString agent = onlineAgentExe();
    const bool llmReady = llm_ != nullptr && llm_->ready();
    const QString cue = TaskCue::of(text);
    // 分流读数每句重记：这句没交出去就留空，别让上一句的判据冒充这一句的。
    handoffCue_ = cue;
    handoffAgent_ = QString();
    // 干活的话优先给宿主 Agent（已在项目上下文里，也不用另配 key）；
    // 闲聊优先自带 LLM（快、不占会话）。哪边缺就回落另一边——
    // 没有 Agent 时自带 LLM 照常用，反过来也一样，两条腿走路。
    if (agent.isEmpty() && !llmReady) {
        bubbleOnly(
            QString::fromUtf8("没找到 Agent，对话模型也没配：右键 → 对话设置"),
            4600);
        return;
    }
    // 「交给」下拉里点了具体某个 Agent = 用户已经想清楚了，别再替他判一遍：
    // 不跑任务正则、也不问自带 LLM，话直接进那台 Agent 的会话。
    // 只有这个 pin 真的被采纳（它活着且有可交互窗口）才算数——
    // pin 的宿主收进托盘时 onlineAgentExe 不会返回它，这时仍按下面的老逻辑走。
    const QString pinned =
        store_ != nullptr ? store_->uiString(QStringLiteral("agent_target"))
                          : QString();
    if (!pinned.isEmpty() && agent.compare(pinned, Qt::CaseInsensitive) == 0) {
        handoffCue_ = QStringLiteral("定住");
        handoffAgent_ = agent;
        handOffToAgent(text, agent);
        return;
    }
    if (!agent.isEmpty() && (!cue.isEmpty() || !llmReady)) {
        if (cue.isEmpty()) {
            handoffCue_ = QStringLiteral("回落");  // 判据没命中，但对话模型没配
        }
        handoffAgent_ = agent;
        handOffToAgent(text, agent);
        return;
    }
    askLlm(text);
}

void VoicePet::askLlm(const QString &text) {
    // 兜底重读：配置在哪保存的都算数，别依赖某条保存路径记得触发 reload。
    applyLlmOptions();
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
        // 只有这句真在要清单，才把整份功能清单放进这一次请求。
        {"content", chatPersona(TaskCue::wantsList(text))}});
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
    // 音色跟随形象：音色名与描述都取形象包带的（鲸鱼娘=萌系少女），换形象
    // 立即跟着换；指令驱动 TTS 只有钉死 audio.voice 音色才不会每句都漂。
    QString voice = options.ttsVoice;
    QString style = options.ttsStyle;
    if (isFollowThemeVoice(voice)) {
        const Theme &theme = ThemeManager::instance().current();
        voice = theme.voiceName.isEmpty() ? QStringLiteral("Chloe")
                                          : theme.voiceName;
        if (style.isEmpty()) {
            style = theme.voiceStyle;
        }
    }
    tts_->setOptions(options.ttsEnabled, options.ttsBaseUrl, options.ttsApiKey,
                     options.ttsModel, voice, style,
                     options.ttsOutputDevice);
}

void VoicePet::setVoiceFeedbackMuted(bool muted) {
    if (tts_ == nullptr) {
        return;
    }
    if (muted) {
        tts_->setOptions(false, QString(), QString(), QString(), QString(),
                         QString(), QString());
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

    // 主题精灵：帧 + 程序浮动/摆动；没有帧就走下面的圆球画法。
    QPixmap frame;
    qreal rotation = 0;
    qreal dy = 0;
    if (sprite_ != nullptr && sprite_->current(&frame, &rotation, &dy)) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.save();
        painter.translate(center.x(), center.y() + dy);
        painter.rotate(rotation);
        // 形象保持球体直径：窗口为外圈弧扩了 kPetPad 透明边，
        // 按 rect() 缩放会把形象跟着放大。
        const QSize scaled =
            frame.size().scaled(QSize(kPetSize, kPetSize), Qt::KeepAspectRatio);
        painter.drawPixmap(
            QRect(-scaled.width() / 2, -scaled.height() / 2, scaled.width(),
                  scaled.height()),
            frame);
        painter.restore();
        return;
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
    // 对话开关可能在语音主界面/设置里被改过，桌宠出现时以配置为准。
    const bool stored = store_->uiBool(QStringLiteral("pet_chat"), false);
    if (stored != chatMode_) {
        chatMode_ = stored;
        voice_->setChatMode(chatMode_);
    }
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
    // 对话模式不再跟着桌宠收起退出：回复照常语音给出（语音面板/事件流可见），
    // 开关在语音面板「对话」按钮和桌宠右键菜单，用户显式控制。
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

    // 桌宠右键只保留高频动作。识别方式、对话、主题、交给哪个 Agent、
    // 朗读等都在语音面板/完整界面里操作，别把设置项堆到桌面球上。
    QAction *listen = menu.addAction(
        listening_ ? QString::fromUtf8("停止监听")
                   : QString::fromUtf8("开始监听"));
    connect(listen, &QAction::triggered, this,
            [this] { emit listenToggleRequested(); });

    QAction *talk = menu.addAction(recording_ ? QString::fromUtf8("结束说话")
                                              : QString::fromUtf8("说一句"));
    talk->setEnabled(listening_);
    if (!listening_) {
        talk->setToolTip(QString::fromUtf8("先开始监听"));
    }
    connect(talk, &QAction::triggered, this,
            [this] { voice_->mediaToggle(); });

    menu.addSeparator();
    QAction *panel = menu.addAction(QString::fromUtf8("语音面板"));
    connect(panel, &QAction::triggered, this, [this] { openPanel(); });
    QAction *full = menu.addAction(QString::fromUtf8("完整界面"));
    connect(full, &QAction::triggered, this, [this] { openFullUi(); });

    menu.addSeparator();
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
    card_->refreshDispatch();  // 宿主存活/窗口可见会变，展开时重建列表
    placeCard();
    card_->show();
    card_->raise();
    raise();
}

// 量一次卡片、贴着球摆好。展开和「提示行藏/显后重排」共用：两边都只改几何，
// 展开那边再多一步 show/raise。
void VoicePet::placeCard() {
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

void VoicePet::setStatusDetailForShot(const QString &text) {
    detailOverrideForShot_ = text;
    refreshState();
}

void VoicePet::setTranscriptForShot(const QString &text) {
    if (card_ == nullptr) {
        ensureCard();
    }
    card_->setTranscript(text, 900);
}

void VoicePet::setStateForShot(const QString &state) {
    recording_ = state == QStringLiteral("recording");
    transcribing_ = state == QStringLiteral("transcribing");
    listening_ = state == QStringLiteral("listening");
    sprite_->setState(spriteState());
    refreshState();
    update();
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
        color = ThemeManager::instance().css(QStringLiteral("muted"));
    }
    const QString backend = voiceBackendLabel(options.backend);
    setToolTip(QStringLiteral("%1 · %2\n悬停展开 · 右键菜单 · 拖到屏幕边缘贴边")
                   .arg(state, backend));
    if (card_ != nullptr) {
        // 先量一下旧高度：空态占位行、提示行都会改高度，差多少最后一次性重排。
        const int before = card_->sizeHint().height();
        card_->setStatus(state, color, backend);
        card_->setBackendChecked(int(options.backend));
        // 空转写框那行兼做「此刻该怎么做」：监听态 + 热键都并进这一行，
        // 不再单独占一条常驻的「热键 Alt+V 也可以开始」。
        const QString hotkey =
            options.kbHotkeyEnabled
                ? voiceComboText(options.kbHotkeyMods, options.kbHotkeyVk)
                : QString();
        QString placeholder;
        if (recording_) {
            // 顶行胶囊只说「录音中」；这一行说怎么结束（点击是切换、热键是松手）。
            placeholder = QString::fromUtf8("正在听… 松手或再点一下结束");
        } else if (transcribing_) {
            placeholder = QString::fromUtf8("正在出结果…");
        } else if (listening_) {
            placeholder = QString::fromUtf8("说一句，结果显示在这里");
        } else {
            placeholder = QString::fromUtf8("点一下球开始监听");
        }
        QString placeholderTip = QString::fromUtf8("点击复制最近一句");
        if (!hotkey.isEmpty() && !recording_ && !transcribing_) {
            placeholder += QString::fromUtf8(" · %1").arg(hotkey);
            placeholderTip += QString::fromUtf8(" · 热键 %1 开始说话").arg(hotkey);
        }
        card_->setPlaceholder(placeholder, placeholderTip);
        // 提示行只放「这一轮真要多说一句」的内容：出错/名单、截图覆盖文案。
        // 控制器给的「就绪」这类和顶行同义的状态不再抄一遍。
        const QString notice = !detailOverrideForShot_.isEmpty()
                                   ? detailOverrideForShot_
                                   : detail_;
        const QString hint = isNoticeText(notice) ? notice : QString();
        card_->setHint(hint);
        if (card_->isVisible() && card_->sizeHint().height() != before) {
            // 高度变了：重新量一次并贴回球旁边，
            // 别原地留一截空白，也别被屏幕下缘切掉。
            placeCard();
        }
    }
}

bool VoicePet::isNoticeText(const QString &text) {
    // 顶行胶囊已经说了的状态，不再往提示行抄一遍：控制器在录音时会带一句
    // 「录音中… 松手转写」，那是操作说明却和胶囊同义，用户看到的就是重复。
    // 其余都是这一轮的临时信息（打不开…、成功名单、模型没装、「试一句」用法…）。
    static const QStringList statusPrefixes{
        QString::fromUtf8("就绪"), QString::fromUtf8("未监听"),
        QString::fromUtf8("监听中"), QString::fromUtf8("录音中"),
        QString::fromUtf8("转写中")};
    if (text.isEmpty()) {
        return false;
    }
    for (const QString &prefix : statusPrefixes) {
        if (text.startsWith(prefix)) {
            return false;
        }
    }
    return true;
}

void VoicePet::tick() {
    if (!isVisible()) {
        return;
    }
    phase_ = (phase_ + 1) % 100000;
    level_ = recording_ ? voice_->micLevel() : qMax(0.0, level_ - 0.08);
    sprite_->advance(50);
    update();
}

QString VoicePet::chatPersona(bool withInventory) const {
    const VoiceOptions options = voice_->options();
    QString persona = options.llmApiSystem;
    if (persona.isEmpty()) {
        // 名字跟主题走：主题带了人设就用主题的（换主题 = 换角色）。
        persona = ThemeManager::instance().current().persona;
    }
    if (persona.isEmpty()) {
        persona = QString::fromUtf8(kDefaultChatSystem);
    }
    // 人设只定义性格，不负责产品事实。每次请求补一段本机语境，
    // 免得模型把 ZCode / WorkBuddy 当成编程语言，或者反问用户「什么是 ZCODE」。
    QString context = QString::fromUtf8(
        "\n补充事实：AgentRMC 是用户开发的 Windows 桌面语音桌宠项目；"
        "ZCode、WorkBuddy、WorkBuddy AI、Qoder 都是 AI 编程助手"
        "（本程序联动的宿主），不是编程语言，也不是用户本人。"
        "你没有工具或函数调用能力，不要输出 <tool call>、mcp 函数名或 JSON "
        "tool_calls；任务状态只依据下面提供的当前待办。"
        "回答保持一到两句话，不连续反问；用户要清单或步骤时照下面的功能答。");
    if (workSummaryProvider_) {
        const QString tasks = workSummaryProvider_();
        if (!tasks.isEmpty()) {
            context += QString::fromUtf8("\n当前待办：%1。").arg(tasks);
        }
    }
    const QString fg = AgentWindow::foregroundAgentExe();
    if (!fg.isEmpty()) {
        context += QString::fromUtf8(" 用户此刻前台是 %1。")
                       .arg(AgentWindow::displayName(fg));
    }
    // 软件自己的功能只能自己说，但平时只带一句索引；整份清单（350 字）只在
    // 用户这句真在要清单时进这一次请求——不然系统提示词就成了上下文垃圾桶。
    // 词表直接问解析层，不在这儿抄一份。
    context += QString::fromUtf8(
        " 你自己会：语音操控电脑的音量、亮度、媒体播放、屏幕与电源"
        "（关机/重启/睡眠/休眠要先说「确认」）、听写粘贴、把话转交给在跑的 "
        "AI 编程助手、替宿主喊人（等授权 / 等选方案 / 做完 / 出错）、"
        "把回复念出来。");
    QStringList hosts;
    for (const QString &exe : AgentWindow::knownExes()) {
        if (AgentWindow::alive(exe)) {
            hosts << AgentWindow::displayName(exe);
        }
    }
    context += QString::fromUtf8("现在在跑的 AI 编程助手：%1。")
                   .arg(hosts.isEmpty() ? QString::fromUtf8("无")
                                        : hosts.join(QStringLiteral("、")));
    if (withInventory) {
        context += QString::fromUtf8("\n逐条清单（这一句用户要的就是清单）：\n%1")
                       .arg(SystemCommandText::spokenList());
    }
    return persona + context;
}

QString VoicePet::spriteState() const {
    if (recording_) {
        return QStringLiteral("recording");
    }
    if (transcribing_) {
        return QStringLiteral("transcribing");
    }
    if (chatMode_) {
        return QStringLiteral("chat");
    }
    if (listening_) {
        return QStringLiteral("listening");
    }
    return QStringLiteral("idle");
}
