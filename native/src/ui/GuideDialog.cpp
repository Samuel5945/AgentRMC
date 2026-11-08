#include "GuideDialog.h"

#include "core/ModelImport.h"
#include "core/ThemeManager.h"

#include <QAbstractButton>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <functional>

namespace {

constexpr int kTileWidth = 300;
constexpr int kTileHeight = 340;
// 页序：0 分支选择，1-4 遥控器流程，5 无遥控器语音路线。
constexpr int kVoiceOnlyPage = 5;

const char *const kPairingSteps[] = {
    "遥控器靠近要连接的设备",
    "同时长按「返回」+「主页」约 3 秒",
    "指示灯快闪后，在蓝牙列表里点遥控器",
    "回到本程序按一下任意键，键位亮起即连上",
};

const struct {
    const char *key;
    const char *text;
} kGestures[] = {
    {"语音键", "按住说话、松手结束（Qoder / WorkBuddy 自动切换接法）"},
    {"耳机", "电脑没在放媒体时，播放/暂停 = 同一个语音开关"},
    {"方向 / OK", "浏览与发送；导航模式内翻页"},
    {"双击", "返回=音量·亮度 菜单=鼠标 主页=导航"},
    {"红外键", "电源与音量拨杆电脑收不到"},
};

// 可换行的选择卡片：QPushButton 自己的文本不换行，长描述会被裁掉两头
// （「蓝牙配对，按键控制电脑、按住语音键说」后面那个「话」就这么没了）。
// 所以按钮里放两个 QLabel，并让它们对鼠标透明——点击照旧落在按钮上。
class ChoiceCard final : public QPushButton {
public:
    ChoiceCard(const QString &title, const QString &desc,
               QWidget *parent = nullptr)
        : QPushButton(parent) {
        setObjectName("choiceCard");
        setCursor(Qt::PointingHandCursor);
        // 文案在子 QLabel 里，按钮自己的 text() 是空的；无障碍和自动化工具都靠这个名字。
        setAccessibleName(title + QStringLiteral(" · ") + desc);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(10);
        auto *titleLabel = new QLabel(title, this);
        titleLabel->setObjectName("choiceCardTitle");
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *descLabel = new QLabel(desc, this);
        descLabel->setObjectName("choiceCardDesc");
        descLabel->setAlignment(Qt::AlignCenter);
        descLabel->setWordWrap(true);
        descLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        lay->addStretch(1);
        lay->addWidget(titleLabel);
        lay->addWidget(descLabel);
        lay->addStretch(1);
    }
};

// 引导插图：透明底 + alpha 描边，描边颜色取调色板。
// 把底板色交给 QSS（guide.image.bg）、描边色交给 text.dim，两套配色才都读得清，
// 图与控件比例不一致时也不会露出一圈异色边。
// 缩放放在 paintEvent 里按控件物理尺寸一次算好：先缩成逻辑像素再交给 QLabel，
// 高 DPI 屏会二次放大糊掉细线。
class GuideImage final : public QLabel {
public:
    explicit GuideImage(const QString &path, QWidget *parent = nullptr)
        : QLabel(parent), art_(path) {
        setObjectName("guideImage");
        setAlignment(Qt::AlignCenter);
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
                [this](const Theme &) {
                    tinted_ = QPixmap();
                    update();
                });
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QLabel::paintEvent(event);  // 底板：guide.image.bg + 圆角描边
        if (art_.isNull()) {
            return;
        }
        const QRect area = rect().adjusted(7, 7, -7, -7);
        if (area.width() <= 0 || area.height() <= 0) {
            return;
        }
        if (tinted_.isNull()) {
            const QColor ink =
                ThemeManager::instance().color(QStringLiteral("text.dim"));
            tinted_ = tintArt(ink.isValid() ? ink : QColor(0xc9, 0xd1, 0xd9));
        }
        const QSize size = art_.size().scaled(area.size(), Qt::KeepAspectRatio);
        const QPoint at(area.x() + (area.width() - size.width()) / 2,
                        area.y() + (area.height() - size.height()) / 2);
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(QRect(at, size), tinted_);
    }

private:
    QPixmap tintArt(const QColor &ink) const {
        QImage image =
            art_.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(), ink);
        painter.end();
        return QPixmap::fromImage(image);
    }

    QImage art_;
    QPixmap tinted_;
};

}  // namespace

