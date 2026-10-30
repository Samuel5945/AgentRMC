#include "MainWindow.h"

#include "ui/KeymapPage.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QCloseEvent>
#include <QCoreApplication>
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

    auto *tag = new QLabel("NATIVE · M2");
    tag->setObjectName("tag");

    lay->addWidget(logo);
    lay->addWidget(brand);
    lay->addWidget(tag);
    lay->addStretch(1);

    auto *dot = new QLabel;
    dot->setObjectName("dot");
    dot->setFixedSize(6, 6);
    auto *status = new QLabel(QString::fromUtf8("未监听"));
    status->setObjectName("muted");
    lay->addWidget(dot, 0, Qt::AlignVCenter);
    lay->addWidget(status);
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
    auto *badge = new QLabel("M2");
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
    card->setFixedHeight(210);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(8);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("语音"));
    title->setObjectName("cardTitle");
    auto *badge = new QLabel("M2");
    badge->setObjectName("badge");
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(badge);
    lay->addLayout(head);

    auto *box = new QLabel(QString::fromUtf8("语音输入 · hotkey 模式 / 里程碑 5 接入 whisper.cpp"));
    box->setObjectName("voiceBox");
    box->setWordWrap(true);
    box->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lay->addWidget(box, 1);

    auto *foot = new QHBoxLayout;
    auto *state = new QLabel(QString::fromUtf8("本地模型：未接入"));
    state->setObjectName("muted");
    foot->addWidget(state);
    foot->addStretch(1);
    lay->addLayout(foot);
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
    auto *count = new QLabel(QString::fromUtf8("0 条"));
    count->setObjectName("muted");
    auto *clear = new QPushButton(QString::fromUtf8("清空"));
    clear->setObjectName("ghostText");
    clear->setEnabled(false);
    head->addWidget(count);
    head->addStretch(1);
    head->addWidget(clear);
    cl->addLayout(head);

    auto *waiting = new QLabel(QString::fromUtf8("等待迁移 · 原生壳尚未接入输入"));
    waiting->setObjectName("muted");
    waiting->setAlignment(Qt::AlignCenter);
    cl->addWidget(waiting, 1);
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
        QString::fromUtf8("里程碑 3（下一步）· Raw Input + KeyRouter + 动作注入"),
        QString::fromUtf8("里程碑 4 · 原生键接管 / 鼠标模式 / 耳机播放键"),
        QString::fromUtf8("里程碑 5 · hotkey 语音 + whisper.cpp（GGML 模型）"),
        QString::fromUtf8("里程碑 6 · BLE 电量 / 跨屏 DPI / windeployqt 打包"),
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
    tray_->setToolTip("AgentRMC Native · 里程碑 1");
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
            "C++/Qt + whisper.cpp 原生迁移\n\n"
            "里程碑 1（当前）：窗口 / 主题 / 托盘 / 构建\n"
            "2：config + profiles + 映射表\n"
            "3：Raw Input + Router + 注入\n"
            "4：原生键接管 / 鼠标模式\n"
            "5：hotkey 语音 + whisper.cpp\n"
            "6：BLE / DPI / 打包"));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (tray_ && !quitting_) {
        hide();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}
