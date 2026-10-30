#include "GuideDialog.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace {

constexpr int kTileWidth = 300;
constexpr int kTileHeight = 340;

const char *const kPairingSteps[] = {
    "遥控器靠近要连接的设备",
    "同时长按「返回」+「主页」约 3 秒",
    "指示灯快闪后，在蓝牙列表里点遥控器",
    "回到本程序按一下任意键，卡片键位亮起即连上",
};

const struct {
    const char *key;
    const char *text;
} kGestures[] = {
    {"耳机播放", "Qoder / WorkBuddy 前台时开 / 关语音"},
    {"语音键", "按住说话、松手结束"},
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

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 18);
    root->setSpacing(14);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    auto *title = new QLabel(QString::fromUtf8("使用引导"));
    title->setObjectName("captureTitle");
    auto *sub = new QLabel(QString::fromUtf8("配对 · 常用手势"));
    sub->setObjectName("muted");
    auto *close = new QPushButton(QString::fromUtf8("✕  关闭"));
    close->setObjectName("ghostText");
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    head->addWidget(title);
    head->addWidget(sub);
    head->addStretch(1);
    head->addWidget(close);
    root->addLayout(head);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(20);

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
    columns->addWidget(left, 0, Qt::AlignTop);

    auto *right = new QWidget;
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(16);

    auto *steps = new QWidget;
    auto *stepsLay = new QVBoxLayout(steps);
    stepsLay->setContentsMargins(0, 0, 0, 0);
    stepsLay->setSpacing(9);
    stepsLay->addWidget(guide::heading(QString::fromUtf8("配对步骤")));
    int index = 1;
    for (const char *step : kPairingSteps) {
        stepsLay->addWidget(
            guide::row(QString::number(index++), QString::fromUtf8(step), 26));
    }
    rightLay->addWidget(steps);

    auto *gestures = new QWidget;
    auto *gestureLay = new QVBoxLayout(gestures);
    gestureLay->setContentsMargins(0, 0, 0, 0);
    gestureLay->setSpacing(9);
    gestureLay->addWidget(guide::heading(QString::fromUtf8("常用手势")));
    for (const auto &gesture : kGestures) {
        gestureLay->addWidget(guide::row(QString::fromUtf8(gesture.key),
                                         QString::fromUtf8(gesture.text), 84));
    }
    rightLay->addWidget(gestures);

    rightLay->addStretch(1);
    auto *foot = new QHBoxLayout;
    foot->setContentsMargins(0, 0, 0, 0);
    auto *bluetooth = new QPushButton(QString::fromUtf8("打开蓝牙设置  ↗"));
    bluetooth->setObjectName("guideLink");
    bluetooth->setCursor(Qt::PointingHandCursor);
    connect(bluetooth, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:bluetooth")));
    });
    foot->addStretch(1);
    foot->addWidget(bluetooth);
    rightLay->addLayout(foot);

    columns->addWidget(right, 1);
    root->addLayout(columns);
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