namespace guide {

QWidget *heading(const QString &text) {
    auto *label = new QLabel(text);
    label->setObjectName("secTitle");
    return label;
}

QWidget *row(const QString &key, const QString &text, int keyWidth) {
    auto *line = new QWidget;
    auto *lay = new QHBoxLayout(line);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(9);
    auto *chip = new QLabel(key);
    chip->setObjectName("kbd");
    chip->setMinimumWidth(keyWidth);
    chip->setAlignment(Qt::AlignCenter);
    lay->addWidget(chip, 0, Qt::AlignTop);
    auto *body = new QLabel(text);
    body->setObjectName("secBody");
    body->setWordWrap(true);
    lay->addWidget(body, 1);
    return line;
}

}  // namespace guide

GuideDialog::GuideDialog(QWidget *parent) : QDialog(parent) {
    setObjectName("guideDialog");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint);
    // 只给「最小宽度」而不是固定宽度：文案和按钮会随版本变长（比如底部多了
    // 「联动插件」），写死 700 就会把内容压扁——文字被裁掉两头，看着像丢了内容。
    setMinimumWidth(700);
    setAcceptDrops(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);
    // 让窗口跟着内容长：宁可对话框宽一点，也不要把字压没。
    root->setSizeConstraint(QLayout::SetMinimumSize);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    auto *title = new QLabel(QString::fromUtf8("使用引导"));
    title->setObjectName("captureTitle");
    stepLabel_ = new QLabel;
    stepLabel_->setObjectName("muted");
    auto *closeButton = new QPushButton(QString::fromUtf8("✕  关闭"));
    closeButton->setObjectName("ghostText");
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    head->addWidget(title);
    head->addWidget(stepLabel_);
    head->addStretch(1);
    head->addWidget(closeButton);
    root->addLayout(head);

    pages_ = new QStackedWidget;
    pages_->addWidget(buildChoicePage());
    pages_->addWidget(buildPairingPage());
    pages_->addWidget(buildKeyTestPage());
    pages_->addWidget(buildVoicePage());
    pages_->addWidget(buildGesturePage());
    pages_->addWidget(buildVoiceOnlyPage());
    root->addWidget(pages_);

    auto *foot = new QHBoxLayout;
    auto *skip = new QPushButton(QString::fromUtf8("跳过"));
    skip->setObjectName("ghostText");
    skip->setCursor(Qt::PointingHandCursor);
    connect(skip, &QPushButton::clicked, this, &QDialog::close);
    prev_ = new QPushButton(QString::fromUtf8("上一步"));
    prev_->setObjectName("ghostSmall");
    prev_->setCursor(Qt::PointingHandCursor);
    connect(prev_, &QPushButton::clicked, this, [this] {
        // 无遥控器路线的上一步回到分支选择页，别串进遥控器流程。
        showStep(pages_->currentIndex() == kVoiceOnlyPage ? 0
                                                          : pages_->currentIndex() - 1);
    });
    next_ = new QPushButton(QString::fromUtf8("下一步"));
    next_->setObjectName("primary");
    next_->setCursor(Qt::PointingHandCursor);
    next_->setFixedWidth(96);
    connect(next_, &QPushButton::clicked, this, [this] {
        if (pages_->currentIndex() + 1 >= pages_->count()) {
            close();
            return;
        }
        showStep(pages_->currentIndex() + 1);
    });
    foot->addWidget(skip);
    foot->addStretch(1);
    foot->addWidget(prev_);
    foot->addWidget(next_);
    root->addLayout(foot);

    for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
        button->setFocusPolicy(Qt::NoFocus);
    }

    dropHint_ = new QLabel(QString::fromUtf8("松手，导入这个模型包"), this);
    dropHint_->setAlignment(Qt::AlignCenter);
    dropHint_->setStyleSheet(QStringLiteral(
        "background: rgba(11,14,20,232); color: #e6edf3; font-size: %1px;"
        " border: 2px dashed rgba(79,140,255,190); border-radius: 12px;")
                                 .arg(ThemeManager::scaledPx(18)));
    dropHint_->hide();

    updateStepUi();
}

