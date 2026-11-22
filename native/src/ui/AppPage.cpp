#include "AppPage.h"

#include "core/ThemeManager.h"
#include "core/AppLauncher.h"
#include "core/Profiles.h"
#include "core/ShellIcon.h"
#include "core/SystemControl.h"
#include "ui/ToggleSwitch.h"

#include <QDialog>
#include <QBuffer>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

#include <functional>

namespace {

// 底部汇总文案：Qt 页和 WebView 外壳共用，别让两处各写一份。
QString appSummaryText(int installed, int total) {
    return QString::fromUtf8(
               "本机找到 %1 / %2 个 · 语音启动要用本地识别（SenseVoice / Qwen3）；"
               "转发热键时语音交给前台应用。想用手上的遥控器，"
               "可以在「按键映射」里把「打开 xx」设成某个键的单击 / 双击 / 长按。")
        .arg(installed)
        .arg(total);
}

// 卡片上的状态短文案：窄卡里放不下「开始菜单 · Chrome.lnk」这种长串，
// 只给类型，完整来源放 tooltip。
QString appStateShort(const AppEntry &entry, const AppTarget &target) {
    if (!target.found) {
        return QString::fromUtf8("没装");
    }
    if (!entry.customPath.isEmpty()) {
        return QString::fromUtf8("自定义");
    }
    if (target.kind == QStringLiteral("url")) {
        return QString::fromUtf8("网址");
    }
    if (target.kind == QStringLiteral("aumid")) {
        return QString::fromUtf8("Windows 应用");
    }
    if (target.kind == QStringLiteral("exe")) {
        return QString::fromUtf8("默认浏览器");
    }
    return QString::fromUtf8("开始菜单");
}

QString appStateTip(const AppEntry &entry, const AppTarget &target) {
    if (!target.found) {
        return QString::fromUtf8("开始菜单里没有这个应用的快捷方式");
    }
    if (!entry.customPath.isEmpty()) {
        return entry.customPath;
    }
    return target.detail.isEmpty() ? target.target : target.detail;
}

constexpr int kIconPx = 44;          // 卡片图标边长
constexpr int kCardMinWidth = 96;    // 瀑布流单卡最小宽（决定列数）
constexpr int kCardMaxColumns = 6;   // 最多几列，宽屏也不铺太密

// —— 图标瀑布流布局 ——
// 每张卡高度随名字/状态行数变化，普通 QVBoxLayout 会把卡片顶对齐成等高网格，
// 这里按「最短列优先」手动摆位，做出真正的瀑布流。列数随可用宽度变化。
class MasonryLayout : public QLayout {
public:
    explicit MasonryLayout(QWidget *parent = nullptr) : QLayout(parent) {}
    ~MasonryLayout() override { qDeleteAll(items_); }

    void addItem(QLayoutItem *item) override {
        if (item != nullptr) {
            items_.append(item);
            cachedValid_ = false;
            invalidate();
        }
    }
    int count() const override { return items_.size(); }
    QLayoutItem *itemAt(int index) const override {
        return (index >= 0 && index < items_.size()) ? items_.at(index) : nullptr;
    }
    QLayoutItem *takeAt(int index) override {
        if (index < 0 || index >= items_.size()) {
            return nullptr;
        }
        cachedValid_ = false;
        return items_.takeAt(index);
    }
    QSize sizeHint() const override {
        return cachedValid_ ? cached_ : roughSize(geometry_.width());
    }
    QSize minimumSize() const override {
        return cachedValid_ ? cached_ : roughSize(geometry_.width());
    }
    QSize maximumSize() const override { return QSize(16777215, 16777215); }
    int spacing() const override { return spacing_; }
    void setSpacing(int space) override {
        spacing_ = space;
        cachedValid_ = false;
    }

protected:
    // Qt6 里 QLayout 没有 doLayout()，自定义布局覆写 setGeometry() 摆位。
    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);
        geometry_ = rect;
        arrange();
    }

