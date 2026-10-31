#include "GuideDialog.h"

#include "core/ModelImport.h"

#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
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

// 原图按控件物理尺寸一次缩放：先缩成逻辑像素再交给 QLabel，高 DPI 屏会二次放大糊掉细线。
class GuideImage final : public QLabel {
public:
    explicit GuideImage(const QString &path, QWidget *parent = nullptr)
        : QLabel(parent), pixmap_(path) {
        setObjectName("guideImage");
        setAlignment(Qt::AlignCenter);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QLabel::paintEvent(event);
        if (pixmap_.isNull()) {
            return;
        }
        const QRect area = rect().adjusted(7, 7, -7, -7);
        if (area.width() <= 0 || area.height() <= 0) {
            return;
        }
        const QSize size =
            pixmap_.size().scaled(area.size(), Qt::KeepAspectRatio);
        const QPoint at(area.x() + (area.width() - size.width()) / 2,
                        area.y() + (area.height() - size.height()) / 2);
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(QRect(at, size), pixmap_);
    }

private:
    QPixmap pixmap_;
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
    setFixedWidth(700);
    setAcceptDrops(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);

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
    pages_->addWidget(buildPairingPage());
    pages_->addWidget(buildKeyTestPage());
    pages_->addWidget(buildVoicePage());
    pages_->addWidget(buildGesturePage());
    root->addWidget(pages_);

    auto *foot = new QHBoxLayout;
    auto *skip = new QPushButton(QString::fromUtf8("跳过"));
    skip->setObjectName("ghostText");
    skip->setCursor(Qt::PointingHandCursor);
    connect(skip, &QPushButton::clicked, this, &QDialog::close);
    prev_ = new QPushButton(QString::fromUtf8("上一步"));
    prev_->setObjectName("ghostSmall");
    prev_->setCursor(Qt::PointingHandCursor);
    connect(prev_, &QPushButton::clicked, this,
            [this] { showStep(pages_->currentIndex() - 1); });
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

    dropHint_ = new QLabel(QString::fromUtf8("松手，导入这个模型包"), this);
    dropHint_->setAlignment(Qt::AlignCenter);
    dropHint_->setStyleSheet(QStringLiteral(
        "background: rgba(11,14,20,232); color: #e6edf3; font-size: 18px;"
        " border: 2px dashed rgba(79,140,255,190); border-radius: 12px;"));
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
    rightLay->addWidget(guide::heading(QString::fromUtf8("1 · 配对遥控器")));
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

QWidget *GuideDialog::buildKeyTestPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(14);
    lay->addWidget(guide::heading(QString::fromUtf8("2 · 试按一下")));
    auto *body = new QLabel(QString::fromUtf8(
        "连上之后随便按几个键：左边遥控器卡片上对应的键位会亮起，"
        "右下角显示刚触发的「键位 · 动作」。"));
    body->setObjectName("secBody");
    body->setWordWrap(true);
    lay->addWidget(body);

    keyStatus_ = new QLabel(QString::fromUtf8("还没收到按键"));
    keyStatus_->setObjectName("guideKeyStatus");
    keyStatus_->setStyleSheet(QStringLiteral("color:#8b949e; font-size:16px;"));
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
    lay->addWidget(guide::heading(QString::fromUtf8("3 · 语音怎么用")));

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
               QString::fromUtf8("离线识别：导入模型包（约 230MB）后本机转写；"
                                 "模型包可以直接拖进窗口"),
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
    lay->addWidget(guide::heading(QString::fromUtf8("4 · 手势与模式")));
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
        QStringLiteral("color:#3fb950; font-size:16px;"));
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
}

void GuideDialog::updateStepUi() {
    const int index = pages_->currentIndex();
    stepLabel_->setText(
        QString::fromUtf8("第 %1 / %2 步").arg(index + 1).arg(pages_->count()));
    // 每步按自己的内容定高，别被第 1 步的配图撑成空白页。
    if (QWidget *page = pages_->currentWidget()) {
        pages_->setFixedHeight(page->sizeHint().height());
    }
    adjustSize();
    prev_->setEnabled(index > 0);
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