void GuideDialog::dragEnterEvent(QDragEnterEvent *event) {
    if (modelPackageFromMime(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    dropHint_->setGeometry(rect());
    dropHint_->raise();
    dropHint_->show();
}

void GuideDialog::dragLeaveEvent(QDragLeaveEvent *) {
    dropHint_->hide();
}

void GuideDialog::dropEvent(QDropEvent *event) {
    dropHint_->hide();
    const QString file = modelPackageFromMime(event->mimeData());
    if (file.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    emit modelPackageDropped(file);
}

QWidget *GuideDialog::buildChoicePage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 4, 0, 0);
    lay->setSpacing(14);

    auto *intro = new QLabel(QString::fromUtf8(
        "开始之前选一下：这台电脑怎么用？之后随时可以在语音卡片里切换。"));
    intro->setObjectName("secBody");
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *cards = new QHBoxLayout;
    cards->setSpacing(16);
    auto makeCard = [&cards](const QString &title, const QString &desc,
                             const std::function<void()> &action) {
        auto *card = new ChoiceCard(title, desc);
        card->setFixedHeight(190);
        connect(card, &QPushButton::clicked, card, action);
        cards->addWidget(card, 1);
    };
    makeCard(QString::fromUtf8("🎮  有遥控器"),
             QString::fromUtf8("先蓝牙配对，按键控制电脑、按住语音键说话"),
             [this] { showStep(1); });
    makeCard(QString::fromUtf8("🎙  没有遥控器"),
             QString::fromUtf8("全局热键 / 语音主界面 / 耳机播放键都能说话；"
                               "也可随时再接遥控器"),
             [this] { showStep(kVoiceOnlyPage); });
    lay->addLayout(cards);
    lay->addStretch(1);
    return page;
}

QWidget *GuideDialog::buildPairingPage() {
    auto *page = new QWidget;
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(20);

    auto *left = new QWidget;
    auto *leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(6);
    auto *image = new GuideImage(QStringLiteral(":/remote/pairing-earbuds.png"));
    image->setFixedSize(kTileWidth, kTileHeight);
    leftLay->addWidget(image);
    auto *caption = new QLabel(
        QString::fromUtf8("长按「返回」+「主页」进入配对"));
    caption->setObjectName("muted");
    caption->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    caption->setWordWrap(true);
    leftLay->addWidget(caption);
    lay->addWidget(left, 0, Qt::AlignTop);

    auto *right = new QWidget;
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(10);
    rightLay->addWidget(guide::heading(QString::fromUtf8("2 · 配对遥控器")));
    int index = 1;
    for (const char *step : kPairingSteps) {
        rightLay->addWidget(
            guide::row(QString::number(index++), QString::fromUtf8(step), 26));
    }
    rightLay->addStretch(1);
    auto *bluetooth = new QPushButton(QString::fromUtf8("打开蓝牙设置  ↗"));
    bluetooth->setObjectName("guideLink");
    bluetooth->setCursor(Qt::PointingHandCursor);
    connect(bluetooth, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:bluetooth")));
    });
    auto *btRow = new QHBoxLayout;
    btRow->addStretch(1);
    btRow->addWidget(bluetooth);
    rightLay->addLayout(btRow);
    lay->addWidget(right, 1);
    return page;
}