private:
    void arrange() {
        QRect area = contentsRect();
        if (area.width() <= 0) {
            return;
        }
        const int cols = columnCount(area.width());
        const int cardW = cardWidth(area.width(), cols);
        if (cardW <= 0) {
            return;
        }
        QVector<int> colH(cols, 0);
        for (QLayoutItem *item : items_) {
            if (item == nullptr) {
                continue;
            }
            QWidget *wgt = item->widget();
            int h = 0;
            if (wgt != nullptr) {
                // 先按目标列宽约束，让卡片内部的文字折行算出真实高度。
                wgt->setFixedWidth(cardW);
                wgt->adjustSize();
                h = wgt->height();
            }
            if (h <= 0) {
                h = item->sizeHint().height();
            }
            int c = 0;
            for (int i = 1; i < cols; ++i) {
                if (colH[i] < colH[c]) {
                    c = i;
                }
            }
            const int x = area.left() + c * (cardW + spacing_);
            const int y = area.top() + colH[c];
            item->setGeometry(QRect(x, y, cardW, h));
            colH[c] += h + spacing_;
        }
        int maxH = 0;
        for (int h : colH) {
            maxH = qMax(maxH, h);
        }
        cached_ = QSize(area.width(), qMax(0, maxH - spacing_));
        cachedValid_ = true;
        update();
    }

    int columnCount(int width) const {
        const int cols = (width + spacing_) / (kCardMinWidth + spacing_);
        return qBound(1, cols, kCardMaxColumns);
    }
    int cardWidth(int width, int cols) const {
        return (width - (cols - 1) * spacing_) / cols;
    }
    // 缓存未建立时的粗算（不改动子控件宽度），仅用于首帧尺寸提示；
    // arrange() 完成后会用精确值覆盖。
    QSize roughSize(int width) const {
        if (width <= 0) {
            return QSize(0, 0);
        }
        const int cols = columnCount(width);
        const int cardW = cardWidth(width, cols);
        if (cardW <= 0) {
            return QSize(0, 0);
        }
        QVector<int> colH(cols, 0);
        for (const QLayoutItem *item : items_) {
            if (item == nullptr) {
                continue;
            }
            const int h = qMax(0, item->sizeHint().height());
            int c = 0;
            for (int i = 1; i < cols; ++i) {
                if (colH[i] < colH[c]) {
                    c = i;
                }
            }
            colH[c] += h + spacing_;
        }
        int maxH = 0;
        for (int h : colH) {
            maxH = qMax(maxH, h);
        }
        return QSize(width, qMax(0, maxH - spacing_));
    }

    QVector<QLayoutItem *> items_;
    QRect geometry_;
    int spacing_ = 10;
    mutable QSize cached_;
    mutable bool cachedValid_ = false;
};

// 可点击的卡片：整卡点一下启动应用（按钮类子控件会自己吃掉点击，不会误触发）。
class AppCard : public QFrame {
public:
    using LaunchFn = std::function<void()>;
    explicit AppCard(const LaunchFn &launch, bool clickable,
                     QWidget *parent = nullptr)
        : QFrame(parent), launch_(launch), clickable_(clickable) {
        setObjectName("appCard");
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setAttribute(Qt::WA_Hover, true);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (clickable_ && event->button() == Qt::LeftButton && launch_) {
            launch_();
            return;
        }
        QFrame::mousePressEvent(event);
    }

private:
    LaunchFn launch_;
    bool clickable_ = false;
};

// 内置应用表里某些商店应用的系统图标根本取不到（Win10 老栈对 AUMID
// 枚举/解析全部失败），改用随包资源图标：id -> qrc 路径。
QString bundledIconPath(const AppEntry &entry) {
    if (entry.id == QStringLiteral("codex")) {
        return QStringLiteral(":/icons/app-codex.png");
    }
    // 壁纸引擎是用户自定义项：slug 带中文编码、不同机器路径不同，按名字认。
    if (entry.id.startsWith(QStringLiteral("custom:")) &&
        entry.name.contains(QString::fromUtf8("壁纸引擎"))) {
        return QStringLiteral(":/icons/app-wallpaper-engine.png");
    }
    return {};
}

// 图标解析用真实路径（exe / .lnk / 自定义路径）；URL 和没装没有路径。
QString targetPathForIcon(const AppEntry &entry, const AppTarget &target) {
    if (target.found &&
        (target.kind == QStringLiteral("shortcut") ||
         target.kind == QStringLiteral("exe"))) {
        return target.target;
    }
    if (!entry.customPath.isEmpty() &&
        !entry.customPath.contains(QStringLiteral("://"))) {
        return entry.customPath;
    }
    return {};
}