QWidget *GuideDialog::buildVoiceOnlyPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);
    lay->addWidget(guide::heading(
        QString::fromUtf8("没有遥控器 · 语音照样完整用")));

    auto *intro = new QLabel(QString::fromUtf8(
        "遥控器只是可选的按键终端。没有它，用热键、语音主界面或耳机播放键都能说话；"
        "以后接上遥控器也不影响这里的选择。"));
    intro->setObjectName("secBody");
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *cols = new QHBoxLayout;
    cols->setContentsMargins(0, 0, 0, 0);
    cols->setSpacing(20);

    auto *left = new QWidget;
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(7);
    ll->addWidget(guide::heading(QString::fromUtf8("1 · 怎么触发")));
    ll->addWidget(guide::row(
        QString::fromUtf8("Ctrl+Alt+V"),
        QString::fromUtf8("全局热键（默认）：任何界面按一下开始、再按结束；"
                          "被占用可在语音设置里换。"),
        88));
    ll->addWidget(guide::row(
        QString::fromUtf8("波形键"),
        QString::fromUtf8("长条波形按钮：点击说话、再点结束；绿色=录音中，"
                          "蓝色=待命，灰色=未监听；停嘴 6 秒自动收尾。"),
        88));
    ll->addWidget(guide::row(
        QString::fromUtf8("耳机"),
        QString::fromUtf8("电脑没在放媒体时，播放/暂停 = 同一个语音开关。"),
        88));
    ll->addWidget(guide::row(
        QString::fromUtf8("桌宠"),
        QString::fromUtf8("单击桌宠 = 开始/结束说话；右键 →「说一句」。"),
        88));
    ll->addStretch(1);
    cols->addWidget(left, 1);

    auto *right = new QWidget;
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(7);
    rl->addWidget(guide::heading(QString::fromUtf8("2 · 话交给谁识别")));

    auto makeRoute = [this](const QString &name, const QString &text,
                            const QString &buttonText,
                            const std::function<void()> &action) {
        auto *line = new QWidget;
        auto *row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *chip = new QLabel(name);
        chip->setObjectName("kbd");
        chip->setMinimumWidth(52);
        chip->setAlignment(Qt::AlignCenter);
        auto *body = new QLabel(text);
        body->setObjectName("secBody");
        body->setWordWrap(true);
        row->addWidget(chip, 0, Qt::AlignTop);
        row->addWidget(body, 1);
        if (!buttonText.isEmpty()) {
            auto *button = new QPushButton(buttonText);
            button->setObjectName("ghostSmall");
            button->setCursor(Qt::PointingHandCursor);
            connect(button, &QPushButton::clicked, this, action);
            row->addWidget(button, 0, Qt::AlignTop);
        }
        return line;
    };
    rl->addWidget(makeRoute(
        QString::fromUtf8("转发"),
        QString::fromUtf8("0 模型，推荐给 Qoder / WorkBuddy 用户：只把语音键"
                          "转给前台 Agent，识别由它自己做。"),
        QString::fromUtf8("用这个"),
        [this] { emit useHotkeyRequested(); }));
    rl->addWidget(makeRoute(
        QString::fromUtf8("本地"),
        QString::fromUtf8("SenseVoice 约 230MB / Qwen3-ASR 约 1GB；"
                          "任何输入框都能出字，可离线。"),
        QString(), nullptr));
    rl->addWidget(makeRoute(
        QString::fromUtf8("云端"),
        QString::fromUtf8("不装模型；音频上传到你配置的 OpenAI 兼容 ASR 服务。"),
        QString(), nullptr));

    voiceOnlyStatus_ = new QLabel;
    voiceOnlyStatus_->setObjectName("muted");
    voiceOnlyStatus_->setWordWrap(true);
    rl->addWidget(voiceOnlyStatus_);
    rl->addStretch(1);
    cols->addWidget(right, 1);

    lay->addLayout(cols);

    auto *enhance = new QWidget;
    auto *el = new QVBoxLayout(enhance);
    el->setContentsMargins(0, 0, 0, 0);
    el->setSpacing(7);
    el->addWidget(guide::heading(QString::fromUtf8("3 · 按需加一点")));
    el->addWidget(guide::row(
        QString::fromUtf8("对话"),
        QString::fromUtf8("右键桌宠 → 对话模式：闲聊走自带 LLM，"
                          "干活的话交给前台 Agent。"),
        68));
    el->addWidget(guide::row(
        QString::fromUtf8("语音反馈"),
        QString::fromUtf8("填 TTS key 后，回复和提醒会念出来；没 key 只有气泡。"),
        68));
    el->addWidget(guide::row(
        QString::fromUtf8("提醒"),
        QString::fromUtf8("装 agentrmc-bridge 联动插件：等授权、等选方案、任务完成时"
                          "桌宠气泡 + 语音。ZCode / WorkBuddy / Qoder / "
                          "DeepSeek Harness / OpenSquilla 都能接，装法见「使用引导 → "
                          "接入 AI Agent」。"),
        68));
    lay->addWidget(enhance);

    // 底部动作行：联动插件 / 语音设置 / 进入语音主界面 都是同级入口，
    // 主按钮是「进入完整界面」——第一次装完先落在完整界面上更稳，语音主界面只是
    // 一种可选外观，不该是默认落点。
    auto *enterRow = new QHBoxLayout;
    enterRow->setSpacing(8);

    auto *pluginButton = new QPushButton(QString::fromUtf8("联动插件"));
    pluginButton->setObjectName("ghostSmall");
    pluginButton->setCursor(Qt::PointingHandCursor);
    pluginButton->setToolTip(QString::fromUtf8(
        "让 Agent 在等确认 / 等选方案 / 任务结束时提醒你\n"
        "装法与「复制安装话术」在「使用引导 → 接入 AI Agent」"));
    connect(pluginButton, &QPushButton::clicked, this,
            [this] { emit pluginGuideRequested(); });

    auto *settings = new QPushButton(QString::fromUtf8("语音设置…"));
    settings->setObjectName("ghostSmall");
    settings->setCursor(Qt::PointingHandCursor);
    connect(settings, &QPushButton::clicked, this,
            [this] { emit openVoiceSettingsRequested(); });

    auto *voiceButton = new QPushButton(QString::fromUtf8("进入语音主界面"));
    voiceButton->setObjectName("ghost");
    voiceButton->setCursor(Qt::PointingHandCursor);
    connect(voiceButton, &QPushButton::clicked, this,
            [this] { emit voiceModeRequested(); });

    auto *enter = new QPushButton(QString::fromUtf8("进入完整界面"));
    enter->setObjectName("primary");
    enter->setCursor(Qt::PointingHandCursor);
    enter->setFixedWidth(150);
    connect(enter, &QPushButton::clicked, this,
            [this] { emit mainWindowRequested(); });

    enterRow->addStretch(1);
    enterRow->addWidget(pluginButton);
    enterRow->addWidget(settings);
    enterRow->addWidget(voiceButton);
    enterRow->addWidget(enter);
    lay->addLayout(enterRow);
    return page;
}