// 真实文件图标一律走 ShellIcon（IShellItemImageFactory，64px）。
QIcon shellIconFor(const AppEntry &entry, const AppTarget &target) {
    const QString path = targetPathForIcon(entry, target);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        return ShellIcon::fromPath(path);
    }
    return {};
}

// 取应用图标：随包资源优先（Codex / 壁纸引擎），商店应用走 AppsFolder；
// 文件路径图标由 rebuild 走 ShellIcon → QFileIconProvider 两级兜底。
QIcon appIconFor(const AppEntry &entry, const AppTarget &target) {
    const QString bundled = bundledIconPath(entry);
    if (!bundled.isEmpty()) {
        const QIcon icon(bundled);
        if (!icon.isNull()) {
            return icon;
        }
    }
    // Windows 商店应用：没有文件路径，经 shell:AppsFolder 枚举按名字取。
    if (target.found && target.kind == QStringLiteral("aumid")) {
        const QIcon big = ShellIcon::fromAumid(target.target);
        if (!big.isNull()) {
            return big;
        }
    }
    return {};
}

// 通用图标：网址=地球、Windows 应用=方块、没装=虚线占位。
// 按主题色现画，不做缓存；换主题时随 themeChanged 重画。
QIcon fallbackAppIcon(const AppEntry &entry, const AppTarget &target) {
    Q_UNUSED(entry);
    const QString kind =
        !target.found ? QStringLiteral("miss")
                      : (target.kind == QStringLiteral("url")
                             ? QStringLiteral("url")
                             : QStringLiteral("aumid"));
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor muted = ThemeManager::instance().color(QStringLiteral("muted"));
    const QColor faint = ThemeManager::instance().color(QStringLiteral("faint"));
    const QRectF r(1.5, 1.5, kIconPx - 3.0, kIconPx - 3.0);
    p.setPen(QPen(muted, qMax(1.0, kIconPx * 0.055)));
    p.setBrush(Qt::NoBrush);
    if (kind == QStringLiteral("url")) {
        p.drawEllipse(r);
        p.drawEllipse(QRectF(r.center().x() - kIconPx * 0.2, r.top(),
                             kIconPx * 0.4, kIconPx - 3.0));
        p.drawLine(QPointF(r.center().x(), r.top() + 2),
                   QPointF(r.center().x(), r.bottom() - 2));
        p.drawLine(QPointF(r.left() + 2, r.center().y()),
                   QPointF(r.right() - 2, r.center().y()));
    } else if (kind == QStringLiteral("aumid")) {
        p.drawRoundedRect(r, kIconPx * 0.2, kIconPx * 0.2);
        p.drawRoundedRect(
            QRectF(r.left() + kIconPx * 0.22, r.top() + kIconPx * 0.28,
                   kIconPx * 0.56, kIconPx * 0.44),
            kIconPx * 0.1, kIconPx * 0.1);
    } else {
        QPen dash(faint, qMax(1.0, kIconPx * 0.055), Qt::DashLine);
        p.setPen(dash);
        p.drawRoundedRect(r, kIconPx * 0.2, kIconPx * 0.2);
        p.setPen(QPen(faint, qMax(1.0, kIconPx * 0.07)));
        p.drawLine(QPointF(r.center().x() - kIconPx * 0.12, r.center().y()),
                   QPointF(r.center().x() + kIconPx * 0.12, r.center().y()));
    }
    return QIcon(pm);
}

// 图标转 data URL，给 WebView 外壳直接当 <img src> 用。
QString iconDataUrl(const QIcon &icon) {
    const QPixmap pm = icon.pixmap(kIconPx, kIconPx);
    if (pm.isNull()) {
        return {};
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!pm.save(&buffer, "PNG")) {
        return {};
    }
    return QStringLiteral("data:image/png;base64,") +
           QString::fromLatin1(png.toBase64());
}

}  // namespace

AppPage::AppPage(ProfileStore *store, QWidget *parent)
    : QFrame(parent), store_(store) {
    setObjectName("card");

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(18, 15, 16, 13);
    lay->setSpacing(9);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("应用启动"));
    title->setObjectName("cardTitle");
    auto *add = new QPushButton(QString::fromUtf8("添加应用"));
    add->setObjectName("ghostText");
    add->setCursor(Qt::PointingHandCursor);
    add->setToolTip(QString::fromUtf8(
        "加一个自定义应用：填名字和一个 exe/快捷方式/网址路径\n"
        "之后说「打开 名字」就能启动"));
    connect(add, &QPushButton::clicked, this, [this] { addCustomApp(); });
    auto *rescan = new QPushButton(QString::fromUtf8("重新扫描"));
    rescan->setObjectName("ghostText");
    rescan->setCursor(Qt::PointingHandCursor);
    rescan->setToolTip(QString::fromUtf8("装好或删掉应用后点这里，重新扫一遍开始菜单"));
    connect(rescan, &QPushButton::clicked, this, [this] {
        AppLauncher::refreshCache();
        rebuild();
    });
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(add);
    head->addWidget(rescan);
    lay->addLayout(head);

    auto *hint = new QLabel(QString::fromUtf8(
        "说「打开剪映」「打开 Qoder」「打开浏览器」就启动，不用再打字；"
        "开始菜单里的其它应用按名字说也能开（例如「打开 WorkBuddy AI」）。"));
    hint->setObjectName("muted");
    hint->setWordWrap(true);
    lay->addWidget(hint);

    auto *hair = new QFrame;
    hair->setObjectName("mapHair");
    hair->setFixedHeight(1);
    lay->addWidget(hair);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *holder = new QWidget;
    holder->setObjectName("appHolder");
    rows_ = new MasonryLayout(holder);
    rows_->setContentsMargins(0, 0, 6, 0);
    rows_->setSpacing(10);
    scroll->setWidget(holder);
    lay->addWidget(scroll, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(8);
    summary_ = new QLabel;
    summary_->setObjectName("muted");
    summary_->setWordWrap(true);
    const QString tip = QStringLiteral(
        "开着：本地识别出的「打开 xx」直接启动应用，不再贴进输入框\n"
        "想把这句话原样打给 Agent，就关掉它");
    auto *voiceLabel = new QLabel(QStringLiteral("语音启动"));
    voiceLabel->setObjectName("muted");
    voiceLabel->setToolTip(tip);
    voiceSwitch_ = new ToggleSwitch(store_->uiBool("voice_launch", true));
    voiceSwitch_->setToolTip(tip);
    connect(voiceSwitch_, &ToggleSwitch::toggled, this, [this](bool on) {
        store_->setUiBool(QStringLiteral("voice_launch"), on);
    });
    foot->addWidget(summary_, 1);
    foot->addSpacing(6);
    foot->addWidget(voiceLabel);
    foot->addWidget(voiceSwitch_, 0, Qt::AlignVCenter);
    const QString systemTip = QStringLiteral(
        "开着：说「音量调到 50」「亮度暗一点」「下一首」「关屏幕」直接执行，不再贴进输入框\n"
        "关机/重启/睡眠/休眠要再说一句「确认关机」这样的口令才动手\n"
        "分类开关在语音设置里；想把这句话原样打给 Agent，就关掉它");
    auto *systemLabel = new QLabel(QStringLiteral("语音系统控制"));
    systemLabel->setObjectName("muted");
    systemLabel->setToolTip(systemTip);
    systemSwitch_ = new ToggleSwitch(
        store_->uiBool(SystemControlSettings::masterKey(), true));
    systemSwitch_->setToolTip(systemTip);
    connect(systemSwitch_, &ToggleSwitch::toggled, this, [this](bool on) {
        store_->setUiBool(SystemControlSettings::masterKey(), on);
    });
    foot->addSpacing(10);
    foot->addWidget(systemLabel);
    foot->addWidget(systemSwitch_, 0, Qt::AlignVCenter);
    lay->addLayout(foot);

    // 通用图标（网址 / Windows 应用 / 没装）是按主题色画的，换主题或切深浅
    // 色时颜色要跟着重画；真实文件图标不受影响，重画也无副作用。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &) {
                iconUrls_.clear();
                rebuild();
            });

    rebuild();
    // 首次启动不要卡在建窗：AppsFolder 枚举 + 真实图标解析推迟到事件循环
    // 空转时再做一次（卡片先出通用占位，随后进真实图标）。
    QTimer::singleShot(0, this, [this] { rebuild(); });
}