QWidget *GuideDialog::buildKeyTestPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(14);
    lay->addWidget(guide::heading(QString::fromUtf8("3 · 试按一下")));
    auto *body = new QLabel(QString::fromUtf8(
        "连上之后随便按几个键：左边遥控器卡片上对应的键位会亮起，"
        "右下角显示刚触发的「键位 · 动作」。"));
    body->setObjectName("secBody");
    body->setWordWrap(true);
    lay->addWidget(body);

    keyStatus_ = new QLabel(QString::fromUtf8("还没收到按键"));
    keyStatus_->setObjectName("guideKeyStatus");
    keyStatus_->setStyleSheet(QStringLiteral("color:%1; font-size:%2px;")
                                  .arg(ThemeManager::instance().css(
                                           QStringLiteral("muted")),
                                       QString::number(
                                           ThemeManager::scaledPx(16))));
    // 换主题 / 拖字体滑块后按当前状态重算样式串（px 写在里面）。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, keyStatus_,
            [this](const Theme &) {
                keyStatus_->setStyleSheet(
                    keyCount_ > 0
                        ? QStringLiteral("color:%1; font-size:%2px;")
                              .arg(ThemeManager::instance().css(
                                       QStringLiteral("ok")),
                                   QString::number(ThemeManager::scaledPx(16)))
                        : QStringLiteral("color:%1; font-size:%2px;")
                              .arg(ThemeManager::instance().css(
                                       QStringLiteral("muted")),
                                   QString::number(
                                       ThemeManager::scaledPx(16))));
            });
    lay->addWidget(keyStatus_);

    lay->addStretch(1);
    lay->addWidget(guide::row(QString::fromUtf8("按什么"),
                              QString::fromUtf8("方向键、OK、返回都行；语音键按住会"
                                                "进入听写"),
                              68));
    lay->addWidget(guide::row(QString::fromUtf8("小提示"),
                              QString::fromUtf8("按键没反应时，先看遥控器卡片左下角的"
                                                "连接状态灯，或回上一步重新配对"),
                              68));
    return page;
}

QWidget *GuideDialog::buildVoicePage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(14);
    lay->addWidget(guide::heading(QString::fromUtf8("4 · 语音怎么用")));

    auto makeChoice = [this, lay](const QString &name, const QString &text,
                                  QLabel **mark, const QString &buttonText,
                                  const std::function<void()> &action) {
        auto *line = new QWidget;
        auto *row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(9);
        auto *chip = new QLabel(name);
        chip->setObjectName("kbd");
        chip->setMinimumWidth(68);
        chip->setAlignment(Qt::AlignCenter);
        auto *body = new QLabel(text);
        body->setObjectName("secBody");
        body->setWordWrap(true);
        *mark = new QLabel;
        (*mark)->setObjectName("muted");
        auto *button = new QPushButton(buttonText);
        button->setObjectName("ghostSmall");
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, action);
        row->addWidget(chip, 0, Qt::AlignTop);
        row->addWidget(body, 1);
        row->addWidget(*mark, 0, Qt::AlignVCenter);
        row->addWidget(button, 0, Qt::AlignVCenter);
        lay->addWidget(line);
    };

    makeChoice(QString::fromUtf8("转发"),
               QString::fromUtf8("推荐，0 模型：语音键交给 Qoder / WorkBuddy "
                                 "自己的识别"),
               &hotkeyMark_, QString::fromUtf8("用这个"),
               [this] { emit useHotkeyRequested(); });
    makeChoice(QString::fromUtf8("本地"),
               QString::fromUtf8("离线识别，任何软件的输入框都能出字：导入约 230MB 的模型"
                                 "（可直接拖进窗口），再到「模型设置」里切过来"),
               &localMark_, QString::fromUtf8("导入模型…"),
               [this] { emit openVoiceSettingsRequested(); });

    lay->addStretch(1);
    lay->addWidget(guide::row(QString::fromUtf8("耳机"),
                              QString::fromUtf8("电脑没在放媒体时，耳机播放/暂停 "
                                                "= 同一个语音开关"),
                              68));
    return page;
}