void AppPage::rebuild() {
    while (QLayoutItem *item = rows_->takeAt(0)) {
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    iconUrls_.clear();

    int installed = 0;
    const QVector<AppEntry> &catalog = AppLauncher::appPageCatalog();
    for (const AppEntry &entry : catalog) {
        const AppTarget target = AppLauncher::resolve(entry);
        if (target.found) {
            ++installed;
        }

        const bool custom = entry.id.startsWith(QStringLiteral("custom:"));
        // 图标解析优先级：随包资源 → 已解析缓存 → Shell 系统图标
        // → QFileIconProvider（Qt 稳定路径）→ 通用占位。
        // Shell 解析只进缓存、不进 appIconFor：首帧失败时缓存记 null，
        // QFileIconProvider 仍能在 rebuild 里兜住（壁纸引擎那类 .lnk）。
        QIcon icon;
        const QString bundled = bundledIconPath(entry);
        if (!bundled.isEmpty()) {
            icon = QIcon(bundled);
        }
        if (icon.isNull()) {
            auto cached = iconCache_.constFind(entry.id);
            if (cached != iconCache_.constEnd()) {
                icon = cached.value();
            } else {
                icon = appIconFor(entry, target);  // 资源 + aumid
                if (icon.isNull()) {
                    // ShellIcon 失败（本线程 COM 态问题）也要记，避免每次重试。
                    const QIcon shell = shellIconFor(entry, target);
                    if (!shell.isNull()) {
                        icon = shell;
                    } else {
                        const QString path = targetPathForIcon(entry, target);
                        if (!path.isEmpty() && QFileInfo::exists(path)) {
                            QFileIconProvider provider;
                            const QIcon qtIcon = provider.icon(QFileInfo(path));
                            if (!qtIcon.isNull()) {
                                icon = qtIcon;
                            }
                        }
                    }
                }
                iconCache_.insert(entry.id, icon);
            }
        }
        if (icon.isNull()) {
            icon = fallbackAppIcon(entry, target);
        }
        iconUrls_.insert(entry.id, iconDataUrl(icon));

        auto *card = new AppCard(
            [this, entry] {
                QString error;
                const bool ok = AppLauncher::launch(entry.id, &error);
                emit launched(entry.name, ok, error);
            },
            target.found);

        auto *v = new QVBoxLayout(card);
        v->setContentsMargins(8, 9, 8, 9);
        v->setSpacing(5);

        // 图标行：图标居中；自定义应用在右上角放一个删除键。
        auto *top = new QHBoxLayout;
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(0);
        top->addStretch(1);
        auto *iconLabel = new QLabel;
        iconLabel->setObjectName("appIcon");
        iconLabel->setFixedSize(kIconPx, kIconPx);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setPixmap(icon.pixmap(kIconPx, kIconPx));
        top->addWidget(iconLabel, 0, Qt::AlignCenter);
        top->addStretch(1);
        if (custom) {
            auto *del = new QPushButton(QString::fromUtf8("×"));
            del->setObjectName("appDel");
            del->setCursor(Qt::PointingHandCursor);
            del->setToolTip(QString::fromUtf8("从自定义应用里移除（不影响应用本身）"));
            const QString name = entry.name;
            const QString path = entry.customPath;
            connect(del, &QPushButton::clicked, this,
                    [this, name, path] { removeCustomApp(name, path); });
            top->addWidget(del, 0, Qt::AlignRight | Qt::AlignTop);
        }
        v->addLayout(top);

        auto *name = new QLabel(entry.name);
        name->setObjectName("appName");
        name->setAlignment(Qt::AlignCenter);
        name->setWordWrap(true);
        name->setTextInteractionFlags(Qt::NoTextInteraction);
        v->addWidget(name);

        auto *state = new QLabel(appStateShort(entry, target));
        state->setObjectName(target.found ? "appState" : "appStateMiss");
        state->setAlignment(Qt::AlignCenter);
        state->setWordWrap(true);
        state->setTextInteractionFlags(Qt::NoTextInteraction);
        state->setToolTip(appStateTip(entry, target));
        v->addWidget(state);

        card->setToolTip(QString::fromUtf8("现在打开一次（语音说「打开 %1」同样有效）")
                             .arg(entry.name));

        rows_->addWidget(card);
    }

    summary_->setText(appSummaryText(installed, catalog.size()));
}

void AppPage::persistCustomApps(const QJsonArray &apps) {
    store_->setUiArray(QStringLiteral("custom_apps"), apps);
    AppLauncher::setCustomApps(AppLauncher::parseCustomApps(apps));
    rebuild();
}

void AppPage::addCustomApp() {
    // 语音设置同款深色小对话框。
    QDialog dialog(this);
    dialog.setObjectName("voiceDialog");
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setFixedWidth(460);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("添加自定义应用"));
    title->setObjectName("captureTitle");
    head->addWidget(title);
    head->addStretch(1);
    root->addLayout(head);

    const auto addRow = [root](const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("muted");
        root->addWidget(label);
        auto *edit = new QLineEdit;
        root->addWidget(edit);
        return edit;
    };
    QLineEdit *nameEdit = addRow(QString::fromUtf8("名称（说话用）"));
    nameEdit->setPlaceholderText(QString::fromUtf8("例如：OpenSquilla"));
    QLineEdit *pathEdit = addRow(QString::fromUtf8("路径（exe / .lnk / 网址）"));
    pathEdit->setPlaceholderText(QString::fromUtf8("C:\\Program Files\\...\\app.exe"));
    QLineEdit *aliasEdit = addRow(QString::fromUtf8("别名（可选，空格分隔）"));
    aliasEdit->setPlaceholderText(QString::fromUtf8("例如：squilla 斯齐拉"));

    auto *browse = new QPushButton(QString::fromUtf8("浏览…"));
    browse->setObjectName("ghostSmall");
    browse->setCursor(Qt::PointingHandCursor);
    connect(browse, &QPushButton::clicked, this, [this, pathEdit] {
        const QString file = QFileDialog::getOpenFileName(
            this, QString::fromUtf8("选择应用"),
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
            QString::fromUtf8("程序/快捷方式 (*.exe *.lnk *.url *.bat *.cmd);;所有文件 (*.*)"));
        if (!file.isEmpty()) {
            pathEdit->setText(file);
        }
    });
    root->addWidget(browse);

    auto *note = new QLabel(QString::fromUtf8(
        "保存后说「打开 名称」即可启动；名字也会出现在按键映射的"
        "「打开 xx」动作里。"));
    note->setObjectName("dialogNote");
    note->setWordWrap(true);
    root->addWidget(note);

    auto *foot = new QHBoxLayout;
    auto *cancel = new QPushButton(QString::fromUtf8("取消"));
    cancel->setObjectName("ghostSmall");
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto *save = new QPushButton(QString::fromUtf8("保存"));
    save->setObjectName("primary");
    save->setCursor(Qt::PointingHandCursor);
    save->setFixedWidth(86);
    foot->addStretch(1);
    foot->addWidget(cancel);
    foot->addWidget(save);
    root->addLayout(foot);

    connect(save, &QPushButton::clicked, &dialog, [&] {
        const QString name = nameEdit->text().trimmed();
        const QString path = pathEdit->text().trimmed();
        if (name.isEmpty() || path.isEmpty()) {
            note->setStyleSheet(QStringLiteral("color:%1;").arg(ThemeManager::instance().css(QStringLiteral("danger"))));
            note->setText(QString::fromUtf8("名称和路径都要填。"));
            return;
        }
        QJsonArray apps = store_->uiArray(QStringLiteral("custom_apps"));
        for (const QJsonValue &value : apps) {
            if (value.toObject().value("name").toString().compare(
                    name, Qt::CaseInsensitive) == 0) {
                note->setStyleSheet(QStringLiteral("color:%1;").arg(ThemeManager::instance().css(QStringLiteral("danger"))));
                note->setText(QString::fromUtf8("已经有叫这个名字的自定义应用了。"));
                return;
            }
        }
        QJsonObject entry{{"name", name}, {"path", path}};
        QJsonArray aliases;
        for (const QString &alias : aliasEdit->text().split(
                 QRegularExpression(QStringLiteral("[\\s,，]+")),
                 Qt::SkipEmptyParts)) {
            aliases.append(alias.trimmed());
        }
        if (!aliases.isEmpty()) {
            entry["aliases"] = aliases;
        }
        apps.append(entry);
        persistCustomApps(apps);
        dialog.accept();
    });
    dialog.exec();
}

void AppPage::removeCustomApp(const QString &name, const QString &path) {
    // 不用 QMessageBox：它的按钮没有样式规则，深色弹窗里原生按钮几乎看不清，
    // 也没法落「确认」文案。这里用添加应用同款的深色对话框。
    QDialog dialog(this);
    dialog.setObjectName("voiceDialog");
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setModal(true);
    dialog.setFixedWidth(360);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);

    auto *title = new QLabel(QString::fromUtf8("移除自定义应用"));
    title->setObjectName("captureTitle");
    root->addWidget(title);

    auto *text = new QLabel(QString::fromUtf8(
        "不再通过语音/按键打开「%1」？应用本身不会被卸载。").arg(name));
    text->setWordWrap(true);
    root->addWidget(text);

    auto *foot = new QHBoxLayout;
    auto *cancel = new QPushButton(QString::fromUtf8("取消"));
    cancel->setObjectName("ghostSmall");
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto *confirm = new QPushButton(QString::fromUtf8("确认移除"));
    confirm->setObjectName("primary");
    confirm->setCursor(Qt::PointingHandCursor);
    confirm->setFixedWidth(86);
    connect(confirm, &QPushButton::clicked, &dialog, &QDialog::accept);
    foot->addStretch(1);
    foot->addWidget(cancel);
    foot->addWidget(confirm);
    root->addLayout(foot);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QJsonArray apps;
    for (const QJsonValue &value : store_->uiArray(QStringLiteral("custom_apps"))) {
        const QJsonObject obj = value.toObject();
        if (obj.value("name").toString() == name &&
            obj.value("path").toString() == path) {
            continue;
        }
        apps.append(value);
    }
    persistCustomApps(apps);
}