QWidget *GuideDialog::buildGesturePage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(9);
    lay->addWidget(guide::heading(QString::fromUtf8("5 · 手势与模式")));
    for (const auto &gesture : kGestures) {
        lay->addWidget(guide::row(QString::fromUtf8(gesture.key),
                                  QString::fromUtf8(gesture.text), 84));
    }
    lay->addStretch(1);
    auto *note = new QLabel(QString::fromUtf8(
        "更细的说明、映射改法都在「使用引导」页签；改完立即生效。"));
    note->setObjectName("muted");
    note->setWordWrap(true);
    lay->addWidget(note);
    return page;
}

void GuideDialog::showStep(int index) {
    const int clamped =
        qBound(0, index, pages_->count() - 1);
    pages_->setCurrentIndex(clamped);
    updateStepUi();
}

void GuideDialog::noteKey(const QString &keyLabel) {
    if (keyStatus_ == nullptr) {
        return;
    }
    ++keyCount_;
    keyStatus_->setText(
        QString::fromUtf8("✓ 已收到 %1 次 · 最近：%2").arg(keyCount_).arg(keyLabel));
    keyStatus_->setStyleSheet(
        QStringLiteral("color:%1; font-size:%2px;")
            .arg(ThemeManager::instance().css(QStringLiteral("ok")),
                 QString::number(ThemeManager::scaledPx(16))));
}

void GuideDialog::setBackend(const QString &backend) {
    const bool hotkey = backend == QStringLiteral("hotkey");
    if (hotkeyMark_ != nullptr) {
        hotkeyMark_->setText(hotkey ? QString::fromUtf8("✓ 当前使用")
                                    : QString());
    }
    if (localMark_ != nullptr) {
        localMark_->setText(hotkey ? QString()
                                   : QString::fromUtf8("✓ 当前使用"));
    }
    if (voiceOnlyStatus_ != nullptr) {
        QString label = backend;
        if (backend == QStringLiteral("hotkey")) {
            label = QString::fromUtf8("转发热键 · 0 模型");
        } else if (backend == QStringLiteral("sensevoice")) {
            label = QStringLiteral("本地 SenseVoice");
        } else if (backend == QStringLiteral("qwen3")) {
            label = QStringLiteral("本地 Qwen3-ASR");
        } else if (backend == QStringLiteral("api")) {
            label = QString::fromUtf8("云端 ASR");
        }
        voiceOnlyStatus_->setText(
            QString::fromUtf8("当前识别方式：%1").arg(label));
    }
}

void GuideDialog::updateStepUi() {
    const int index = pages_->currentIndex();
    // 分支页只管选择，语音路线不算步数：步数按遥控器流程显示。
    if (index == 0) {
        stepLabel_->setText(QString::fromUtf8("先选一条路线"));
    } else if (index == kVoiceOnlyPage) {
        stepLabel_->setText(QString::fromUtf8("没有遥控器 · 语音路线"));
    } else {
        stepLabel_->setText(QString::fromUtf8("第 %1 / %2 步")
                                .arg(index)
                                .arg(pages_->count() - 2));
    }
    // 每步按自己的内容定高，别被第 1 步的配图撑成空白页。
    if (QWidget *page = pages_->currentWidget()) {
        pages_->setFixedHeight(page->sizeHint().height());
    }
    adjustSize();
    prev_->setVisible(index != 0);
    next_->setVisible(index != 0 && index != kVoiceOnlyPage);
    prev_->setText(index == kVoiceOnlyPage ? QString::fromUtf8("重选路线")
                                           : QString::fromUtf8("上一步"));
    next_->setText(index + 1 >= pages_->count()
                       ? QString::fromUtf8("开始使用")
                       : QString::fromUtf8("下一步"));
}

void GuideDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QDialog::keyPressEvent(event);
}

void GuideDialog::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (auto *handle = windowHandle()) {
            handle->startSystemMove();
        }
    }
    QDialog::mousePressEvent(event);
}