QJsonObject AppPage::webState() {
    int installed = 0;
    QJsonArray rows;
    const QVector<AppEntry> &catalog = AppLauncher::appPageCatalog();
    for (const AppEntry &entry : catalog) {
        const AppTarget target = AppLauncher::resolve(entry);
        if (target.found) {
            ++installed;
        }
        rows.append(QJsonObject{
            {QStringLiteral("id"), entry.id},
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("icon"), iconUrls_.value(entry.id)},
            {QStringLiteral("state"), appStateShort(entry, target)},
            {QStringLiteral("stateTip"), appStateTip(entry, target)},
            {QStringLiteral("found"), target.found},
            {QStringLiteral("custom"),
             entry.id.startsWith(QStringLiteral("custom:"))},
            {QStringLiteral("customPath"), entry.customPath},
        });
    }
    return QJsonObject{
        {QStringLiteral("rows"), rows},
        {QStringLiteral("summary"), appSummaryText(installed, catalog.size())},
        {QStringLiteral("voiceLaunch"),
         store_->uiBool(QStringLiteral("voice_launch"), true)},
        {QStringLiteral("systemControl"),
         store_->uiBool(SystemControlSettings::masterKey(), true)},
    };
}

void AppPage::webLaunch(const QString &id) {
    QString name = id;
    for (const AppEntry &entry : AppLauncher::catalog()) {
        if (entry.id == id) {
            name = entry.name;
            break;
        }
    }
    QString error;
    const bool ok = AppLauncher::launch(id, &error);
    emit launched(name, ok, error);
}

void AppPage::webAddCustom() {
    addCustomApp();
}

void AppPage::webRemoveCustom(const QString &name, const QString &path) {
    removeCustomApp(name, path);
}

void AppPage::webRescan() {
    AppLauncher::refreshCache();
    rebuild();
}

void AppPage::webSetVoiceLaunch(bool on) {
    store_->setUiBool(QStringLiteral("voice_launch"), on);
    if (voiceSwitch_ != nullptr) {
        voiceSwitch_->setChecked(on);
    }
}

void AppPage::webSetSystemControl(bool on) {
    store_->setUiBool(SystemControlSettings::masterKey(), on);
    if (systemSwitch_ != nullptr) {
        systemSwitch_->setChecked(on);
    }
}
