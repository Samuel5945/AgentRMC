#include "MainWindow.h"

#include "ui/AppPage.h"
#include "ui/AgentBridgeDialog.h"
#include "ui/CursorOverlay.h"
#include "ui/GuideDialog.h"
#include "ui/KeymapPage.h"
#include "ui/RemotePanel.h"
#include "ui/ThemeDialog.h"
#include "ui/Toast.h"
#include "ui/TtsSettingsDialog.h"
#include "ui/VoicePet.h"
#include "ui/VoiceSettingsDialog.h"
#include "ui/DispatchPicker.h"
#include "ui/VoiceWindow.h"
#include "ui/WorkWebView.h"
#include "ui/WaveformButton.h"

#include "core/AgentBridge.h"
#include "core/AgentCursor.h"
#include "core/DshSessions.h"
#include "core/LanWebServer.h"
#include "core/PluginUpgrade.h"
#include "core/AgentWindow.h"
#include "core/HostRuntime.h"
#include "core/AppLauncher.h"
#include "core/InputEngine.h"
#include "core/ModelImport.h"
#include "core/Presets.h"
#include "core/SessionDispatch.h"
#include "core/SessionCleaner.h"
#include "core/SessionTranscript.h"
#include "core/ZcodeDispatch.h"
#include "core/ThemeManager.h"
#include "core/Updater.h"
#include "core/VoiceController.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QIcon>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpSocket>
#include <QEventLoop>
#include <QCheckBox>
#include <QDir>
#include <functional>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeData>
#include <QSettings>
#include <QDateTime>
#include <QDialog>
#include <QPointer>
#include <QPair>
#include <QThreadPool>
#include <QCursor>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QProcess>
#include <QJsonArray>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QSignalBlocker>
#include <QTime>
#include <QTimer>
#include <QThread>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM（WM_NCHITTEST 取坐标）
#include <QVector>
#include <QWindow>

#include <algorithm>

namespace {

// 「使用引导」/「接入 Agent」里的一句话安装话术：插件源和 zip 随安装包落在 exe 旁的
// plugins\ 下，用户不用认路径——复制这一句发给正在干活的 Agent，它自己装好并启用。
// 让 Agent 走宿主自己的安装器（install_extension）而不是手改注册表：本地包的版本号
// 只在文件名和清单里，装错了没人看得出来。
// 文案本身在 pluginupgrade::installSentence()，那儿有测试盯着占位符替换。
QString agentBridgeInstallSentence() {
    return pluginupgrade::installSentence(QCoreApplication::applicationDirPath());
}

// 调色板 token → CSS 颜色串。注意不能直接把 palette 里的字符串丢给网页：
// QSS 的 rgba(r,g,b,26) 里 alpha 是 0-255，CSS 要 0-1，照抄过去全是全透明。
QString cssColor(const QColor &color) {
    if (!color.isValid()) {
        return QString();
    }
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);
    }
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alphaF(), 0, 'f', 3);
}

// 语义 token（ok/pending/info/danger/warn）→ 当前主题的颜色串。
// 状态点、胶囊、事件流一律走这里，别再写死深色盘的值（浅色模式会发虚）。
QString themeToken(const char *token) {
    return ThemeManager::instance().css(QString::fromLatin1(token));
}

// 引导页数据：定义在 buildGuidePage 前面（Qt 页和外壳共用），这里先声明，
// 让前面的 webShellState 也能序列化。
struct GuideSection {
    QString title;
    QVector<QPair<QString, QString>> rows;
};
QString guideIntro();
QVector<GuideSection> guideSections();

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

    // 双击空白处最大化 / 还原：桌面应用的肌肉记忆。按下子控件的
    // （按钮/胶囊）各自吃掉事件，落到这里的都是标题栏空白。
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            auto *win = window();
            if (win->isFullScreen() || win->isMaximized()) {
                win->showNormal();
            } else {
                win->showMaximized();
            }
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }
};

// 全局语音热键（无遥控器触发用）的注册 ID。
constexpr UINT kVoiceHotkeyId = 0xB00B;

// 主界面底色：主题带背景图就铺满整窗再压一层暗色遮罩。
// 以前只有语音面板做了遮罩，主窗直接把 11px 灰字压在花纹上，正文基本读不了。
// 这里给一个下限——主窗是信息密集界面，主题作者写的 dim 只当「想更暗」的加成。
class BackdropRoot final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override {
        const Theme &theme = ThemeManager::instance().current();
        if (theme.mainBackground.isEmpty()) {
            return;  // classic：QSS 的纯色底就够
        }
        const QImage art = ThemeManager::cachedImage(theme.mainBackground);
        if (art.isNull()) {
            return;
        }
        // 4K 源图每次 paint 都 Smooth 缩放太贵（全屏下 3840×2400 → 窗口尺寸
        // 一帧几十毫秒）：按 (路径, 窗口尺寸) 缓存缩放结果。换主题换路径、
        // 拉窗口换尺寸才重算一次，平时 repaint 直接贴缓存。同名主题更新时
        // 路径不变，所以还要看图片缓存代次（否则一直贴旧背景）。
        const quint64 generation = ThemeManager::imageGeneration();
        if (scaledPath_ != theme.mainBackground || scaledSize_ != size() ||
            scaledGeneration_ != generation) {
            scaled_ = art.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                 Qt::SmoothTransformation);
            scaledPath_ = theme.mainBackground;
            scaledSize_ = size();
            scaledGeneration_ = generation;
        }
        QPainter painter(this);
        painter.drawImage(QPoint((width() - scaled_.width()) / 2,
                                 (height() - scaled_.height()) / 2),
                          scaled_);
        const qreal dim = ThemeManager::instance().backdropDim();
        // 深色模式压暗、浅色模式蒙白纱：同一 dim 值两种语义，跟背景走。
        const QColor veil = ThemeManager::instance().isLightActive()
                                ? QColor(255, 255, 255, int(255 * dim))
                                : QColor(11, 14, 20, int(255 * dim));
        painter.fillRect(rect(), veil);
    }

private:
    QImage scaled_;
    QString scaledPath_;
    QSize scaledSize_;
    quint64 scaledGeneration_ = 0;
};

// 待办行：状态色条 + 一行「Agent · 阶段 —— 时间」+ 一行消息。
// 原来整行一个颜色、状态用方括号塞在正文里，扫十行找不出哪条在等自己。
// 行高与纵向节奏跟着字号倍率走，不然字放大了行还是 56px，两行文字互相叠。
class WorkItemDelegate final : public QStyledItemDelegate {
public:
    explicit WorkItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(0, ThemeManager::scaledPx(56));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setClipRect(option.rect);

        const QRectF row = QRectF(option.rect).adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            // 选中色也走主题 token（a150 描边 / a36 底色），换强调色的主题能跟上。
            painter->setPen(selected ? QPen(ThemeManager::instance().color(
                                          QStringLiteral("a150")),
                                      1.0)
                                     : QPen(Qt::NoPen));
            painter->setBrush(selected ? ThemeManager::instance().color(
                                             QStringLiteral("a36"))
                                       : ThemeManager::instance().color(
                                             QStringLiteral("w10")));
            painter->drawRoundedRect(row, 10, 10);
        }

        const QColor stage = index.data(Qt::UserRole + 2).value<QColor>();
        painter->setPen(Qt::NoPen);
        painter->setBrush(stage);
        painter->drawRoundedRect(
            QRectF(row.left() + 9,
                   row.top() + ThemeManager::scaledPx(12), 3,
                   row.height() - ThemeManager::scaledPx(24)),
            1.5, 1.5);

        const qreal textLeft = row.left() + 21;
        const qreal textRight = row.right() - 12;

        const QString agent = index.data(Qt::UserRole + 4).toString();
        const QString stageText = index.data(Qt::UserRole + 3).toString();
        const QString when = index.data(Qt::UserRole + 5).toString();
        const QString where = index.data(Qt::UserRole + 6).toString();
        const QString message = index.data(Qt::DisplayRole).toString();

        QFont nameFont = painter->font();
        nameFont.setPixelSize(ThemeManager::scaledPx(12));
        nameFont.setWeight(QFont::DemiBold);
        QFont metaFont = painter->font();
        metaFont.setPixelSize(ThemeManager::scaledPx(10));
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics metaMetrics(metaFont);

        // 时间列固定宽、右贴边：「12:18」和「12:18 · 等 5 小时」按各自宽度让位
        // 会把一列数字画成锯齿（用户 2026-11-06 指），所以先钉死列宽再让正文缩。
        const qreal timeCol = metaMetrics.horizontalAdvance(
                                  QStringLiteral("00:00 · 等 00 天")) + 10;
        const QRectF timeRect(textRight - timeCol,
                              row.top() + ThemeManager::scaledPx(9), timeCol,
                              ThemeManager::scaledPx(16));
        const QRectF line1(textLeft, row.top() + ThemeManager::scaledPx(9),
                           textRight - textLeft - timeCol - 8,
                           ThemeManager::scaledPx(16));

        painter->setFont(metaFont);
        painter->setPen(ThemeManager::instance().color(QStringLiteral("ghost")));
        painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, when);

        const qreal stageWidth = metaMetrics.horizontalAdvance(stageText);
        // 同一宿主并行开两个会话时，两行都只写「Qoder」分不出谁是谁：把项目
        // （或「未关联会话」）钉在状态后面，宽度不够时先让名字让步。
        const qreal whereRoom =
            where.isEmpty()
                ? 0.0
                : qMin<qreal>(metaMetrics.horizontalAdvance(where) + 12,
                              line1.width() * 0.45);
        const qreal nameRoom =
            qMax<qreal>(40, line1.width() - stageWidth - whereRoom - 26);
        painter->setFont(nameFont);
        painter->setPen(ThemeManager::instance().color(QStringLiteral("text")));
        const QString shownAgent =
            nameMetrics.elidedText(agent, Qt::ElideRight, int(nameRoom));
        painter->drawText(line1, Qt::AlignLeft | Qt::AlignVCenter, shownAgent);
        const qreal agentWidth = nameMetrics.horizontalAdvance(shownAgent);

        painter->setFont(metaFont);
        painter->setPen(stage);
        painter->drawText(QRectF(line1.left() + agentWidth + 7, line1.top(),
                                 stageWidth + 4, line1.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, stageText);

        if (!where.isEmpty()) {
            const qreal whereLeft =
                line1.left() + agentWidth + 7 + stageWidth + 12;
            const QRectF whereRect(whereLeft, line1.top(),
                                   line1.right() - whereLeft, line1.height());
            painter->setFont(metaFont);
            painter->setPen(
                ThemeManager::instance().color(QStringLiteral("muted")));
            // 项目名用中间省略：两个目录常是前后缀相近的兄弟名，砍尾巴就分不出了。
            painter->drawText(
                whereRect, Qt::AlignLeft | Qt::AlignVCenter,
                metaMetrics.elidedText(where, Qt::ElideMiddle,
                                       int(whereRect.width())));
        }

        QFont bodyFont = painter->font();
        bodyFont.setPixelSize(ThemeManager::scaledPx(11));
        painter->setFont(bodyFont);
        // 正文吃主字色（用户 2026-11-05 选 B）：浅底上灰字边缘最先被读成
        // 「发虚」，层级改由粗体承担——标题是 700，正文是常规字重。
        painter->setPen(ThemeManager::instance().color(QStringLiteral("text")));
        const QRectF line2(textLeft, row.top() + ThemeManager::scaledPx(29),
                           textRight - textLeft, ThemeManager::scaledPx(16));
        painter->drawText(
            line2, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(bodyFont).elidedText(message.simplified(),
                                              Qt::ElideRight,
                                              int(line2.width())));
        painter->restore();
    }
};

// 统计小胶囊：等你 / 待验收 / 进行中。颜色走语义 token（浅色模式自动压深），
// 字号随全局倍率走：主题广播时按当前倍率重刷，拖字体滑块立即跟上。
class ChipLabel final : public QLabel {
public:
    explicit ChipLabel(const char *token, QWidget *parent = nullptr)
        : QLabel(parent), token_(token) {
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
                [this](const Theme &) { applyStyle(); });
        applyStyle();
        hide();
    }

private:
    void applyStyle() {
        const QColor color =
            ThemeManager::instance().color(QString::fromLatin1(token_));
        QColor border = color;
        border.setAlpha(95);
        QColor fill = color;
        fill.setAlpha(26);
        setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: %2;"
                           " border: 1px solid %3; border-radius: 9px;"
                           " padding: 1px 8px; font-size: %4px;"
                           " font-weight: 600; }")
                .arg(themeToken(token_), cssColor(fill), cssColor(border),
                     QString::number(ThemeManager::scaledPx(10.5))));
    }

    const char *token_;
};

QLabel *makeChip(const QString &label, const char *token) {
    auto *chip = new ChipLabel(token);
    chip->setText(label);
    return chip;
}

// 重启对账的预热期：插件应答件轮询每 1.2s 一拍，等它连上满这么久，才算「账已经
// 建好了」——启动瞬间就把遗留卡判死，会把插件还没来得及重推的活请求一起清掉。
constexpr qint64 kPluginWarmupMs = 5000;

// ---- 最小 zip（store 模式，不压缩）----
// 用途单一：Chrome 在 HTTP 明文下会把 .exe/.msi/.apk 拦成「无法安全地下载
// 文件」（用户 2026-11-18 的截图就是它）。包一层 zip，浏览器只按外层扩展名
// 判断，解压后拿到的还是原文件。
quint32 crc32Of(const QByteArray &data) {
    static quint32 table[256];
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    const auto *p = reinterpret_cast<const uchar *>(data.constData());
    for (int i = 0; i < data.size(); ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void put16(QByteArray &b, quint16 v) {
    b.append(static_cast<char>(v & 0xFF));
    b.append(static_cast<char>((v >> 8) & 0xFF));
}
void put32(QByteArray &b, quint32 v) {
    put16(b, static_cast<quint16>(v & 0xFFFF));
    put16(b, static_cast<quint16>((v >> 16) & 0xFFFF));
}

QByteArray zipSingleStore(const QString &name, const QByteArray &data) {
    const QByteArray fn = name.toUtf8();
    const quint32 crc = crc32Of(data);
    const quint32 size = static_cast<quint32>(data.size());
    const quint16 dosTime = 0;
    const quint16 dosDate = 0x21;  // 1980-01-01，zip 的最小合法日期
    QByteArray out;
    out.reserve(data.size() + fn.size() * 2 + 128);
    put32(out, 0x04034b50);          // local file header
    put16(out, 20);
    put16(out, 0x0800);              // 文件名按 UTF-8
    put16(out, 0);                   // store（不压缩）
    put16(out, dosTime);
    put16(out, dosDate);
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, static_cast<quint16>(fn.size()));
    put16(out, 0);
    out.append(fn);
    out.append(data);
    // 中央目录紧跟数据段之后（不是 local header 之后——这里踩过一次，
    // 写完用 Python zipfile 解了一遍才验出来）。
    const quint32 cdOffset = static_cast<quint32>(out.size());
    put32(out, 0x02014b50);          // central directory
    put16(out, 20);
    put16(out, 20);
    put16(out, 0x0800);
    put16(out, 0);
    put16(out, dosTime);
    put16(out, dosDate);
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, static_cast<quint16>(fn.size()));
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    put32(out, 0);  // local header 偏移：单文件就是 0（不是 cdOffset！）
    out.append(fn);
    const quint32 cdSize = static_cast<quint32>(out.size()) - cdOffset;
    put32(out, 0x06054b50);          // EOCD
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);
    put16(out, 1);
    put32(out, cdSize);
    put32(out, cdOffset);
    put16(out, 0);
    return out;
}

// 小文本文件直接把内容带回去（≤16KB）：正文内联后 agent 一定能看到，不会
// "没读就编"。二进制/大文件返回空，仍走路径 + Read 指令那条路。
QString inlineTextOf(const QString &path, qint64 size) {
    constexpr qint64 kLimit = 16 * 1024;
    if (size <= 0 || size > kLimit) {
        return QString();
    }
    static const QSet<QString> kTextExt = {
        QStringLiteral("txt"),  QStringLiteral("md"),   QStringLiteral("json"),
        QStringLiteral("log"),  QStringLiteral("csv"),  QStringLiteral("yml"),
        QStringLiteral("yaml"), QStringLiteral("ini"),  QStringLiteral("toml"),
        QStringLiteral("xml"),  QStringLiteral("html"), QStringLiteral("css"),
        QStringLiteral("js"),   QStringLiteral("ts"),   QStringLiteral("py"),
        QStringLiteral("cpp"),  QStringLiteral("h"),    QStringLiteral("c"),
        QStringLiteral("java"), QStringLiteral("go"),   QStringLiteral("rs"),
        QStringLiteral("sh"),   QStringLiteral("bat"),  QStringLiteral("ps1")};
    if (!kTextExt.contains(QFileInfo(path).suffix().toLower())) {
        return QString();
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray raw = f.read(kLimit);
    if (raw.contains('\0')) {  // 二进制改后缀的，别当文本
        return QString();
    }
    return QString::fromUtf8(raw);
}

// Chrome/Edge 会拦的扩展名：这些打完包再给。
bool riskyDownloadExt(const QString &suffix) {
    static const QSet<QString> kRisky = {
        QStringLiteral("exe"), QStringLiteral("msi"), QStringLiteral("apk"),
        QStringLiteral("bat"), QStringLiteral("cmd"), QStringLiteral("ps1"),
        QStringLiteral("vbs"), QStringLiteral("jar")};
    return kRisky.contains(suffix.toLower());
}

// 宿主自带的本地网关端口。OpenSquilla 的网关是它自己的子服务（:18791），
// 进程起来 ≠ 能接活：冷启动实测要 22 秒才监听，桥取件时若 gatewayUp 还是
// false 就回「网关没连上，未注入」。派发前必须等到端口真的可连。
quint16 gatewayPortFor(const QString &exe) {
    if (exe.compare(QStringLiteral("OpenSquilla.exe"), Qt::CaseInsensitive) == 0) {
        return 18791;
    }
    return 0;
}

// 轮询等端口可连。用 processEvents 保界面响应，别让主窗假死几十秒。
bool waitPortOpen(quint16 port, int timeoutMs) {
    if (port == 0) {
        return true;
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, port);
        const bool ok = probe.waitForConnected(400);
        probe.abort();
        if (ok) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QThread::msleep(350);
    }
    return false;
}

}  // namespace

MainWindow::MainWindow(bool withTray, const QString &pin, bool autoListen,
                       QWidget *parent)
    : QMainWindow(parent) {
    store_ = std::make_unique<ProfileStore>(
        QCoreApplication::applicationDirPath() + "/profiles.json");
    store_->load();
    store_->loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
    // Work Item 落在用户数据目录，不跟安装目录走：升级安装不会覆盖，
    // 也不会被 installer 打进包里。
    QString workDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (workDir.isEmpty()) {
        workDir = QCoreApplication::applicationDirPath();
    }
    QDir().mkpath(workDir);
    workItems_ = std::make_unique<WorkItemStore>(workDir + "/work-items.json");
    workItems_->load();
    // 记下「上一个进程留下的注意力卡」：请求账只在内存，重启后这些卡已经无从
    // 应答（点了只会得到「已不在待答清单」）。等插件重推整批时按它归档，别把
    // 死卡一直挂在「等你处理」里。必须在 load() 之后、任何新工单产生之前取。
    for (const WorkItem &legacy : workItems_->items()) {
        if (!legacy.done && (legacy.stage == QStringLiteral("needs-me"))) {
            legacyAttentionIds_.insert(legacy.id);
        }
    }
    dispatch_ = new SessionDispatch(this);
    connect(dispatch_, &SessionDispatch::receipt, this,
            [this](const QString &dispatchId, const QString &exe,
                   const QString &sid, const QString &proj, const QString &text) {
                if (workItems_ != nullptr) {
                    // 回执落账＝这件事真的送到那条会话了：工单转进行中。
                    workItems_->upsert(QStringLiteral("dispatch"), exe, sid, proj,
                                       QStringLiteral("progress"),
                                       AgentWindow::displayName(exe), text, exe);
                }
                // 回执落账＝送达成功。聊天页已能直接看记录，不再弹「回执已落账」
                // 通知（用户 2026-11-14 反馈冗余）；ZCode 桌面端不热刷新，保留重启提示。
                if (ZcodeDispatch::supports(exe)) {
                    dispatchNotice_ = QStringLiteral(
                        "ZCode 对话区要重启后才显示新消息，回复先看卡片");
                    dispatchNoticeKind_ = QStringLiteral("good");
                } else {
                    dispatchNotice_.clear();
                    dispatchNoticeKind_.clear();
                }
                dispatchNoticeId_ = itemIdForSession(sid);
                dispatchNoticeExe_ = exe;
                appendEvent(QStringLiteral("status"), QString(), QString(),
                            QStringLiteral("派发回执：%1… 会话 %2")
                                .arg(dispatchId.left(8), sid.left(8)), 0);
                refreshWorkItems();
            });
    connect(dispatch_, &SessionDispatch::failed, this,
            [this](const QString &dispatchId, const QString &reason) {
                dispatchNotice_ = reason;
                dispatchNoticeKind_ = QStringLiteral("bad");
                dispatchNoticeId_ =
                    itemIdForSession(dispatchSessionById_.value(dispatchId));
                // 失败通知也带宿主：ZCode 启动类失败可在卡片上直接重启重试。
                if (workItems_ != nullptr) {
                    for (const WorkItem &it : workItems_->items()) {
                        if (it.sessionId ==
                            dispatchSessionById_.value(dispatchId)) {
                            dispatchNoticeExe_ = it.agentExe;
                            break;
                        }
                    }
                }
                appendEvent(QStringLiteral("error"), QString(), QString(),
                            QStringLiteral("派发失败：%1… %2")
                                .arg(dispatchId.left(8), reason), 0);
                refreshWorkItems();
            });
    connect(dispatch_, &SessionDispatch::zcodeTurnFinished, this,
            [this](const QString &dispatchId, const QString &exe,
                   const QString &sid, const QString &proj,
                   const QString &reply) {
                // 回执只代表送达；这里补 turn 跑完的结果。回复写进卡（空＝
                // 超时/turn 失败，卡维持进行中，等 hook 或人工结账）。
                dispatchReplyBySession_.insert(sid, reply);
                if (reply.isEmpty() || workItems_ == nullptr) {
                    return;
                }
                appendEvent(QStringLiteral("status"), QString(), QString(),
                            QStringLiteral("派发回复：%1").arg(reply), 0);
                // hook 可能已把这张卡推进到「待验收/等你处理」：晚到的回复
                // 只能补正文，不能把注意力档位降回进行中——那会让待验收卡
                // 从列表里消失，人以为活还没干完（2026-11-08 审查实测）。
                const WorkItem *live = nullptr;
                for (const WorkItem &it : workItems_->items()) {
                    if (!it.done && it.sessionId == sid) {
                        live = &it;
                        break;
                    }
                }
                if (live == nullptr) {
                    return;  // 人已经结账：别再翻出来
                }
                if (live->stage == QStringLiteral("working")) {
                    workItems_->upsert(QStringLiteral("dispatch"), exe, sid, proj,
                                       QStringLiteral("progress"),
                                       AgentWindow::displayName(exe), reply, exe);
                } else {
                    workItems_->updateLiveMessage(sid, reply);
                }
                refreshWorkItems();
            });
    connect(dispatch_, &SessionDispatch::zcodeInteractionSkipped, this,
            [this](const QString &dispatchId, const QString &exe,
                   const QString &sid, const QString &proj, const QString &what) {
                // 外部通道没有代答 UI：授权/提问已按协议拒绝。授权提醒本身
                // 由宿主的 hook 负责（spawn 的 app-server 也跑 hooks），这里
                // 只把「这一轮为什么可能没做成」写进事件流和派发提示。
                Q_UNUSED(proj);
                dispatchNotice_ =
                    QStringLiteral("派发轮遇到%1，外部通道已代为拒绝；要它继续请回 "
                                   "ZCode 桌面处理")
                        .arg(what);
                dispatchNoticeKind_ = QStringLiteral("bad");
                dispatchNoticeId_ = itemIdForSession(sid);
                dispatchNoticeExe_ = exe;
                appendEvent(QStringLiteral("status"), QString(), QString(),
                            QStringLiteral("派发交互：%1（%2…）")
                                .arg(dispatchNotice_, dispatchId.left(8)),
                            0);
                refreshWorkItems();
            });
    spoolPath_ = workDir + "/pending-events.jsonl";
    startedAt_ = QDateTime::currentDateTimeUtc();
    // 主题在窗口构建前应用：渲染 QSS + 载入 ui.theme 指向的主题。
    ThemeManager::instance().init(store_.get());
    // ui.custom_apps 里的自定义应用灌进启动目录（应用页/语音「打开 xx」共用）。
    AppLauncher::setCustomApps(
        AppLauncher::parseCustomApps(store_->uiArray("custom_apps")));
    if (!pin.isEmpty()) {
        store_->setPinned(pin);
    }
    engine_ = new InputEngine(store_.get(), this);
    voice_ = new VoiceController(store_.get(), this);
    system_ = std::make_unique<WinSystemControl>();
    voice_->setSystemControl(system_.get());
    connect(voice_, &VoiceController::spokenFeedback, this, [this](const QString &text) {
        if (voicePet_ != nullptr) {
            voicePet_->speak(text);
        }
    });
    connect(engine_, &InputEngine::eventReady, this, &MainWindow::onEngineEvent);
    connect(engine_, &InputEngine::runningChanged, this,
            &MainWindow::onEngineRunning);
    connect(engine_, &InputEngine::runningChanged, this,
            [this](bool running, const QString &) {
                voice_->setListening(running);
            });
    connect(engine_, &InputEngine::profileChanged, this,
            [this](const QString &name) {
                sceneName_ = name;
                updateTrayState();
                refreshRemoteCard();
                if (!voice_->busy()) {
                    refreshVoiceUi();
                }
                if (remoteState_ != nullptr && engine_->isRunning()) {
                    remoteState_->setText(
                        QString::fromUtf8("监听中 · ") +
                        voiceProfileLabel(store_->current(true).name));
                }
            });
    connect(engine_, &InputEngine::voiceDown, voice_, &VoiceController::voiceDown);
    connect(engine_, &InputEngine::voiceDown, this, [this] {
        if (remotePanel_ != nullptr) {
            remotePanel_->flash(116);
        }
        if (remoteState_ != nullptr && remotePanel_ != nullptr) {
            remoteState_->setText(QString::fromUtf8("语音 · ") +
                                  remotePanel_->actionFor(116));
        }
    });
    connect(engine_, &InputEngine::voiceUp, voice_, &VoiceController::voiceUp);
    connect(engine_, &InputEngine::mediaVoice, voice_,
            &VoiceController::mediaToggle);
    connect(engine_, &InputEngine::mediaVoice, this, [this] {
        if (remoteState_ != nullptr) {
            remoteState_->setText(QString::fromUtf8("耳机 · 语音开关"));
        }
    });
    connect(voice_, &VoiceController::stateChanged, this,
            &MainWindow::onVoiceState);
    voiceLevelTimer_ = new QTimer(this);
    connect(voiceLevelTimer_, &QTimer::timeout, this, [this] {
        const double level = voiceRecording_ ? voice_->micLevel() : 0.0;
        if (voiceKey_ != nullptr) {
            voiceKey_->setLevel(level);
        }
        if (workWeb_ != nullptr) {
            // 波形 20Hz 走轻量通道：整页 refresh 会重建 DOM，扛不住这个频率。
            workWeb_->pushLive(QJsonObject{
                {QStringLiteral("level"), level},
                {QStringLiteral("recording"), voiceRecording_}});
        }
    });
    voiceLevelTimer_->start(50);
    connect(voice_, &VoiceController::transcribed, this,
            &MainWindow::onVoiceTranscribed);
    connect(voice_, &VoiceController::eventReady, this,
            [this](const QString &kind, const QString &text) {
                appendEvent(kind, QString(), QString(), text, 0);
            });
    setWindowTitle("AgentRMC Native");
    setWindowIcon(QIcon(":/icons/icon.ico"));
    setAcceptDrops(true);
    dropHint_ = new QLabel(QString::fromUtf8("松手，导入这个模型包"), this);
    dropHint_->setAlignment(Qt::AlignCenter);
    dropHint_->setStyleSheet(QStringLiteral(
        "background: rgba(11,14,20,225); color: #e6edf3; font-size: %1px;"
        " border: 2px dashed rgba(79,140,255,190); border-radius: 12px;")
                                 .arg(ThemeManager::scaledPx(18)));
    // 字号倍率变了以后样式要跟着重算，不然拖放提示停在旧字号上。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, dropHint_,
            [this](const Theme &) {
                dropHint_->setStyleSheet(QStringLiteral(
                    "background: rgba(11,14,20,225); color: #e6edf3;"
                    " font-size: %1px; border: 2px dashed rgba(79,140,255,190);"
                    " border-radius: 12px;")
                                             .arg(ThemeManager::scaledPx(18)));
            });
    dropHint_->hide();
    qApp->installEventFilter(this);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    // 再窄左栏就挤不下了：语音卡 + 遥控器卡是固定高度，列表也需要宽度放时间列。
    // 2026-11-17 用户要求「整体界面宽高可拉伸缩小」：下限从 940×620 放到 860×540
    // （外壳形态下真正撑住的是 workWeb_ 的最小尺寸，两边必须同步改，否则外壳
    // 会把窗口顶回去）；再小就由页面里的 @media 收窄左栏/收掉品牌名与设备图。
    setMinimumSize(860, 540);
    // F11 全屏开关：完整界面铺满整块屏幕（含任务栏区），再看时再按一次回来。
    auto *fullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fullscreenShortcut, &QShortcut::activated, this, [this] {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
    });

    auto *root = new BackdropRoot;
    root->setObjectName("root");
    // 主题背景图走 QSS background-image；plain QWidget 显式声明按样式表绘制。
    root->setAttribute(Qt::WA_StyledBackground, true);
    setCentralWidget(root);
    // 换主题时 QSS 会重刷，但背景是我们自己画的，得手动重绘一次。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, root,
            [root](const Theme &) { root->update(); });
    // 状态点 / 语音状态 / 遥控器灯这些内联样式的字色是运行时写进去的，全局 QSS
    // 重刷带不上它们：切浅色/深色或换主题时再刷一遍。只改色，不碰热键注册那些副作用。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme &) {
                // 外壳模式下顶栏/左栏这些 Qt 控件全藏着，重刷它们一次要几百
                // 毫秒（setStyleSheet 会 repolish 整窗）；网页自己从 token 换色。
                if (workWeb_ != nullptr) {
                    return;
                }
                const bool running = engine_ != nullptr && engine_->isRunning();
                const QString line =
                    ThemeManager::instance().css(QStringLiteral("line"));
                const QString dot = running ? themeToken("ok") : line;
                if (statusDot_ != nullptr) {
                    statusDot_->setStyleSheet(
                        QStringLiteral("background: %1; border-radius: 3px;").arg(dot));
                }
                if (statusText_ != nullptr) {
                    statusText_->setStyleSheet(
                        running
                            ? QStringLiteral("color:%1; font-size:%2px;"
                                            " font-weight:600;")
                                  .arg(themeToken("ok"),
                                       QString::number(ThemeManager::scaledPx(11)))
                            : QStringLiteral("color:%1; font-size:%2px;")
                                  .arg(ThemeManager::instance().css(
                                           QStringLiteral("muted")),
                                       QString::number(
                                           ThemeManager::scaledPx(11))));
                }
                if (remoteDot_ != nullptr) {
                    remoteDot_->setStyleSheet(
                        QStringLiteral("background:%1; border-radius:3px;").arg(dot));
                }
                refreshVoiceUi();
                if (voiceKey_ != nullptr) {
                    voiceKey_->update();
                }
                if (workList_ != nullptr) {
                    // 行内阶段色存在模型里，换主题要重新灌一遍，不然停在旧色。
                    refreshWorkItems();
                    workList_->viewport()->update();
                }
            });

    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    headerBar_ = buildHeader();
    outer->addWidget(headerBar_);

    auto *body = new QWidget;
    leftBody_ = body;
    auto *grid = new QHBoxLayout(body);
    grid->setContentsMargins(14, 12, 14, 14);
    grid->setSpacing(12);

    auto *left = new QWidget;
    left->setFixedWidth(286);
    auto *leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(12);
    // 语音卡功能保留，只有大按钮换成波形语音键（监听开关在顶栏胶囊）。
    // 下面这块可切换：遥控器卡 ↔ 语音主界面（选择持久化 ui.left_panel_voice）。
    // 语音页自带波形和全套控件，切过去时上面这张卡连波形一起藏掉，
    // 保证任何一种形态里同功能波形只出现一个。
    voiceCard_ = buildVoiceCard();
    // 遥控器形态：多余高度由语音卡里的状态/对话文本框吸收，
    // 遥控器卡固定高、底边贴齐右栏底边（用户 2026-11-03）。
    leftLay->addWidget(voiceCard_, 1);
    leftStack_ = new QStackedWidget;
    leftStack_->addWidget(buildRemoteCard());
    embeddedVoice_ = new VoiceWindow(voice_, store_.get(), this, true);
    // 控件整体不滚动（用户 19:32）：只有面板内对话块内部滚；布局已紧凑到
    // 主窗最小高度也放得下，不再套外层 QScrollArea。
    leftStack_->addWidget(embeddedVoice_);
    connect(embeddedVoice_, &VoiceWindow::embedExitRequested, this,
            [this] { setLeftPanel(false); });
    connect(embeddedVoice_, &VoiceWindow::typedSubmitted, this,
            [this](const QString &text) { voice_->submitTyped(text); });
    connect(embeddedVoice_, &VoiceWindow::speakRequested, this,
            [this](const QString &text) {
                if (voicePet_ != nullptr) {
                    voicePet_->speak(text);
                }
            });
    leftLay->addWidget(leftStack_, 0);
    setLeftPanel(store_->uiBool(QStringLiteral("voice_mode"), false) ||
                 store_->uiBool(QStringLiteral("left_panel_voice"), false));
    grid->addWidget(left);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(false);
    tabs_->addTab(buildWorkPage(), QString::fromUtf8("待我处理"));
    tabs_->addTab(buildLogPage(), QString::fromUtf8("事件流"));
    tabs_->addTab(buildSystemPage(), QString::fromUtf8("系统"));
    keymapPage_ = new KeymapPage(store_.get(), this);
    connect(keymapPage_, &KeymapPage::mappingsChanged, this,
            &MainWindow::refreshRemoteCard);
    connect(keymapPage_, &KeymapPage::viewChanged, this,
            &MainWindow::refreshRemoteCard);
    connect(keymapPage_, &KeymapPage::keySelected, this, [this](int vk) {
        if (remotePanel_ != nullptr) {
            remotePanel_->setLinkedVk(vk);
        }
    });
    connect(keymapPage_, &KeymapPage::keyHovered, this,
            [this](int vk, bool on) {
                if (remotePanel_ != nullptr) {
                    remotePanel_->setLinkedVk(on ? vk : keymapPage_->selectedVk());
                }
            });
    // 外壳版：映射/场景/选中/底部提示任何一处变了，重推一次状态（增量，不重载）。
    connect(keymapPage_, &KeymapPage::mappingsChanged, this, [this] {
        if (workWeb_ != nullptr) {
            workWeb_->refresh();
        }
    });
    connect(keymapPage_, &KeymapPage::viewChanged, this, [this] {
        if (workWeb_ != nullptr) {
            workWeb_->refresh();
        }
    });
    connect(keymapPage_, &KeymapPage::keySelected, this, [this](int) {
        if (workWeb_ != nullptr) {
            workWeb_->refresh();
        }
    });
    connect(keymapPage_, &KeymapPage::hintChanged, this, [this] {
        if (workWeb_ != nullptr) {
            workWeb_->refresh();
        }
    });
    tabs_->addTab(keymapPage_, QString::fromUtf8("按键映射"));
    appPage_ = new AppPage(store_.get());
    connect(appPage_, &AppPage::launched, this,
            [this](const QString &name, bool ok, const QString &error) {
                if (ok) {
                    appendEvent(QStringLiteral("voice"), QString(), QString(),
                                QStringLiteral("已打开 ") + name, 0);
                    if (toast_ != nullptr) {
                        toast_->showText(QStringLiteral("已打开 ") + name, 1800);
                    }
                } else {
                    appendEvent(QStringLiteral("error"), QString(), QString(),
                                QStringLiteral("打不开 ") + name +
                                    (error.isEmpty() ? QString()
                                                     : QStringLiteral(" · ") + error),
                                0);
                    if (toast_ != nullptr) {
                        toast_->showText(QStringLiteral("打不开 ") + name, 2400);
                    }
                }
            });
    tabs_->addTab(appPage_, QString::fromUtf8("应用"));
    tabs_->addTab(buildGuidePage(), QString::fromUtf8("使用引导"));
    // 整个右半区（页签栏 + 页面）交给一个 WebView2 画，Qt 只留左侧语音面板。
    // 不塞进 QTabWidget：布局那一步把它直接放到 grid 的 1 号位，Qt 页签整块藏起来。
    // 默认就走外壳；留 AGENTRMC_WORKWEB=0 回退老页签（现场排查/运行库缺失时用）。
    const QByteArray workWebFlag = qgetenv("AGENTRMC_WORKWEB");
    const bool wantWorkWeb = workWebFlag != "0";
    if (wantWorkWeb && WorkWebView::runtimeAvailable()) {
        auto *shell = new WorkWebView;
        shell->setSources([this] { return webShellState(); },
                          [this](const QJsonObject &action) { handleWebAction(action); });
        shell->setResourceRoot(ThemeManager::themesRoot());
        workWeb_ = shell;
        // 换主题 / 拖外观滑块（含字号倍率）都只重推一次状态：页面不重载，
        // CSS 变量自己换掉，选中态和滚动位置不动。
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
                [this] {
                    if (workWeb_ != nullptr) {
                        // 只推 theme 这一块：整份 state 要序列化六个页（实测
                        // ~500ms），换色用不着惊动它们。
                        workWeb_->pushTheme(webThemeJson());
                    }
                });
    }
    if (wantWorkWeb && workWeb_ == nullptr) {
        qWarning("workweb: WebView2 运行库不可用，回退 Qt 页签");
    }
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (remotePanel_ != nullptr) {
            const int keymapIndex =
                keymapPage_ != nullptr ? tabs_->indexOf(keymapPage_) : -1;
            remotePanel_->setLinkedVk(
                index == keymapIndex && keymapPage_ != nullptr
                    ? keymapPage_->selectedVk()
                    : 0);
        }
    });
    // 打开先看待办：有人等你确认才是需要你回来处理的事；没有待办时才落到按键映射。
    const bool hasAttention = workItems_ != nullptr &&
                              workItems_->attentionCount() > 0;
    if (workWeb_ == nullptr) {
        tabs_->setCurrentIndex(hasAttention || keymapPage_ == nullptr
                                   ? 0
                                   : tabs_->indexOf(keymapPage_));
    }
    refreshWorkItems();
    if (workWeb_ != nullptr) {
        // 整窗外壳：顶栏/左栏/页签全由 HTML 画，Qt chrome 藏起来但控件仍活着
        // 当状态机（语音/遥控器/映射的状态都从它们读）。混渲染会让 Qt  backing
        // store 发虚（用户实测），所以画面只留 WebView2 一条管线。
        tabs_->hide();
        headerBar_->hide();
        leftBody_->hide();
        workWeb_->setMinimumSize(900, 520);
        outer->addWidget(workWeb_, 1);
    } else {
        grid->addWidget(tabs_, 1);
    }

    outer->addWidget(body, 1);

    toast_ = new Toast(this);
    // 语音桌宠：主窗/语音面板关掉之后留在屏幕上的小球，跟着监听状态变色。
    voicePet_ = new VoicePet(store_.get(), voice_);
    // Agent 专用虚拟指针：逐屏一块穿透覆盖层，只画（指针/署名/涟漪），一根也不
    // 碰系统光标。桥的 /cursor/* 全打在它上面。
    cursor_ = new CursorOverlay(this);
    // 对话里问「任务状态」时直接给模型真实待办，别让它编造工具调用。
    voicePet_->setWorkSummaryProvider([this] {
        if (workItems_ == nullptr) {
            return QString();
        }
        int needs = 0;
        int review = 0;
        QString latest;
        for (const WorkItem &item : workItems_->items()) {
            if (item.done) {
                continue;
            }
            if (item.stage == QStringLiteral("needs-me")) {
                ++needs;
                if (latest.isEmpty()) {
                    latest = item.message;
                }
            } else if (item.stage == QStringLiteral("review")) {
                ++review;
                if (latest.isEmpty()) {
                    latest = item.message;
                }
            }
        }
        if (needs == 0 && review == 0) {
            return QString::fromUtf8("没有需要用户处理的待办");
        }
        QString summary =
            QString::fromUtf8("%1 件等你处理、%2 件待验收")
                .arg(needs)
                .arg(review);
        if (!latest.isEmpty()) {
            summary += QString::fromUtf8("；最近一条：%1").arg(latest);
        }
        return summary;
    });
    connect(voicePet_, &VoicePet::fullUiRequested, this, &MainWindow::wake);
    connect(voicePet_, &VoicePet::panelRequested, this,
            &MainWindow::showVoicePanel);
    connect(voicePet_, &VoicePet::listenToggleRequested, this,
            &MainWindow::toggleListening);
    // 语音派活成功：先落一条 working，后续 permission/stop 会合并到同一张卡。
    connect(voicePet_, &VoicePet::workHandedOff, this,
            [this](const QString &exe, const QString &text) {
                if (workItems_ == nullptr) {
                    return;
                }
                workItems_->upsert(QStringLiteral("voice"), exe, QString(),
                                   QString(), QStringLiteral("voice"),
                                   AgentWindow::displayName(exe), text, exe);
                refreshWorkItems();
                // 「下次开新任务」被消费后下拉要回到默认项，别留在旧选择上。
                if (voiceWindow_ != nullptr) {
                    voiceWindow_->refreshDispatchMode();
                }
            });
    // 桌宠收起时对话回复照常给出：语音面板转写框 + 事件流。
    connect(voicePet_, &VoicePet::chatReplied, this, [this](const QString &text) {
        if (voiceWindow_ != nullptr) {
            voiceWindow_->showChatReply(text);
        }
        if (embeddedVoice_ != nullptr) {
            embeddedVoice_->showChatReply(text);
        }
        if (workWeb_ != nullptr) {
            workWeb_->refresh();
        }
        appendEvent(QStringLiteral("voice"), QString(), QString(),
                    QString::fromUtf8("鲸鱼娘：%1").arg(text), 0);
    });
    // 点气泡回跳：提醒不再靠自动抢前台，用户点了才把宿主窗口拉回来。
    connect(voicePet_, &VoicePet::bubbleActivated, this,
            [](const QString &exe) { AgentWindow::raise(exe); });
    // 桌宠交互卡（借鉴蛋仔）：允许/拒绝/选项 → 落收件箱走插件/桥应答（同聊天页）。
    connect(voicePet_, &VoicePet::requestAnswered, this,
            [this](const QString &sessionId, const QString &requestId,
                   const QString &kind, const QJsonArray &answers,
                   const QString &outcome) {
                Q_UNUSED(kind);
                QString workItemId;
                for (const WorkItem &it : workItems_->items()) {
                    if (it.sessionId == sessionId) {
                        workItemId = it.id;
                        break;
                    }
                }
                const QString why =
                    enqueueAnswer(workItemId, requestId, answers, outcome);
                if (!why.isEmpty()) {
                    appendEvent(QStringLiteral("error"), QString(), QString(),
                                QStringLiteral("桌宠应答未提交：%1").arg(why), 0);
                    // 光进事件流用户看不见（他正盯着卡）：把原因写回卡上。
                    if (voicePet_ != nullptr) {
                        voicePet_->reportAnswerFailure(requestId, why);
                    }
                } else if (voicePet_ != nullptr) {
                    // 已排进收件箱：卡先留着标「等宿主确认」，等插件 ack 再收。
                    voicePet_->markAnswerPending(requestId);
                }
                refreshWorkItems();
                if (workWeb_ != nullptr) {
                    workWeb_->refresh();  // 桌宠答完同步收掉桌面聊天页的卡
                }
            });
    // 交互卡「去聊天页」：打开该会话聊天页（复杂多选/多题去那里答）。
    connect(voicePet_, &VoicePet::requestOpenChat, this,
            [this](const QString &sessionId) {
                for (const WorkItem &it : workItems_->items()) {
                    if (it.sessionId == sessionId) {
                        handleWebAction(QJsonObject{
                            {QStringLiteral("cmd"), QStringLiteral("chatOpen")},
                            {QStringLiteral("id"), it.id}});
                        return;
                    }
                }
            });
    connect(voicePet_, &VoicePet::voiceFeedbackError, this,
            [this](const QString &error) {
                appendEvent(QStringLiteral("error"), QString(), QString(),
                            QString::fromUtf8("语音反馈失败：%1").arg(error), 0);
            });
    connect(voicePet_, &VoicePet::hiddenByUser, this, [this] {
        if (petAction_ != nullptr) {
            const QSignalBlocker block(petAction_);
            petAction_->setChecked(false);
        }
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QString::fromUtf8("语音桌宠已关闭；托盘菜单里可重新打开"), 0);
    });
    connect(voice_, &VoiceController::optionsChanged, this, [this] {
        refreshVoiceUi();
        refreshRemoteCard();
    });
    refreshRemoteCard();
    for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
        button->setFocusPolicy(Qt::NoFocus);
    }
    if (autoListen && store_->uiBool("auto_listen", true)) {
        QTimer::singleShot(0, this, &MainWindow::toggleListening);
    }
    if (withTray) {
        QTimer::singleShot(12000, this, [this] {
            QSettings settings;
            const qint64 last = settings
                                    .value(QStringLiteral("update/last_check"), 0)
                                    .toLongLong();
            if (QDateTime::currentSecsSinceEpoch() - last < 12 * 3600) {
                return;
            }
            checkUpdates(false);
        });
    }
    if (withTray) {
        buildTray();
    }
    if (withTray) {
        // ZCode 等 Agent 的进度通知与交互：桌宠在就冒气泡，不在就托盘弹一条；
        // speak 走气泡 + TTS（桌宠收起也照念）。截图模式不起（会跟正在跑的真实例抢端口）。
        auto *bridge = new AgentBridge(this);
        bridge_ = bridge;
        bridgeRequested_ = true;
        bridge->setStateProvider([this] {
            return voicePet_ != nullptr ? voicePet_->stateJson()
                                        : QStringLiteral("{}");
        });
        bridge->setHealthProvider([this] { return healthJson(); });
        bridge->setWorkItemsProvider([this] { return workItemsJson(); });
        bridge->setWorkItemAckHandler([this](const QString &id) {
            if (workItems_ == nullptr || !workItems_->ack(id)) {
                return false;
            }
            refreshWorkItems();
            return true;
        });
        bridge->setWorkersProvider([this] { return workersJson(); });
        bridge->setWorkItemUpdateHandler(
            [this](const QJsonObject &payload) { return reportWorkItem(payload); });
        bridge->setCommandHandler(
            [this](const QJsonObject &payload) { return onBridgeCommand(payload); });
        // Agent 专用虚拟指针：所有接入本桥的 Agent 共用一根自绘指针，从移动到
        // 点击全程不动系统光标（实现见 core/AgentCursor / ui/CursorOverlay）。
        bridge->setCursorStateProvider([this] {
            // 关掉开关时连状态也不给（visible 恒 false），别让 /cursor/state 看起来
            // 还活着——调用方据此就能判断「这条通道被用户关了」。
            QJsonObject state = cursor_ != nullptr ? cursor_->stateJson()
                                                   : QJsonObject{};
            if (!cursorEnabled()) {
                state = QJsonObject{{QStringLiteral("enabled"), false},
                                    {QStringLiteral("visible"), false}};
            } else {
                state.insert(QStringLiteral("enabled"), true);
            }
            return QString::fromUtf8(
                QJsonDocument(state).toJson(QJsonDocument::Compact));
        });
        bridge->setCursorHandler(
            [this](const QJsonObject &payload) { return onCursorAction(payload); });
        // DSH 定向派发通道：插件轮询取件 + 注入 ack（回执 = ack ok，followup
        // 同步落 durable inbox，与 WorkBuddy 的 jsonl user 行同级证明）。
        bridge->setDispatchPendingProvider([this](const QString &exe) {
            const QJsonArray items = dispatch_ != nullptr
                                         ? dispatch_->takePluginDispatches(exe)
                                         : QJsonArray{};
            return QString::fromUtf8(
                QJsonDocument(items).toJson(QJsonDocument::Compact));
        });
        bridge->setDispatchAckHandler([this](const QJsonObject &payload) {
            if (dispatch_ == nullptr) {
                return false;
            }
            // 不认识的 dispatch_id 要回 404：重启丢账/重复 ack 不能当成功。
            return dispatch_->pluginAck(
                payload.value(QStringLiteral("dispatch_id")).toString(),
                payload.value(QStringLiteral("ok")).toBool(),
                payload.value(QStringLiteral("error")).toString());
        });
        // OpenSquilla 外部桥推来的可切换模型目录（models.list 只有桥能读）。
        // 存内存账：dispatchStateJson 据此给页面列菜单，dispatch() 据此预检
        // override（目录没新鲜推来就不拦，交给桥侧反查拒）。
        bridge->setModelsHandler([this](const QJsonObject &payload) {
            const QString exe = payload.value(QStringLiteral("exe")).toString();
            if (exe.compare(QStringLiteral("OpenSquilla.exe"), Qt::CaseInsensitive) !=
                0) {
                return;
            }
            const QJsonArray providers =
                payload.value(QStringLiteral("providers")).toArray();
            if (providers.isEmpty()) {
                return;
            }
            osqModels_ = providers;
            osqModelsAt_ = QDateTime::currentDateTimeUtc();
            // 目录到了不空手：桥只推不改 state，这里催一次刷新让聊天页/手机页
            // 胶囊看见新目录（5 秒整窗周期太慢）。
            if (workWeb_ != nullptr) {
                workWeb_->refresh();
            }
        });
        // 派发口的模型预检读同一份账。
        if (dispatch_ != nullptr) {
            dispatch_->setOsqModelsProvider([this] { return osqModelsFresh(); });
        }
        // B1：DSH 插件推送会话当前待答请求，存内存账供聊天页展示。
        // 插件 1.2s 轮询应答件 = 在线心跳：每次推送都刷新该 exe 的在线表
        // （answerable 据此标，掉线 5s 内自动退回只读）。
        bridge->setRequestsHandler([this](const QJsonObject &payload) {
            const QString sid =
                payload.value(QStringLiteral("session_id")).toString();
            if (sid.isEmpty()) {
                return;
            }
            const QString exe = payload.value(QStringLiteral("exe")).toString();
            if (!exe.isEmpty()) {
                pluginSeenAt_.insert(exe.toLower(), QDateTime::currentDateTimeUtc());
            }
            const QJsonArray reqs =
                payload.value(QStringLiteral("requests")).toArray();
            const QJsonArray old = pendingRequests_.value(sid);
            if (reqs.isEmpty()) {
                pendingRequests_.remove(sid);
                // 卡片在 DSH 侧已关闭/已答：排队中还没送的应答件一并作废，
                // 否则会把答案送给已经不存在的请求。
                dropAnswerPendingForSession(sid);
                // 该会话已经没有待答了：挂在聊天页/手机底部的那条「已排队，等宿主
                // 确认…」也该撤（应答可能已被别处处理，ack 永远等不到）。
                if (dispatchNoticeId_ == itemIdForSession(sid)) {
                    dispatchNotice_.clear();
                    dispatchNoticeKind_.clear();
                    dispatchNoticeRequest_.clear();
                }
            } else {
                pendingRequests_.insert(sid, reqs);
            }
            // 借鉴蛋仔：待答请求驱动桌宠交互卡（新增→入队上卡，移除→收卡）。
            // 2026-11-16 方案 A：桌宠只有一张交互卡，改成**整批转发**给 VoicePet
            // 的待答队列（队首上卡 + 卡头「还有 N 张待答」），不再只挑最后一条；
            // 以前那样多条待答时，其余几条永远轮不到（用户实测「只显示一张」）。
            if (voicePet_ != nullptr) {
                QSet<QString> oldIds;
                for (const QJsonValue &v : old) {
                    oldIds.insert(v.toObject().value(QStringLiteral("id")).toString());
                }
                QSet<QString> newIds;
                for (const QJsonValue &v : reqs) {
                    newIds.insert(v.toObject().value(QStringLiteral("id")).toString());
                }
                // 已经不在这批里的：收卡（正在显示的那条会让队列顶下一张）。
                for (const QString &rid : oldIds) {
                    if (!newIds.contains(rid)) {
                        voicePet_->clearRequestCard(rid);
                    }
                }
                // 可答请求排队；question 类即使不可答（ZCode 只读选方案卡）也排
                // ——展示题干 + 去聊天页查看。同 id 重推只更新内容，不重复排队。
                int queued = 0;
                for (const QJsonValue &v : reqs) {
                    const QJsonObject r = v.toObject();
                    if (r.value(QStringLiteral("answerable")).toBool() ||
                        r.value(QStringLiteral("kind")).toString() ==
                            QStringLiteral("question")) {
                        voicePet_->queueRequestCard(sid, r);
                        ++queued;
                    }
                }
                // 只在待答集合真的变了时记一笔：插件每 10s 重推整批，别刷事件流。
                if (newIds != oldIds) {
                    appendEvent(QStringLiteral("status"), QString(), QString(),
                                queued > 0
                                    ? QStringLiteral("桌宠待答队列：会话 %1 共 %2 张")
                                          .arg(sid.left(8))
                                          .arg(queued)
                                    : QStringLiteral("会话 %1 待答清空")
                                          .arg(sid.left(8)),
                                0);
                }
            }
            // 待答清单变化（新增/被别处应答移除）→ 刷新桌面聊天页，收掉残留卡。
            if (workWeb_ != nullptr) {
                workWeb_->refresh();
            }
        });
        // B2：插件轮询取应答件 / 应答结果回执。
        bridge->setAnswerPendingProvider(
            [this](const QString &exe, const QString &session) {
                // 应答件轮询每 1.2s 一拍，与「有没有待答请求」无关：这是判断
                // 插件在不在线最可靠的读数（重启对账靠它，见
                // reconcileLegacyAttention）。
                if (!exe.isEmpty()) {
                    const QString key = exe.toLower();
                    if (!pluginAliveSince_.contains(key)) {
                        pluginAliveSince_.insert(
                            key, QDateTime::currentDateTimeUtc());
                    }
                }
                return takeAnswerPending(exe, session);
            });
        bridge->setAnswerAckHandler([this](const QJsonObject &payload) {
            const QString answerId =
                payload.value(QStringLiteral("answer_id")).toString();
            const bool ok = payload.value(QStringLiteral("ok")).toBool();
            if (answerId.isEmpty() || !pendingAnswers_.contains(answerId)) {
                return false;
            }
            ackAnswer(answerId, ok,
                      payload.value(QStringLiteral("error")).toString());
            return true;
        });
        connect(bridge, &AgentBridge::notify, this,
                [this](const QString &kind, const QString &title,
                       const QString &message, const QString &raise,
                       const QString &sessionId, const QString &project,
                       const QString &eventId,
                       const QString &pluginVersion,
                       const QString &speech, bool noRaise,
                       const QString &reply, bool bubble) {
                    handleNotify(kind, title, message, raise, sessionId,
                                 project, eventId, pluginVersion, speech,
                                 reply, noRaise, bubble, false);
                });
        if (!bridge->start()) {
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QString::fromUtf8(
                            "桥接端口 45915 被占用，5 秒后自动重试"),
                        0);
        }
        // 手机访问（局域网 Web）：默认关闭；上次开着才恢复监听。设备令牌
        // 持久化在配置里，重启不用重新配对。截图模式（withTray=false）不起。
        if (store_->uiBool(QStringLiteral("lan_enabled"), false)) {
            ensureLanServer();
            lan_->setDevices(store_->uiArray(QStringLiteral("lan_devices")));
            if (!lan_->start()) {
                appendEvent(QStringLiteral("error"), QString(), QString(),
                            QString::fromUtf8(
                                "手机访问端口 45916 被占用，可在系统页重试"),
                            0);
            }
        }
    }
    // 离线 spool 补投 + 健康行；5 秒一拍，启动时先补一次。
    healthTimer_ = new QTimer(this);
    connect(healthTimer_, &QTimer::timeout, this, &MainWindow::watchdogTick);
    healthTimer_->start(5000);
    QTimer::singleShot(0, this, &MainWindow::watchdogTick);
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

    lay->addWidget(logo);
    lay->addWidget(brand);
    lay->addStretch(1);

    // 监听状态胶囊 = 监听开关：整窗最该一眼看到的状态，也顺手能点。
    // 开始/停止监听从左栏大按钮上移到这里，左栏大按钮让位给语音键。
    auto *pill = new QPushButton;
    pill->setObjectName("statusPill");
    pill->setCursor(Qt::PointingHandCursor);
    pill->setToolTip(QString::fromUtf8("监听开关：点击开始 / 停止监听"));
    connect(pill, &QPushButton::clicked, this, &MainWindow::toggleListening);
    auto *pillLay = new QHBoxLayout(pill);
    pillLay->setContentsMargins(9, 3, 11, 3);
    pillLay->setSpacing(6);
    statusDot_ = new QLabel;
    statusDot_->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 3px;")
            .arg(ThemeManager::instance().css(QStringLiteral("line"))));
    statusDot_->setFixedSize(6, 6);
    statusDot_->setAttribute(Qt::WA_TransparentForMouseEvents);
    statusText_ = new QLabel(QString::fromUtf8("未监听"));
    statusText_->setObjectName("statusText");
    statusText_->setAttribute(Qt::WA_TransparentForMouseEvents);
    pillLay->addWidget(statusDot_, 0, Qt::AlignVCenter);
    pillLay->addWidget(statusText_);
    // QPushButton 的 sizeHint 不算子布局，不约束会把胶囊挤到截字。
    pill->setFixedHeight(26);
    pill->setMinimumWidth(pillLay->minimumSize().width());
    lay->addWidget(pill, 0, Qt::AlignVCenter);
    lay->addSpacing(4);

    updateButton_ = new QPushButton(QString::fromUtf8("检查更新"));
    updateButton_->setObjectName("ghostText");
    updateButton_->setCursor(Qt::PointingHandCursor);
    updateButton_->setToolTip(QString::fromUtf8("检查是否有新版本"));
    connect(updateButton_, &QPushButton::clicked, this,
            [this] { checkUpdates(true); });
    lay->addWidget(updateButton_);

    // 换肤 + 调卡片透明度 / 背景压暗。以前只在「语音设置」里有个下拉，
    // 用户找不到，也调不了「字看不清」这件事。
    auto *themeButton = new QPushButton(QString::fromUtf8("主题"));
    themeButton->setObjectName("ghostText");
    themeButton->setCursor(Qt::PointingHandCursor);
    themeButton->setToolTip(
        QString::fromUtf8("切换主题、调卡片不透明度与背景压暗"));
    connect(themeButton, &QPushButton::clicked, this,
            &MainWindow::openThemeDialog);
    lay->addWidget(themeButton);

    // 分步图文引导（配对遥控器 / 选识别方式 / 接入 Agent）。
    // 「使用引导」那一页是纯文字速查表，两个入口靠名字区分开。
    auto *guideButton = new QPushButton(QString::fromUtf8("新手引导"));
    guideButton->setObjectName("ghost");
    guideButton->setCursor(Qt::PointingHandCursor);
    guideButton->setToolTip(
        QString::fromUtf8("分步引导：没有遥控器怎么用、怎么配对、怎么接入 Agent"));
    connect(guideButton, &QPushButton::clicked, this, &MainWindow::showGuide);
    lay->addWidget(guideButton);

    auto *min = new QToolButton;
    min->setObjectName("winBtn");
    min->setText(QString::fromUtf8("—"));
    min->setFixedSize(28, 24);
    min->setCursor(Qt::PointingHandCursor);

    // 最大化 / 还原（双击标题栏等效；F11 是占满整屏的全屏，两个都留）。
    maximizeBtn_ = new QToolButton;
    maximizeBtn_->setObjectName("winBtn");
    maximizeBtn_->setText(QString::fromUtf8("□"));
    maximizeBtn_->setFixedSize(28, 24);
    maximizeBtn_->setCursor(Qt::PointingHandCursor);
    maximizeBtn_->setToolTip(
        QString::fromUtf8("最大化 / 还原（双击标题栏，F11 全屏）"));

    auto *close = new QToolButton;
    close->setObjectName("closeBtn");
    close->setText(QString::fromUtf8("✕"));
    close->setFixedSize(28, 24);
    close->setCursor(Qt::PointingHandCursor);

    connect(min, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(maximizeBtn_, &QToolButton::clicked, this,
            &MainWindow::toggleMaximize);
    connect(close, &QToolButton::clicked, this, &QWidget::close);
    lay->addWidget(min);
    lay->addWidget(maximizeBtn_);
    lay->addWidget(close);
    return bar;
}

QWidget *MainWindow::buildRemoteCard() {
    auto *card = new QFrame;
    card->setObjectName("card");
    card->setFixedWidth(286);
    // 遥控器是可选设备：卡片别跟着窗口一起长高，否则中间一大片空。
    card->setMaximumHeight(392);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(8);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("遥控器"));
    title->setObjectName("cardTitle");
    remoteBadge_ = new QLabel(QString::fromUtf8("待机"));
    remoteBadge_->setObjectName("badge");
    auto *toVoice = new QPushButton(QString::fromUtf8("语音"));
    toVoice->setObjectName("ghostSmall");
    toVoice->setCursor(Qt::PointingHandCursor);
    toVoice->setToolTip(QString::fromUtf8("把这块换成语音主界面（打字/说话都行）"));
    connect(toVoice, &QPushButton::clicked, this, [this] { setLeftPanel(true); });
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(toVoice);
    head->addWidget(remoteBadge_);
    lay->addLayout(head);

    remotePanel_ = new RemotePanel;
    lay->addWidget(remotePanel_, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(7);
    remoteDot_ = new QLabel;
    remoteDot_->setObjectName("dot");
    remoteDot_->setFixedSize(6, 6);
    remoteState_ = new QLabel(QString::fromUtf8("未监听"));
    remoteState_->setObjectName("muted");
    auto *bt = new QPushButton(QString::fromUtf8("蓝牙设置"));
    bt->setObjectName("ghostSmall");
    bt->setCursor(Qt::PointingHandCursor);
    bt->setToolTip(QString::fromUtf8("打开 Windows 蓝牙设置"));
    connect(bt, &QPushButton::clicked, this, [this] {
        if (!QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:bluetooth")))) {
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QString::fromUtf8("打不开系统蓝牙设置"), 0);
        }
    });
    foot->addWidget(remoteDot_, 0, Qt::AlignVCenter);
    foot->addWidget(remoteState_);
    foot->addStretch(1);
    foot->addWidget(bt);
    lay->addLayout(foot);

    refreshRemoteCard();
    return card;
}

QWidget *MainWindow::buildVoiceCard() {
    auto *card = new QFrame;
    card->setObjectName("card");

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 13, 16, 13);
    lay->setSpacing(8);

    // 标题行右侧直接放识别方式：少一行、少一个「选什么」的疑问。
    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    auto *title = new QLabel(QString::fromUtf8("语音"));
    title->setObjectName("cardTitle");
    head->addWidget(title);
    head->addStretch(1);

    voiceBackend_ = new QComboBox;
    voiceBackend_->addItem(QString::fromUtf8("转发热键 · 0 模型"), "hotkey");
    voiceBackend_->addItem(QString::fromUtf8("本地 SenseVoice"), "sensevoice");
    voiceBackend_->addItem(QString::fromUtf8("本地 Qwen3-ASR"), "qwen3");
    voiceBackend_->addItem(QString::fromUtf8("云端 ASR（需联网）"), "api");
    voiceBackend_->setCursor(Qt::PointingHandCursor);
    voiceBackend_->setToolTip(
        QString::fromUtf8("语音怎么识别：转发当前应用的快捷键，或本地 / 云端模型"));
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
                refreshRemoteCard();
                if (key == QStringLiteral("api") && !voice_->modelReady()) {
                    toast_->showText(
                        QString::fromUtf8(
                            "云端 ASR 还没配好：语音设置 → 云端 ASR 填模型和 Key"),
                        3600);
                }
                appendEvent(QStringLiteral("status"), QString(),
                            QString(),
                            QString::fromUtf8("语音后端切换为 ") +
                                voiceBackend_->itemText(index),
                            0);
            });
    head->addWidget(voiceBackend_, 0);
    lay->addLayout(head);

    // 识别方式与模型设置之间的状态/对话文字框保留（模型就绪、最近转写）。
    voiceState_ = new QLabel;
    voiceState_->setObjectName("voiceBox");
    voiceState_->setWordWrap(true);
    voiceState_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    voiceState_->setMinimumHeight(48);
    lay->addWidget(voiceState_, 1);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(6);
    voiceTest_ = new QPushButton(QString::fromUtf8("试一句"));
    voiceTest_->setToolTip(QString::fromUtf8(
        "录 3 秒并转写一次，用来确认本地模型是否正常"));
    voiceTest_->setObjectName("ghostSmall");
    voiceTest_->setCursor(Qt::PointingHandCursor);
    connect(voiceTest_, &QPushButton::clicked, voice_,
            &VoiceController::startTest);
    auto *settings = new QPushButton(QString::fromUtf8("模型设置"));
    settings->setObjectName("ghostSmall");
    settings->setCursor(Qt::PointingHandCursor);
    connect(settings, &QPushButton::clicked, this,
            &MainWindow::openVoiceSettings);
    auto *voicePanel = new QPushButton(QString::fromUtf8("语音主界面"));
    voicePanel->setObjectName("ghostSmall");
    voicePanel->setCursor(Qt::PointingHandCursor);
    voicePanel->setToolTip(QString::fromUtf8(
        "切到无遥控器的简洁语音界面：波形键一键说话"));
    connect(voicePanel, &QPushButton::clicked, this,
            &MainWindow::showVoicePanel);
    tools->addWidget(voiceTest_);
    tools->addWidget(settings);
    tools->addWidget(voicePanel);
    tools->addStretch(1);
    lay->addLayout(tools);

    // 原「开始监听」大按钮换成波形语音键：点击说话/再点结束，颜色表状态；
    // 监听开关上移到顶栏胶囊。
    voiceKey_ = new WaveformButton;
    voiceKey_->setFixedHeight(40);
    connect(voiceKey_, &QPushButton::clicked, this, [this] {
        if (engine_ == nullptr || !engine_->isRunning()) {
            toast_->showText(QString::fromUtf8(
                "还没开始监听：点右上角「未监听」胶囊开启"));
            return;
        }
        voice_->mediaToggle();
    });
    lay->addWidget(voiceKey_);
    refreshVoiceKey();

    // 宿主 + 任务会话放波形下面；卡中间不再留常态文字。
    cardPicker_ = new DispatchPicker(store_.get());
    lay->addWidget(cardPicker_);

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
    logCount_->setObjectName("workCount");
    auto *clear = new QPushButton(QString::fromUtf8("清空"));
    clear->setObjectName("ghostText");
    clear->setCursor(Qt::PointingHandCursor);
    connect(clear, &QPushButton::clicked, this, &MainWindow::clearLog);
    head->addWidget(logCount_);
    head->addStretch(1);
    head->addWidget(clear);
    cl->addLayout(head);

    // 空状态给一句「这里会有什么」，比一个居中的「等待按键…」有用。
    auto *emptyBox = new QWidget;
    auto *emptyLay = new QVBoxLayout(emptyBox);
    emptyLay->setContentsMargins(0, 0, 0, 0);
    emptyLay->setSpacing(6);
    emptyLay->addStretch(1);
    logEmpty_ = new QLabel(QString::fromUtf8("还没有事件"));
    logEmpty_->setObjectName("emptyTitle");
    logEmpty_->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(logEmpty_);
    auto *emptyHint = new QLabel(QString::fromUtf8(
        "遥控器按键、语音转写、Agent 通知都会按时间记在这里，\n"
        "排查「按了没反应」时先看这一页。"));
    emptyHint->setObjectName("emptyHint");
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyLay->addWidget(emptyHint);
    emptyLay->addStretch(1);
    cl->addWidget(emptyBox, 1);

    logList_ = new QListWidget;
    logList_->setObjectName("logList");
    logList_->setFrameShape(QFrame::NoFrame);
    logList_->setSelectionMode(QAbstractItemView::NoSelection);
    logList_->setFocusPolicy(Qt::NoFocus);
    logList_->setWordWrap(true);
    logList_->setSpacing(2);
    logList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    logList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    logList_->hide();
    cl->addWidget(logList_, 1);
    lay->addWidget(card);
    return page;
}

QWidget *MainWindow::buildWorkPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *card = new QFrame;
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(16, 13, 14, 12);
    cl->setSpacing(9);

    // 一行说清「有多少、卡在哪」：总数 + 三个分类胶囊 + Worker 过滤 + 清理。
    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    workCount_ = new QLabel(QString::fromUtf8("没有待办"));
    workCount_->setObjectName("workCount");
    head->addWidget(workCount_);

    chipNeeds_ = makeChip(QString(), "pending");
    chipReview_ = makeChip(QString(), "ok");
    chipWorking_ = makeChip(QString(), "info");
    head->addWidget(chipNeeds_);
    head->addWidget(chipReview_);
    head->addWidget(chipWorking_);
    head->addStretch(1);

    workFilter_ = new QComboBox;
    workFilter_->setMinimumWidth(132);
    workFilter_->setToolTip(QString::fromUtf8("只看某个 Agent 的待办"));
    connect(workFilter_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshWorkItems(); });
    head->addWidget(workFilter_);

    auto *clear = new QPushButton(QString::fromUtf8("清空已完成"));
    clear->setObjectName("ghostText");
    clear->setCursor(Qt::PointingHandCursor);
    clear->setToolTip(QString::fromUtf8("把已经处理完的记录从列表里删掉"));
    connect(clear, &QPushButton::clicked, this, [this] {
        if (workItems_ != nullptr) {
            workItems_->clearFinished();
        }
        refreshWorkItems();
    });
    head->addWidget(clear);
    cl->addLayout(head);

    // 系统健康压成一行：桥接 / Worker / 最近事件，异常才变色。
    workHealth_ = new QLabel;
    workHealth_->setObjectName("workHealth");
    workHealth_->setTextFormat(Qt::RichText);
    cl->addWidget(workHealth_);

    workList_ = new QListWidget;
    workList_->setObjectName("workList");
    workList_->setFrameShape(QFrame::NoFrame);
    workList_->setWordWrap(false);
    workList_->setSpacing(2);
    workList_->setMouseTracking(true);
    workList_->setUniformItemSizes(true);
    workList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    workList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    workList_->setItemDelegate(new WorkItemDelegate(workList_));
    workList_->setSelectionMode(QAbstractItemView::SingleSelection);
    workList_->hide();
    connect(workList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                if (item != nullptr) {
                    openWorkItem(item->data(Qt::UserRole).toString());
                }
            });
    connect(workList_, &QListWidget::itemSelectionChanged, this, [this] {
        const bool has = !selectedWorkId().isEmpty();
        if (workRaise_ != nullptr) {
            workRaise_->setEnabled(has);
        }
        if (workDone_ != nullptr) {
            workDone_->setEnabled(has);
        }
        if (workHint_ != nullptr) {
            workHint_->setText(
                has ? QString::fromUtf8("Enter 回到 Agent · Delete 标为已处理")
                    : QString::fromUtf8(
                          "选中一条待办后可操作 · 双击打开 Agent（没开着会先启动）并销账"));
        }
    });
    cl->addWidget(workList_, 1);

    // 空状态：说清楚这里会出现什么，并给出下一步入口，而不是一句「没有」。
    workEmptyBox_ = new QWidget;
    auto *emptyLay = new QVBoxLayout(workEmptyBox_);
    emptyLay->setContentsMargins(0, 0, 0, 0);
    emptyLay->setSpacing(6);
    emptyLay->addStretch(1);
    workEmpty_ = new QLabel(QString::fromUtf8("没有需要你处理的事"));
    workEmpty_->setObjectName("emptyTitle");
    workEmpty_->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(workEmpty_);
    auto *emptyHint = new QLabel(QString::fromUtf8(
        "Agent 等你确认、或任务做完时，这里会自动出现一条。\n"
        "先给 Qoder / ZCode / WorkBuddy 装上 agentrmc-bridge 插件。"));
    emptyHint->setObjectName("emptyHint");
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyLay->addWidget(emptyHint);
    auto *emptyRow = new QHBoxLayout;
    auto *emptyBtn = new QPushButton(QString::fromUtf8("看怎么接入 Agent"));
    emptyBtn->setObjectName("ghostSmall");
    emptyBtn->setCursor(Qt::PointingHandCursor);
    connect(emptyBtn, &QPushButton::clicked, this, [this] {
        if (tabs_ != nullptr) {
            for (int i = 0; i < tabs_->count(); ++i) {
                if (tabs_->tabText(i).startsWith(QString::fromUtf8("使用引导"))) {
                    tabs_->setCurrentIndex(i);
                    return;
                }
            }
        }
    });
    emptyRow->addStretch(1);
    emptyRow->addWidget(emptyBtn);
    emptyRow->addStretch(1);
    emptyLay->addLayout(emptyRow);
    emptyLay->addStretch(1);
    cl->addWidget(workEmptyBox_, 1);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    workHint_ = new QLabel(
        QString::fromUtf8("选中一条待办后可操作 · 双击直接回到对应 Agent"));
    workHint_->setObjectName("workHint");
    actions->addWidget(workHint_, 1);
    workRaise_ = new QPushButton(QString::fromUtf8("回到 Agent"));
    workRaise_->setObjectName("ghostSmall");
    workRaise_->setCursor(Qt::PointingHandCursor);
    workRaise_->setToolTip(QString::fromUtf8(
        "把发这条待办的 Agent 切到前台；它没开着就先启动宿主"));
    workRaise_->setEnabled(false);
    connect(workRaise_, &QPushButton::clicked, this,
            &MainWindow::raiseSelectedWork);
    workDone_ = new QPushButton(QString::fromUtf8("标为已处理"));
    workDone_->setObjectName("primarySmall");
    workDone_->setCursor(Qt::PointingHandCursor);
    workDone_->setToolTip(QString::fromUtf8("处理完了，从待办里移走（Delete）"));
    workDone_->setEnabled(false);
    connect(workDone_, &QPushButton::clicked, this,
            &MainWindow::ackSelectedWork);
    actions->addWidget(workRaise_);
    actions->addWidget(workDone_);
    cl->addLayout(actions);

    // 键盘也能干活：Enter 回去，Delete 结掉。
    auto *enterKey = new QShortcut(QKeySequence(Qt::Key_Return), workList_);
    enterKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(enterKey, &QShortcut::activated, this,
            &MainWindow::raiseSelectedWork);
    auto *delKey = new QShortcut(QKeySequence::Delete, workList_);
    delKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delKey, &QShortcut::activated, this, &MainWindow::ackSelectedWork);

    lay->addWidget(card);
    refreshWorkItems();
    return page;
}

void MainWindow::refreshWorkItems() {
    if (workWeb_ != nullptr) {
        // WebView2 版：页面自己排版，这里只推一次数据 + 同步页签上的计数。
        workWeb_->refresh();
        if (tabs_ != nullptr && tabs_->count() > 0 && workItems_ != nullptr) {
            const int attention = workItems_->attentionCount();
            tabs_->setTabText(
                0, attention > 0
                       ? QString::fromUtf8("待我处理 (%1)").arg(attention)
                       : QString::fromUtf8("待我处理"));
        }
        return;
    }
    if (workList_ == nullptr || workItems_ == nullptr) {
        return;
    }
    const QSignalBlocker block(workList_);
    const QString selected = selectedWorkId();
    workList_->clear();

    QStringList aliveWorkers;
    for (const QString &exe : AgentWindow::knownExes()) {
        // 过滤下拉按进程算：宿主收进托盘时它的待办仍要能被筛出来。
        if (AgentWindow::processAlive(exe)) {
            aliveWorkers.append(exe);
        }
    }

    const QString filterExe =
        workFilter_ != nullptr ? workFilter_->currentData().toString()
                               : QString();
    if (workFilter_ != nullptr) {
        const QSignalBlocker filterBlock(workFilter_);
        workFilter_->clear();
        workFilter_->addItem(QString::fromUtf8("全部 Worker"), QString());
        for (const QString &exe : aliveWorkers) {
            workFilter_->addItem(AgentWindow::displayName(exe), exe);
        }
        const int index = workFilter_->findData(filterExe);
        workFilter_->setCurrentIndex(index >= 0 ? index : 0);
    }

    const QVector<WorkItem> items = workItems_->displayOrder();

    // 「在哪儿」：项目目录名 > 会话标题 > 会话 <短 id>；都没有就直说是未关联
    // 会话，不拿宿主名冒充身份（同宿主并行两个会话时那是唯一的区分）。
    // DSH 的标题/工作目录来自宿主投影缓存（只读、按 mtime 缓存）：AI 标题
    // 比短 id 好认，项目为空的老卡也能补回 cwd。
    const auto dshInfoFor = [](const WorkItem &item) {
        if (item.sessionId.isEmpty() ||
            item.agentExe.compare(QStringLiteral("DeepSeek Harness.exe"),
                                  Qt::CaseInsensitive) != 0) {
            return DshSessions::Info{};
        }
        return DshSessions::find(item.sessionId);
    };
    const auto whereFor = [&dshInfoFor](const WorkItem &item) {
        if (item.sessionId.isEmpty()) {
            return QString::fromUtf8("未关联会话");
        }
        const DshSessions::Info info = dshInfoFor(item);
        QString project = item.project;
        if (project.isEmpty() && info.valid) {
            project = info.cwd;
        }
        QString dirName = QDir(project).dirName();
        // 宿主给的是相对路径时 dirName 会是 "." ".."，读起来像坏数据。
        if (dirName == QLatin1String(".") || dirName == QLatin1String("..")) {
            dirName.clear();
        }
        if (!dirName.isEmpty()) {
            return dirName;
        }
        if (!info.title.isEmpty()) {
            return info.title;
        }
        return QString::fromUtf8("会话 %1").arg(item.sessionId.left(8));
    };
    const auto matchesFilter = [&filterExe](const WorkItem &item) {
        if (filterExe.isEmpty()) {
            return true;
        }
        return item.agentExe.compare(filterExe, Qt::CaseInsensitive) == 0 ||
               item.title.compare(AgentWindow::displayName(filterExe),
                                  Qt::CaseInsensitive) == 0;
    };
    const auto agentFor = [](const WorkItem &item) {
        return item.title.isEmpty() ? AgentWindow::displayName(item.agentExe)
                                    : item.title;
    };
    // 同宿主同项目并行两个会话时，两行都只写「Agentrmc」等于没写：
    // 先数一遍重名，重名的行才补会话标题（没标题才退短 id）。
    QHash<QString, int> whereHits;
    for (const WorkItem &item : items) {
        if (item.done || !matchesFilter(item)) {
            continue;
        }
        ++whereHits[agentFor(item) + QLatin1Char('|') + whereFor(item)];
    }

    int needs = 0;
    int review = 0;
    int working = 0;
    int open = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const WorkItem &item : items) {
        if (item.done) {
            continue;
        }
        if (!matchesFilter(item)) {
            continue;
        }
        ++open;
        QString stage = QString::fromUtf8("待办");
        QColor color = ThemeManager::instance().color(QStringLiteral("muted"));
        if (item.stage == QStringLiteral("needs-me")) {
            stage = QString::fromUtf8("等你处理");
            color = ThemeManager::instance().color(QStringLiteral("pending"));
            ++needs;
        } else if (item.stage == QStringLiteral("review")) {
            stage = QString::fromUtf8("待验收");
            color = ThemeManager::instance().color(QStringLiteral("ok"));
            ++review;
        } else if (item.stage == QStringLiteral("working")) {
            if (item.staleWorking(now)) {
                // 半小时没有任何事件：不能说它还在跑，也不能说它做完了。
                // 把「静默多久」写出来——「状态待确认」看不出是刚静默还是
                // 挂了半天，用户只会觉得卡死（2026-11-19 反馈）。
                const qint64 quiet = item.updatedAt.secsTo(now) / 60;
                stage = quiet < 60
                            ? QString::fromUtf8("静默 %1 分").arg(quiet)
                            : quiet < 60 * 24
                                  ? QString::fromUtf8("静默 %1 小时")
                                        .arg(quiet / 60)
                                  : QString::fromUtf8("静默 %1 天")
                                        .arg(quiet / (60 * 24));
                color = ThemeManager::instance().color(QStringLiteral("muted"));
            } else {
                stage = QString::fromUtf8("进行中");
                color = ThemeManager::instance().color(QStringLiteral("info"));
            }
            ++working;
        }

        const QString agent = agentFor(item);
        const QDateTime when = item.updatedAt.isValid() ? item.updatedAt
                                                        : item.createdAt;

        QString where = whereFor(item);
        // 只有「按项目分不出来」时才占用宽度：优先补会话标题（DSH 投影缓存），
        // 没标题才退短 id——短 id 是最后手段，不是常态。
        const DshSessions::Info info = dshInfoFor(item);
        if (!item.sessionId.isEmpty() &&
            !where.startsWith(QString::fromUtf8("会话 ")) &&
            where != QString::fromUtf8("未关联会话") &&
            whereHits.value(agent + QLatin1Char('|') + where) > 1) {
            if (!info.title.isEmpty() && where != info.title) {
                QString suffix = info.title.left(12);
                if (info.title.size() > 12) {
                    suffix += QString::fromUtf8("…");
                }
                where += QString::fromUtf8(" · %1").arg(suffix);
            } else {
                where += QString::fromUtf8(" · %1").arg(item.sessionId.left(6));
            }
        }

        // 行内不再拼「[待验收] Xxx」这种字符串：阶段色、Agent、时间各占各的位置，
        // 由 WorkItemDelegate 分栏画，扫一眼就能分出「谁在等我」。
        auto *row = new QListWidgetItem(workList_);
        row->setData(Qt::DisplayRole, item.message);
        row->setData(Qt::UserRole, item.id);
        row->setData(Qt::UserRole + 1, item.raiseExe);
        row->setData(Qt::UserRole + 2, color);
        row->setData(Qt::UserRole + 3, stage);
        row->setData(Qt::UserRole + 4, agent);
        // 时间位顺手报「等了多久」：十分钟以内看钟点就够，久了得让人看见账。
        QString whenText = when.isValid()
                               ? when.toLocalTime().toString(
                                     QStringLiteral("HH:mm"))
                               : QString();
        const qint64 waited = item.waitedSecs(now);
        if (waited >= 600) {
            const qint64 mins = waited / 60;
            if (mins < 60) {
                whenText += QString::fromUtf8(" · 等 %1 分").arg(mins);
            } else if (mins < 60 * 24) {
                whenText += QString::fromUtf8(" · 等 %1 小时").arg(mins / 60);
            } else {
                // 过了两天还挂着，报小时已经读不出量级了。
                whenText += QString::fromUtf8(" · 等 %1 天").arg(mins / (60 * 24));
            }
        }
        row->setData(Qt::UserRole + 5, whenText);
        row->setData(Qt::UserRole + 6, where);
        row->setSizeHint(QSize(0, 56));
        QString tipProject = item.project;
        if (tipProject.isEmpty() && info.valid) {
            tipProject = info.cwd;
        }
        QString tipSession;
        if (!item.sessionId.isEmpty()) {
            tipSession = QString::fromUtf8("\n会话：%1").arg(item.sessionId);
            if (!info.title.isEmpty()) {
                tipSession += QString::fromUtf8("\n标题：%1").arg(info.title);
            }
        }
        row->setToolTip(
            QString::fromUtf8("%1 · %2  %3\n%4\n%5%6")
                .arg(stage, agent,
                     when.isValid()
                         ? when.toLocalTime().toString(
                               QStringLiteral("MM-dd HH:mm"))
                         : QString(),
                     item.message,
                     tipProject.isEmpty()
                         ? QStringLiteral("（无项目路径）")
                         : QString::fromUtf8("项目：%1").arg(tipProject),
                     tipSession));
        if (item.id == selected) {
            workList_->setCurrentItem(row);
        }
    }

    if (open == 0) {
        workList_->hide();
        if (workEmptyBox_ != nullptr) {
            workEmptyBox_->show();
        }
        const bool filtered = !filterExe.isEmpty();
        workEmpty_->setText(
            filtered ? QString::fromUtf8("这个 Worker 没有待办")
                     : QString::fromUtf8("没有需要你处理的事"));
        workCount_->setText(
            filtered ? QString::fromUtf8("这个 Worker 没有待办")
                     : QString::fromUtf8("没有待办"));
    } else {
        if (workEmptyBox_ != nullptr) {
            workEmptyBox_->hide();
        }
        workList_->show();
        workCount_->setText(QString::fromUtf8("%1 件待办").arg(open));
    }

    // 分类胶囊只显示非 0 的那几类，0 值不占版面。
    const auto setChip = [](QLabel *chip, int count, const char *format) {
        if (chip == nullptr) {
            return;
        }
        chip->setVisible(count > 0);
        if (count > 0) {
            chip->setText(QString::fromUtf8(format).arg(count));
        }
    };
    setChip(chipNeeds_, needs, "等你 %1");
    setChip(chipReview_, review, "待验收 %1");
    setChip(chipWorking_, working, "进行中 %1");

    if (tabs_ != nullptr && tabs_->count() > 0) {
        const int attention = workItems_->attentionCount();
        tabs_->setTabText(
            0, attention > 0
                   ? QString::fromUtf8("待我处理 (%1)").arg(attention)
                   : QString::fromUtf8("待我处理"));
    }
    const bool has = !selectedWorkId().isEmpty();
    if (workRaise_ != nullptr) {
        workRaise_->setEnabled(has);
    }
    if (workDone_ != nullptr) {
        workDone_->setEnabled(has);
    }
    if (workHint_ != nullptr) {
        workHint_->setText(
            has ? QString::fromUtf8("Enter 回到 Agent · Delete 标为已处理")
                : QString::fromUtf8(
                      "选中一条待办后可操作 · 双击打开 Agent（没开着会先启动）并销账"));
    }
    refreshHealth();
}

QString MainWindow::selectedWorkId() const {
    if (workList_ == nullptr) {
        return QString();
    }
    if (const QListWidgetItem *item = workList_->currentItem()) {
        return item->data(Qt::UserRole).toString();
    }
    // 辅助技术（UIA）可能只设置 selected 而不设 current：退回第一项选中。
    const QList<QListWidgetItem *> selected = workList_->selectedItems();
    return selected.isEmpty()
               ? QString()
               : selected.first()->data(Qt::UserRole).toString();
}

QJsonArray MainWindow::workersArray() const {
    QJsonArray workers;
    const QString foreground = AgentWindow::foregroundAgentExe();
    for (const QString &exe : AgentWindow::knownExes()) {
        // 进程在跑就算 Worker（收进托盘 / 隐藏窗口的宿主不能从表里消失，
        // 否则待办「进了列表没人认领」）；有没有可交互窗口单独给 window 字段。
        if (!AgentWindow::processAlive(exe)) {
            continue;
        }
        int open = 0;
        int needs = 0;
        int review = 0;
        int working = 0;
        QDateTime last;
        const QString name = AgentWindow::displayName(exe);
        if (workItems_ != nullptr) {
            for (const WorkItem &item : workItems_->items()) {
                if (item.done) {
                    continue;
                }
                const bool match =
                    item.agentExe.compare(exe, Qt::CaseInsensitive) == 0 ||
                    item.title.compare(name, Qt::CaseInsensitive) == 0;
                if (!match) {
                    continue;
                }
                ++open;
                if (item.stage == QStringLiteral("needs-me")) {
                    ++needs;
                } else if (item.stage == QStringLiteral("review")) {
                    ++review;
                } else if (item.stage == QStringLiteral("working")) {
                    ++working;
                }
                if (!last.isValid() || item.updatedAt > last) {
                    last = item.updatedAt;
                }
            }
        }
        workers.append(QJsonObject{
            {QStringLiteral("exe"), exe},
            {QStringLiteral("name"), name},
            {QStringLiteral("alive"), true},
            {QStringLiteral("window"), AgentWindow::alive(exe)},
            {QStringLiteral("foreground"),
             foreground.compare(exe, Qt::CaseInsensitive) == 0},
            {QStringLiteral("open_items"), open},
            {QStringLiteral("needs_me"), needs},
            {QStringLiteral("review"), review},
            {QStringLiteral("working"), working},
            {QStringLiteral("last_activity"),
             last.isValid()
                 ? last.toUTC().toString(Qt::ISODateWithMs)
                 : QString()},
        });
    }
    return workers;
}

QString MainWindow::workersJson() const {
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("workers"), workersArray()}})
            .toJson(QJsonDocument::Compact));
}

namespace {

// 联动插件随安装包落在 exe 旁边：源目录用来读内置版本，zip 用来升级各家宿主。
QString pluginSourceDir() {
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/plugins/agentrmc-bridge");
}

QString pluginZipPath() {
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/plugins/agentrmc-bridge.zip");
}

// DeepSeek Harness 的联动插件是一个 bundle 目录（不是 zip）：装的时候由 DSH 里的
// Agent 拿这个目录去 install_bundle，所以引导话术要给的是目录而不是包。
QString pluginBundleDir() {
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/plugins/deepseek-harness");
}

// 宿主进程名 → 应用表 id。表里的名字和 displayName 不完全一路（DSH 的表项叫
// DeepSeek、快捷方式才叫 DeepSeek Harness），所以按 exe 手写对应，不靠猜。
// 表里没有的宿主（比如 CodeBuddy）回空串：启动不了就明说，别把提醒销了。
QString hostAppId(const QString &exe) {
    static const QVector<QPair<QString, QString>> kIds = {
        {QStringLiteral("Qoder.exe"), QStringLiteral("qoder")},
        {QStringLiteral("Qoder CN.exe"), QStringLiteral("qodercn")},
        {QStringLiteral("WorkBuddyAI.exe"), QStringLiteral("workbuddyai")},
        {QStringLiteral("WorkBuddy.exe"), QStringLiteral("workbuddy")},
        {QStringLiteral("ZCode.exe"), QStringLiteral("zcode")},
        {QStringLiteral("DeepSeek Harness.exe"), QStringLiteral("deepseek")},
        {QStringLiteral("OpenSquilla.exe"), QStringLiteral("opensquilla")},
    };
    for (const auto &entry : kIds) {
        if (entry.first.compare(exe, Qt::CaseInsensitive) == 0) {
            return entry.second;
        }
    }
    return QString();
}

}  // namespace

QJsonObject MainWindow::pluginHealth(QStringList *problems) const {
    QJsonArray rows;
    const QString bundled = pluginupgrade::bundledVersion(pluginSourceDir());
    const QString bundledBundle =
        pluginupgrade::bundledBundleVersion(pluginBundleDir());
    const QList<pluginupgrade::Host> hosts =
        pluginupgrade::scanHomes(QDir::homePath());
    for (const pluginupgrade::Host &host : hosts) {
        QString state;
        if (!host.problem.isEmpty()) {
            state = QString::fromUtf8("读不出");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("%1 插件状态读不出：%2")
                                     .arg(host.name, host.problem));
            }
        } else if (!host.installed) {
            // 没装过是一种状态，不是故障：首次安装走「使用引导」那句话，
            // 不该每 5 秒在问题行里刷一条。
            state = QString::fromUtf8("未装");
        } else if (bundled.isEmpty()) {
            state = QString::fromUtf8("安装包没带版本");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("安装包里没有 plugins/agentrmc-bridge/VERSION"));
            }
        } else if (versionIsNewer(bundled, host.version)) {
            state = QString::fromUtf8("落后");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("%1 插件 %2 → 内置 %3")
                                     .arg(host.name, host.version, bundled));
            }
        } else {
            state = QString::fromUtf8("已装齐");
        }
        // 盘上装齐了但进程还在跑旧版，是「升级完没重启」，和「没升级」两回事。
        const QString loaded = pluginLoaded_.value(host.name);
        const bool staleProcess =
            state == QString::fromUtf8("已装齐") && !loaded.isEmpty() &&
            loaded != bundled;
        if (staleProcess && problems != nullptr) {
            problems->append(QString::fromUtf8("%1 盘上 %2，进程里还是 %3，重启宿主才生效")
                                 .arg(host.name, bundled, loaded));
        }
        // 升级后一条 hook 都没收到：已开着的会话把命令指向上一版目录，而那个目录
        // 常被安装器删掉，于是提醒静默断掉——不报旧版本号，是什么都不报。
        const bool awaiting = pluginAwaitingReport_.contains(host.name);
        if (awaiting && problems != nullptr) {
            problems->append(
                QString::fromUtf8("%1 升级后还没收到 hook：开着的会话仍指向旧版本目录，"
                                  "新开会话或重启宿主才恢复提醒").arg(host.name));
        }
        const QDateTime lastReport = pluginLastReport_.value(host.name);
        rows.append(QJsonObject{
            {QStringLiteral("name"), host.name},
            {QStringLiteral("installed"), host.version},
            {QStringLiteral("loaded"), loaded},
            {QStringLiteral("enabled"), host.enabled},
            {QStringLiteral("running"), AgentWindow::processAlive(host.exe)},
            {QStringLiteral("state"), state},
            {QStringLiteral("last_report"),
             lastReport.isValid()
                 ? lastReport.toUTC().toString(Qt::ISODateWithMs)
                 : QString()},
            {QStringLiteral("awaiting_report"), awaiting},
        });
    }

    // 装在宿主 profile 里的 bundle（DeepSeek Harness）：没有注册表，装法、版本来源、
    // 升级方式都和上面五家不一样，所以单独一组，字段名保持一致好让界面用同一套渲染。
    QJsonArray bundleRows;
    for (const pluginupgrade::Bundle &bundle :
         pluginupgrade::scanBundles(QDir::homePath())) {
        QString state;
        if (!bundle.problem.isEmpty()) {
            state = QString::fromUtf8("读不出");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("%1 插件状态读不出：%2")
                                     .arg(bundle.name, bundle.problem));
            }
        } else if (!bundle.installed) {
            // 和上面一样：没装是一种状态，不是故障。
            state = QString::fromUtf8("未装");
        } else if (bundledBundle.isEmpty()) {
            state = QString::fromUtf8("安装包没带版本");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8(
                    "安装包里没有 plugins/deepseek-harness/package.json"));
            }
        } else if (versionIsNewer(bundledBundle, bundle.version)) {
            state = QString::fromUtf8("落后");
            if (problems != nullptr) {
                problems->append(QString::fromUtf8("%1 插件 %2 → 内置 %3（不代升级）")
                                     .arg(bundle.name, bundle.version,
                                          bundledBundle));
            }
        } else {
            state = QString::fromUtf8("已装齐");
        }
        // 进程里真在跑的版本：DSH 插件的 /notify 带 plugin_version，title 就是
        // 「DeepSeek Harness」，所以这个读数一直都在，只是以前没人读。
        // 注意它是**按宿主名**记的，分不清是哪个 profile 在跑；没装的那个 profile
        // 再显示这个数就是撒谎，所以只在已装的行上给。
        const QString loaded =
            bundle.installed ? pluginLoaded_.value(bundle.name) : QString();
        const bool staleProcess =
            state == QString::fromUtf8("已装齐") && !loaded.isEmpty() &&
            loaded != bundledBundle;
        if (staleProcess && problems != nullptr) {
            problems->append(QString::fromUtf8("%1 盘上 %2，进程里还是 %3，重启宿主才生效")
                                 .arg(bundle.name, bundledBundle, loaded));
        }
        const QDateTime lastReport =
            bundle.installed ? pluginLastReport_.value(bundle.name) : QDateTime();
        bundleRows.append(QJsonObject{
            {QStringLiteral("name"), bundle.name},
            {QStringLiteral("kind"), QStringLiteral("bundle")},
            {QStringLiteral("profile"), bundle.profile},
            {QStringLiteral("path"), bundle.path},
            {QStringLiteral("installed"), bundle.version},
            {QStringLiteral("loaded"), loaded},
            {QStringLiteral("enabled"), bundle.enabled},
            {QStringLiteral("running"), AgentWindow::processAlive(bundle.exe)},
            {QStringLiteral("state"), state},
            {QStringLiteral("last_report"),
             lastReport.isValid()
                 ? lastReport.toUTC().toString(Qt::ISODateWithMs)
                 : QString()},
            {QStringLiteral("awaiting_report"),
             bundle.installed && pluginAwaitingReport_.contains(bundle.name)},
        });
    }

    return QJsonObject{
        {QStringLiteral("bundled"), bundled},
        {QStringLiteral("bundled_bundle"), bundledBundle},
        {QStringLiteral("hosts"), rows},
        {QStringLiteral("bundles"), bundleRows},
        {QStringLiteral("bridges"), pluginBridges()},
    };
}

// 不走插件包的外部桥（目前只有 OpenSquilla）：它没有插件注册表，也不该参与
// 「内置版本 vs 已装版本」的对账——硬塞进 kFamily 只会每 5 秒报一次「读不出」。
// 这里只回答两件事：本机装没装、桥是不是活着（进程在不在）。
QJsonArray MainWindow::pluginBridges() const {
    QJsonArray bridges;
    // OpenSquilla：状态家 ~/.opensquilla，桥脚本自己开机自启，靠 /notify
    // 与插件走同一条路。判据只认这家的配置家存在，不猜别家。
    const QString home = QDir::homePath() + QStringLiteral("/.opensquilla");
    if (QDir(home).exists()) {
        const bool running =
            AgentWindow::processAlive(QStringLiteral("OpenSquilla.exe"));
        bridges.append(QJsonObject{
            {QStringLiteral("name"), QStringLiteral("OpenSquilla")},
            {QStringLiteral("exe"), QStringLiteral("OpenSquilla.exe")},
            {QStringLiteral("installed"), true},
            {QStringLiteral("running"), running},
            {QStringLiteral("kind"), QStringLiteral("bridge")},
            {QStringLiteral("detail"),
             QString::fromUtf8("靠外部桥接入，不占插件版本，也不用升级")},
        });
    }
    return bridges;
}

void MainWindow::upgradePlugins() {
    const QString bundled = pluginupgrade::bundledVersion(pluginSourceDir());
    const QString zip = pluginZipPath();
    const QString backupDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/plugin-backups");
    QStringList lines;
    int upgraded = 0;
    for (const pluginupgrade::Host &host :
         pluginupgrade::scanHomes(QDir::homePath())) {
        if (!host.problem.isEmpty()) {
            lines << QString::fromUtf8("%1 读不出：%2").arg(host.name, host.problem);
            continue;
        }
        if (!host.installed) {
            lines << QString::fromUtf8("%1 没装过，先在宿主里装一次").arg(host.name);
            continue;
        }
        if (!versionIsNewer(bundled, host.version)) {
            lines << QString::fromUtf8("%1 已 %2").arg(host.name, host.version);
            continue;
        }
        // WorkBuddy 落后也由它自己的市场源（autoUpdate）去拉：这里只报状态。
        // 提前把话说清，免得点完以为「WorkBuddy 怎么没动」。
        if (!host.upgradable) {
            lines << QString::fromUtf8("%1 落后（%2），由宿主自己更新")
                         .arg(host.name, host.version);
            continue;
        }
        const pluginupgrade::Result result =
            pluginupgrade::upgrade(host, zip, bundled, backupDir);
        if (result.ok) {
            ++upgraded;
            // 用 result.detail 而不是自己拼 from→to：镜像没同步这类附注也挂在
            // detail 上，自己拼会把它吞掉。
            lines << QString::fromUtf8("%1 %2%3")
                         .arg(host.name, result.detail,
                              AgentWindow::processAlive(host.exe)
                                  ? QString::fromUtf8("（重启该宿主才加载）")
                                  : QString());
        } else {
            lines << QString::fromUtf8("%1 没成：%2").arg(host.name, result.detail);
        }
    }
    // 五家都在上面走了一遍：能代升级的已经升了，WorkBuddy 只报状态。
    // DSH 的 bundle 不在这条链里——升它要 pnpm 重装 + 重启 Host，改注册表做不到。
    // 只把「落后了」这件事说出来，并指向引导里的那句话，免得用户以为按钮漏了它。
    const QString bundledBundle =
        pluginupgrade::bundledBundleVersion(pluginBundleDir());
    for (const pluginupgrade::Bundle &bundle :
         pluginupgrade::scanBundles(QDir::homePath())) {
        if (bundle.problem.isEmpty() && bundle.installed &&
            !bundledBundle.isEmpty() &&
            versionIsNewer(bundledBundle, bundle.version)) {
            lines << QString::fromUtf8(
                         "%1 插件 %2 → %3：不代升级，让 DSH 里的 Agent 重装后再重启 "
                         "DeepSeek Harness")
                         .arg(bundle.name, bundle.version, bundledBundle);
        }
    }
    const QString detail = lines.join(QString::fromUtf8(" · "));
    appendEvent(upgraded > 0 ? QStringLiteral("status") : QStringLiteral("error"),
                QString(), QString(),
                QString::fromUtf8("联动插件升级：%1").arg(detail), 0);
    if (toast_ != nullptr) {
        toast_->showText(detail, 4200);
    }
    refreshSystemPage();
}

void MainWindow::trackPluginTransitions() {
    for (const pluginupgrade::Host &host :
         pluginupgrade::scanHomes(QDir::homePath())) {
        if (!host.installed) {
            continue;
        }
        const QString seen = pluginSeenInstalled_.value(host.name);
        if (!seen.isEmpty() && seen != host.version &&
            AgentWindow::processAlive(host.exe)) {
            // 版本是在运行期间换的，而这家宿主还开着：它已经起来的会话里 hook 命令
            // 仍指向上一版目录。那一版目录要是被安装器删了，提醒就静默没了。
            pluginAwaitingReport_.insert(host.name, true);
        }
        pluginSeenInstalled_.insert(host.name, host.version);
    }
}

QString MainWindow::healthJson() const {
    const int spool = spoolPendingCount();
    QJsonArray problems;
    if (bridge_ == nullptr || !bridge_->isRunning()) {
        problems.append(QString::fromUtf8("桥接未启动"));
    }
    if (spool > 0) {
        problems.append(QString::fromUtf8("离线待补 %1 条").arg(spool));
    }
    QStringList pluginProblems;
    const QJsonObject plugin = pluginHealth(&pluginProblems);
    for (const QString &problem : pluginProblems) {
        problems.append(problem);
    }
    QJsonObject root{
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("bridge"), bridge_ != nullptr && bridge_->isRunning()},
        {QStringLiteral("uptime_seconds"),
         static_cast<double>(
             startedAt_.secsTo(QDateTime::currentDateTimeUtc()))},
        {QStringLiteral("spool_pending"), spool},
        {QStringLiteral("plugin"), plugin},
        {QStringLiteral("last_event_at"),
         lastEventAt_.isValid()
             ? lastEventAt_.toUTC().toString(Qt::ISODateWithMs)
             : QString()},
        {QStringLiteral("work_items"),
         QJsonObject{{QStringLiteral("attention"),
                      workItems_ != nullptr ? workItems_->attentionCount() : 0},
                     {QStringLiteral("open"),
                      workItems_ != nullptr ? workItems_->openCount() : 0}}},
        {QStringLiteral("workers"), workersArray()},
        {QStringLiteral("problems"), problems},
    };
    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MainWindow::refreshHealth() {
    if (workHealth_ == nullptr) {
        return;
    }
    int workers = 0;
    for (const QString &exe : AgentWindow::knownExes()) {
        // 健康行的 Worker 数按进程算，和「待我处理」页 / Worker 表同口径。
        if (AgentWindow::processAlive(exe)) {
            ++workers;
        }
    }
    const bool bridgeOk = bridge_ != nullptr && bridge_->isRunning();
    const int spool = spoolPendingCount();
    QStringList problems;
    if (!bridgeOk) {
        problems.append(QString::fromUtf8("桥接未启动"));
    }
    if (spool > 0) {
        problems.append(QString::fromUtf8("离线待补 %1 条").arg(spool));
    }
    const QString last =
        lastEventAt_.isValid()
            ? lastEventAt_.toLocalTime().toString(QStringLiteral("HH:mm"))
            : QStringLiteral("—");
    // 一行富文本：正常时只有一个绿点 + 灰字；有问题就直接列问题，不重复报「异常」。
    const QString dot = problems.isEmpty() ? themeToken("ok")
                                           : themeToken("pending");
    const QString body =
        problems.isEmpty()
            ? QString::fromUtf8("桥接正常 · %1 个 Worker · 最近 %2")
                  .arg(workers)
                  .arg(last)
            : QString::fromUtf8("%1 · %2 个 Worker")
                  .arg(problems.join(QString::fromUtf8(" · ")))
                  .arg(workers);
    workHealth_->setText(
        QStringLiteral("<span style=\"color:%1;\">●</span>&nbsp;%2")
            .arg(dot, body));
    refreshSystemPage();
}

// 系统页的显示就绪状态：Qt 标签/表格与 WebView 外壳共用这一份，
// 文案和对账规则只在这里维护一遍。
QJsonObject MainWindow::systemState() {
    const bool bridgeOk = bridge_ != nullptr && bridge_->isRunning();
    const qint64 secs = startedAt_.secsTo(QDateTime::currentDateTimeUtc());
    const int spool = spoolPendingCount();
    QStringList pluginProblems;
    const QJsonObject plugin = pluginHealth(&pluginProblems);

    QString pluginText = QString::fromUtf8("联动插件：内置 %1")
                             .arg(plugin.value(QStringLiteral("bundled"))
                                          .toString()
                                          .isEmpty()
                                      ? QString::fromUtf8("读不到")
                                      : plugin.value(QStringLiteral("bundled")).toString());
    const QJsonArray rows = plugin.value(QStringLiteral("hosts")).toArray();
    const QJsonArray bundleRows = plugin.value(QStringLiteral("bundles")).toArray();
    if (rows.isEmpty() && bundleRows.isEmpty()) {
        pluginText += QString::fromUtf8(" · 本机没有宿主配置家");
    }
    // 五家宿主各占一段会把这行撑成一堵墙，而绝大多数时候它们只是「都装齐了」。
    // 所以按状态分两堆：干净的合并成一句「已同步 A、B、C」，有事的单独说。
    // DSH 的 bundle 行字段名一样，用同一套规则渲染（未装时说清去哪儿装）。
    QStringList synced;
    QStringList pending;
    auto renderRow = [&](const QJsonObject &row) {
        const QString name = row.value(QStringLiteral("name")).toString();
        const bool isBundle =
            row.value(QStringLiteral("kind")).toString() ==
            QStringLiteral("bundle");
        const QString installed =
            row.value(QStringLiteral("installed")).toString();
        const QString state = row.value(QStringLiteral("state")).toString();
        const bool enabled = row.value(QStringLiteral("enabled")).toBool();
        // 盘上已装齐但进程还跑旧版：只有发通知上报的 loaded 能看出来。
        const QString loaded = row.value(QStringLiteral("loaded")).toString();
        const bool staleProcess =
            !loaded.isEmpty() && loaded != installed;
        const bool awaiting =
            row.value(QStringLiteral("awaiting_report")).toBool();

        // 只有「装齐 + 启用 + 进程也是这版 + 没在等上报」才算真同步，
        // 少任何一条都得单独露出来，别被合并吃掉。
        if (installed.isEmpty()) {
            pending << (isBundle
                            ? QString::fromUtf8("%1 插件未装（装法见「使用引导」）")
                                  .arg(name)
                            : QString::fromUtf8("%1 未装").arg(name));
            return;
        }
        if (state == QString::fromUtf8("已装齐") && enabled &&
            !staleProcess && !awaiting) {
            synced << (isBundle ? QString::fromUtf8("%1 插件").arg(name) : name);
            return;
        }
        QString line = QString::fromUtf8("%1 %2（%3%4）")
                           .arg(name, installed, state,
                                enabled ? QString()
                                        : QString::fromUtf8("·未启用"));
        if (staleProcess) {
            line += QString::fromUtf8("·进程 %1").arg(loaded);
        }
        if (awaiting) {
            line += QString::fromUtf8("·等新版上报");
        }
        pending << line;
    };
    for (const QJsonValue &value : rows) {
        renderRow(value.toObject());
    }
    // DSH 的 bundle：多 profile 时只报「装了的那些」（用 profile 名区分）；
    // 一个都没装才给一行「未装」，否则每个没用过的 profile 都会刷一句。
    QList<QJsonObject> installedBundles;
    for (const QJsonValue &value : bundleRows) {
        const QJsonObject row = value.toObject();
        if (!row.value(QStringLiteral("installed")).toString().isEmpty()) {
            installedBundles.append(row);
        }
    }
    if (installedBundles.isEmpty() && !bundleRows.isEmpty()) {
        const QString name = bundleRows.first()
                                 .toObject()
                                 .value(QStringLiteral("name"))
                                 .toString();
        pending << QString::fromUtf8("%1 插件未装（装法见「使用引导」）").arg(name);
    }
    for (const QJsonObject &row : installedBundles) {
        QJsonObject named = row;
        const QString profile = row.value(QStringLiteral("profile")).toString();
        if (bundleRows.size() > 1 && !profile.isEmpty()) {
            named.insert(QStringLiteral("name"),
                         QString::fromUtf8("%1（%2）")
                             .arg(row.value(QStringLiteral("name")).toString(),
                                  profile));
        }
        renderRow(named);
    }
    if (!synced.isEmpty()) {
        pluginText += QString::fromUtf8(" · 已同步 %1")
                          .arg(synced.join(QString::fromUtf8("、")));
    }
    for (const QString &line : pending) {
        pluginText += QString::fromUtf8(" · ") + line;
    }
    // 外部桥（OpenSquilla）不占插件版本，单独提一句：用户否则会奇怪
    // “每天在用的那个 Agent 怎么没出现在这行里”。
    const QJsonArray bridges = plugin.value(QStringLiteral("bridges")).toArray();
    for (const QJsonValue &value : bridges) {
        const QJsonObject bridge = value.toObject();
        const QString name = bridge.value(QStringLiteral("name")).toString();
        const bool running = bridge.value(QStringLiteral("running")).toBool();
        pluginText += QString::fromUtf8(" · %1 外部桥（%2）")
                          .arg(name, running ? QString::fromUtf8("运行中")
                                             : QString::fromUtf8("未启动"));
    }
    const QString pluginTip = QString::fromUtf8(
        "这一行对账 Qoder / Qoder CN / ZCode / WorkBuddy AI / WorkBuddy 五家"
        "已装的插件，外加装在宿主 profile 里的 DeepSeek Harness；"
        "版本和内置一致的合并成「已同步」，其余单独列。\n"
        "Qoder 家族把上传的 zip 记成本地插件，插件页的「更新」按钮不会去拉新包；\n"
        "ZCode 的目录市场源只在宿主自己重扫时才重新拉，重启宿主并不会重扫，"
        "所以也会停在旧版——这两类由「升级联动插件」代劳。\n"
        "WorkBuddy 的市场源条目带 autoUpdate，宿主自己就会拉到新版，"
        "这里只报状态，不动它的注册表。\n"
        "DeepSeek Harness 的插件是个 bundle 目录（~/.dsh/profiles/<profile> 里"
        "一条 link 依赖 + dsh.profile.bundles），没有注册表可改："
        "升它要 pnpm 重装并重启 Host，所以这里只报盘上版本和进程版本，"
        "装法是「使用引导 → 接入 AI Agent」里那句给 DSH 的 Agent 的话。\n"
        "OpenSquilla 不同：它没有插件目录，也不能把插件的 zip 塞进去，走的是"
        "外部桥（技能目录里的 opensquilla-pet-bridge.mjs）向同一个 45915 端口"
        "发提醒，所以不占插件版本、也不用升级；只报进程在不在。\n"
        "改完插件重启对应宿主即可；行里的「进程」是此刻真在跑的版本。");

    QStringList problems = pluginProblems;
    if (!bridgeOk) {
        problems.append(QString::fromUtf8("桥接未启动（端口可能被占用）"));
    }
    if (spool > 0) {
        problems.append(
            QString::fromUtf8("有 %1 条离线通知待补投").arg(spool));
    }

    QJsonArray workerRows;
    const QJsonArray workers = workersArray();
    for (const QJsonValue &value : workers) {
        const QJsonObject worker = value.toObject();
        const bool foreground =
            worker.value(QStringLiteral("foreground")).toBool();
        const bool window = worker.value(QStringLiteral("window")).toBool();
        const QString last =
            worker.value(QStringLiteral("last_activity")).toString();
        QDateTime lastAt = QDateTime::fromString(last, Qt::ISODateWithMs);
        if (!lastAt.isValid()) {
            lastAt = QDateTime::fromString(last, Qt::ISODate);
        }
        // 进程活着但窗口收起来了要跟「在线」区分开：用户得知道为什么
        // 语音派活没送到它头上（没有可交互窗口就不注入）。
        workerRows.append(QJsonObject{
            {QStringLiteral("name"),
             worker.value(QStringLiteral("name")).toString()},
            {QStringLiteral("state"),
             foreground ? QString::fromUtf8("前台")
                        : window ? QString::fromUtf8("在线")
                                 : QString::fromUtf8("窗口隐藏")},
            {QStringLiteral("foreground"), foreground},
            {QStringLiteral("needs"),
             worker.value(QStringLiteral("needs_me")).toInt()},
            {QStringLiteral("review"),
             worker.value(QStringLiteral("review")).toInt()},
            {QStringLiteral("working"),
             worker.value(QStringLiteral("working")).toInt()},
            {QStringLiteral("last"),
             lastAt.isValid()
                 ? lastAt.toLocalTime().toString(QStringLiteral("HH:mm"))
                 : QString::fromUtf8("—")},
        });
    }

    return QJsonObject{
        {QStringLiteral("bridge"),
         QString::fromUtf8("桥接：%1（127.0.0.1:45915）")
             .arg(bridgeOk ? QString::fromUtf8("正常")
                           : QString::fromUtf8("异常"))},
        {QStringLiteral("uptime"),
         QString::fromUtf8("运行时长：%1 分 %2 秒").arg(secs / 60).arg(secs % 60)},
        {QStringLiteral("last"),
         QString::fromUtf8("最近事件：%1")
             .arg(lastEventAt_.isValid()
                      ? lastEventAt_.toLocalTime().toString(
                            QStringLiteral("MM-dd HH:mm:ss"))
                      : QString::fromUtf8("—"))},
        {QStringLiteral("spool"),
         QString::fromUtf8("离线待补：%1 条").arg(spool)},
        {QStringLiteral("plugin"), pluginText},
        {QStringLiteral("pluginTip"), pluginTip},
        {QStringLiteral("problems"),
         problems.isEmpty()
             ? QString::fromUtf8("问题：无")
             : QString::fromUtf8("问题：%1")
                   .arg(problems.join(QString::fromUtf8("；")))},
        {QStringLiteral("problemsOk"), problems.isEmpty()},
        {QStringLiteral("workers"), workerRows},
        {QStringLiteral("lan"), lanState()},
        // Agent 指针总开关的当前值：外壳系统页那颗开关与 Qt 老界面共用。
        {QStringLiteral("cursor"), QJsonObject{{QStringLiteral("enabled"), cursorEnabled()}}},
    };
}

void MainWindow::refreshSystemPage() {
    if (sysBridge_ == nullptr) {
        return;
    }
    const QJsonObject state = systemState();
    sysBridge_->setText(state.value(QStringLiteral("bridge")).toString());
    sysUptime_->setText(state.value(QStringLiteral("uptime")).toString());
    sysLast_->setText(state.value(QStringLiteral("last")).toString());
    sysSpool_->setText(state.value(QStringLiteral("spool")).toString());
    sysPlugin_->setText(state.value(QStringLiteral("plugin")).toString());
    sysPlugin_->setToolTip(state.value(QStringLiteral("pluginTip")).toString());
    sysProblems_->setText(state.value(QStringLiteral("problems")).toString());
    sysProblems_->setStyleSheet(
        state.value(QStringLiteral("problemsOk")).toBool()
            ? QStringLiteral("color:%1;")
                  .arg(ThemeManager::instance().css(QStringLiteral("muted")))
            : QStringLiteral("color:%1;").arg(themeToken("pending")));

    const QJsonArray workers = state.value(QStringLiteral("workers")).toArray();
    sysWorkers_->setRowCount(workers.size());
    for (int i = 0; i < workers.size(); ++i) {
        const QJsonObject worker = workers.at(i).toObject();
        const bool foreground =
            worker.value(QStringLiteral("foreground")).toBool();
        const QVector<QString> columns = {
            worker.value(QStringLiteral("name")).toString(),
            worker.value(QStringLiteral("state")).toString(),
            QString::number(worker.value(QStringLiteral("needs")).toInt()),
            QString::number(worker.value(QStringLiteral("review")).toInt()),
            QString::number(worker.value(QStringLiteral("working")).toInt()),
            worker.value(QStringLiteral("last")).toString(),
        };
        for (int column = 0; column < columns.size(); ++column) {
            auto *item = new QTableWidgetItem(columns.at(column));
            if (foreground && column == 0) {
                item->setForeground(
                    ThemeManager::instance().color(QStringLiteral("info")));
            }
            sysWorkers_->setItem(i, column, item);
        }
    }
}

void MainWindow::watchdogTick() {
    if (bridgeRequested_ && bridge_ != nullptr && !bridge_->isRunning()) {
        if (bridge_->start()) {
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("桥接已恢复"), 0);
        }
    }
    importSpool();
    trackPluginTransitions();
    // 重启对账：插件在线且过了预热期后，把上一个进程遗留的死待答卡归档。
    // 放这里而不是 /requests 推送里——插件只推还有待答的会话，死卡对应的会话
    // 根本不会再被推一次（见 reconcileLegacyAttention 的注释）。
    reconcileLegacyAttention();
    refreshHealth();
    HostRuntime::refreshHint();
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
}

int MainWindow::spoolPendingCount() const {
    const auto lines = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return 0;
        }
        int count = 0;
        while (!file.atEnd()) {
            if (!file.readLine().trimmed().isEmpty()) {
                ++count;
            }
        }
        return count;
    };
    return lines(spoolPath_) + lines(spoolPath_ + QStringLiteral(".processing"));
}

int MainWindow::importSpool() {
    if (spoolPath_.isEmpty()) {
        return 0;
    }
    int imported = 0;
    const QString processing = spoolPath_ + QStringLiteral(".processing");
    for (const QString &path : {spoolPath_, processing}) {
        QFile file(path);
        if (!file.exists()) {
            continue;
        }
        QString readPath = path;
        if (path == spoolPath_) {
            QFile::remove(processing);
            if (!file.rename(processing)) {
                continue;
            }
            readPath = processing;
        }
        QFile input(readPath);
        if (!input.open(QIODevice::ReadOnly)) {
            continue;
        }
        while (!input.atEnd()) {
            const QByteArray line = input.readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            QJsonParseError error{};
            const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                continue;
            }
            const QJsonObject obj = doc.object();
            handleNotify(obj.value(QStringLiteral("event")).toString(),
                         obj.value(QStringLiteral("title")).toString(),
                         obj.value(QStringLiteral("message")).toString(),
                         obj.value(QStringLiteral("raise")).toString(),
                         obj.value(QStringLiteral("session_id")).toString(),
                         obj.value(QStringLiteral("project")).toString(),
                         obj.value(QStringLiteral("event_id")).toString(),
                         obj.value(QStringLiteral("plugin_version")).toString(),
                         obj.value(QStringLiteral("speech")).toString(),
                         obj.value(QStringLiteral("reply")).toString(),
                         true, false, true);
            ++imported;
        }
        input.close();
        QFile::remove(readPath);
    }
    if (imported > 0) {
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QString::fromUtf8("补投离线通知 %1 条").arg(imported), 0);
    }
    return imported;
}

QJsonObject MainWindow::reportWorkItem(const QJsonObject &payload) {
    const QString stage =
        payload.value(QStringLiteral("stage")).toString().trimmed();
    if (WorkItemStore::kindForReport(stage).isEmpty()) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QString::fromUtf8("stage 只支持 progress / blocked / ready")}};
    }
    if (workItems_ == nullptr) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("work item store not ready")}};
    }
    const QString title =
        payload.value(QStringLiteral("title")).toString().trimmed();
    QString agentExe =
        payload.value(QStringLiteral("agent_exe")).toString().trimmed();
    if (agentExe.isEmpty()) {
        // MCP 只知道自己叫什么产品名，进程映像名在这里反查——Worker 行按 exe
        // 归堆，认错了待办就会挂到另一个宿主头上（本机有两份 WorkBuddy）。
        for (const QString &exe : AgentWindow::knownExes()) {
            if (!title.isEmpty() &&
                AgentWindow::displayName(exe).compare(title,
                                                      Qt::CaseInsensitive) == 0) {
                agentExe = exe;
                break;
            }
        }
    }
    QString message =
        payload.value(QStringLiteral("message")).toString().trimmed();
    if (message.isEmpty()) {
        // 没给文案也要有话说，否则列表里是一行空白。
        const QString s = stage.toLower();
        message = s == QStringLiteral("progress")
                      ? QString::fromUtf8("正在处理")
                      : s == QStringLiteral("blocked")
                            ? QString::fromUtf8("卡住了，等你")
                            : QString::fromUtf8("做完了，等你验收");
    }
    const QString raise =
        payload.value(QStringLiteral("raise")).toString().trimmed();
    const QString sessionId =
        payload.value(QStringLiteral("session_id")).toString().trimmed();
    const QString project =
        payload.value(QStringLiteral("project")).toString().trimmed();
    const WorkItem item = workItems_->report(agentExe, raise, sessionId, project,
                                             stage, title, message);
    if (item.id.isEmpty()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("stage rejected")}};
    }
    lastEventAt_ = QDateTime::currentDateTimeUtc();
    refreshWorkItems();

    // 语音是主提醒通道（PRD：前台不静音、要人动手的必须喊到）。
    // progress 只是状态，不出声；blocked / ready 走和 hook 完全同一条路——
    // 气泡 + TTS，permission/question 还会顺手把宿主拉回前台。
    const QString kind = WorkItemStore::kindForReport(stage);
    const QString key = sessionId.isEmpty() ? title : sessionId;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (kind == QStringLiteral("question")) {
        // 连报几次 blocked 别喊成一串：同一个会话 2 分钟内只喊一次。
        // planChoice=false：blocked 不是选方案，若套 180s 迟到去重，周期连报
        // （如每 130s 一次）会被吞掉且只刷新时间戳，永远不再出声。
        const QDateTime last = attentionAnnounced_.value(key);
        if (!last.isValid() || last.secsTo(now) > 120) {
            announceWork(kind, title, message, message, raise, sessionId, false,
                         QString(), false, 2);
            attentionAnnounced_.insert(key, now);
            // 记一笔「blocked 已喊过」：同一件事的 question hook 可能晚到，
            // 按弹窗身份消费一次，别让下一道新题跟着被吞。
            blockedAnnounced_.insert(key, now);
        }
    } else if (kind == QStringLiteral("stop")) {
        // 先念再记账：紧接着到的 hook stop 会看到这笔账，不再念第二遍。
        announceWork(kind, title, message, message, raise, sessionId, false,
                     QString(), false, 1);
        readyAnnounced_.insert(key, now);
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("id"), item.id},
                       {QStringLiteral("stage"), item.stage},
                       {QStringLiteral("title"), item.title}};
}

void MainWindow::handleNotify(const QString &kind, const QString &title,
                              const QString &message, const QString &raise,
                              const QString &sessionId,
                              const QString &project, const QString &eventId,
                              const QString &pluginVersion,
                              const QString &speech, const QString &reply,
                              bool noRaise, bool showBubble, bool replay) {
    lastEventAt_ = QDateTime::currentDateTimeUtc();
    // 念的和显示的不是一句：speech 是插件给的短语音文案（带宿主名、不含工具名和
    // 路径），没带就退回念 message。气泡和工单始终用 message。
    const QString spoken = speech.isEmpty() ? message : speech;
    // 记「谁在跑哪一版」要在判事件类型之前：turn-start 每轮都发，是最常到的读数。
    if (!title.isEmpty()) {
        pluginLastReport_.insert(title, QDateTime::currentDateTimeUtc());
        pluginAwaitingReport_.remove(title);  // 收到上报就说明这条路径还活着
    }
    if (!pluginVersion.isEmpty() && !title.isEmpty()) {
        pluginLoaded_.insert(title, pluginVersion);
    }
    // 有些宿主的 hook 不带 raise（ZCode 的 stop hook 就没带）：按显示名反查
    // 白名单 exe 补上，否则待办卡 agent_exe 为空，外壳派发行永远不亮。
    QString raiseExe = raise;
    if (raiseExe.isEmpty() && !title.isEmpty()) {
        raiseExe = AgentWindow::exeForDisplayName(title);
    }
    if (kind == QStringLiteral("turn-start")) {
        // 同一 session 已经开始新对话：上一轮的 review 视为已看到，直接归档。
        // 上一轮缓存的派发回复也要清：新一轮若没回复，不能拿旧正文顶上。
        if (!sessionId.isEmpty()) {
            dispatchReplyBySession_.remove(sessionId);
        }
        if (workItems_ != nullptr && !sessionId.isEmpty() &&
            workItems_->resolveReviews(sessionId)) {
            refreshWorkItems();
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("新对话开始，旧验收已归档"), 0);
        }
        return;
    }
    if (workItems_ != nullptr &&
        (kind == QStringLiteral("permission") ||
         kind == QStringLiteral("question") ||
         kind == QStringLiteral("progress") || kind == QStringLiteral("stop"))) {
        QString cardMessage = message;
        if (kind == QStringLiteral("stop") && !sessionId.isEmpty()) {
            // 通用收尾文案（"做完了，等你回来看"）没有信息量：换成该会话最后
            // 一条回复正文，待办卡直接能读结果；要人动手的文案保留原语义。
            // bridge hook 已从 transcript 尾部提取好第一句并随 payload 带
            // "reply" 上来：直接用，绝不走 lastReply——那是主线程全文逐行读，
            // 大会话（开发轮次动辄几十 MB）能把事件循环卡住几十秒，期间
            // /requests 推送排队，桌宠卡弹不出来（2026-11-15 实测）。
            const bool actionable =
                message.startsWith(QStringLiteral("等你回话")) ||
                message.startsWith(QStringLiteral("等你选方案")) ||
                message.startsWith(QStringLiteral("等你确认")) ||
                message.startsWith(QStringLiteral("模型出错"));
            if (!reply.isEmpty()) {
                if (!actionable) {
                    cardMessage = reply;
                    appendEvent(QStringLiteral("status"), QString(), QString(),
                                QStringLiteral("会话回复：%1").arg(reply), 0);
                }
            } else {
                // 老插件（payload 没带 reply）才走全文读：ZCode 会话
                // （configHome 为空）走派发通道记下的回复。
                QString last = SessionDispatch::lastReply(
                    SessionDispatch::configHomeFor(raiseExe), sessionId);
                if (last.isEmpty()) {
                    last = dispatchReplyBySession_.value(sessionId);
                }
                if (!last.isEmpty() && !actionable) {
                    cardMessage = last;
                    appendEvent(QStringLiteral("status"), QString(), QString(),
                                QStringLiteral("会话回复：%1").arg(last), 0);
                }
            }
        }
        workItems_->upsert(QStringLiteral("hook"), raiseExe, sessionId, project,
                           kind, title, cardMessage, raiseExe, eventId);
        refreshWorkItems();
    }
    if (replay) {
        // 离线补投只落工单，不补喊/抢前台，避免启动时一次念一串旧提醒。
        return;
    }
    // 「等你回话」残留修复（2026-11-04）：Agent 若先用 pet_work_update 报过
    // ready/blocked（那时已经喊过一声），这一轮又用问句收尾，就别再喊第二声、
    // 再抢一次前台——工单上面已经更新成问句了。ready 的 90s「压 stop」只挡 stop
    // 事件，等回话走的是 permission，会绕过它，所以在这里单独补一道。
    // MCP 上报不带 session，账记在 title 名下：先按 session 找（DSH 等带 session
    // 的宿主能精确命中），找不到再按 title 找。命中即取走作废——只压这一次，
    // 不 lingering 去吞并行的别的会话。放在 announceWork 之前拦，压掉时就不记
    // 那条带「念：」的误导事件了。
    if (message.startsWith(QString::fromUtf8("等你回话"))) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QStringList keys;
        if (!sessionId.isEmpty()) {
            keys << sessionId;
        }
        if (!title.isEmpty() && title != sessionId) {
            keys << title;
        }
        bool announcedRecently = false;
        for (const QString &key : keys) {
            const QDateTime readyAt = readyAnnounced_.value(key);
            if (readyAt.isValid() && readyAt.secsTo(now) <= 90) {
                readyAnnounced_.remove(key);
                announcedRecently = true;
                break;
            }
            const QDateTime blockedAt = attentionAnnounced_.value(key);
            if (blockedAt.isValid() && blockedAt.secsTo(now) <= 120) {
                attentionAnnounced_.remove(key);
                announcedRecently = true;
                break;
            }
        }
        if (announcedRecently) {
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("等你回话已并入刚才的 ready/blocked 提醒，不重复喊"),
                        0);
            return;
        }
    }
    // 选方案认语义不认文案：question 类事件（DSH / OpenSquilla）直接算；
    // bridge 系把 question 映射成 permission 发，只能靠「等你选方案」前缀认。
    const bool planChoice =
        kind == QStringLiteral("question") ||
        message.startsWith(QString::fromUtf8("等你选方案"));
    // 语音紧急度（只读最紧急的待办，用户 2026-11-15）：授权/出错=3 >
    // 选方案/回话=2 > 完成=1。忙碌窗口内同级/更低的只冒气泡不插话。
    const int urgency =
        message.startsWith(QStringLiteral("等你确认")) ||
                message.startsWith(QStringLiteral("模型出错"))
            ? 3
            : (message.startsWith(QStringLiteral("等你选方案")) ||
               message.startsWith(QStringLiteral("等你回话")))
                  ? 2
                  : 1;
    announceWork(kind, title, message, spoken, raise, sessionId, planChoice,
                 eventId, noRaise, urgency, showBubble);
}

// 只负责「喊人」：事件流一行、记宿主、气泡 + 语音 + 要不要抢前台。
// 不碰工单——handleNotify 和 pet_work_update 都已经各自写过工单了，
// 再 upsert 一次会在 review 状态下多长出一张卡片（review 不在合并窗口里）。
void MainWindow::announceWork(const QString &kind, const QString &title,
                              const QString &message, const QString &spoken,
                              const QString &raise, const QString &sessionId,
                              bool planChoice, const QString &eventId,
                              bool noRaise, int priority, bool showBubble) {
    appendEvent(QStringLiteral("status"), QString(), QString(),
                spoken.isEmpty() || spoken == message
                    ? title + QStringLiteral(" · ") + message
                    : title + QStringLiteral(" · ") + message +
                          QStringLiteral("（念：") + spoken + QStringLiteral("）"),
                0);
    // 记住发通知的宿主，桌宠分流时要靠它判断「Agent 在不在」，
    // 冷启动也能直接用上次的值，不用重新猜。只在变了才写盘。
    if (!raise.isEmpty() &&
        store_->uiString(QStringLiteral("agent_exe")) != raise) {
        store_->setUiString(QStringLiteral("agent_exe"), raise);
    }
    if (kind == QStringLiteral("stop")) {
        // Agent 已经用 pet_work_update 报过 ready 并念过了：同一件事别念两遍。
        // 取走即作废，下一轮真的 stop 仍然照念。
        const QString readyKey = sessionId.isEmpty() ? title : sessionId;
        const QDateTime readyAt = readyAnnounced_.take(readyKey);
        if (readyAt.isValid() &&
            readyAt.secsTo(QDateTime::currentDateTimeUtc()) <= 90) {
            return;
        }
        // 任务完成：念出来 + 气泡停满 7s。**不自动抢前台**——
        // 活儿干完就把焦点拽走很烦，气泡可点，点了再回去。
        // 是否前台不影响是否朗读：用户可能没看屏幕，语音是主通道。
        if (voicePet_ != nullptr) {
            voicePet_->speak(spoken, 7000, raise, priority);
        }
        return;
    }
    if (kind == QStringLiteral("permission") ||
        kind == QStringLiteral("question")) {
        // 宿主对 AskUserQuestion 的 hook 是**用户答完之后**才派发的（WorkBuddy
        // 实测：问题 17:24 弹出，通知 17:27 才到），所以「等你选方案」这条常常
        // 迟到。Agent 若已经用 pet_work_update(blocked) 提前喊过，这里就不再喊
        // 第二声——工单上面已经更新过了，喊重复只会让人以为又出了新问题。
        const QString askKey = sessionId.isEmpty() ? title : sessionId;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (planChoice) {
            // 同一弹窗只喊一次：认 tool_use_id。同会话连问两道题时 id 不同，
            // 第二题照喊——按会话压 180s 会把第二题吞掉（2026-11-08 审查实测）。
            if (!eventId.isEmpty()) {
                const QDateTime seen = announcedPlanIds_.value(eventId);
                if (seen.isValid() && seen.secsTo(now) < 1800) {
                    return;
                }
            } else {
                // 老宿主不给 id：退回按会话压 180s（维持旧行为，宁多喊不吞新题
                // 依赖宿主补 id；OpenSquilla 桥已改成带事件身份）。
                const QDateTime announced = attentionAnnounced_.value(askKey);
                if (announced.isValid() && announced.secsTo(now) <= 180) {
                    return;
                }
            }
            // MCP 已报 blocked 并喊过：这一条是同一件事的晚到回声，只压一次。
            const QDateTime blockedAt = blockedAnnounced_.take(askKey);
            if (blockedAt.isValid() && blockedAt.secsTo(now) <= 180) {
                if (!eventId.isEmpty()) {
                    announcedPlanIds_.insert(eventId, now);
                }
                return;
            }
            if (!eventId.isEmpty()) {
                if (announcedPlanIds_.size() > 64) {
                    for (auto it = announcedPlanIds_.begin();
                         it != announcedPlanIds_.end();) {
                        if (it.value().secsTo(now) > 1800) {
                            it = announcedPlanIds_.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                announcedPlanIds_.insert(eventId, now);
            }
            attentionAnnounced_.insert(askKey, now);
        }
        // 等确认要把人喊回来：气泡停久一点 + 语音，桌宠收起也照念；
        // 收起又没 TTS 时退回托盘，别让提醒凭空消失。
        if (voicePet_ != nullptr) {
            voicePet_->speak(spoken, 7000, raise, priority, showBubble);
            if (!voicePet_->isVisible() && !voicePet_->ttsReady() &&
                tray_ != nullptr) {
                tray_->showMessage(title, message,
                                   QSystemTrayIcon::Information, 6000);
            }
        } else if (tray_ != nullptr) {
            tray_->showMessage(title, message,
                               QSystemTrayIcon::Information, 6000);
        }
        // noRaise（决策式授权 hook 带 "no_raise":true）：出声提醒保留，
        // 不抢前台——人正盯着桌宠卡片作答，把宿主拽回来等于把卡片作废。
        // 超时后宿主自己弹窗、自己抢焦点，不需要这里代劳。
        if (!noRaise) {
            AgentWindow::raise(raise);
        }
        return;
    }
    if (kind == QStringLiteral("speak")) {
        // MCP 主动让桌宠说话：只念，不抢前台。
        if (voicePet_ != nullptr) {
            voicePet_->speak(message);
        }
        return;
    }
    if (voicePet_ != nullptr && voicePet_->isVisible() &&
        voicePet_->petEnabled()) {
        voicePet_->notify(message, raise);
    } else if (tray_ != nullptr) {
        tray_->showMessage(title, message, QSystemTrayIcon::Information, 6000);
    }
}

void MainWindow::raiseWorkItem(const QString &id) {
    bringToAgent(id);
}

// 回到那条待办对应的 Agent：有窗口就切过去；窗口没了（被关掉、或收进托盘
// 拿不到可见窗口）就先把宿主启动起来——Electron 单实例，再启一次等于叫到前台。
// 启动不了返回 false：人没送到跟前，调用方不该把提醒销掉。
bool MainWindow::bringToAgent(const QString &id) {
    if (workItems_ == nullptr || id.isEmpty()) {
        return false;
    }
    QString exe;
    for (const WorkItem &item : workItems_->items()) {
        if (item.id == id) {
            exe = item.raiseExe.isEmpty() ? item.agentExe : item.raiseExe;
            break;
        }
    }
    if (AgentWindow::alive(exe)) {
        AgentWindow::raise(exe);
        return true;
    }
    if (exe.isEmpty()) {
        AgentWindow::raise(QString());  // 老数据没带宿主名：按缺省找 ZCode
        return true;
    }
    const QString appId = hostAppId(exe);
    QString error;
    if (appId.isEmpty()) {
        appendEvent(QStringLiteral("error"), QString(), QString(),
                    QString::fromUtf8("%1 不在应用表里，启动不了；提醒先留着")
                        .arg(AgentWindow::displayName(exe)),
                    0);
        return false;
    }
    if (!AppLauncher::launch(appId, &error)) {
        appendEvent(QStringLiteral("error"), QString(), QString(),
                    QString::fromUtf8("启动 %1 没成（%2）；提醒先留着")
                        .arg(AgentWindow::displayName(exe),
                             error.isEmpty() ? QStringLiteral("launch failed")
                                             : error),
                    0);
        return false;
    }
    return true;
}

void MainWindow::raiseSelectedWork() {
    raiseWorkItem(selectedWorkId());
}

// 外壳页的配色：只把 ThemeManager 已经解析好的 token 翻译成 CSS 能吃的写法。
QJsonObject MainWindow::webThemeJson() {
    // token 名与 style.qss 里的 @token@ 一致：网页不许自带一套配色，
    // 浅色/深色/美术主题（半透明卡片）都从这一份走。
    static const char *const kTokens[] = {
        "bg",   "card.bg",   "card.bg2",  "text",      "text.soft",
        "text.dim", "muted", "muted.2",   "ghost",     "faint",
        "line", "raised",    "accent",    "accent.hover", "on.accent",
        "danger", "warn",    "ok",        "pending",   "info",
        "w6",   "w8",        "w10",
        "w14",  "w16",       "w18",       "w30",       "a26",
        "a36",  "a64"};
    QJsonObject theme;
    ThemeManager &tm = ThemeManager::instance();
    // 用「已叠加卡片透明度」的调色板：网页卡片和 Qt 卡片必须同步变。
    const QHash<QString, QString> palette = tm.effectivePalette();
    for (const char *token : kTokens) {
        const QString value = cssColor(ThemeManager::parsePaletteColor(
            palette.value(QString::fromLatin1(token))));
        if (!value.isEmpty()) {
            theme.insert(QString::fromLatin1(token), value);
        }
    }
    theme.insert(QStringLiteral("fontScale"), tm.fontScale());
    // 首帧兜底色：背景图要等 Chromium 解码，先垫一层不透明纯色，别闪白。
    QColor base = ThemeManager::parsePaletteColor(
        palette.value(QStringLiteral("bg")));
    base.setAlpha(255);
    theme.insert(QStringLiteral("base"), cssColor(base));
    // 美术主题的插画走虚拟域名（素材目录只读映射），不把 3MB PNG 编进状态里。
    const QString art = tm.current().mainBackground;
    if (!art.isEmpty()) {
        const QString rel = QDir(ThemeManager::themesRoot()).relativeFilePath(art);
        if (!rel.isEmpty() && !QDir::isAbsolutePath(rel) &&
            !rel.startsWith(QStringLiteral("../"))) {
            theme.insert(QStringLiteral("image"),
                         QStringLiteral("https://agentrmc.theme/") + rel);
        }
    }
    // 压暗纱和 BackdropRoot::paintEvent 同一个公式：深色压黑、浅色压白。
    const QColor veil = tm.isLightActive()
                            ? QColor(255, 255, 255, int(255 * tm.backdropDim()))
                            : QColor(11, 14, 20, int(255 * tm.backdropDim()));
    theme.insert(QStringLiteral("veil"), cssColor(veil));
    return theme;
}

// WebView2 版外壳要的整份状态：待办队列 + Worker 列表 + 健康行 + 主题 + 事件流
// + 系统/按键映射/应用/引导四页，一次推过去。
QString MainWindow::itemIdForSession(const QString &sessionId) const {
    if (workItems_ == nullptr || sessionId.isEmpty()) {
        return QString();
    }
    for (const WorkItem &it : workItems_->items()) {
        if (it.sessionId == sessionId) {
            return it.id;
        }
    }
    return QString();
}

QJsonArray MainWindow::supportedDispatchExes() const {
    QJsonArray out;
    for (const QString &exe : AgentWindow::knownExes()) {
        // WorkBuddy 系走 UIA（configHomeFor 非空）；DSH / OpenSquilla 走插件
        // 通道；ZCode 走自带 app-server 协议通道。
        if (!SessionDispatch::configHomeFor(exe).isEmpty() ||
            SessionDispatch::supportsPluginChannel(exe) ||
            SessionDispatch::supportsZcodeChannel(exe)) {
            out.append(exe);
        }
    }
    return out;
}

QString MainWindow::workItemsJson() {
    if (workItems_ == nullptr) {
        return QStringLiteral("{}");
    }
    QJsonObject root =
        QJsonDocument::fromJson(workItems_->toJson().toUtf8()).object();
    QJsonArray items = root.value(QStringLiteral("items")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        QJsonObject obj = items.at(i).toObject();
        if (obj.value(QStringLiteral("agent_exe"))
                .toString()
                .compare(QStringLiteral("DeepSeek Harness.exe"),
                         Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString sessionId = obj.value(QStringLiteral("session_id")).toString();
        if (sessionId.isEmpty()) {
            continue;
        }
        const DshSessions::Info info = DshSessions::find(sessionId);
        if (!info.valid) {
            continue;
        }
        if (!info.title.isEmpty()) {
            obj.insert(QStringLiteral("session_title"), info.title);
        }
        // 老卡/workingDirectory 服务缺位时项目可能为空：投影里有 cwd 就补上，
        // 行身份不必退到「会话 <短 id>」。
        if (obj.value(QStringLiteral("project")).toString().isEmpty() &&
            !info.cwd.isEmpty()) {
            obj.insert(QStringLiteral("project"), info.cwd);
        }
        items.replace(i, obj);
    }
    root.insert(QStringLiteral("items"), items);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// OpenSquilla 目录只在桥还活着时端出去：桥每 30s 一拍（目录没变不重推），
// 两分钟没动静就是桥/网关已断，给页面一份过期菜单比不给更坏（选了必被拒）。
QJsonArray MainWindow::osqModelsFresh() const {
    if (osqModels_.isEmpty() || !osqModelsAt_.isValid()) {
        return {};
    }
    if (osqModelsAt_.secsTo(QDateTime::currentDateTimeUtc()) > 120) {
        return {};
    }
    return osqModels_;
}

QJsonObject MainWindow::dispatchStateJson() const {
    return QJsonObject{{QStringLiteral("notice"), dispatchNotice_},
                       {QStringLiteral("noticeId"), dispatchNoticeId_},
                       {QStringLiteral("noticeExe"), dispatchNoticeExe_},
                       {QStringLiteral("noticeRequest"), dispatchNoticeRequest_},
                       {QStringLiteral("noticeKind"), dispatchNoticeKind_},
                       {QStringLiteral("draftId"), dispatchDraftId_},
                       {QStringLiteral("draftText"), dispatchDraftText_},
                       {QStringLiteral("supported"), supportedDispatchExes()},
                       // 个人供应商模型目录：ZCode 会话派发时选 override 模型
                       // 用（账号套餐模型会话必须选，个人供应商可选切）。空目录
                       // 时页面把选择入口整个藏掉。思考强度一律「跟随会话」，
                       // 目录不占状态体积。
                       {QStringLiteral("models"),
                        ZcodeDispatch::personalProviderCatalog()},
                       // OpenSquilla 的目录另一个字段：网关 models.list 只有外部桥
                       // 能读（要 operator 连接），桥 POST /models 推来的账存这里，
                       // 页面按当前会话的 exe 取哪份。两宿主共用一条 models 会把
                       // ZCode 会话列出 OpenSquilla 的套餐模型（选了必被拒）。
                       {QStringLiteral("osqModels"), osqModelsFresh()},
                       // 思考强度档位（remote v4 滑杆同款）：override 可带
                       // options.reasoningLevel，滑杆最左＝跟随会话原值。
                       {QStringLiteral("thoughtLevels"),
                        QJsonArray{QStringLiteral("low"), QStringLiteral("medium"),
                                   QStringLiteral("high"), QStringLiteral("xhigh"),
                                   QStringLiteral("max")}}};
}

QString MainWindow::webShellState() {
    QJsonObject work;
    if (workItems_ != nullptr) {
        work = QJsonDocument::fromJson(workItemsJson().toUtf8()).object();
    }
    QJsonArray sections;
    for (const GuideSection &section : guideSections()) {
        QJsonArray rows;
        for (const auto &line : section.rows) {
            rows.append(QJsonArray{line.first, line.second});
        }
        sections.append(QJsonObject{{QStringLiteral("title"), section.title},
                                     {QStringLiteral("rows"), rows}});
    }
    const QJsonObject guide{
        {QStringLiteral("intro"), guideIntro()},
        {QStringLiteral("sections"), sections},
        {QStringLiteral("version"), appVersion()},
        {QStringLiteral("installSentence"), agentBridgeInstallSentence()},
    };
    const QJsonObject root{
        {QStringLiteral("work"), work},
        {QStringLiteral("workers"), workersArray()},
        {QStringLiteral("health"),
         QJsonDocument::fromJson(healthJson().toUtf8()).object()},
        {QStringLiteral("theme"), webThemeJson()},
        {QStringLiteral("events"), webEvents_},
        {QStringLiteral("system"), systemState()},
        {QStringLiteral("keymap"),
         keymapPage_ != nullptr ? keymapPage_->webState() : QJsonObject()},
        {QStringLiteral("apps"),
         appPage_ != nullptr ? appPage_->webState() : QJsonObject()},
        {QStringLiteral("guide"), guide},
        {QStringLiteral("header"),
         QJsonObject{
             {QStringLiteral("listening"),
              engine_ != nullptr && engine_->isRunning()},
             {QStringLiteral("recording"), voiceRecording_},
             {QStringLiteral("maximized"), isMaximized() || isFullScreen()}}},
        {QStringLiteral("left"), leftShellState()},
        {QStringLiteral("dispatch"), dispatchStateJson()},
        {QStringLiteral("chat"), chatState()},
        // 宿主窗口诊断：遥控派发报「找不到宿主窗口」时，先看这几个数就知道是
        // 没启动、被隐藏、还是映像名对不上（2026-11-19 排查用，留着成本很低）。
        {QStringLiteral("winDiag"),
         QJsonObject{{QStringLiteral("WorkBuddy.exe"),
                      AgentWindow::diagnose(QStringLiteral("WorkBuddy.exe"))},
                     {QStringLiteral("WorkBuddyAI.exe"),
                      AgentWindow::diagnose(QStringLiteral("WorkBuddyAI.exe"))},
                     {QStringLiteral("byTitle"),
                      AgentWindow::mainWindowByTitle(QStringLiteral("WorkBuddy"))
                              != nullptr
                          ? QStringLiteral("found")
                          : QStringLiteral("null")}}},
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// ---------- 局域网手机遥控（M1）：与外壳同一份数据源，动作同一个入口 ----------

void MainWindow::ensureLanServer() {
    if (lan_ != nullptr) {
        return;
    }
    lan_ = new LanWebServer(this);
    lan_->setStateProvider([this] { return lanShellState(); });
    lan_->setTranscriptProvider([this](const QString &id, int offset, int limit) {
        return lanTranscript(id, offset, limit);
    });
    lan_->setSessionListProvider([this] { return lanSessionsJson(); });
    lan_->setSessionTranscriptProvider(
        [this](const QString &exe, const QString &session, int offset, int limit) {
            return lanSessionTranscript(exe, session, offset, limit);
        });
    lan_->setUploadHandler(
        [this](const QJsonObject &meta, const QByteArray &data) {
            return lanUpload(meta, data);
        });
    // 流式上传优先：/m/upload 走边收边写盘，内存恒定、不限大小（LocalSend 同思路）。
    // uploadHandler 仍保留，作为 provider 没配时的整包兜底。
    lan_->setUploadStreamProviders(
        [this](const QJsonObject &meta, const QString &filename) {
            return lanUploadTarget(meta, filename);
        },
        [this](const QJsonObject &meta, const QString &tmpPath,
               const QString &filename) {
            return lanUploadCommit(meta, tmpPath, filename);
        });
    // 分片上传（并发 + 续传）：init → N 次 PUT chunk → done 合并。
    lan_->setChunkUploadProviders(
        [this](const QJsonObject &meta, const QString &filename, qint64 size) {
            return lanChunkInit(meta, filename, size);
        },
        [this](const QString &uploadId, int index, const QByteArray &data) {
            return lanChunkWrite(uploadId, index, data);
        },
        [this](const QString &uploadId) { return lanChunkDone(uploadId); },
        [this](const QString &uploadId) { lanChunkAbort(uploadId); });
    lan_->setInboxListProvider(
        [this](const QJsonObject &meta) { return lanInboxList(meta); });
    lan_->setInboxFileProvider(
        [this](const QJsonObject &meta) { return lanInboxFile(meta); });
    lan_->setThemeRoot(ThemeManager::themesRoot());
    // 白名单已在服务端验过；这里与桌面外壳走同一个 handleWebAction，
    // 做完它内部统一 refresh，桌面外壳自动跟着刷新，双端一致免费。
    lan_->setActionHandler([this](const QJsonObject &action) {
        // 任意会话派发：不像待办派发那样有 work id，成功/拒绝要立刻回给手机。
        if (action.value(QStringLiteral("cmd")).toString() ==
            QStringLiteral("dispatchSession")) {
            const QString why = dispatchToSession(
                action.value(QStringLiteral("agent")).toString(),
                action.value(QStringLiteral("session")).toString(),
                action.value(QStringLiteral("project")).toString(),
                action.value(QStringLiteral("text")).toString(),
                action.value(QStringLiteral("model")).toObject());
            if (!why.isEmpty()) {
                return QJsonObject{{QStringLiteral("ok"), false},
                                   {QStringLiteral("code"), 409},
                                   {QStringLiteral("error"), why}};
            }
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("code"), 200}};
        }
        handleWebAction(action);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("code"), 200}};
    });
    connect(lan_, &LanWebServer::devicesChanged, this,
            [this](const QJsonArray &devices) {
                store_->setUiArray(QStringLiteral("lan_devices"), devices);
            });
}

void MainWindow::setLanEnabled(bool on) {
    store_->setUiBool(QStringLiteral("lan_enabled"), on);
    if (on) {
        ensureLanServer();
        lan_->setDevices(store_->uiArray(QStringLiteral("lan_devices")));
        if (lan_->start()) {
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("手机访问已开启（端口 45916）"), 0);
        } else {
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QString::fromUtf8("手机访问启动失败：端口 45916 被占用"),
                        0);
        }
    } else if (lan_ != nullptr) {
        lan_->endPairing();
        lan_->stop();
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QString::fromUtf8("手机访问已关闭"), 0);
    }
}

QString MainWindow::lanShellState() {
    QJsonObject work;
    if (workItems_ != nullptr) {
        work = QJsonDocument::fromJson(workItemsJson().toUtf8()).object();
    }
    const QJsonObject root{
        {QStringLiteral("work"), work},
        {QStringLiteral("health"),
         QJsonDocument::fromJson(healthJson().toUtf8()).object()},
        // 手机页不再展示事件流：目录/聊天/派发都用独立路由，轮询体里别带它。
        {QStringLiteral("theme"), webThemeJson()},
        {QStringLiteral("dispatch"), dispatchStateJson()},
        {QStringLiteral("meta"),
         QJsonObject{
             {QStringLiteral("version"), appVersion()},
             {QStringLiteral("listening"),
              engine_ != nullptr && engine_->isRunning()},
             {QStringLiteral("devices"),
              lan_ != nullptr ? lan_->devices().size() : 0}}},
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QJsonObject MainWindow::lanState() {
    const bool enabled = store_->uiBool(QStringLiteral("lan_enabled"), false);
    const bool running = lan_ != nullptr && lan_->isRunning();
    return QJsonObject{
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("running"), running},
        {QStringLiteral("port"), static_cast<int>(LanWebServer::kPort)},
        {QStringLiteral("urls"),
         running ? QJsonArray::fromStringList(
                       LanWebServer::lanUrls(lan_->port()))
                 : QJsonArray()},
        // 配对码只进桌面外壳（systemState），不进 lanShellState——手机拿不到。
        {QStringLiteral("pairing"),
         lan_ != nullptr ? lan_->pairingInfo() : QJsonObject()},
        {QStringLiteral("devices"),
         lan_ != nullptr ? lan_->devices() : QJsonArray()},
    };
}

// ---------- 聊天页：待办 → 会话记录（只读分页，电脑/手机同一读取器） ----------

// B2/B3：给推送来的待答请求标可答性（answerable）。优先信宿主插件/桥下发的
// answerable 提示（DSH 提问恒真、授权仅 approvalInline 开启时真；OpenSquilla
// 审批/澄清恒真——网关是按 id 可随时 resolve 的干净 RPC）；没带提示的老推送
// 退回「只认提问」。再叠加 15s 心跳在线判据——插件掉线时退回只读，别让用户
// 点了石沉大海。
QJsonArray MainWindow::requestsWithAnswerable(const QString &sessionId) const {
    QJsonArray out;
    const QJsonArray reqs = pendingRequests_.value(sessionId);
    bool pluginLive = false;
    for (auto it = pluginSeenAt_.constBegin(); it != pluginSeenAt_.constEnd(); ++it) {
        if (it.value().secsTo(QDateTime::currentDateTimeUtc()) <= 15) {
            pluginLive = true;
            break;
        }
    }
    for (const QJsonValue &v : reqs) {
        QJsonObject r = v.toObject();
        const bool hint = r.contains(QStringLiteral("answerable"))
                              ? r.value(QStringLiteral("answerable")).toBool()
                              : r.value(QStringLiteral("kind")).toString() ==
                                    QStringLiteral("question");
        r.insert(QStringLiteral("answerable"), pluginLive && hint);
        out.append(r);
    }
    return out;
}

// B2/B3：聊天页 answer 与桌宠交互卡共用的落收件箱逻辑。kind 从待答清单认，
// 校验后插 pendingAnswers_ 等插件/桥取件。返回拒绝原因（空=已排队）。
QString MainWindow::enqueueAnswer(const QString &workItemId, const QString &requestId,
                                  const QJsonArray &answers, const QString &outcome) {
    const WorkItem *item = nullptr;
    for (const WorkItem &it : workItems_->items()) {
        if (it.id == workItemId) {
            item = &it;
            break;
        }
    }
    if (item == nullptr || requestId.isEmpty()) {
        return QStringLiteral("应答缺字段（待办/requestId）");
    }
    if (!SessionDispatch::supportsInlineAnswer(item->agentExe)) {
        return QStringLiteral("该宿主暂不支持内联应答");
    }
    QString kind;
    const QJsonArray reqs = pendingRequests_.value(item->sessionId);
    for (const QJsonValue &v : reqs) {
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("id")).toString() == requestId) {
            kind = r.value(QStringLiteral("kind")).toString();
            break;
        }
    }
    if (kind.isEmpty()) {
        return QStringLiteral("这条请求已不在待答清单（可能刚被别处应答）");
    }
    if (kind == QStringLiteral("question") && answers.isEmpty()) {
        return QStringLiteral("提问应答缺 answers");
    }
    if (kind == QStringLiteral("approval") &&
        outcome != QStringLiteral("allowed-once") &&
        outcome != QStringLiteral("rejected")) {
        return QStringLiteral("授权应答 outcome 须为 allowed-once/rejected");
    }
    PendingAnswer pa;
    pa.sessionId = item->sessionId;
    pa.kind = kind;
    pa.id = requestId;
    pa.answers = answers;
    pa.outcome = outcome;
    pa.exe = item->agentExe;
    pa.enqueuedAt = QDateTime::currentDateTimeUtc();
    pendingAnswers_.insert(QUuid::createUuid().toString(QUuid::WithoutBraces), pa);
    dispatchNotice_ = kind == QStringLiteral("approval")
                          ? QStringLiteral("授权应答已排队，等宿主确认…")
                          : QStringLiteral("应答已排队，等插件送 DSH…");
    dispatchNoticeKind_ = QStringLiteral("good");
    dispatchNoticeId_ = workItemId;
    dispatchNoticeExe_ = item->agentExe;
    // 归属请求 id 必须在这里设：这是**唯一**设置提示文案的地方，而应答入口有三条
    // （桌宠卡 / 桌面聊天页 / 手机页）。以前只有后两条在 handleWebAction 里设它，
    // 从桌宠卡应答时字段还是旧值 → ackAnswer 的清除条件对不上 → 手机底部那条
    // 「授权应答已排队，等宿主确认…」永远挂着（2026-11-16 用户实测）。
    dispatchNoticeRequest_ = requestId;
    appendEvent(QStringLiteral("status"), QString(), QString(),
                QStringLiteral("内联应答已排队：会话 %1 %2 %3")
                    .arg(item->sessionId.left(8), kind, requestId.left(12)),
                0);
    return QString();
}

// 重启对账（2026-11-16 用户实测「重启后旧卡变死卡」）：工单落盘、待答请求账
// 只在内存，所以上一个进程留下的「等你处理」卡重启后仍然挂着，可它对应的授权/
// 提问早就不在插件账上了——点允许只会得到「这条请求已不在待答清单」。
//
// 为什么不能靠 /requests 推送触发对账：插件（以及 bridge 系 hook）**只推还有
// 待答请求的会话**，死卡对应的会话重启后根本不会再被推一次，等推送等于永远等
// 不到。所以判据改成两条，都成立才归档：
//   ① 该宿主插件已在线（/request-answer/pending 每 1.2s 来一拍，与有无待答无关，
//      是唯一可靠的在线读数；等满 kPluginWarmupMs 才动手，避开启动瞬间的账未建期）；
//   ② 这个会话当前在待答账里没有请求（插件推过空集，或压根没推过——后者同样
//      意味着没有待答）。
// 只动 legacyAttentionIds_：本次运行新建的卡不进这个集合，永远不会被误杀。
void MainWindow::reconcileLegacyAttention() {
    if (workItems_ == nullptr || legacyAttentionIds_.isEmpty()) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QSet<QString> liveExes;  // 已在线且过了预热期的宿主（小写）
    for (auto it = pluginAliveSince_.constBegin();
         it != pluginAliveSince_.constEnd(); ++it) {
        if (it.value().msecsTo(now) >= kPluginWarmupMs) {
            liveExes.insert(it.key());
        }
    }
    if (liveExes.isEmpty()) {
        return;  // 插件还没证明自己在跑：账可能还没建好，别急着杀
    }
    QVector<QString> stale;
    for (const WorkItem &item : workItems_->items()) {
        if (item.done || item.stage != QStringLiteral("needs-me")) {
            continue;
        }
        if (!legacyAttentionIds_.contains(item.id)) {
            continue;  // 本次运行新建的卡：不碰
        }
        if (!liveExes.contains(item.agentExe.toLower())) {
            continue;  // 这个宿主的插件没在线：它的账不可信，留着卡
        }
        if (pendingRequests_.contains(item.sessionId)) {
            continue;  // 该会话仍有待答请求：卡是活的
        }
        stale.append(item.id);
    }
    if (stale.isEmpty()) {
        return;
    }
    for (const QString &id : stale) {
        workItems_->ack(id);
        legacyAttentionIds_.remove(id);
    }
    appendEvent(QStringLiteral("status"), QString(), QString(),
                QStringLiteral("重启后对账：%1 张旧待答卡对应的请求已不存在，"
                               "已归档（重启前遗留，点了也不会生效）")
                    .arg(stale.size()),
                0);
    refreshWorkItems();
}

// B2：插件 1.2s 轮询取走本 exe 的应答件（未取走的超时 60s 作废：插件没在跑，
// 别攒一肚子旧答案）。下发后标 handed，ack 时核销。session 非空时只取该会话
// 的（bridge 系并发授权 hook 各自轮询，不按会话分会互踩别人家的应答件）。
QString MainWindow::takeAnswerPending(const QString &exe, const QString &session) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonArray out;
    for (auto it = pendingAnswers_.begin(); it != pendingAnswers_.end();) {
        if (it->enqueuedAt.secsTo(now) > 60) {
            // 60s 没人取件 = 插件/桥根本没在跑：这条答案作废，但**必须告诉用户**
            // ——否则又是「点了没反应」（2026-11-16 实测这类静默失败最误导）。
            const QString deadId = it->id;
            const QString deadSid = it->sessionId;
            dropAnswerPendingForSession(deadSid);
            it = pendingAnswers_.erase(it);
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QStringLiteral("应答超时作废：宿主插件 60 秒内没来取件"
                                       "（插件没在运行？）"),
                        0);
            if (voicePet_ != nullptr) {
                voicePet_->reportAnswerFailure(
                    deadId, QStringLiteral("宿主插件没来取件，答案没送到——"
                                           "检查 DSH 里插件是否在运行"));
            }
            if (dispatchNoticeRequest_ == deadId) {
                dispatchNotice_ = QStringLiteral("应答超时作废：插件没来取件");
                dispatchNoticeKind_ = QStringLiteral("bad");
            }
            continue;
        }
        if (!it->handed && (exe.isEmpty() ||
                            it->exe.compare(exe, Qt::CaseInsensitive) == 0) &&
            (session.isEmpty() || it->sessionId == session)) {
            it->handed = true;
            QJsonObject item{{QStringLiteral("answer_id"), it.key()},
                             {QStringLiteral("session_id"), it->sessionId},
                             {QStringLiteral("kind"), it->kind},
                             {QStringLiteral("id"), it->id}};
            if (it->kind == QStringLiteral("question")) {
                item.insert(QStringLiteral("answers"), it->answers);
            } else {
                item.insert(QStringLiteral("outcome"), it->outcome);
            }
            out.append(item);
        }
        ++it;
    }
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}

// B2：插件应答回执。ok=true：从账上核销 + 推空刷新聊天页（卡片由插件的推送
// 消失，这里只清自己这侧）；ok=false：保留请求展示并把原因写进事件流。
void MainWindow::ackAnswer(const QString &answerId, bool ok, const QString &error) {
    const auto it = pendingAnswers_.constFind(answerId);
    if (it == pendingAnswers_.constEnd()) {
        return;
    }
    const PendingAnswer pa = it.value();
    if (ok) {
        pendingAnswers_.remove(answerId);
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QStringLiteral("内联应答送达：会话 %1 %2")
                        .arg(pa.sessionId.left(8), pa.kind), 0);
        // 插件确认送达：现在才收卡、顶下一张。
        if (voicePet_ != nullptr) {
            voicePet_->acceptAnswer(pa.id);
        }
        // 提示文案按「归属请求」清；同时兜一层：这条请求已经不在这条会话的待答账里
        // 了（插件重推过空集/被别处答掉），那条提示同样该撤——否则一旦归属字段
        // 因任何原因对不上，手机底部就会永久挂着一条过期提示。
        const bool noticeGone =
            dispatchNoticeRequest_ == pa.id ||
            pendingRequests_.value(pa.sessionId).isEmpty();
        if (noticeGone) {
            dispatchNotice_.clear();
            dispatchNoticeKind_.clear();
            dispatchNoticeRequest_.clear();
        }
    } else {
        // 插件明确拒绝（不可达/无活表/界面没在线/请求已不在等待）：撤掉这条待发，
        // 避免反复取件；请求卡留在 pendingRequests_（插件推送为准），聊天页仍可
        // 重试。拒绝原因也要回到卡上——这条路径才是「点了允许却没生效」最常见
        // 的现场（旧插件没 prepend 抢不到瀑布时回的就是「已不在等待」）。
        pendingAnswers_.remove(answerId);
        appendEvent(QStringLiteral("error"), QString(), QString(),
                    QStringLiteral("内联应答失败：%1").arg(error), 0);
        if (voicePet_ != nullptr) {
            voicePet_->reportAnswerFailure(pa.id, error);
        }
        dispatchNoticeKind_ = QStringLiteral("bad");
        dispatchNotice_ = error.isEmpty()
                              ? QStringLiteral("应答未生效，请重试")
                              : error;
        dispatchNoticeId_ = itemIdForSession(pa.sessionId);
        dispatchNoticeRequest_ = pa.id;
    }
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
}

// 会话的插件账没了（推送空列表 = 卡片在 DSH 侧被关闭/已答）：连带清待发件。
void MainWindow::dropAnswerPendingForSession(const QString &sessionId) {
    for (auto it = pendingAnswers_.begin(); it != pendingAnswers_.end();) {
        if (it->sessionId == sessionId) {
            it = pendingAnswers_.erase(it);
        } else {
            ++it;
        }
    }
}

QJsonObject MainWindow::chatStateFor(const QString &id, int offset, int limit) {
    if (workItems_ == nullptr || id.isEmpty()) {
        return QJsonObject();
    }
    const WorkItem *item = nullptr;
    for (const WorkItem &it : workItems_->items()) {
        if (it.id == id) {
            item = &it;
            break;
        }
    }
    if (item == nullptr) {
        return QJsonObject();
    }
    const SessionTranscript::Result r =
        SessionTranscript::read(item->agentExe, item->sessionId, limit, offset);
    return QJsonObject{
        {QStringLiteral("id"), item->id},
        {QStringLiteral("title"), item->title},
        {QStringLiteral("agentExe"), item->agentExe},
        // 待办会话页顶栏要跟派发页同款格式（Agent 名 · 项目名），
        // agentName 由 native 归一（AgentWindow::displayName）。
        {QStringLiteral("agentName"),
         AgentWindow::displayName(item->agentExe)},
        {QStringLiteral("sessionId"), item->sessionId},
        {QStringLiteral("stage"), item->stage},
        // 手机页顶栏副标题用：待办标题往往只是宿主名（WorkBuddy），
        // 补个项目工作目录才知道这条会话是哪个仓库的。
        {QStringLiteral("project"), item->project},
        {QStringLiteral("supported"), r.supported},
        {QStringLiteral("error"), r.error},
        {QStringLiteral("sessionTitle"), r.title},
        // 会话当前模型（宿主记的显示名）；空＝读不到，页面显示「跟随会话」。
        {QStringLiteral("model"), r.model},
        {QStringLiteral("messages"), r.messages},
        {QStringLiteral("total"), r.total},
        {QStringLiteral("hasMore"), r.hasMore},
        {QStringLiteral("offset"), offset},
        // B1：该会话当前待答请求（DSH 插件推送），聊天页渲染；B2 带可答性。
        {QStringLiteral("requests"), requestsWithAnswerable(item->sessionId)},
    };
}

QJsonObject MainWindow::chatState() {
    if (chatItemId_.isEmpty()) {
        return QJsonObject();
    }
    const QJsonObject chat = chatStateFor(chatItemId_);
    if (chat.isEmpty()) {
        chatItemId_.clear();  // 待办被清掉了，退回列表
        return QJsonObject();
    }
    return chat;
}

QString MainWindow::lanTranscript(const QString &id, int offset, int limit) {
    const QJsonObject chat = chatStateFor(id, offset, limit);
    if (chat.isEmpty()) {
        return QString();  // 待办不存在 → 服务端回 404
    }
    return QString::fromUtf8(QJsonDocument(chat).toJson(QJsonDocument::Compact));
}

// 手机「派发」：最近会话目录。这里只读宿主索引，不改变 PC 当前会话；
// 每个 Agent 最多 12 条，页面按项目分组，点进去再走 lanSessionTranscript。
QString MainWindow::lanSessionsJson() {
    const QJsonArray supportedArray = supportedDispatchExes();
    QStringList supported;
    for (const QJsonValue &v : supportedArray) {
        supported.append(v.toString());
    }
    QVector<QPair<QDateTime, QJsonObject>> orderedAgents;
    for (const QString &exe : AgentWindow::knownExes()) {
        // 不再只取最近 12 条：那个上限会整组吞掉较老的项目。200 条覆盖
        // 本机所有常规宿主会话；目录页只按选中 Agent 渲染，不会一次铺满。
        const QVector<SessionTranscript::SessionInfo> sessions =
            SessionTranscript::listSessions(exe, 200);
        if (sessions.isEmpty()) {
            continue;
        }
        QDateTime latest;
        QJsonArray rows;
        for (const SessionTranscript::SessionInfo &info : sessions) {
            QJsonObject row{
                {QStringLiteral("id"), info.id},
                {QStringLiteral("title"), info.title},
                {QStringLiteral("project"), info.project},
                {QStringLiteral("updated_at"), info.updatedAt},
                {QStringLiteral("archived"), info.archived},
                // 宿主自动生成的临时工作区（WorkBuddy 日期目录等）：单任务没有
                // 真项目，目录页不能拿目录名当项目名分组。
                {QStringLiteral("generated"),
                 SessionTranscript::generatedWorkspace(info.project)},
            };
            const WorkItem *active = nullptr;
            if (workItems_ != nullptr) {
                for (const WorkItem &item : workItems_->items()) {
                    if (!item.done && item.sessionId == info.id) {
                        active = &item;  // 同会话只留一张活卡，后写覆盖即可
                    }
                }
            }
            if (active != nullptr) {
                row.insert(QStringLiteral("work_id"), active->id);
                row.insert(QStringLiteral("stage"), active->stage);
            }
            // 手机页只对确有派发通道的宿主开放输入；活卡状态继续显示，
            // 不把「这个会话有卡」误当成「这个宿主能接收手机派发」。
            row.insert(QStringLiteral("can_dispatch"),
                       supported.contains(exe) && !info.archived);
            row.insert(QStringLiteral("can_delete"),
                       info.archived && SessionCleaner::supportsHost(exe));
            rows.append(row);
            const QDateTime updated =
                QDateTime::fromString(info.updatedAt, Qt::ISODateWithMs);
            if (updated.isValid() &&
                (!latest.isValid() || updated > latest)) {
                latest = updated;
            }
        }
        const QJsonObject agent{
            {QStringLiteral("exe"), exe},
            {QStringLiteral("name"), AgentWindow::displayName(exe)},
            {QStringLiteral("alive"), AgentWindow::processAlive(exe)},
            {QStringLiteral("window"), AgentWindow::alive(exe)},
            {QStringLiteral("can_dispatch"), supported.contains(exe)},
            {QStringLiteral("count"), rows.size()},
            {QStringLiteral("sessions"), rows},
        };
        orderedAgents.append({latest, agent});
    }
    std::sort(orderedAgents.begin(), orderedAgents.end(),
              [](const QPair<QDateTime, QJsonObject> &a,
                 const QPair<QDateTime, QJsonObject> &b) {
                  return a.first > b.first;
              });
    QJsonArray agents;
    for (const auto &entry : orderedAgents) {
        agents.append(entry.second);
    }
    const QJsonObject root{
        {QStringLiteral("agents"), agents},
        {QStringLiteral("generated_at"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
    };
    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString MainWindow::lanSessionTranscript(const QString &agentExe,
                                         const QString &sessionId, int offset,
                                         int limit) {
    if (agentExe.isEmpty() || sessionId.isEmpty()) {
        return QString();
    }
    const SessionTranscript::Result r =
        SessionTranscript::read(agentExe, sessionId, limit, offset);
    QJsonArray reqs = requestsWithAnswerable(sessionId);
    // 任意会话没有稳定的待办 id，应答入口先保持只读；要代答请走待办卡。
    for (int i = 0; i < reqs.size(); ++i) {
        QJsonObject req = reqs.at(i).toObject();
        req.insert(QStringLiteral("answerable"), false);
        reqs.replace(i, req);
    }
    const QJsonObject chat{
        {QStringLiteral("agentExe"), agentExe},
        {QStringLiteral("agentName"), AgentWindow::displayName(agentExe)},
        {QStringLiteral("sessionId"), sessionId},
        {QStringLiteral("title"), r.title},
        {QStringLiteral("sessionTitle"), r.title},
        // 会话当前模型（宿主记的显示名）；空＝读不到，页面显示「跟随会话」。
        {QStringLiteral("model"), r.model},
        {QStringLiteral("supported"), r.supported},
        {QStringLiteral("error"), r.error},
        {QStringLiteral("messages"), r.messages},
        {QStringLiteral("total"), r.total},
        {QStringLiteral("hasMore"), r.hasMore},
        {QStringLiteral("offset"), offset},
        {QStringLiteral("requests"), reqs},
    };
    return QString::fromUtf8(QJsonDocument(chat).toJson(QJsonDocument::Compact));
}

QString MainWindow::dispatchToSession(const QString &agentExe,
                                      const QString &sessionId,
                                      const QString &project,
                                      const QString &text,
                                      const QJsonObject &modelOverride) {
    if (dispatch_ == nullptr) {
        return QStringLiteral("派发通道还没就绪");
    }
    if (sessionId.trimmed().isEmpty() || text.trimmed().isEmpty()) {
        return QStringLiteral("会话或派发文案为空");
    }
    // 不信任页面传来的 exe：只允许与桌面端同一份派发能力清单。
    const QJsonArray supported = supportedDispatchExes();
    if (!supported.contains(QJsonValue(agentExe))) {
        return QStringLiteral("这个 Agent 暂不支持手机派发");
    }
    // 宿主没在跑时先把它拉起来——**这才是「网关没连上」的真正原因**：
    // OpenSquilla 的网关是它自己的子服务，进程不在就没有网关，外部桥只会回
    // 「网关没连上，未注入」（用户 2026-11-19 一句话点破）。
    // 拉起沿用遥控器那套（AppLauncher），和 UI 派发的兜底同源。
    if (!AgentWindow::processRunning(agentExe)) {
        const QString appId = AppLauncher::appIdForExe(agentExe);
        if (!appId.isEmpty()) {
            QString launchErr;
            if (AppLauncher::launch(appId, &launchErr)) {
                // 冷启动：等进程出现（最多 3s），再给桥/网关 1.5s 重连。
                bool up = false;
                for (int i = 0; i < 30; ++i) {  // 进程最多等 9s
                    QCoreApplication::processEvents(
                        QEventLoop::ExcludeUserInputEvents);
                    QThread::msleep(300);
                    if (AgentWindow::processRunning(agentExe)) {
                        up = true;
                        break;
                    }
                }
                if (!up) {
                    return QStringLiteral("%1 启动失败（进程没起来），先在电脑上手动打开它")
                        .arg(AgentWindow::displayName(agentExe));
                }
                // 进程起来了还得等它自己的网关就绪（OpenSquilla 冷启动约 22s）。
                // 桥取件时网关没通只会回「网关没连上」，所以这里必须等端口。
                const quint16 gw = gatewayPortFor(agentExe);
                if (gw != 0 && !waitPortOpen(gw, 40000)) {
                    return QStringLiteral("%1 已启动，但网关（:%2）没就绪；"
                                          "等它加载完再派发一次")
                        .arg(AgentWindow::displayName(agentExe))
                        .arg(gw);
                }
                if (gw != 0) {
                    // 端口通 ≠ 桥已连上：外部桥是独立进程，断线后自己的重连
                    // 计时器（封顶 3s）可能还没落地。这时派发会让桥在
                    // gatewayUp=false 下把这条拒掉——「第一条指令被吞」就是
                    // 这么来的（2026-11-19 桥日志：02s 拒单、18s 才重连）。
                    for (int i = 0; i < 8; ++i) {
                        QCoreApplication::processEvents(
                            QEventLoop::ExcludeUserInputEvents);
                        QThread::msleep(500);
                    }
                }
            }
        }
    }
    QString reason;
    QString dispatchId;
    const QString cleaned = text.trimmed();
    if (!dispatch_->dispatch(agentExe, sessionId, project, cleaned, &reason,
                             &dispatchId, modelOverride)) {
        dispatchNotice_ = reason;
        dispatchNoticeKind_ = QStringLiteral("bad");
        dispatchNoticeId_ = itemIdForSession(sessionId);
        dispatchNoticeExe_ = agentExe;
        appendEvent(QStringLiteral("error"), QString(), QString(),
                    QStringLiteral("手机派发未发：%1").arg(reason), 0);
        refreshWorkItems();
        if (workWeb_ != nullptr) {
            workWeb_->refresh();
        }
        return reason;
    }
    dispatchSessionById_.insert(dispatchId, sessionId);
    dispatchReplyBySession_.remove(sessionId);
    dispatchNotice_ =
        QStringLiteral("已派往会话 %1，等回执…").arg(sessionId.left(8));
    dispatchNoticeKind_ = QStringLiteral("good");
    dispatchNoticeId_ = itemIdForSession(sessionId);
    dispatchNoticeExe_ = agentExe;
    appendEvent(QStringLiteral("status"), QString(), QString(),
                QStringLiteral("手机派发：%1 → 会话 %2")
                    .arg(cleaned.left(40), sessionId.left(8)),
                0);
    refreshWorkItems();
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
    return QString();
}

bool MainWindow::resolveInboxTarget(const QJsonObject &meta, QString *project,
                                    QString *sessionKey) const {
    const QString id = meta.value(QStringLiteral("id")).toString();
    QString proj = meta.value(QStringLiteral("project")).toString().trimmed();
    QString sess = meta.value(QStringLiteral("session")).toString().trimmed();
    // 会话上下文两条来源：① 待办 id（队列里的活，project/sessionId 可信）；
    // ② 客户端带上来的 session/project（从派发页直接进会话时没有待办）。
    if (workItems_ != nullptr && !id.isEmpty()) {
        for (const WorkItem &it : workItems_->items()) {
            if (it.id == id) {
                if (!it.project.isEmpty()) {
                    proj = it.project;
                }
                sess = it.sessionId.isEmpty() ? it.id : it.sessionId;
                break;
            }
        }
    }
    if (proj.isEmpty() && sess.isEmpty()) {
        return false;
    }
    // project 可能是客户端带上来的路径：只接受**已存在的绝对目录**、拒盘根。
    // 校验不过就丢掉（退到中央 uploads 目录），绝不按客户端给的坏路径写。
    if (!proj.isEmpty()) {
        const QFileInfo pi(proj);
        const QString norm = QDir::fromNativeSeparators(proj);
        if (!pi.isDir() || !pi.isAbsolute() || norm.size() <= 3) {
            proj.clear();
        }
    }
    // sessionKey 直接拼进路径：去掉分隔符与控制字符，杜绝 .. 穿越。
    sess.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")));
    sess = sess.trimmed();
    if (sess.isEmpty() || sess == QLatin1String("..") || sess == QLatin1String(".")) {
        sess = QStringLiteral("adhoc");
    }
    *project = proj;
    *sessionKey = sess;
    return true;
}

// 上传文件名清洗：只取 basename、去路径分隔符与控制字符、限长保扩展名。
// 服务端不信任客户端文件名，杜绝 ../ 穿越。整包上传和流式上传共用这一份。
QString sanitizeUploadName(const QString &raw) {
    QString name = raw;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        name = name.mid(slash + 1);
    }
    name.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")));
    name = name.trimmed();
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        name = QStringLiteral("file");
    }
    if (name.size() > 120) {
        name = name.right(120);
    }
    return name;
}

QString MainWindow::inboxDirFor(const QJsonObject &meta) const {
    QString project, session;
    if (!resolveInboxTarget(meta, &project, &session)) {
        return QString();
    }
    if (!project.isEmpty()) {
        return project + QStringLiteral("/.agentrmc/inbox/") + session;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/uploads/") + session;
}

QJsonObject MainWindow::lanUpload(const QJsonObject &meta,
                                  const QByteArray &data) {
    const auto fail = [](int code, const QString &why) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("code"), code},
                           {QStringLiteral("error"), why}};
    };
    QString filename = meta.value(QStringLiteral("filename")).toString();
    QString project, sessionKey;
    if (!resolveInboxTarget(meta, &project, &sessionKey)) {
        return fail(404, QStringLiteral("待办不存在"));
    }
    if (data.isEmpty()) {
        return fail(400, QStringLiteral("空文件"));
    }
    // 文件名 sanitize：只取 basename，去路径分隔符/控制字符，限长保扩展名。
    // 服务端不信任客户端文件名，杜绝 ../ 穿越。
    const QString name = sanitizeUploadName(filename);
    // 落盘：会话工作目录下 .agentrmc/inbox/<session>/（agent 天然可达）；
    // 无项目退统一 uploads 目录。时间戳前缀防重名。
    QString dir;
    if (!project.isEmpty()) {
        dir = project + QStringLiteral("/.agentrmc/inbox/") + sessionKey;
    } else {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
              QStringLiteral("/uploads/") + sessionKey;
    }
    const QString stamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString path = dir + QLatin1Char('/') + stamp + QLatin1Char('-') + name;
    if (!QDir().mkpath(dir)) {
        return fail(400, QStringLiteral("无法创建上传目录"));
    }
    // 项目相对落盘时自动写 .agentrmc/.gitignore（忽略全部），防止上传文件被误
    // 提交进项目仓库（用户 2026-11-14 顾虑仓库污染）；中央 uploads 目录无需。
    if (!project.isEmpty()) {
        const QString gi = project + QStringLiteral("/.agentrmc/.gitignore");
        if (!QFileInfo::exists(gi)) {
            QFile g(gi);
            if (g.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                g.write("*\n");
                g.close();
            }
        }
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(400, QStringLiteral("无法写入文件"));
    }
    if (f.write(data) != data.size()) {
        f.close();
        return fail(400, QStringLiteral("写入不完整"));
    }
    f.close();
    const QString abs = QFileInfo(path).absoluteFilePath();
    // 落盘器两条来源共用（手机上传 / 桌面 Ctrl+V 粘贴），事件流按来源记。
    const bool pasted =
        meta.value(QStringLiteral("source")).toString() == QLatin1String("paste");
    appendEvent(QStringLiteral("status"), QString(), QString(),
                (pasted ? QStringLiteral("粘贴图片：%1")
                        : QStringLiteral("上传文件：%1"))
                    .arg(name),
                0);
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("code"), 200},
                       {QStringLiteral("path"), abs},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("size"), data.size()}};
}

// 流式上传①：header 阶段就要目标临时路径。算会话 inbox、建目录、定名，
// 返回 "<dir>/<时间戳>-<安全名>.part"；空串＝定位不了（服务端回 400）。
QString MainWindow::lanUploadTarget(const QJsonObject &meta,
                                    const QString &filename) {
    const QString dir = inboxDirFor(meta);
    if (dir.isEmpty()) {
        return QString();
    }
    QDir().mkpath(dir);
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    return dir + QLatin1Char('/') + stamp + QLatin1Char('-') +
           sanitizeUploadName(filename) + QStringLiteral(".part");
}

// 流式上传②：字节收满了收尾——.part 改名成正式文件、记一条事件流。
// 失败时返回 ok=false（服务端会顺手把 .part 删掉）。
QJsonObject MainWindow::lanUploadCommit(const QJsonObject &meta,
                                        const QString &tmpPath,
                                        const QString &filename) {
    const auto fail = [](const QString &why) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), why}};
    };
    Q_UNUSED(filename)
    if (!QFileInfo::exists(tmpPath)) {
        return fail(QStringLiteral("临时文件不见了"));
    }
    QString finalPath = tmpPath;
    finalPath.chop(5);  // 去掉 ".part"
    if (!QFile::rename(tmpPath, finalPath)) {
        return fail(QStringLiteral("改名失败"));
    }
    const QFileInfo fi(finalPath);
    // 事件流按来源记（桌面 Ctrl+V 也走这条落盘器）。
    const bool pasted =
        meta.value(QStringLiteral("source")).toString() == QLatin1String("paste");
    appendEvent(QStringLiteral("status"), QString(), QString(),
                (pasted ? QStringLiteral("粘贴图片：%1")
                        : QStringLiteral("上传文件：%1"))
                    .arg(fi.fileName()),
                0);
    QJsonObject out{{QStringLiteral("ok"), true},
                    {QStringLiteral("code"), 200},
                    {QStringLiteral("path"), fi.absoluteFilePath()},
                    {QStringLiteral("name"), fi.fileName()},
                    {QStringLiteral("size"), static_cast<double>(fi.size())}};
    const QString inlined = inlineTextOf(fi.absoluteFilePath(), fi.size());
    if (!inlined.isEmpty()) {
        out.insert(QStringLiteral("text"), inlined);
    }
    return out;
}

// 分片上传①：建会话。分片落 <inbox>/.parts/<uploadId>/<index>，done 时按序
// 合并成正式文件。顺带清掉一小时没动静的残局（手机半路退出的遗留）。
LanWebServer::ChunkInit MainWindow::lanChunkInit(const QJsonObject &meta,
                                                 const QString &filename,
                                                 qint64 size) {
    LanWebServer::ChunkInit out;
    const QString dir = inboxDirFor(meta);
    if (dir.isEmpty()) {
        out.error = QStringLiteral("定位不到会话目录");
        return out;
    }
    if (size <= 0) {
        out.error = QStringLiteral("文件是空的");
        return out;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = chunkSessions_.begin(); it != chunkSessions_.end();) {
        if (now - it->lastAt > 60LL * 60 * 1000) {
            QDir(it->dir).removeRecursively();
            it = chunkSessions_.erase(it);
        } else {
            ++it;
        }
    }
    QDir().mkpath(dir);
    const QString uploadId = QUuid::createUuid().toString(QUuid::Id128);
    const QString partsDir =
        dir + QStringLiteral("/.parts/") + uploadId;
    if (!QDir().mkpath(partsDir)) {
        out.error = QStringLiteral("建分片目录失败");
        return out;
    }
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    ChunkSession s;
    s.dir = partsDir;
    s.name = stamp + QLatin1Char('-') + sanitizeUploadName(filename);
    s.finalPath = dir + QLatin1Char('/') + s.name;
    s.total = size;
    s.chunkSize = 8 * 1024 * 1024;  // 8MB/片：重试便宜，也不至于请求太碎
    s.chunks = static_cast<int>((size + s.chunkSize - 1) / s.chunkSize);
    s.lastAt = now;
    chunkSessions_.insert(uploadId, s);
    out.ok = true;
    out.uploadId = uploadId;
    out.chunkSize = s.chunkSize;
    out.chunks = s.chunks;
    return out;
}

// 分片上传②：写一片。重复写同一片是幂等的（直接覆盖），断点续传靠这个。
QJsonObject MainWindow::lanChunkWrite(const QString &uploadId, int index,
                                      const QByteArray &data) {
    auto it = chunkSessions_.find(uploadId);
    if (it == chunkSessions_.end()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("上传会话不存在（可能已超时）")}};
    }
    ChunkSession &s = *it;
    if (index < 0 || index >= s.chunks) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QStringLiteral("分片序号越界")}};
    }
    const QString path = s.dir + QLatin1Char('/') + QString::number(index);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QStringLiteral("写分片失败")}};
    }
    f.close();
    s.got.insert(index);
    s.lastAt = QDateTime::currentMSecsSinceEpoch();
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("received"), s.got.size()},
                       {QStringLiteral("chunks"), s.chunks}};
}

// 分片上传③：按序合并。缺片时回 missing，前端只补那几片（真续传）。
QJsonObject MainWindow::lanChunkDone(const QString &uploadId) {
    auto it = chunkSessions_.find(uploadId);
    if (it == chunkSessions_.end()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("上传会话不存在（可能已超时）")}};
    }
    const ChunkSession s = *it;
    if (s.got.size() != s.chunks) {
        QJsonArray missing;
        for (int i = 0; i < s.chunks; ++i) {
            if (!s.got.contains(i) && missing.size() < 64) {
                missing.append(i);
            }
        }
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QStringLiteral("分片不全")},
                           {QStringLiteral("missing"), missing}};
    }
    QFile out(s.finalPath);
    if (!out.open(QIODevice::WriteOnly)) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("建目标文件失败")}};
    }
    bool bad = false;
    for (int i = 0; i < s.chunks && !bad; ++i) {
        QFile part(s.dir + QLatin1Char('/') + QString::number(i));
        if (!part.open(QIODevice::ReadOnly)) {
            bad = true;
            break;
        }
        while (!part.atEnd()) {
            const QByteArray block = part.read(256 * 1024);
            if (out.write(block) != block.size()) {
                bad = true;
                break;
            }
        }
    }
    out.close();
    if (bad || out.size() != s.total) {
        QFile::remove(s.finalPath);  // 合并出问题不留残文件
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QStringLiteral("合并失败")}};
    }
    QDir(s.dir).removeRecursively();
    chunkSessions_.erase(it);
    appendEvent(QStringLiteral("status"), QString(), QString(),
                QStringLiteral("上传文件：%1").arg(s.name), 0);
    QJsonObject result{{QStringLiteral("ok"), true},
                       {QStringLiteral("path"), s.finalPath},
                       {QStringLiteral("name"), s.name},
                       {QStringLiteral("size"), static_cast<double>(s.total)}};
    const QString inlined = inlineTextOf(s.finalPath, s.total);
    if (!inlined.isEmpty()) {
        result.insert(QStringLiteral("text"), inlined);
    }
    return result;
}

void MainWindow::lanChunkAbort(const QString &uploadId) {
    auto it = chunkSessions_.find(uploadId);
    if (it == chunkSessions_.end()) {
        return;
    }
    QDir(it->dir).removeRecursively();
    chunkSessions_.erase(it);
}

// 手机「电脑文件」：列会话 inbox。空目录也回合法空列表——前端据此显示
// 「还没有文件」，不要用 404 把整个面板当错误处理。
// 前置声明：shareableExt 定义在 lanInboxList 之后（同一 TU 内共用白名单）。
bool shareableExt(const QString &path);

// 工作区产物：agent 刚生成的文件（安装包 / 截图 / 文档），给手机端「产物」区。
// 规则：project 下递归，跳过依赖与版本库等黑名单目录；只要白名单扩展名、
// 最近 24h 内修改的；按修改时间倒序最多 30 个。扫描有 800ms 预算，超了就用
// 已收集的（大仓库不至于把主窗卡住）。
QJsonArray lanProjectArtifacts(const QString &project) {
    QJsonArray arr;
    if (project.isEmpty() || !QDir(project).exists()) {
        return arr;
    }
    static const QStringList kSkip = {
        QStringLiteral("node_modules"), QStringLiteral(".git"),
        QStringLiteral(".agentrmc"),    QStringLiteral(".workbuddy"),
        QStringLiteral(".agents"),      QStringLiteral("build"),
        QStringLiteral(".vs"),          QStringLiteral(".vscode"),
        QStringLiteral("coverage"),     QStringLiteral("tmp_t"),
        QStringLiteral("tmp_test"),     QStringLiteral("__pycache__")};
    const qint64 cutoff =
        QDateTime::currentDateTime().addSecs(-24 * 3600).toMSecsSinceEpoch();
    struct Item {
        QString rel;
        qint64 size = 0;
        qint64 mtime = 0;
    };
    QVector<Item> items;
    QElapsedTimer budget;
    budget.start();
    // 自己递归而不是 QDirIterator：iterator 带 Subdirectories 会照样钻进
    // node_modules/.git，800ms 预算全耗在巨库里，产物一个都扫不到
    // （2026-11-19 用户实测「产物 0」就是它）。黑名单必须在**进目录前**跳。
    std::function<void(const QString &, const QString &, int)> walk =
        [&](const QString &dir, const QString &prefix, int depth) {
            if (depth > 6 || items.size() >= 500 || budget.elapsed() > 800) {
                return;
            }
            const QFileInfoList infos =
                QDir(dir).entryInfoList(QDir::Files | QDir::Dirs |
                                            QDir::NoDotAndDotDot |
                                            QDir::NoSymLinks,
                                        QDir::Time);
            for (const QFileInfo &fi : infos) {
                if (budget.elapsed() > 800 || items.size() >= 500) {
                    return;
                }
                const QString name = fi.fileName();
                if (kSkip.contains(name, Qt::CaseInsensitive)) {
                    continue;  // 进目录之前跳，不递归进去
                }
                if (fi.isDir()) {
                    walk(fi.absoluteFilePath(), prefix + name + QLatin1Char('/'),
                         depth + 1);
                } else if (shareableExt(name)) {
                    const qint64 m = fi.lastModified().toMSecsSinceEpoch();
                    if (m >= cutoff) {
                        items.append({prefix + name, fi.size(), m});
                    }
                }
            }
        };
    walk(project, QString(), 0);
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.mtime > b.mtime; });
    int n = 0;
    for (const Item &item : items) {
        if (n++ >= 30) {
            break;
        }
        // path/dir：桌面「产物与文件」面板要左键打开、右键跳目录，得给本机
        // 绝对路径（手机页只读 rel/name，多余字段无副作用）。
        const QString abs = QDir(project).filePath(item.rel);
        arr.append(QJsonObject{
            {QStringLiteral("rel"), item.rel},
            {QStringLiteral("name"), QFileInfo(item.rel).fileName()},
            {QStringLiteral("size"), static_cast<double>(item.size)},
            {QStringLiteral("path"), abs},
            {QStringLiteral("dir"), QFileInfo(abs).absolutePath()},
            {QStringLiteral("mtime"),
             QDateTime::fromMSecsSinceEpoch(item.mtime)
                 .toUTC()
                 .toString(Qt::ISODate)}});
    }
    return arr;
}

QString MainWindow::lanInboxList(const QJsonObject &meta) {
    const QString dir = inboxDirFor(meta);
    if (dir.isEmpty()) {
        return QString();
    }
    QJsonArray arr;
    // NoSymLinks：inbox 里出现符号链接一律不列（下载侧还有 canonical 校验）。
    const QFileInfoList infos =
        QDir(dir).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Time);
    int n = 0;
    for (const QFileInfo &fi : infos) {
        if (n++ >= 100) {
            break;
        }
        arr.append(QJsonObject{
            {QStringLiteral("name"), fi.fileName()},
            {QStringLiteral("size"), static_cast<double>(fi.size())},
            {QStringLiteral("path"), fi.absoluteFilePath()},
            {QStringLiteral("dir"), fi.absolutePath()},
            {QStringLiteral("mtime"),
             fi.lastModified().toUTC().toString(Qt::ISODate)}});
    }
    QJsonObject out{{QStringLiteral("ok"), true},
                    {QStringLiteral("files"), arr}};
    // 产物区：agent 在会话工作区里生成的文件。拿不到 project 就不给。
    QString project, session;
    if (resolveInboxTarget(meta, &project, &session) && !project.isEmpty()) {
        out.insert(QStringLiteral("artifacts"), lanProjectArtifacts(project));
    }
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

// 下载 inbox 里的一个文件。name 由客户端给、不可信：只认 basename，再用
// canonical 前缀校验兜住符号链接——不许读 inbox 以外任何东西。
// 手机能下载的文件扩展名白名单。工作区里有 .env / 私钥 / 源码，不能因为
// 「能给 agent 写文件」就把整个目录放开。只放产物类：媒体、文档、压缩包、安装包。
bool shareableExt(const QString &path) {
    static const QSet<QString> kOk = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("webp"), QStringLiteral("bmp"),
        QStringLiteral("svg"),  QStringLiteral("ico"),  QStringLiteral("mp4"),
        QStringLiteral("mov"),  QStringLiteral("webm"), QStringLiteral("mkv"),
        QStringLiteral("mp3"),  QStringLiteral("wav"),  QStringLiteral("m4a"),
        QStringLiteral("pdf"),  QStringLiteral("md"),   QStringLiteral("txt"),
        QStringLiteral("doc"),  QStringLiteral("docx"), QStringLiteral("xls"),
        QStringLiteral("xlsx"), QStringLiteral("ppt"),  QStringLiteral("pptx"),
        QStringLiteral("zip"),  QStringLiteral("7z"),   QStringLiteral("rar"),
        QStringLiteral("exe"),  QStringLiteral("msi"),  QStringLiteral("apk")};
    const QString ext = QFileInfo(path).suffix().toLower();
    return !ext.isEmpty() && kOk.contains(ext);
}

LanWebServer::Download MainWindow::lanInboxFile(const QJsonObject &meta) {
    const auto fail = [] { return LanWebServer::Download{}; };
    // 两种作用域：
    //   （默认）inbox —— 手机传上来的、桌面粘进来的，只认 basename；
    //   project —— agent 在会话工作区里产出的文件（图/视频/安装包），允许子目录，
    //             但要 canonical 校验 + 扩展名白名单。
    const QString scope = meta.value(QStringLiteral("scope")).toString();
    const bool projectScope = scope == QLatin1String("project");
    QString dir;
    if (projectScope) {
        QString project, session;
        if (!resolveInboxTarget(meta, &project, &session) || project.isEmpty()) {
            return fail();
        }
        dir = project;
    } else {
        dir = inboxDirFor(meta);
    }
    if (dir.isEmpty()) {
        return fail();
    }
    QString name = projectScope
                       ? meta.value(QStringLiteral("rel")).toString()
                       : meta.value(QStringLiteral("name")).toString();
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!projectScope) {
        name = name.mid(name.lastIndexOf(QLatin1Char('/')) + 1);
    }
    name.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")));
    // 去掉 .. 段与前导斜杠：canonical 校验是最后一道，这里先粗筛。
    name.remove(QRegularExpression(QStringLiteral("(^|/)\\.\\.?(/|$)")));
    name.remove(QRegularExpression(QStringLiteral("^/+")));
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return fail();
    }
    if (projectScope && !shareableExt(name)) {
        return fail();
    }
    const QString canonDir = QFileInfo(dir).canonicalFilePath();
    const QString canonFile =
        QFileInfo(dir + QLatin1Char('/') + name).canonicalFilePath();
    if (canonDir.isEmpty() || canonFile.isEmpty()
        || !canonFile.startsWith(canonDir + QLatin1Char('/'), Qt::CaseInsensitive)) {
        return fail();
    }
    QFile f(canonFile);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 100LL * 1024 * 1024) {
        f.close();
        return fail();
    }
    LanWebServer::Download out;
    const QByteArray data = f.readAll();
    f.close();
    const QString mime =
        QMimeDatabase()
            .mimeTypeForFile(canonFile, QMimeDatabase::MatchExtension)
            .name();
    out.ok = true;
    out.name = name;
    out.data = data;
    out.contentType =
        mime.isEmpty() ? QByteArray("application/octet-stream") : mime.toUtf8();
    // Chrome 在 HTTP 明文下拦 .exe/.msi/.apk：包层 zip 才能顺滑下载。
    if (riskyDownloadExt(QFileInfo(name).suffix())) {
        const QString inner = QFileInfo(name).fileName();
        out.data = zipSingleStore(inner, data);
        out.name = inner + QStringLiteral(".zip");
        out.contentType = QByteArray("application/zip");
    }
    return out;
}

// 页面上的按钮/双击回到这里：全部转给既有实现，不在页面里另起一套状态机。
// 改键菜单、组合键捕获、添加/移除应用这类重交互弹 Qt 原生弹窗，网页只负责
// 把动作送回来；做完统一重推一次状态。
QJsonObject MainWindow::voiceCardState() {
    const VoiceOptions options = voice_->options();
    const bool hintSeen =
        store_->uiBool(QStringLiteral("voice_hint_seen"), false);
    QJsonObject o{
        {QStringLiteral("backend"), voiceBackendKey(options.backend)},
        {QStringLiteral("status"), voice_->statusText()},
        {QStringLiteral("transcript"), lastTranscript_},
        {QStringLiteral("testEnabled"),
         options.backend != VoiceBackend::Hotkey}};
    if (options.backend == VoiceBackend::Hotkey) {
        o.insert(QStringLiteral("kind"), QStringLiteral("accent"));
        o.insert(QStringLiteral("foot"),
                 QStringLiteral("0 模型 · 跟随前台场景转发"));
        o.insert(QStringLiteral("footVisible"), !hintSeen);
    } else if (options.backend == VoiceBackend::CloudAsr) {
        const bool ready = voice_->modelReady();
        o.insert(QStringLiteral("kind"),
                 ready ? QStringLiteral("dim") : QStringLiteral("warn"));
        o.insert(QStringLiteral("foot"),
                 QStringLiteral("云端转写 · 音频将上传到所填服务"));
        o.insert(QStringLiteral("footVisible"), true);
    } else if (voice_->modelReady()) {
        o.insert(QStringLiteral("kind"), QStringLiteral("dim"));
        o.insert(QStringLiteral("foot"),
                 QStringLiteral("本地转写 · 音频不出本机"));
        o.insert(QStringLiteral("footVisible"), !hintSeen);
    } else {
        o.insert(QStringLiteral("kind"), QStringLiteral("drop"));
        o.insert(QStringLiteral("status"),
                 QStringLiteral("⇣  把模型包（.zip）拖到这里导入"));
        o.insert(QStringLiteral("foot"),
                 QStringLiteral("未安装模型 · 拖入模型包，或点「模型设置」"));
        o.insert(QStringLiteral("footVisible"), true);
    }
    return o;
}

QJsonObject MainWindow::remoteCardState() {
    QJsonArray entries;
    if (remotePanel_ != nullptr) {
        for (const RemotePanel::Entry &e : remotePanel_->entries()) {
            entries.append(QJsonObject{
                {QStringLiteral("vk"), e.vk},
                {QStringLiteral("glyph"), e.glyph},
                {QStringLiteral("label"), e.label},
                {QStringLiteral("action"), e.action},
                {QStringLiteral("infrared"), e.infrared}});
        }
    }
    QJsonArray modeKeys;
    if (remotePanel_ != nullptr) {
        for (int vk : remotePanel_->modeKeys()) {
            modeKeys.append(vk);
        }
    }
    return QJsonObject{
        {QStringLiteral("badge"),
         remoteBadge_ != nullptr ? remoteBadge_->text()
                                 : QString::fromUtf8("待机")},
        {QStringLiteral("state"),
         remoteState_ != nullptr ? remoteState_->text() : QString()},
        {QStringLiteral("listening"),
         engine_ != nullptr && engine_->isRunning()},
        {QStringLiteral("entries"), entries},
        {QStringLiteral("modeKeys"), modeKeys},
        {QStringLiteral("linkedVk"),
         remotePanel_ != nullptr ? remotePanel_->linkedVk() : 0}};
}

QJsonObject MainWindow::leftShellState() {
    return QJsonObject{
        {QStringLiteral("form"),
         leftStack_ != nullptr && leftStack_->currentIndex() == 1},
        {QStringLiteral("voice"), voiceCardState()},
        {QStringLiteral("picker"),
         cardPicker_ != nullptr ? cardPicker_->webState() : QJsonObject()},
        {QStringLiteral("remote"), remoteCardState()},
        {QStringLiteral("vpage"),
         embeddedVoice_ != nullptr ? embeddedVoice_->webState()
                                   : QJsonObject()}};
}

void MainWindow::handleWebAction(const QJsonObject &action) {
    const QString cmd = action.value(QStringLiteral("cmd")).toString();
    const QString id = action.value(QStringLiteral("id")).toString();
    // 电脑外壳「派发」页的数据通道：目录和任意会话记录都按需推脚本，
    // 不进 5 秒整窗 state。LAN 的 /m/sessions 走 LanWebServer 独立 provider。
    if (cmd == QStringLiteral("sessionsLoad")) {
        if (workWeb_ != nullptr) {
            workWeb_->pushSessions(
                QJsonDocument::fromJson(lanSessionsJson().toUtf8()).object());
        }
        return;
    }
    if (cmd == QStringLiteral("sessionOpen") ||
        cmd == QStringLiteral("sessionOlder")) {
        const QString exe = action.value(QStringLiteral("exe")).toString();
        const QString session =
            action.value(QStringLiteral("session")).toString();
        const int offset = action.value(QStringLiteral("offset")).toInt(0);
        if (workWeb_ != nullptr) {
            workWeb_->pushSessionChat(QJsonDocument::fromJson(
                lanSessionTranscript(exe, session, offset, 50).toUtf8())
                                          .object());
        }
        return;
    }
    // 桌面聊天页 📎：弹系统选文件对话框，把**本机绝对路径**回推给页面当附件
    // （同机不复制文件，agent 直接按路径读；手机那条相反，必须先把字节传上来）。
    // WebView2 的 HTML5 拖拽拿不到本机路径，所以入口只留这一个对话框。
    if (cmd == QStringLiteral("pickFiles")) {
        const QString plane = action.value(QStringLiteral("plane")).toString(
            QStringLiteral("dchat"));
        const QStringList picked = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择要交给 Agent 的文件"));
        if (!picked.isEmpty() && workWeb_ != nullptr) {
            QJsonArray arr;
            for (const QString &p : picked) {
                const QFileInfo fi(p);
                arr.append(QJsonObject{
                    {QStringLiteral("name"), fi.fileName()},
                    {QStringLiteral("path"), fi.absoluteFilePath()},
                    {QStringLiteral("size"), static_cast<double>(fi.size())}});
            }
            workWeb_->pushFiles(QJsonObject{{QStringLiteral("plane"), plane},
                                            {QStringLiteral("files"), arr}});
        }
        return;
    }
    // 桌面聊天页 Ctrl+V 截图：剪贴板里的图页面只能拿到 blob（没有本机路径），
    // 所以把 base64 送上来、由 native 落进会话 inbox——跟手机上传同一个落盘器
    // （lanUpload），回来后还是「路径当附件」那套。落盘失败也要回一句，
    // 否则页面那边点了粘贴毫无反应。
    if (cmd == QStringLiteral("savePaste")) {
        const QString plane = action.value(QStringLiteral("plane")).toString(
            QStringLiteral("dchat"));
        const QJsonObject meta{
            {QStringLiteral("filename"), action.value(QStringLiteral("name"))},
            {QStringLiteral("id"), action.value(QStringLiteral("id"))},
            {QStringLiteral("session"), action.value(QStringLiteral("session"))},
            {QStringLiteral("project"), action.value(QStringLiteral("project"))},
            {QStringLiteral("source"), QStringLiteral("paste")}};
        const QByteArray data = QByteArray::fromBase64(
            action.value(QStringLiteral("data")).toString().toLatin1());
        const QJsonObject res = lanUpload(meta, data);
        if (workWeb_ != nullptr) {
            if (res.value(QStringLiteral("ok")).toBool()) {
                workWeb_->pushFiles(QJsonObject{
                    {QStringLiteral("plane"), plane},
                    {QStringLiteral("files"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("name"), res.value(QStringLiteral("name"))},
                         {QStringLiteral("path"), res.value(QStringLiteral("path"))},
                         {QStringLiteral("size"),
                          res.value(QStringLiteral("size"))}}}}});
            } else {
                workWeb_->pushFiles(QJsonObject{
                    {QStringLiteral("plane"), plane},
                    {QStringLiteral("error"),
                     res.value(QStringLiteral("error")).toString()}});
            }
        }
        return;
    }
    // 桌面聊天页「产物与文件」面板：数据不进 5 秒整窗 state，问了才给。
    // 响应走专用回推 __setFilePanel（pushFiles 的附件语义不串用）。
    if (cmd == QStringLiteral("filesList")) {
        const QString resp =
            lanInboxList(action.value(QStringLiteral("meta")).toObject());
        if (workWeb_ != nullptr) {
            QJsonObject obj = resp.isEmpty()
                                  ? QJsonObject()
                                  : QJsonDocument::fromJson(resp.toUtf8()).object();
            obj.insert(QStringLiteral("ok"), resp.isEmpty() ? false : true);
            obj.insert(QStringLiteral("plane"),
                       action.value(QStringLiteral("plane")).toString());
            workWeb_->pushFilePanel(QString::fromUtf8(
                QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        }
        return;
    }
    // 面板行左键＝本机打开、右键＝资源管理器定位。桌面是同机信任面（与
    // pickFiles 同级），但只认绝对存在的文件/目录，不拼不猜。
    if (cmd == QStringLiteral("openFile") ||
        cmd == QStringLiteral("revealFile")) {
        const QString path = QDir::fromNativeSeparators(
            action.value(QStringLiteral("path")).toString().trimmed());
        const QFileInfo fi(path);
        const QString why = [&] {
            if (path.isEmpty() || !fi.isAbsolute()) {
                return QStringLiteral("路径无效");
            }
            if (cmd == QStringLiteral("openFile")) {
                if (!fi.exists()) {
                    return QStringLiteral("文件已经不在了");
                }
                return QDesktopServices::openUrl(
                           QUrl::fromLocalFile(fi.absoluteFilePath()))
                           ? QString()
                           : QStringLiteral("本机没有能打开它的默认程序");
            }
            // 资源管理器：/select 只在目标存在时高亮；目录直接进目录。
            if (!fi.exists()) {
                return QStringLiteral("文件已经不在了");
            }
            const QString native = QDir::toNativeSeparators(fi.absoluteFilePath());
            if (fi.isDir()) {
                QProcess::startDetached(QStringLiteral("explorer.exe"), {native});
                return QString();
            }
            QProcess::startDetached(QStringLiteral("explorer.exe"),
                                    {QStringLiteral("/select,"), native});
            return QString();
        }();
        if (!why.isEmpty() && workWeb_ != nullptr) {
            const QJsonObject err{{QStringLiteral("error"), why},
                                  {QStringLiteral("op"), cmd},
                                  {QStringLiteral("plane"),
                                   action.value(QStringLiteral("plane")).toString()}};
            workWeb_->pushFilePanel(QString::fromUtf8(
                QJsonDocument(err).toJson(QJsonDocument::Compact)));
        }
        return;
    }
    if (cmd == QStringLiteral("dispatchSession")) {
        const QString why = dispatchToSession(
            action.value(QStringLiteral("exe")).toString(),
            action.value(QStringLiteral("session")).toString(),
            action.value(QStringLiteral("project")).toString(),
            action.value(QStringLiteral("text")).toString(),
            action.value(QStringLiteral("model")).toObject());
        if (workWeb_ != nullptr) {
            workWeb_->pushSessionResult(
                QJsonObject{{QStringLiteral("ok"), why.isEmpty()},
                            {QStringLiteral("error"), why}});
        }
        return;
    }
    if (cmd == QStringLiteral("sessionDelete")) {
        const QJsonArray items = action.value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) {
            if (workWeb_ != nullptr) {
                workWeb_->pushDeleteResult(
                    QJsonObject{{QStringLiteral("ok"), false},
                                {QStringLiteral("deleted"), 0},
                                {QStringLiteral("error"),
                                 QStringLiteral("没有选择会话")}});
            }
            return;
        }
        QStringList lines;
        for (int i = 0; i < items.size() && i < 40; ++i) {
            const QJsonObject item = items.at(i).toObject();
            lines.append(QStringLiteral("%1 · %2 · %3")
                             .arg(item.value(QStringLiteral("title")).toString(),
                                  item.value(QStringLiteral("exe")).toString(),
                                  item.value(QStringLiteral("project")).toString()));
        }
        if (items.size() > 40) {
            lines.append(QStringLiteral("…另有 %1 条").arg(items.size() - 40));
        }
        raise();
        activateWindow();
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("删除已归档会话"));
        box.setText(QStringLiteral("将同步删除 %1 条已归档会话。").arg(items.size()));
        box.setInformativeText(QStringLiteral(
            "删除前会先完整备份到 AgentRMC 数据目录的 session-trash。"
            "宿主中的原会话会被移除，备份仅用于人工找回。"));
        box.setDetailedText(lines.join(QLatin1Char('\n')));
        QPushButton *erase = box.addButton(QStringLiteral("删除并备份"),
                                           QMessageBox::DestructiveRole);
        QPushButton *cancel = box.addButton(QStringLiteral("取消"),
                                            QMessageBox::RejectRole);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != erase) {
            if (workWeb_ != nullptr) {
                workWeb_->pushDeleteResult(
                    QJsonObject{{QStringLiteral("ok"), false},
                                {QStringLiteral("canceled"), true},
                                {QStringLiteral("deleted"), 0},
                                {QStringLiteral("error"),
                                 QStringLiteral("已取消")}});
            }
            return;
        }
        QString error;
        QJsonObject result = SessionCleaner::removeArchived(items, &error);
        if (!error.isEmpty() && !result.contains(QStringLiteral("error"))) {
            result.insert(QStringLiteral("error"), error);
        }
        if (workWeb_ != nullptr) {
            workWeb_->pushDeleteResult(result);
            // 目录立刻重推一份，删除成功/部分失败都能反映真实剩余项。
            workWeb_->pushSessions(
                QJsonDocument::fromJson(lanSessionsJson().toUtf8()).object());
        }
        return;
    }
    if (cmd == QStringLiteral("open")) {
        openWorkItem(id);
        return;
    }
    if (cmd == QStringLiteral("raiseZcode")) {
        // ZCode 派发通知态的 Enter/双击：只聚焦不销账——对话区还没重启，
        // 人看到的是陈旧视图，不能算「等我看」的账已结。
        raiseWorkItem(id);
        return;
    }
    if (cmd == QStringLiteral("clearLog")) {
        clearLog();
    } else if (cmd == QStringLiteral("sysRefresh")) {
        watchdogTick();
    } else if (cmd == QStringLiteral("sysOpenDir")) {
        const QString dir = QFileInfo(spoolPath_).absolutePath();
        if (!dir.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    } else if (cmd == QStringLiteral("sysUpgrade")) {
        upgradePlugins();
    } else if (cmd == QStringLiteral("sysBridge")) {
        showAgentBridgeDialog();
    } else if (cmd == QStringLiteral("lanToggle")) {
        // 手机访问开关：桌面专用命令，不在 LAN 白名单里（手机不能自己开自己）。
        setLanEnabled(action.value(QStringLiteral("on")).toBool());
    } else if (cmd == QStringLiteral("cursorToggle")) {
        setCursorEnabled(action.value(QStringLiteral("on")).toBool());
    } else if (cmd == QStringLiteral("lanPair")) {
        ensureLanServer();
        lan_->beginPairing();
    } else if (cmd == QStringLiteral("lanPairEnd")) {
        if (lan_ != nullptr) {
            lan_->endPairing();
        }
    } else if (cmd == QStringLiteral("lanRevoke")) {
        if (lan_ != nullptr && lan_->revokeDevice(id)) {
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("已吊销一台手机的访问令牌"), 0);
        }
    } else if (cmd == QStringLiteral("chatOpen")) {
        // 点待办进聊天页：打开即最新一页（读取器从末尾切），旧记录上翻再加载。
        chatItemId_ = id;
        // 补一拍刷新：首帧定位若因 WebView2 布局晚落定落空，350ms 后再贴底。
        QTimer::singleShot(350, this, [this] {
            if (workWeb_ != nullptr) {
                workWeb_->refresh();
            }
        });
    } else if (cmd == QStringLiteral("chatBack")) {
        chatItemId_.clear();
    } else if (cmd == QStringLiteral("chatRefresh")) {
        // 待办会话页的「刷新」：与 dchat 同构（sessionOpen 重推）。清缓存后
        // 前端 __setChatPage 走 fresh 全量重绘，半死的列表自愈。
        if (workWeb_ != nullptr) {
            workWeb_->pushChat(chatStateFor(id, 0, 50));
        }
    } else if (cmd == QStringLiteral("chatOlder")) {
        // 上翻加载更旧的一页：单独推给页面（不进整份 state，避免重排列表）。
        if (workWeb_ != nullptr) {
            const int offset = action.value(QStringLiteral("offset")).toInt(0);
            workWeb_->pushChat(chatStateFor(id, offset, 50));
        }
    } else if (cmd == QStringLiteral("keyScene")) {
        if (keymapPage_ != nullptr) {
            keymapPage_->webSetPinned(action.value(QStringLiteral("name")).toString());
        }
    } else if (cmd == QStringLiteral("keySelect")) {
        if (keymapPage_ != nullptr) {
            keymapPage_->webSelect(action.value(QStringLiteral("vk")).toInt());
        }
    } else if (cmd == QStringLiteral("keyEdit")) {
        if (keymapPage_ != nullptr) {
            keymapPage_->webEdit(action.value(QStringLiteral("vk")).toInt(),
                                 action.value(QStringLiteral("gesture")).toString());
        }
    } else if (cmd == QStringLiteral("keyTakeover")) {
        if (keymapPage_ != nullptr) {
            keymapPage_->webSetTakeover(action.value(QStringLiteral("on")).toBool());
        }
    } else if (cmd == QStringLiteral("keyProfiles")) {
        if (keymapPage_ != nullptr) {
            keymapPage_->webOpenProfiles();
        }
    } else if (cmd == QStringLiteral("keyReload")) {
        if (keymapPage_ != nullptr) {
            keymapPage_->webReload();
        }
    } else if (cmd == QStringLiteral("appOpen")) {
        if (appPage_ != nullptr) {
            appPage_->webLaunch(id);
        }
    } else if (cmd == QStringLiteral("appAdd")) {
        if (appPage_ != nullptr) {
            appPage_->webAddCustom();
        }
    } else if (cmd == QStringLiteral("appRemove")) {
        if (appPage_ != nullptr) {
            appPage_->webRemoveCustom(
                action.value(QStringLiteral("name")).toString(),
                action.value(QStringLiteral("path")).toString());
        }
    } else if (cmd == QStringLiteral("appRescan")) {
        if (appPage_ != nullptr) {
            appPage_->webRescan();
        }
    } else if (cmd == QStringLiteral("appVoice")) {
        if (appPage_ != nullptr) {
            appPage_->webSetVoiceLaunch(action.value(QStringLiteral("on")).toBool());
        }
    } else if (cmd == QStringLiteral("appSystem")) {
        if (appPage_ != nullptr) {
            appPage_->webSetSystemControl(action.value(QStringLiteral("on")).toBool());
        }
    } else if (cmd == QStringLiteral("guideSite")) {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://ligdesign.win/agentrmc")));
    } else if (cmd == QStringLiteral("guideGithub")) {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/Samuel5945/AgentRMC")));
    } else if (cmd == QStringLiteral("guideCopy")) {
        QApplication::clipboard()->setText(agentBridgeInstallSentence());
    } else if (cmd == QStringLiteral("winMin")) {
        showMinimized();
    } else if (cmd == QStringLiteral("winMax") ||
               cmd == QStringLiteral("winMaxToggle")) {
        toggleMaximize();
    } else if (cmd == QStringLiteral("winClose")) {
        close();
    } else if (cmd == QStringLiteral("winDrag")) {
#ifdef Q_OS_WIN
        // startSystemMove() 只是 PostMessage(SC_MOVE)，从 WebView2 的 COM 回调里
        // 发出去等排到时 Chromium 已经把鼠标抓回子窗口，移动循环起不来（实测返回
        // true 但窗口纹丝不动）。同步发 WM_NCLBUTTONDOWN 当场进系统拖动循环。
        POINT pt{};
        GetCursorPos(&pt);
        ReleaseCapture();
        SendMessageW(reinterpret_cast<HWND>(winId()), WM_NCLBUTTONDOWN, HTCAPTION,
                     MAKELPARAM(pt.x, pt.y));
#endif
    } else if (cmd == QStringLiteral("winResize")) {
#ifdef Q_OS_WIN
        // 无边框窗 + WebView2 铺满：Chromium 的子窗把 WM_NCHITTEST 一律答成
        // HTCLIENT，系统认不出「鼠标压在窗口边上」，所以拉边由页面热区发起，
        // 这里同步转成系统的缩放循环（和 winDrag 同一套路，不能只 PostMessage
        // SC_SIZE：Chromium 已经抓住鼠标，模态循环起不来，实测窗口不动）。
        const QString dir = action.value(QStringLiteral("dir")).toString();
        UINT ht = HTBOTTOMRIGHT;
        if (dir == QStringLiteral("top")) {
            ht = HTTOP;
        } else if (dir == QStringLiteral("bottom")) {
            ht = HTBOTTOM;
        } else if (dir == QStringLiteral("left")) {
            ht = HTLEFT;
        } else if (dir == QStringLiteral("right")) {
            ht = HTRIGHT;
        } else if (dir == QStringLiteral("topleft")) {
            ht = HTTOPLEFT;
        } else if (dir == QStringLiteral("topright")) {
            ht = HTTOPRIGHT;
        } else if (dir == QStringLiteral("bottomleft")) {
            ht = HTBOTTOMLEFT;
        }
        POINT pt{};
        GetCursorPos(&pt);
        ReleaseCapture();
        SendMessageW(reinterpret_cast<HWND>(winId()), WM_NCLBUTTONDOWN, ht,
                     MAKELPARAM(pt.x, pt.y));
#else
        Q_UNUSED(action);
#endif
    } else if (cmd == QStringLiteral("listen")) {
        toggleListening();
    } else if (cmd == QStringLiteral("update")) {
        checkUpdates(true);
    } else if (cmd == QStringLiteral("theme")) {
        openThemeDialog();
    } else if (cmd == QStringLiteral("guide")) {
        showGuide();
    } else if (cmd == QStringLiteral("vBackend")) {
        const QString key = action.value(QStringLiteral("key")).toString();
        store_->updateVoice(QJsonObject{{QStringLiteral("backend"), key}});
        voice_->reload();
        lastTranscript_.clear();
        refreshVoiceUi();
        refreshRemoteCard();
        if (key == QStringLiteral("api") && !voice_->modelReady()) {
            toast_->showText(QString::fromUtf8(
                "云端 ASR 还没配好：语音设置 → 云端 ASR 填模型和 Key"), 3600);
        }
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QStringLiteral("语音后端切换为 ") + key, 0);
    } else if (cmd == QStringLiteral("vTest")) {
        voice_->startTest();
    } else if (cmd == QStringLiteral("vSettings")) {
        openVoiceSettings();
    } else if (cmd == QStringLiteral("vMain")) {
        showVoicePanel();
    } else if (cmd == QStringLiteral("vWave")) {
        if (engine_ == nullptr || !engine_->isRunning()) {
            toast_->showText(QString::fromUtf8(
                "还没开始监听：点右上角「未监听」胶囊开启"));
        } else {
            voice_->mediaToggle();
        }
    } else if (cmd == QStringLiteral("pkAgent")) {
        const QString exe = action.value(QStringLiteral("id")).toString();
        if (cardPicker_ != nullptr) {
            cardPicker_->webSelectAgent(exe);
            cardPicker_->refresh();
        }
        if (embeddedVoice_ != nullptr) {
            embeddedVoice_->webSelectAgent(exe);
            embeddedVoice_->webRefreshPicker();
        }
    } else if (cmd == QStringLiteral("pkMode")) {
        const QString mode = action.value(QStringLiteral("id")).toString();
        if (cardPicker_ != nullptr) {
            cardPicker_->webSelectMode(mode);
        }
        if (embeddedVoice_ != nullptr) {
            embeddedVoice_->webSelectMode(mode);
        }
    } else if (cmd == QStringLiteral("leftForm")) {
        setLeftPanel(action.value(QStringLiteral("voice")).toBool());
    } else if (cmd == QStringLiteral("bt")) {
        if (!QDesktopServices::openUrl(
                QUrl(QStringLiteral("ms-settings:bluetooth")))) {
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QString::fromUtf8("打不开系统蓝牙设置"), 0);
        }
    } else if (cmd == QStringLiteral("readLast")) {
        if (embeddedVoice_ != nullptr) {
            embeddedVoice_->webReadLast();
        }
    } else if (cmd == QStringLiteral("restartZcode")) {
        // 页面按钮已带确认（会中断该宿主正在跑的其它轮次）。
        QString error;
        if (ZcodeDispatch::restartDesktop(&error)) {
            dispatchNotice_ = QStringLiteral(
                "ZCode 已重启；打开会话即可看到派发消息全文");
            dispatchNoticeKind_ = QStringLiteral("good");
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("已重启 ZCode（派发可见性）"), 0);
        } else {
            dispatchNotice_ = error;
            dispatchNoticeKind_ = QStringLiteral("bad");
            appendEvent(QStringLiteral("error"), QString(), QString(),
                        QStringLiteral("重启 ZCode 失败：%1").arg(error), 0);
        }
        refreshWorkItems();
    } else if (cmd == QStringLiteral("vTyped")) {
        const QString text = action.value(QStringLiteral("text")).toString();
        if (!text.trimmed().isEmpty()) {
            voice_->submitTyped(text);
        }
    } else if (cmd == QStringLiteral("copy")) {
        QApplication::clipboard()->setText(
            action.value(QStringLiteral("text")).toString());
        toast_->showText(QString::fromUtf8("已复制"), 1500);
    } else if (cmd == QStringLiteral("chatToggle")) {
        const bool on = action.value(QStringLiteral("on")).toBool();
        store_->setUiBool(QStringLiteral("pet_chat"), on);
        voice_->setChatMode(on);
        toast_->showText(on ? QString::fromUtf8(
                                  "对话模式开：说话会得到模型的语音回复，不再粘贴")
                            : QString::fromUtf8("已退出对话模式"), 3000);
    } else if (cmd == QStringLiteral("tts")) {
        auto *dialog = new TtsSettingsDialog(store_.get(), this);
        connect(dialog, &TtsSettingsDialog::saved, this, [this] {
            voice_->reload();
            refreshVoiceUi();
        });
        dialog->exec();
        dialog->deleteLater();
    } else if (workItems_ != nullptr) {
        if (cmd == QStringLiteral("ack")) {
            if (workItems_->ack(id)) {
                appendEvent(QStringLiteral("status"), QString(), QString(),
                            QString::fromUtf8("待办已标记完成"), 0);
            }
            refreshWorkItems();
        } else if (cmd == QStringLiteral("clear")) {
            workItems_->clearFinished();
            refreshWorkItems();
        } else if (cmd == QStringLiteral("dispatch")) {
            const QString text =
                action.value(QStringLiteral("text")).toString().trimmed();
            const WorkItem *item = nullptr;
            for (const WorkItem &it : workItems_->items()) {
                if (it.id == id) {
                    item = &it;
                    break;
                }
            }
            QString reason;
            QString dispatchId;
            if (item == nullptr || text.isEmpty()) {
                reason = QStringLiteral("选中待办或派发文案为空");
            } else if (dispatch_->dispatch(item->agentExe, item->sessionId,
                                           item->project, text, &reason,
                                           &dispatchId,
                                           action.value(QStringLiteral("model"))
                                               .toObject())) {
                dispatchSessionById_.insert(dispatchId, item->sessionId);
                // 新一轮派发：旧回复立刻作废，别让本轮 stop 拿到上一轮正文。
                dispatchReplyBySession_.remove(item->sessionId);
                dispatchNotice_ = QStringLiteral("已派往会话 %1，等回执…")
                                      .arg(item->sessionId.left(8));
                dispatchNoticeKind_ = QStringLiteral("good");
                dispatchNoticeId_ = id;
                dispatchNoticeExe_ = item->agentExe;
                dispatchDraftId_.clear();
                dispatchDraftText_.clear();
                appendEvent(QStringLiteral("status"), QString(), QString(),
                            QStringLiteral("定向派发：%1 → 会话 %2")
                                .arg(text.left(40), item->sessionId.left(8)), 0);
            }
            if (!reason.isEmpty()) {
                // 裁决 6：证明不了就不发；草稿留在页面输入框里别丢。
                dispatchNotice_ = reason;
                dispatchNoticeKind_ = QStringLiteral("bad");
                dispatchNoticeId_ = id;
                dispatchNoticeExe_ = item != nullptr ? item->agentExe : QString();
                dispatchDraftId_ = id;
                dispatchDraftText_ = text;
                appendEvent(QStringLiteral("error"), QString(), QString(),
                            QStringLiteral("定向派发未发：%1").arg(reason), 0);
            }
            refreshWorkItems();
        } else if (cmd == QStringLiteral("answer")) {
            // B2/B3：聊天页内联应答（提问 answers / 授权 outcome），落收件箱。
            const QString answerReqId =
                action.value(QStringLiteral("requestId")).toString();
            const QString why = enqueueAnswer(
                id, answerReqId,
                action.value(QStringLiteral("answers")).toArray(),
                action.value(QStringLiteral("outcome")).toString());
            if (!why.isEmpty()) {
                dispatchNotice_ = why;
                dispatchNoticeKind_ = QStringLiteral("bad");
                dispatchNoticeId_ = id;
                // 归属到具体请求：聊天页把原因写在**那张卡**上（用户 2026-11-16
                // 反馈「点了没反应」——页顶 notice 在长记录里滚出去就看不见了）。
                dispatchNoticeRequest_ = answerReqId;
                appendEvent(QStringLiteral("error"), QString(), QString(),
                            QStringLiteral("内联应答未提交：%1").arg(why), 0);
            } else {
                dispatchNoticeRequest_.clear();
            }
            refreshWorkItems();
        }
    }
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
}

void MainWindow::openWorkItem(const QString &id) {
    // 先送到 Agent 跟前（宿主没开着会先启动），真送到了才销账。
    if (!bringToAgent(id) || workItems_ == nullptr) {
        return;
    }
    for (const WorkItem &item : workItems_->items()) {
        if (item.id != id) {
            continue;
        }
        // 只有「等我看」的账才结：进行中/状态待确认不是提醒，看了也不代表做完。
        if (!item.done && (item.stage == QStringLiteral("needs-me") ||
                           item.stage == QStringLiteral("review"))) {
            workItems_->ack(id);
            appendEvent(QStringLiteral("status"), QString(), QString(),
                        QString::fromUtf8("已打开 %1，这条提醒已销账")
                            .arg(item.title.isEmpty()
                                     ? AgentWindow::displayName(item.agentExe)
                                     : item.title),
                        0);
        }
        break;
    }
    refreshWorkItems();
}

void MainWindow::ackSelectedWork() {
    const QString id = selectedWorkId();
    if (id.isEmpty() || workItems_ == nullptr) {
        return;
    }
    if (workItems_->ack(id)) {
        refreshWorkItems();
        appendEvent(QStringLiteral("status"), QString(), QString(),
                    QString::fromUtf8("待办已标记完成"), 0);
    }
}

QWidget *MainWindow::buildSystemPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *card = new QFrame;
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(14, 11, 12, 12);
    cl->setSpacing(8);

    auto *title = new QLabel(QString::fromUtf8("系统健康"));
    title->setObjectName("cardTitle");
    cl->addWidget(title);

    sysBridge_ = new QLabel;
    sysBridge_->setObjectName("muted");
    cl->addWidget(sysBridge_);
    sysUptime_ = new QLabel;
    sysUptime_->setObjectName("muted");
    cl->addWidget(sysUptime_);
    sysLast_ = new QLabel;
    sysLast_->setObjectName("muted");
    cl->addWidget(sysLast_);
    sysSpool_ = new QLabel;
    sysSpool_->setObjectName("muted");
    cl->addWidget(sysSpool_);
    sysPlugin_ = new QLabel;
    sysPlugin_->setObjectName("muted");
    sysPlugin_->setWordWrap(true);
    cl->addWidget(sysPlugin_);
    sysProblems_ = new QLabel;
    sysProblems_->setObjectName("muted");
    sysProblems_->setWordWrap(true);
    cl->addWidget(sysProblems_);

    sysWorkers_ = new QTableWidget(0, 6);
    sysWorkers_->setHorizontalHeaderLabels(
        {QString::fromUtf8("Worker"), QString::fromUtf8("状态"),
         QString::fromUtf8("待你"), QString::fromUtf8("待验收"),
         QString::fromUtf8("进行中"), QString::fromUtf8("最近")});
    sysWorkers_->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Stretch);
    sysWorkers_->verticalHeader()->setVisible(false);
    sysWorkers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sysWorkers_->setSelectionMode(QAbstractItemView::NoSelection);
    sysWorkers_->setFocusPolicy(Qt::NoFocus);
    sysWorkers_->setShowGrid(false);
    sysWorkers_->setAlternatingRowColors(true);
    cl->addWidget(sysWorkers_, 1);

    auto *actions = new QHBoxLayout;
    auto *refresh = new QPushButton(QString::fromUtf8("刷新"));
    refresh->setObjectName("ghostSmall");
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::watchdogTick);
    auto *openDir = new QPushButton(QString::fromUtf8("打开数据目录"));
    openDir->setObjectName("ghostSmall");
    openDir->setCursor(Qt::PointingHandCursor);
    connect(openDir, &QPushButton::clicked, this, [this] {
        const QString dir = QFileInfo(spoolPath_).absolutePath();
        if (!dir.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });
    pluginUpgrade_ = new QPushButton(QString::fromUtf8("升级联动插件"));
    pluginUpgrade_->setObjectName("ghostSmall");
    pluginUpgrade_->setCursor(Qt::PointingHandCursor);
    pluginUpgrade_->setToolTip(QString::fromUtf8(
        "升级 Qoder / Qoder CN / ZCode 三家已装的插件。\n"
        "WorkBuddy AI / WorkBuddy 只在这里报状态：它们的市场源带自动更新，"
        "宿主自己拉新版，重启宿主生效。"));
    connect(pluginUpgrade_, &QPushButton::clicked, this,
            &MainWindow::upgradePlugins);
    // 装：一句话 + 各家状态。以前只有「复制安装话术」藏在引导页底部，太难找。
    auto *connectAgent = new QPushButton(QString::fromUtf8("接入 Agent"));
    connectAgent->setObjectName("ghostSmall");
    connectAgent->setCursor(Qt::PointingHandCursor);
    connectAgent->setToolTip(QString::fromUtf8(
        "一键复制安装话术：粘给任意一个正在干活的 Agent，它自己按所在宿主装好。\n"
        "顺便看各家装没装、进程里跑的是哪一版。"));
    connect(connectAgent, &QPushButton::clicked, this,
            &MainWindow::showAgentBridgeDialog);
    auto *lanBtn = new QPushButton(QString::fromUtf8("手机访问"));
    lanBtn->setObjectName("ghostSmall");
    lanBtn->setCursor(Qt::PointingHandCursor);
    lanBtn->setToolTip(QString::fromUtf8(
        "局域网手机遥控：开关、配对码、已配对设备。\n"
        "默认关闭；开启后手机浏览器扫码或输入配对码即可访问。"));
    connect(lanBtn, &QPushButton::clicked, this, &MainWindow::showLanDialog);
    actions->addWidget(refresh);
    actions->addWidget(connectAgent);
    actions->addWidget(pluginUpgrade_);
    actions->addWidget(lanBtn);
    actions->addWidget(openDir);
    actions->addStretch(1);
    cl->addLayout(actions);

    lay->addWidget(card);
    refreshSystemPage();
    return page;
}

// Qt 回退页（WebView2 运行库缺失时）的手机访问入口：开关 + 网址 + 配对码 +
// 设备吊销。外壳版系统页有同款卡片且带二维码；这里配对码给纯文本手动输入。
void MainWindow::showLanDialog() {
    auto *dialog = new QDialog(this);
    dialog->setWindowTitle(QString::fromUtf8("手机访问"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(440);
    auto *lay = new QVBoxLayout(dialog);
    lay->setSpacing(8);

    auto *toggle = new QCheckBox(
        QString::fromUtf8("启用手机访问（局域网 Web，端口 45916）"));
    lay->addWidget(toggle);
    auto *hint = new QLabel(QString::fromUtf8(
        "手机和电脑连同一个 Wi-Fi，用手机浏览器打开下面的网址并输入配对码。\n"
        "默认关闭；开启后局域网里的设备也要先配对才能访问，可随时吊销。"));
    hint->setObjectName("muted");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto *urls = new QLabel;
    urls->setObjectName("muted");
    urls->setWordWrap(true);
    urls->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(urls);
    auto *pairRow = new QHBoxLayout;
    auto *pairBtn = new QPushButton;
    pairBtn->setObjectName("ghostSmall");
    pairRow->addWidget(pairBtn);
    auto *codeLabel = new QLabel;
    codeLabel->setObjectName("muted");
    codeLabel->setWordWrap(true);
    codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pairRow->addWidget(codeLabel, 1);
    lay->addLayout(pairRow);
    auto *devTitle = new QLabel(QString::fromUtf8("已配对设备"));
    devTitle->setObjectName("cardTitle");
    lay->addWidget(devTitle);
    auto *devBox = new QVBoxLayout;
    lay->addLayout(devBox);
    auto *closeBtn = new QPushButton(QString::fromUtf8("关闭"));
    closeBtn->setObjectName("ghostSmall");
    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::close);
    lay->addWidget(closeBtn);

    auto refresh = std::make_shared<std::function<void()>>();
    *refresh = [this, toggle, urls, pairBtn, codeLabel, devBox, refresh] {
        const QJsonObject lan = lanState();
        const bool enabled = lan.value(QStringLiteral("enabled")).toBool();
        const bool running = lan.value(QStringLiteral("running")).toBool();
        const QSignalBlocker block(toggle);
        toggle->setChecked(enabled);
        QStringList urlTexts;
        const QJsonArray urlArray = lan.value(QStringLiteral("urls")).toArray();
        for (const QJsonValue &value : urlArray) {
            urlTexts << value.toString();
        }
        urls->setText(running
                          ? urlTexts.join(QLatin1Char('\n'))
                          : enabled ? QString::fromUtf8(
                                          "启动失败：端口被占用，关闭后重开试试")
                                    : QString::fromUtf8("未开启"));
        const QJsonObject pairing =
            lan.value(QStringLiteral("pairing")).toObject();
        const bool active = pairing.value(QStringLiteral("active")).toBool();
        pairBtn->setText(active ? QString::fromUtf8("收起配对码")
                                : QString::fromUtf8("显示配对码"));
        pairBtn->setEnabled(enabled && running);
        codeLabel->setText(
            active ? QString::fromUtf8("配对码：%1（10 分钟内有效）")
                         .arg(pairing.value(QStringLiteral("code")).toString())
                 : QString());
        QLayoutItem *item;
        while ((item = devBox->takeAt(0)) != nullptr) {
            if (item->widget() != nullptr) {
                item->widget()->deleteLater();
            }
            delete item;
        }
        const QJsonArray devices =
            lan.value(QStringLiteral("devices")).toArray();
        if (devices.isEmpty()) {
            auto *none = new QLabel(QString::fromUtf8("还没有配对过的手机"));
            none->setObjectName("muted");
            devBox->addWidget(none);
        }
        for (const QJsonValue &value : devices) {
            const QJsonObject device = value.toObject();
            auto *row = new QWidget;
            auto *rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            auto *name = new QLabel(
                device.value(QStringLiteral("name")).toString());
            rl->addWidget(name, 1);
            auto *revoke = new QPushButton(QString::fromUtf8("吊销"));
            revoke->setObjectName("ghostSmall");
            const QString id = device.value(QStringLiteral("id")).toString();
            connect(revoke, &QPushButton::clicked, this,
                    [this, id, refresh] {
                        if (lan_ != nullptr) {
                            lan_->revokeDevice(id);
                        }
                        (*refresh)();
                    });
            rl->addWidget(revoke);
            devBox->addWidget(row);
        }
    };
    connect(toggle, &QCheckBox::toggled, this, [this, refresh](bool on) {
        setLanEnabled(on);
        (*refresh)();
    });
    connect(pairBtn, &QPushButton::clicked, this, [this, refresh] {
        const bool active =
            lan_ != nullptr &&
            lan_->pairingInfo().value(QStringLiteral("active")).toBool();
        if (active) {
            lan_->endPairing();
        } else {
            ensureLanServer();
            lan_->beginPairing();
        }
        (*refresh)();
    });
    (*refresh)();
    dialog->show();
}

namespace {

QString guideIntro() {
    return QString::fromUtf8(
        "遥控器是可选的：有遥控器就按下面配对；没有也能用语音——先看「没有遥控器」"
        "这一段。桌宠右键菜单里也能完成同样的操作。");
}

// 引导全文：Qt 页和 WebView 外壳共用这一份，改文案只改这里。
QVector<GuideSection> guideSections() {
    return {
        {QString::fromUtf8("没有遥控器也能用"),
         {{QString::fromUtf8("触发"),
           QString::fromUtf8("Ctrl+Alt+V 全局热键 / 波形语音键 / "
                             "耳机播放键 / 单击桌宠")},
          {QString::fromUtf8("识别"),
           QString::fromUtf8("转发热键（0 模型，交给 Qoder / WorkBuddy）或本地模型"
                             "（任何输入框都能出字）")},
          {QString::fromUtf8("对话"),
           QString::fromUtf8("右键桌宠 → 对话模式：闲聊走 LLM，"
                             "干活的话交给前台 Agent")},
          {QString::fromUtf8("提醒"),
           QString::fromUtf8("装 agentrmc-bridge 插件：等授权、等选方案、任务完成时"
                             "桌宠气泡 + 语音")},
          {QString::fromUtf8("入口"),
           QString::fromUtf8("遥控器形态左栏「语音主界面」/ 托盘「语音面板」/ "
                             "桌宠右键；左上角波形键直接说话")},
          {QString::fromUtf8("模型"),
           QString::fromUtf8("把模型 zip 拖进窗口，或「语音面板 → 模型设置 → "
                             "导入模型包…」")}}},
        {QString::fromUtf8("有遥控器：第一次配对"),
         {{QString::fromUtf8("长按 3 秒"),
           QString::fromUtf8("同时按住「返回」+「主页」，直到遥控器指示灯快速闪烁")},
          {QString::fromUtf8("蓝牙设置"),
           QString::fromUtf8("点遥控器卡片右侧的「蓝牙设置」，在列表里连接遥控器")},
          {QString::fromUtf8("看状态"),
           QString::fromUtf8("按一下任意键，卡片上对应键位亮起就说明连上了")},
          {QString::fromUtf8("以后"),
           QString::fromUtf8("配对过一次后无需再配对：按一下任意键就会自动回连")}}},
        {QString::fromUtf8("手势速查"),
         {{QString::fromUtf8("语音键"),
           QString::fromUtf8("按住说话、松手结束（Qoder / WorkBuddy 自动切换接法）")},
          {QString::fromUtf8("方向键"),
           QString::fromUtf8("浏览内容；导航模式内改为翻页")},
          {QString::fromUtf8("OK"),
           QString::fromUtf8("发送消息；鼠标模式内 = 鼠标左键")},
          {QString::fromUtf8("返回"),
           QString::fromUtf8("停止 / 返回；双击进入音量·亮度模式")},
          {QString::fromUtf8("菜单"), QString::fromUtf8("换行；双击进入鼠标模式")},
          {QString::fromUtf8("主页"),
           QString::fromUtf8("回到顶部；双击进入导航模式")},
          {QString::fromUtf8("对焦"), QString::fromUtf8("全屏（走系统原生 F11）")}}},
        {QString::fromUtf8("模式怎么用"),
         {{QString::fromUtf8("进入"),
           QString::fromUtf8("双击对应键，屏幕底部提示「xx模式 开」，并有一声提示音")},
          {QString::fromUtf8("操作"),
           QString::fromUtf8("音量/亮度：↑↓ 音量、←→ 屏幕亮度；鼠标：方向键移动、OK 点击；"
                             "导航：←→ 切换控件、↑↓ 翻页")},
          {QString::fromUtf8("退出"),
           QString::fromUtf8("音量/亮度与鼠标模式单击「返回」或「菜单」退出；"
                             "导航模式单击「主页」退出")},
          {QString::fromUtf8("自动退出"),
           QString::fromUtf8("音量/亮度、鼠标模式 12 秒不操作自动退出")},
          {QString::fromUtf8("接管原生键"),
           QString::fromUtf8("映射页右下角打开后，当前场景中映射键的原生事件被吞；"
                             "实体键盘同名键也会按遥控器映射处理，"
                             "双击键的单击会有一段判定延迟")}}},
        {QString::fromUtf8("语音怎么用（有/无遥控器都适用）"),
         {{QString::fromUtf8("触发"),
           QString::fromUtf8("无遥控器：Ctrl+Alt+V / 波形语音键 / "
                             "耳机播放键 / 单击桌宠；有遥控器：按住语音键")},
          {QString::fromUtf8("转发"),
           QString::fromUtf8("默认，0 模型：把语音键转给当前应用自己的快捷键"
                             "（Qoder Ctrl+Shift+V / WorkBuddy Ctrl+D）")},
          {QString::fromUtf8("本地"),
           QString::fromUtf8("SenseVoice ~230MB / Qwen3-ASR ~1GB：任何输入框都能出字，"
                             "可离线；模型 zip 拖进窗口或「模型设置」导入")},
          {QString::fromUtf8("云端"),
           QString::fromUtf8("不装模型；音频上传到你配置的 OpenAI 兼容 ASR 服务")},
          {QString::fromUtf8("对话"),
           QString::fromUtf8("右键桌宠 → 对话模式：闲聊走 LLM，"
                             "干活的话交给前台 Agent")},
          {QString::fromUtf8("耳机"),
           QString::fromUtf8("电脑没在放媒体时，播放/暂停 = 同一个语音开关")}}},
        {QString::fromUtf8("桌宠右键菜单（只留高频）"),
         {{QString::fromUtf8("一级"),
           QString::fromUtf8("开始/停止监听、说一句/结束说话")},
          {QString::fromUtf8("窗口"),
           QString::fromUtf8("语音面板、完整界面、隐藏桌宠")},
          {QString::fromUtf8("其它"),
           QString::fromUtf8("识别方式、对话、主题、交给哪个 Agent、朗读最近，"
                             "都在语音面板 / 完整界面里操作")}}},
        {QString::fromUtf8("打开应用"),
         {{QString::fromUtf8("怎么说"),
           QString::fromUtf8("按住语音键说「打开剪映」「打开 Qoder」「打开浏览器」，"
                             "松手后直接启动，不会再往输入框里贴这几个字")},
          {QString::fromUtf8("内置"),
           QString::fromUtf8("浏览器 / Chrome / Edge / Qoder / Qoder CN / WorkBuddy / "
                             "DeepSeek / Codex / ZCode / 剪映，说「帮我打开…吧」也算")},
          {QString::fromUtf8("其它应用"),
           QString::fromUtf8("开始菜单里有的，按名字原样说就能开，例如「打开 WorkBuddy AI」")},
          {QString::fromUtf8("应用页"),
           QString::fromUtf8("右侧「应用」页能看到本机装没装、来源在哪，也可以直接点「打开」")},
          {QString::fromUtf8("遥控器"),
           QString::fromUtf8("「按键映射」的单击 / 双击 / 长按菜单里有「启动应用」，"
                             "把常用的绑到键上，不用说话")},
          {QString::fromUtf8("前提"),
           QString::fromUtf8("语音启动只在本地识别（SenseVoice / Qwen3）下生效；"
                             "转发热键时语音是整个交给前台应用的")},
          {QString::fromUtf8("不想被抢"),
           QString::fromUtf8("想把「打开剪映」这句话原样打给 Agent，就在「应用」页右下角"
                             "关掉「语音启动」（默认开）")}}},
        {QString::fromUtf8("接入 AI Agent（桌宠提醒）"),
         {{QString::fromUtf8("自动提醒"),
           QString::fromUtf8("在你干活的那个 Agent（Qoder / ZCode / WorkBuddy / "
                             "DeepSeek Harness / OpenSquilla）里接上：等你确认、"
                             "等你选方案、任务结束时，桌宠冒气泡念一句并可以在点气泡时"
                             "把窗口拉回来")},
          {QString::fromUtf8("怎么装（Qoder / ZCode / WorkBuddy）"),
           QString::fromUtf8("点下面「复制安装话术」，把那句话发给正在干活的 Agent，"
                             "它自己装好并启用——话术里已经带你本机上的位置")},
          {QString::fromUtf8("DeepSeek Harness 怎么装"),
           QString::fromUtf8("它的插件不是 zip，是一个 bundle 目录（安装目录 plugins 里的 "
                             "deepseek-harness）。让 DeepSeek Harness 里的 Agent 用 Plugin "
                             "管理器装：plugin_manager → install_bundle，target 指这个目录；"
                             "或手工两步：pnpm 把它 link 进 ~/.dsh/profiles/<profile>，"
                             "再把 @local/agentrmc-bridge 加进该 profile 的 "
                             "dsh.profile.bundles。装完必须重启 DeepSeek Harness，"
                             "Host 才会加载新代码。上面那句话里已经带了这条路径，"
                             "粘给 DSH 里的 Agent 就行")},
          {QString::fromUtf8("OpenSquilla 怎么接"),
           QString::fromUtf8("它没有插件目录，装不了上面的 zip，走的是外部桥："
                             "把技能目录里的 agentrmc-voice-reminder 技能（含 "
                             "opensquilla-pet-bridge.mjs）拷到 ~/.agents/skills 下，"
                             "再把它加进启动项；桥自己订阅 OpenSquilla 的会话事件，"
                             "转成同一个 45915 端口的 /notify。详见 README「OpenSquilla 接入」")},
          {QString::fromUtf8("自己动手"),
           QString::fromUtf8("只需要认一个落点——安装目录里的 plugins 目录，"
                             "要装的东西全在它里面："
                             "Qoder 上传 plugins 里的 agentrmc-bridge.zip；"
                             "ZCode / WorkBuddy 把 plugins 目录本身加进应用市场"
                             "（它就是目录市场源）；"
                             "DeepSeek Harness 用 plugin_manager 装 "
                             "plugins 里的 deepseek-harness 子目录")},
          {QString::fromUtf8("Agent 专用指针"),
           QString::fromUtf8("插件自带一根「Agent 自己的」指针：Agent 可以在屏幕上指出"
                             "它要操作的位置，也能真替你点一下，而**全程不动你的系统鼠标**"
                             "——你的鼠标照样能用，人机可以同时操作。"
                             "工具名 cursor_point（Qoder/ZCode/WorkBuddy 是 "
                             "mcp__agentrmc__cursor_point，DSH 是 cursor_point），"
                             "坐标用物理像素。系统页有一顆总开关：关掉后 Agent 调它会被"
                             "拒绝（默认开着，因为它只画不动真光标）")},
          {QString::fromUtf8("怎么升级"),
           QString::fromUtf8("Qoder 把上传的包记成本地插件，插件页那个「更新」按钮不拉新包，"
                             "而且 Qoder 和 Qoder CN 是两个独立 app、各装各的。"
                             "升级去「系统」页看「联动插件」那一行：落后的话点"
                             "「升级联动插件」，两家一起指到内置版本；改完要重启对应的 "
                             "app 才真加载新代码（那行里的「进程」就是此刻真在跑的版本）")},
          {QString::fromUtf8("WorkBuddy 怎么升"),
           QString::fromUtf8("不用点「升级联动插件」——那个按钮只认 Qoder 家族。"
                             "WorkBuddy / ZCode 的市场源是安装目录下的 plugins 目录"
                             "（目录源、自动更新），宿主自己会拉到新版；"
                             "改完重启宿主就生效。注意 WorkBuddy 和 WorkBuddy AI "
                             "是两份独立安装，各读各的配置家（.workbuddy / .workbuddy-ai），"
                             "要升就得两家各自刷新")},
          {QString::fromUtf8("DeepSeek Harness 怎么升"),
           QString::fromUtf8("也不走「升级联动插件」：它是装在 DSH profile 里的 bundle，"
                             "升它要 pnpm 重装 + 重启 Host，改注册表做不到。"
                             "让 DSH 里的 Agent 重新 install_bundle 一次"
                             "（或把 profile 里那条 link 指到新目录），再重启 DeepSeek Harness；"
                             "「系统」页那行的「进程」能看到它此刻真在跑的版本")}}},
        {QString::fromUtf8("已知限制"),
         {{QString::fromUtf8("电源 / 音量"),
           QString::fromUtf8("这两个键走红外，电脑收不到，无法映射")},
          {QString::fromUtf8("未开接管"),
           QString::fromUtf8("遥控器按键仍会原生送达前台，映射动作和原生动作可能同时生效")},
          {QString::fromUtf8("双击判定"),
           QString::fromUtf8("两次按下间隔 0.45 秒以内才算双击，慢速连点会当成两次单击")},
          {QString::fromUtf8("任务栏闪"),
           QString::fromUtf8("开始/结束录音时任务栏闪一下，是 Windows 麦克风指示器")}}},
    };
}

}  // namespace

QWidget *MainWindow::buildGuidePage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    guideScroll_ = scroll;
    auto *holder = new QWidget;
    holder->setObjectName("guideHolder");
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    auto *card = new QFrame;
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(18, 16, 18, 16);
    cl->setSpacing(16);

    const auto addSectionQ = [cl](const QString &name,
                                  const QVector<QPair<QString, QString>> &lines) {
        auto *box = new QWidget;
        auto *boxLay = new QVBoxLayout(box);
        boxLay->setContentsMargins(0, 0, 0, 0);
        boxLay->setSpacing(9);
        boxLay->addWidget(guide::heading(name));
        for (const auto &line : lines) {
            boxLay->addWidget(guide::row(line.first, line.second, 92));
        }
        cl->addWidget(box);
        return box;
    };

    auto *intro = new QLabel(guideIntro());
    intro->setObjectName("secBody");
    intro->setWordWrap(true);
    cl->addWidget(intro);

    for (const GuideSection &section : guideSections()) {
        QWidget *box = addSectionQ(section.title, section.rows);
        if (section.title == QString::fromUtf8("接入 AI Agent（桌宠提醒）")) {
            guideAgentSection_ = box;
        }
    }

    auto *links = new QHBoxLayout;
    auto *site = new QPushButton(QString::fromUtf8("官网  ↗"));
    site->setObjectName("ghostSmall");
    site->setCursor(Qt::PointingHandCursor);
    connect(site, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://ligdesign.win/agentrmc")));
    });
    auto *github = new QPushButton(QString::fromUtf8("GitHub  ↗"));
    github->setObjectName("ghostSmall");
    github->setCursor(Qt::PointingHandCursor);
    connect(github, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/Samuel5945/AgentRMC")));
    });
    auto *version = new QLabel(
        QString::fromUtf8("当前版本 v%1").arg(appVersion()));
    version->setObjectName("muted");
    const QString installSentence = agentBridgeInstallSentence();
    auto *copySentence = new QPushButton(QString::fromUtf8("复制安装话术"));
    copySentence->setObjectName("ghostSmall");
    copySentence->setCursor(Qt::PointingHandCursor);
    connect(copySentence, &QPushButton::clicked, copySentence,
            [copySentence, installSentence] {
                QApplication::clipboard()->setText(installSentence);
                copySentence->setText(QString::fromUtf8("已复制 ✓ 去粘给 Agent"));
                QTimer::singleShot(2600, copySentence, [copySentence] {
                    copySentence->setText(QString::fromUtf8("复制安装话术"));
                });
            });
    links->addWidget(site);
    links->addWidget(github);
    links->addWidget(copySentence);
    links->addSpacing(8);
    links->addWidget(version);
    links->addStretch(1);
    cl->addLayout(links);
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
    showAction_ = menu->addAction(QString::fromUtf8("显示主界面"));
    connect(showAction_, &QAction::triggered, this, &MainWindow::wake);
    auto *voicePanelAction = menu->addAction(QString::fromUtf8("语音面板"));
    connect(voicePanelAction, &QAction::triggered, this,
            &MainWindow::showVoicePanel);
    petAction_ = menu->addAction(QString::fromUtf8("语音桌宠"));
    petAction_->setCheckable(true);
    petAction_->setChecked(voicePet_->petEnabled());
    connect(petAction_, &QAction::toggled, this, [this](bool on) {
        voicePet_->setPetEnabled(on);
        if (on) {
            voicePet_->present();
        }
    });
    listenAction_ = menu->addAction(QString::fromUtf8("开始监听"));
    connect(listenAction_, &QAction::triggered, this, [this] {
        toggleListening();
        updateListenUi();
    });
    auto *update = menu->addAction(QString::fromUtf8("检查更新"));
    connect(update, &QAction::triggered, this, [this] { checkUpdates(true); });
    auto *site = menu->addAction(QString::fromUtf8("官网"));
    connect(site, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://ligdesign.win/agentrmc")));
    });
    auto *github = menu->addAction(QString::fromUtf8("GitHub 仓库"));
    connect(github, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/Samuel5945/AgentRMC")));
    });
    menu->addSeparator();
    autoStartAction_ = menu->addAction(QString::fromUtf8("开机自启"));
    autoStartAction_->setCheckable(true);
    autoStartAction_->setChecked(autoStartEnabled());
    connect(autoStartAction_, &QAction::toggled, this, [this](bool on) {
        setAutoStart(on);
        if (autoStartEnabled() != on) {
            QSignalBlocker block(autoStartAction_);
            autoStartAction_->setChecked(!on);
        }
    });
    menu->addSeparator();
    auto *quit = menu->addAction(QString::fromUtf8("退出 AgentRMC Native"));
    connect(quit, &QAction::triggered, this, [this] {
        quitting_ = true;
        QApplication::quit();
    });
    tray_->setContextMenu(menu);
    tray_->show();

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    wake();
                }
            });
    updateTrayState();
}

void MainWindow::updateTrayState() {
    if (tray_ == nullptr) {
        return;
    }
    const bool running = engine_ != nullptr && engine_->isRunning();
    const QString scene =
        sceneName_.isEmpty() ? store_->current(true).name : sceneName_;
    tray_->setToolTip(QString::fromUtf8("AgentRMC · %1 · %2")
                          .arg(running ? QString::fromUtf8("监听中")
                                       : QString::fromUtf8("未监听"),
                               voiceProfileLabel(scene)));
    if (listenAction_ != nullptr) {
        listenAction_->setText(running ? QString::fromUtf8("停止监听")
                                       : QString::fromUtf8("开始监听"));
    }
}

void MainWindow::showOnStartup(bool startHidden) {
    // 起手先按当前屏夹一遍：默认 1060×720 逻辑尺寸在改过分辨率 / 高缩放的屏上
    // 会大过屏幕（无边框窗不会自己缩），开机就是「顶栏显示不全」。
    fitToScreen();
    // 首次运行按「这次安装」判断：标记文件放在 exe 同目录，重装/升级不会误判。
    const QString marker =
        QCoreApplication::applicationDirPath() + QStringLiteral("/.guide_seen");
    const bool firstRun = !QFile::exists(marker);
    if (startHidden && !store_->uiBool("start_minimized", true)) {
        startHidden = false;
    }
    if (startHidden) {
        // 只留托盘时也要先把 HWND 建出来，第二次启动才能 FindWindow 叫回窗口。
        winId();
        // 开机自启只留托盘：桌宠就是这时的“脸面”。
        QTimer::singleShot(0, this, [this] { voicePet_->presentIfEnabled(); });
        return;
    }
    // 上次用的语音主界面就直接开它，不闪一下完整界面。
    if (store_->uiBool("voice_mode", false)) {
        winId();  // 热键注册和二次启动唤醒都要 HWND
        showVoicePanel();
    } else {
        wake();
    }
    // 桌宠常驻：界面开着也露头。以前只有「关窗进托盘」「收起语音面板」「开机只留
    // 托盘」这三处才把桌宠叫出来，所以手动启动时 pet_visible 一直是 false，看起来
    // 像桌宠没开。present() 自己兼顾已可见（只 raise）和贴边收纳（popOut）两种态；
    // 关掉桌宠开关时 presentIfEnabled 什么都不做，尊重用户设置。
    QTimer::singleShot(0, this, [this] { voicePet_->presentIfEnabled(); });
    if (firstRun) {
        QFile file(marker);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("1");
        }
        QTimer::singleShot(0, this, &MainWindow::showGuide);
    }
}

void MainWindow::wake() {
    // 只清最小化/藏起来的状态，保留最大化与全屏：托盘叫回窗口不该把
    // 全屏中的界面缩回去（原来无脑 showNormal，全屏会被砸成小窗）。
    // 叫回来之前先贴一次屏：窗口可能是在别的分辨率/缩放下摆的，比当前屏还大，
    // 那样一露头就是顶栏右侧全在屏幕外（用户 2026-11-17 报的正是这个）。
    fitToScreen();
    if (isMinimized()) {
        setWindowState(windowState() & ~Qt::WindowMinimized);
    }
    show();
    raise();
    activateWindow();
}

void MainWindow::fitToScreen() {
    QScreen *screen = this->screen();
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect avail = screen->availableGeometry();
    if (avail.width() <= 0 || avail.height() <= 0) {
        return;
    }
    // 最小尺寸也得跟着屏走：屏比 860×540（外壳 900×520）还小时，硬撑最小值会把
    // 窗口顶出屏幕，顶栏右侧那排（含最小化/最大化/关闭）就永远在屏幕外，点不到。
    setMinimumSize(qMin(860, avail.width()), qMin(540, avail.height()));
    if (workWeb_ != nullptr) {
        workWeb_->setMinimumSize(qMin(900, avail.width()),
                                 qMin(520, avail.height()));
    }
    if (isFullScreen()) {
        // 全屏本来就铺满，别动几何；边界仍要重喂一次（子窗可能停在旧尺寸）。
        if (workWeb_ != nullptr) {
            workWeb_->syncBounds();
        }
        return;
    }
    if (isMaximized()) {
        // 无边框最大化窗口在改分辨率/换屏后不一定会自己重算，重设一次状态让它铺满新屏。
        if (!avail.contains(geometry())) {
            showNormal();
            showMaximized();
        }
        if (workWeb_ != nullptr) {
            workWeb_->syncBounds();
        }
        return;
    }
    QRect g = geometry();
    if (!avail.contains(g)) {
        // 先压尺寸再挪位置：能留在原位就留，压不进就贴回可用区内。
        g.setWidth(qMin(g.width(), avail.width()));
        g.setHeight(qMin(g.height(), avail.height()));
        g.moveLeft(qBound(avail.left(), g.left(), avail.right() - g.width() + 1));
        g.moveTop(qBound(avail.top(), g.top(), avail.bottom() - g.height() + 1));
        setGeometry(g);
    }
    if (workWeb_ != nullptr) {
        // 即便窗口没被裁，页面边界也可能是旧值（换缩放的坑）：无脑重喂一次。
        workWeb_->syncBounds();
    }
}

void MainWindow::toggleMaximize() {
    if (isFullScreen() || isMaximized()) {
        showNormal();
    } else {
        showMaximized();
    }
}

void MainWindow::changeEvent(QEvent *event) {
    // 窗口态变了（最大化/还原/全屏）就同步标题栏按钮的字形：
    // □ = 可最大化，❐ = 已铺满可还原。
    if (event->type() == QEvent::WindowStateChange &&
        maximizeBtn_ != nullptr) {
        maximizeBtn_->setText(isMaximized() || isFullScreen()
                                  ? QString::fromUtf8("❐")
                                  : QString::fromUtf8("□"));
        if (workWeb_ != nullptr) {
            workWeb_->refresh();
        }
    }
    QMainWindow::changeEvent(event);
}

bool MainWindow::autoStartEnabled() const {
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\"
                                 "Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    return !run.value(QStringLiteral("AgentRMCNative")).toString().isEmpty();
}

void MainWindow::setAutoStart(bool on) {
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\"
                                 "Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    if (!on) {
        run.remove(QStringLiteral("AgentRMCNative"));
        return;
    }
    const QString exe =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    run.setValue(QStringLiteral("AgentRMCNative"),
                 QStringLiteral("\"%1\" --tray").arg(exe));
}

void MainWindow::showGuide() {
    if (guide_ == nullptr) {
        guide_ = new GuideDialog(this);
        connect(guide_, &GuideDialog::useHotkeyRequested, this, [this] {
            store_->updateVoice(QJsonObject{{QStringLiteral("backend"),
                                             QStringLiteral("hotkey")}});
            voice_->reload();
            refreshVoiceUi();
            guide_->setBackend(QStringLiteral("hotkey"));
            if (toast_ != nullptr) {
                toast_->showText(
                    QString::fromUtf8("已切到转发模式（0 模型）"), 2400);
            }
        });
        connect(guide_, &GuideDialog::modelPackageDropped, this,
                [this](const QString &file) {
                    guide_->hide();
                    importDroppedModel(file);
                    guide_->show();
                    guide_->raise();
                });
        connect(guide_, &GuideDialog::voiceModeRequested, this, [this] {
            store_->setUiBool("voice_mode", true);
            if (guide_ != nullptr) {
                guide_->close();
            }
            showVoicePanel();
        });
        // 「进入完整界面」：引导页里现在把它当主按钮——第一次装完看到的是完整界面，
        // 语音主界面只是一个可选外观，不该是默认落点。
        connect(guide_, &GuideDialog::mainWindowRequested, this, [this] {
            store_->setUiBool("voice_mode", false);
            if (guide_ != nullptr) {
                guide_->close();
            }
            wake();
        });
        // 「联动插件」：直接开一键接入弹窗（复制话术 + 各家状态），
        // 不再把人扔进「使用引导」那一页的长文里自己找。
        connect(guide_, &GuideDialog::pluginGuideRequested, this, [this] {
            if (guide_ != nullptr) {
                guide_->close();
            }
            showAgentBridgeDialog();
        });
        connect(guide_, &GuideDialog::openVoiceSettingsRequested, this, [this] {
            // 引导是置顶窗口，直接开设置会被它挡住：先收起引导，设置关掉再放回来。
            guide_->hide();
            openVoiceSettings();
            guide_->setBackend(voiceBackendKey(voice_->options().backend));
            guide_->show();
            guide_->raise();
        });
    }
    guide_->setBackend(voiceBackendKey(voice_->options().backend));
    guide_->showStep(0);
    const QSize size = guide_->sizeHint();
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect area = screen->availableGeometry();
        guide_->move(area.x() + (area.width() - size.width()) / 2,
                     area.y() + (area.height() - size.height()) / 2);
    }
    guide_->show();
    guide_->raise();
}

void MainWindow::showAgentBridgeDialog() {
    if (agentDialog_ == nullptr) {
        agentDialog_ = new AgentBridgeDialog(this);
        connect(agentDialog_, &AgentBridgeDialog::openFolderRequested, this,
                [] {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(
                        QCoreApplication::applicationDirPath() +
                        QStringLiteral("/plugins")));
                });
        connect(agentDialog_, &AgentBridgeDialog::detailsRequested, this,
                [this] {
                    if (agentDialog_ != nullptr) {
                        agentDialog_->hide();
                    }
                    openPluginGuide();
                });
    }
    QStringList problems;
    agentDialog_->setData(
        pluginHealth(&problems), agentBridgeInstallSentence(),
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath() +
                                 QStringLiteral("/plugins")));
    agentDialog_->show();
    agentDialog_->raise();
    agentDialog_->activateWindow();
}

void MainWindow::openPluginGuide() {
    wake();
    if (tabs_ != nullptr) {
        for (int i = 0; i < tabs_->count(); ++i) {
            if (tabs_->tabText(i).startsWith(QString::fromUtf8("使用引导"))) {
                tabs_->setCurrentIndex(i);
                break;
            }
        }
    }
    if (guideScroll_ == nullptr || guideAgentSection_ == nullptr) {
        return;
    }
    // 等这一帧布局稳定再滚：ensureWidgetVisible 用的是当时的几何，切页当帧还没算完。
    QTimer::singleShot(0, this, [this] {
        if (guideScroll_ != nullptr && guideAgentSection_ != nullptr) {
            guideScroll_->ensureWidgetVisible(guideAgentSection_, 0, 16);
        }
    });
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
                .arg(running ? themeToken("ok")
                             : ThemeManager::instance().css(QStringLiteral("line"))));
    }
    if (statusText_ != nullptr) {
        statusText_->setText(running ? QStringLiteral("监听中")
                                     : QStringLiteral("未监听"));
        statusText_->setStyleSheet(
            running
                ? QStringLiteral("color:%1; font-size:%2px;"
                                 " font-weight:600;")
                      .arg(themeToken("ok"),
                           QString::number(ThemeManager::scaledPx(11)))
                : QStringLiteral("color:%1; font-size:%2px;")
                      .arg(ThemeManager::instance().css(QStringLiteral("muted")),
                           QString::number(ThemeManager::scaledPx(11))));
    }
    refreshVoiceKey();
    // 无遥控器的语音触发：监听开着才注册全局热键，停了就还给别人。
    if (running) {
        registerVoiceHotkey();
    } else {
        unregisterVoiceHotkey();
    }
    if (voiceWindow_ != nullptr) {
        voiceWindow_->setListeningUi(running);
    }
    if (embeddedVoice_ != nullptr) {
        embeddedVoice_->setListeningUi(running);
    }
    if (voicePet_ != nullptr) {
        voicePet_->setListening(running);
    }
    updateTrayState();
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
}

void MainWindow::refreshVoiceKey() {
    if (voiceKey_ == nullptr) {
        return;
    }
    const bool running = engine_ != nullptr && engine_->isRunning();
    voiceKey_->setListening(running);
    QString tip =
        !running ? QString::fromUtf8("还没开始监听：点右上角「未监听」胶囊开启")
                 : QString::fromUtf8("语音键：点击说话，再点结束");
    const VoiceOptions options = voice_->options();
    if (running && options.kbHotkeyEnabled) {
        tip += QStringLiteral("\n热键 %1 也可以随时开始")
                   .arg(voiceComboText(options.kbHotkeyMods, options.kbHotkeyVk));
    }
    voiceKey_->setToolTip(tip);
}

void MainWindow::refreshVoiceUi() {
    if (voice_ == nullptr) {
        return;
    }
    // 卡底assurance文案只给首次使用的人看；隐私/缺模型这类警告照常常驻。
    const bool hintSeen =
        store_->uiBool(QStringLiteral("voice_hint_seen"), false);
    const VoiceOptions options = voice_->options();
    if (voiceBackend_ != nullptr) {
        const int index = voiceBackend_->findData(voiceBackendKey(options.backend));
        if (index >= 0 && index != voiceBackend_->currentIndex()) {
            const QSignalBlocker blocker(voiceBackend_);
            voiceBackend_->setCurrentIndex(index);
        }
    }
    if (options.backend == VoiceBackend::Hotkey) {
        setVoiceStatus(voice_->statusText(),
                       ThemeManager::instance().css(QStringLiteral("accent.text")),
                       true);
        if (voiceFoot_ != nullptr) {
            voiceFoot_->setText(QString::fromUtf8("0 模型 · 跟随前台场景转发"));
            voiceFoot_->setVisible(!hintSeen);
        }
    } else if (options.backend == VoiceBackend::CloudAsr) {
        // 云端路线没有「拖模型包」这个动作，配置入口走语音设置。
        const bool ready = voice_->modelReady();
        setVoiceStatus(voice_->statusText(),
                       ready ? ThemeManager::instance().css(QStringLiteral("text.dim"))
                             : themeToken("warn"),
                       true);
        if (voiceFoot_ != nullptr) {
            voiceFoot_->setText(
                QString::fromUtf8("云端转写 · 音频将上传到所填服务"));
            voiceFoot_->setVisible(true);
        }
    } else {
        const bool ready = voice_->modelReady();
        if (ready) {
            if (voiceState_ != nullptr) {
                voiceState_->setStyleSheet(QString());
            }
            setVoiceStatus(voice_->statusText(),
                           ThemeManager::instance().css(QStringLiteral("text.dim")),
                           true);
            if (voiceFoot_ != nullptr) {
                voiceFoot_->setText(QString::fromUtf8("本地转写 · 音频不出本机"));
                voiceFoot_->setVisible(!hintSeen);
            }
        } else {
            if (voiceState_ != nullptr) {
                voiceState_->setTextFormat(Qt::PlainText);
                voiceState_->setStyleSheet(QStringLiteral(
                    "QLabel#voiceBox { border: 1px dashed %1;"
                    " border-radius: 10px; background: %2;"
                    " color: %3; padding: 8px; }")
                    .arg(themeToken("a150"), themeToken("a16"),
                         ThemeManager::instance().css(
                             QStringLiteral("accent.text"))));
                voiceState_->setText(
                    QString::fromUtf8("⇣  把模型包（.zip）拖到这里导入"));
                voiceState_->setToolTip(
                    QString::fromUtf8("也可以点「模型设置 → 导入模型包…」选择文件"));
            }
            if (voiceFoot_ != nullptr) {
                voiceFoot_->setText(
                    QString::fromUtf8("未安装模型 · 拖入模型包，或点「模型设置」"));
                voiceFoot_->setVisible(true);
            }
        }
    }
    if (voiceTest_ != nullptr) {
        // 只随识别方式变；录音中不再闪烁变灰（那会让人以为按钮被按了）。
        voiceTest_->setEnabled(options.backend != VoiceBackend::Hotkey);
    }
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
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
        html += QStringLiteral("<br><span style=\"color:%1;\">%2</span>")
                    .arg(ThemeManager::instance().css(QStringLiteral("text")),
                         text.toHtmlEscaped());
        voiceState_->setToolTip(lastTranscript_);
    } else {
        voiceState_->setToolTip(QString());
    }
    voiceState_->setTextFormat(Qt::RichText);
    voiceState_->setText(html);
}

void MainWindow::registerVoiceHotkey() {
    unregisterVoiceHotkey();
    const VoiceOptions options = voice_->options();
    if (!options.kbHotkeyEnabled) {
        return;
    }
    UINT mods = 0;
    for (const QString &mod : options.kbHotkeyMods) {
        if (mod == QStringLiteral("ctrl")) {
            mods |= MOD_CONTROL;
        } else if (mod == QStringLiteral("alt")) {
            mods |= MOD_ALT;
        } else if (mod == QStringLiteral("shift")) {
            mods |= MOD_SHIFT;
        } else if (mod == QStringLiteral("win")) {
            mods |= MOD_WIN;
        }
    }
    if (RegisterHotKey(reinterpret_cast<HWND>(winId()), kVoiceHotkeyId, mods,
                       UINT(options.kbHotkeyVk))) {
        voiceHotkeyRegistered_ = true;
        return;
    }
    appendEvent(QStringLiteral("error"), QString(), QString(),
                QString::fromUtf8("全局语音热键注册失败（组合键被别的程序占用），"
                                  "可在「模型设置」里换一个"),
                0);
}

void MainWindow::unregisterVoiceHotkey() {
    if (!voiceHotkeyRegistered_) {
        return;
    }
    UnregisterHotKey(reinterpret_cast<HWND>(winId()), kVoiceHotkeyId);
    voiceHotkeyRegistered_ = false;
}

void MainWindow::setLeftPanel(bool voicePage) {
    if (leftStack_ == nullptr) {
        return;
    }
    leftStack_->setCurrentIndex(voicePage ? 1 : 0);
    if (voiceCard_ != nullptr) {
        voiceCard_->setVisible(!voicePage);
        // 语音页自己要吃满高度（历史区伸缩）；遥控器形态让语音卡吃多余高度。
        if (auto *lay = qobject_cast<QVBoxLayout *>(
                voiceCard_->parentWidget()->layout())) {
            lay->setStretchFactor(voiceCard_, voicePage ? 0 : 1);
            lay->setStretchFactor(leftStack_, voicePage ? 1 : 0);
        }
        // stack 的 sizeHint 取两页最大值，嵌入页会把它撑高；
        // 遥控器形态限到遥控器卡自身高度，底边才能贴齐。
        leftStack_->setMaximumHeight(
            voicePage ? QWIDGETSIZE_MAX
                      : leftStack_->widget(0)->maximumHeight());
    }
    if (!voicePage && cardPicker_ != nullptr) {
        cardPicker_->refresh();
    }
    store_->setUiBool(QStringLiteral("left_panel_voice"), voicePage);
}

void MainWindow::showVoicePanel() {
    if (voiceWindow_ == nullptr) {
        voiceWindow_ = new VoiceWindow(voice_, store_.get());
        voiceWindow_->setListeningUi(engine_ != nullptr && engine_->isRunning());
        connect(voiceWindow_, &VoiceWindow::fullUiRequested, this, [this] {
            store_->setUiBool("voice_mode", false);
            voiceWindow_->hide();
            wake();
        });
        // 面板收起（✕）时把桌宠叫出来，监听状态不丢。
        connect(voiceWindow_, &VoiceWindow::panelHidden, this, [this] {
            voicePet_->presentIfEnabled();
        });
        // 面板里的「朗读最近」交给桌宠的 TTS，面板本身不持有 TTS。
        connect(voiceWindow_, &VoiceWindow::speakRequested, this,
                [this](const QString &text) {
                    if (voicePet_ != nullptr) {
                        voicePet_->speak(text);
                    }
                });
        // 打字对话与说话同路由（桌宠判派活/对话）。
        connect(voiceWindow_, &VoiceWindow::typedSubmitted, this,
                [this](const QString &text) { voice_->submitTyped(text); });
    }
    store_->setUiBool("voice_mode", true);
    if (const QPoint cursor = QCursor::pos();
        QGuiApplication::screenAt(cursor) != nullptr) {
        const QRect screen = QGuiApplication::screenAt(cursor)->geometry();
        voiceWindow_->move(screen.center() - QRect(QPoint(0, 0),
                                                   voiceWindow_->size())
                                                  .center());
    }
    hide();
    voiceWindow_->show();
    voiceWindow_->raise();
    voiceWindow_->activateWindow();
}

void MainWindow::refreshRemoteCard() {
    if (remotePanel_ == nullptr) {
        return;
    }
    const Profile &profile = store_->current(true);
    auto actionFor = [&](int vk) {
        const QString semantic =
            profile.keys.value(QString::number(vk)).toString();
        if (semantic.isEmpty()) {
            return QString::fromUtf8("未映射");
        }
        const QJsonObject action =
            profile.actions.value(semantic).toObject();
        if (action.isEmpty()) {
            return QString::fromUtf8("无动作");
        }
        return Presets::shortDesc(action, vk);
    };

    const VoiceOptions voice = voice_->options();
    QString voiceText;
    if (voice.backend == VoiceBackend::Hotkey) {
        voiceText = voiceComboText(voice.hotkeyMods, voice.hotkeyVk);
        voiceText += voice.hotkeyTrigger == QStringLiteral("toggle")
                         ? QString::fromUtf8(" · 开关")
                         : QString::fromUtf8(" · 按住");
    } else {
        voiceText = voiceBackendLabel(voice.backend);
    }

    const QVector<RemotePanel::Entry> entries = {
        {0, QStringLiteral("○"), QString::fromUtf8("电源"),
         QString::fromUtf8("红外"), true},
        {122, QStringLiteral("◎"), QString::fromUtf8("对焦"), actionFor(122),
         false},
        {116, QString(), QString::fromUtf8("语音"), voiceText, false},
        {38, QStringLiteral("↑"), QString::fromUtf8("上键"), actionFor(38), false},
        {40, QStringLiteral("↓"), QString::fromUtf8("下键"), actionFor(40), false},
        {37, QStringLiteral("←"), QString::fromUtf8("左键"), actionFor(37), false},
        {39, QStringLiteral("→"), QString::fromUtf8("右键"), actionFor(39), false},
        {13, QStringLiteral("OK"), QString::fromUtf8("OK"), actionFor(13), false},
        {27, QStringLiteral("↩"), QString::fromUtf8("返回"), actionFor(27), false},
        {93, QStringLiteral("☰"), QString::fromUtf8("菜单"), actionFor(93), false},
        {36, QStringLiteral("⌂"), QString::fromUtf8("主页"), actionFor(36), false},
        {0, QStringLiteral("VOL"), QString::fromUtf8("音量"),
         QString::fromUtf8("红外"), true},
    };
    remotePanel_->setEntries(entries);
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
}

void MainWindow::applyRemoteMode(const QString &badge, const QSet<int> &keys,
                                 bool on) {
    if (remoteBadge_ != nullptr) {
        remoteBadge_->setText(on ? badge : QString::fromUtf8("待机"));
    }
    if (remotePanel_ != nullptr) {
        remotePanel_->setModeKeys(on ? keys : QSet<int>());
    }
    if (workWeb_ != nullptr) {
        workWeb_->refresh();
    }
}

void MainWindow::openThemeDialog() {
    // 盘上主题目录可能被手动更新过：先把列表和当前主题从盘上重读一遍，
    // 否则新文件夹不出现、同名覆盖还是旧图。
    ThemeManager::instance().refreshFromDisk();
    ThemeDialog dialog(store_.get(), this);
    const QSize size = dialog.sizeHint();
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect area = screen->availableGeometry();
        dialog.move(area.x() + (area.width() - size.width()) / 2,
                    area.y() + (area.height() - size.height()) / 2);
    }
    dialog.exec();
}

void MainWindow::openVoiceSettings() {
    auto *dialog = new VoiceSettingsDialog(store_.get(), this);
    connect(dialog, &VoiceSettingsDialog::saved, this, [this] {
        voice_->reload();
        refreshVoiceUi();
        refreshRemoteCard();
        // 键盘热键可能改了：注销再按新配置注册。
        if (engine_ != nullptr && engine_->isRunning()) {
            registerVoiceHotkey();
        }
        appendEvent(QStringLiteral("status"), QString(),
                    QString(), QString::fromUtf8("语音设置已保存"), 0);
        if (toast_ != nullptr) {
            const bool local =
                voice_->options().backend != VoiceBackend::Hotkey;
            toast_->showText(
                local ? QString::fromUtf8(
                            "已切到本地识别：语音键按住说话，松手转写并粘贴")
                      : QString::fromUtf8("已切回转发：语音键转发当前应用自己的语音快捷键"),
                3200);
        }
    });
    // 子对话框保存的 TTS/云端 ASR/桌宠对话配置立即生效，不等外层「保存」。
    connect(dialog, &VoiceSettingsDialog::optionsApplied, this, [this] {
        voice_->reload();
        refreshVoiceUi();
    });
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onVoiceState(const QString &state, const QString &detail) {
    voiceRecording_ = state == QStringLiteral("recording");
    if (voiceKey_ != nullptr) {
        voiceKey_->setRecording(voiceRecording_);
    }
    if (voiceRecording_ &&
        !store_->uiBool(QStringLiteral("voice_hint_seen"), false)) {
        store_->setUiBool(QStringLiteral("voice_hint_seen"), true);
        refreshVoiceUi();
    }
    refreshVoiceKey();
    if (state == QStringLiteral("idle")) {
        const bool warn = detail.contains(QString::fromUtf8("太短")) ||
                          detail.contains(QString::fromUtf8("未"));
        const QString shown =
            detail == QStringLiteral("就绪") ||
                    detail == QStringLiteral("语音快捷键已结束")
                ? voice_->statusText()
                : detail;
        setVoiceStatus(shown,
                       warn ? themeToken("warn")
                            : ThemeManager::instance().css(QStringLiteral("text.dim")),
                       true);
    } else {
        lastTranscript_.clear();
        setVoiceStatus(detail,
                       ThemeManager::instance().css(QStringLiteral("accent.text")),
                       false);
    }
    if (voiceTest_ != nullptr) {
        voiceTest_->setEnabled(voice_->options().backend != VoiceBackend::Hotkey);
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

// Agent 指针总开关：默认开（它只画、不动真光标，开着不影响用户），但「能让本机
// 任何进程在屏幕上画一根指针并真点下去」这件事该有总闸。关掉 = /cursor/* 一律 409
// + 覆盖层立刻淡出 + 不再建。
bool MainWindow::cursorEnabled() const {
    return store_ != nullptr && store_->uiBool(QStringLiteral("agent_cursor"), true);
}

void MainWindow::setCursorEnabled(bool on) {
    store_->setUiBool(QStringLiteral("agent_cursor"), on);
    if (!on && cursor_ != nullptr) {
        cursor_->hide(200);  // 关闸时屏幕立刻干净，不等 hold 超时
    }
    appendEvent(QStringLiteral("status"), QString(), QString(),
                on ? QString::fromUtf8("Agent 指针已开启（所有 Agent 可指、可点）")
                   : QString::fromUtf8("Agent 指针已关闭（/cursor/* 一律拒绝）"),
                0);
    if (workWeb_ != nullptr) {
        workWeb_->refresh();  // 系统页那颗开关的状态跟着变
    }
}

QJsonObject MainWindow::onCursorAction(const QJsonObject &payload) {
    const QString action = payload.value(QStringLiteral("action")).toString();
    if (cursor_ == nullptr) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("code"), 503},
                           {QStringLiteral("error"),
                            QStringLiteral("指针服务没起（截图/无托盘模式）")}};
    }
    // 开关（ui.agent_cursor，默认开）：关掉后连 state 都不给——不是只挡住画面，
    // 是让任何本机进程都没法把指针叫出来。默认开是因为它「只画不动真光标」，
    // 开着不会影响用户；但能真点下去这件事该有个总闸。
    if (!cursorEnabled()) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("code"), 409},
            {QStringLiteral("error"),
             QStringLiteral("Agent 指针已在设置里关闭（系统页 → Agent 指针）")}};
    }
    if (action == QStringLiteral("hide")) {
        const int fade = payload.value(QStringLiteral("fade_ms")).toInt(260);
        cursor_->hide(fade);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("code"), 200}};
    }
    if (action == QStringLiteral("show")) {
        cursor_->show();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("code"), 200}};
    }
    if (action != QStringLiteral("move") && action != QStringLiteral("click")) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("code"), 400},
            {QStringLiteral("error"),
             QStringLiteral("不认识的动作：%1（只认 move/click/hide/show）")
                 .arg(action)}};
    }
    const bool wantsClick = action == QStringLiteral("click");
    const AgentCursor::Request request = wantsClick
                                             ? AgentCursor::parseClick(payload)
                                             : AgentCursor::parseMove(payload);
    if (!request.ok) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("code"), 400},
                           {QStringLiteral("error"), request.error}};
    }
    // hold_ms 缺省 8s：没人再动手就自己淡出，别让一根指针永远杵在屏幕上。
    const int holdMs = payload.contains(QStringLiteral("hold_ms"))
                           ? qBound(0, payload.value(QStringLiteral("hold_ms")).toInt(),
                                    600000)
                           : 8000;
    cursor_->moveTo(request.pos, request.durationMs, request.agent, holdMs);

    if (!wantsClick || !request.click) {
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("code"), 200},
                           {QStringLiteral("moved"), true},
                           {QStringLiteral("x"), request.pos.x()},
                           {QStringLiteral("y"), request.pos.y()},
                           {QStringLiteral("clicked"), false}};
    }
    // 点击不能在这个线程里同步做。UIA 的 Invoke 是「调进目标进程」的语义动作，
    // 目标若正好在等我们（实测：探针窗在自己的消息泵里等 HTTP 回包），双方互等
    // 就把桥线程卡死到超时——点击其实已经生效，调用方却只看到连接断了。
    //
    // 所以：先把指针挪到位、立刻回包；点击扔到一个一次性线程里做。不能用
    // QTimer::singleShot(0)：它会在本次事件处理返回前就排到队首，抢在 socket
    // 写出之前跑，超时照旧。也不能只加延迟——UIA 对某些目标能阻塞好几秒。
    cursor_->pulse(request.button);
    AgentCursor::Request deferred = request;
    QThread *worker = QThread::create([this, deferred] {
        const AgentCursor::ClickResult clicked =
            AgentCursor::performClick(deferred);
        // 回主线程改 UI 状态：reportClick/appendEvent/hide 都动 Qt 对象。
        QMetaObject::invokeMethod(
            this,
            [this, clicked] {
                if (cursor_ != nullptr) {
                    cursor_->reportClick(clicked.ok, clicked.backend,
                                         clicked.verified, clicked.target,
                                         clicked.ok ? QString() : clicked.error);
                    // 点完就收：桌面立刻恢复干净，不用等 8 秒闲置超时。
                    // 用 400ms 淡出而不是瞬间消失——涟漪要看得见，否则用户
                    // 分不清「点了」和「没点」。
                    cursor_->hide(400);
                }
                if (!clicked.ok) {
                    appendEvent(QStringLiteral("error"), QString(), QString(),
                                QString::fromUtf8("Agent 指针点击未生效（%1）：%2")
                                    .arg(clicked.target, clicked.error),
                                0);
                }
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    QJsonObject out{{QStringLiteral("ok"), true},
                    {QStringLiteral("code"), 200},
                    {QStringLiteral("moved"), true},
                    {QStringLiteral("x"), request.pos.x()},
                    {QStringLiteral("y"), request.pos.y()},
                    {QStringLiteral("clicked"), true},
                    {QStringLiteral("backend"), QStringLiteral("async")},
                    {QStringLiteral("verified"), false},
                    {QStringLiteral("note"),
                     QStringLiteral("点击已投递，结果看 /cursor/state 的 clicks"
                                    " 与目标自身反馈")}};
    return out;
}
QJsonObject MainWindow::onBridgeCommand(const QJsonObject &body) {
    const QString action = body.value(QStringLiteral("action")).toString().trimmed();
    const SystemCommand command = SystemCommandText::parse(action);
    if (!command.matched || command.confirmed) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("code"), 400},
            {QStringLiteral("message"),
             QString::fromUtf8("不认识的命令说法：%1").arg(action)}};
    }
    if (system_ == nullptr) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("code"), 400},
                           {QStringLiteral("message"),
                            QString::fromUtf8("系统控制还没就绪")}};
    }
    if (!SystemControlSettings::enabled(store_.get(), command.kind)) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("code"), 403},
            {QStringLiteral("message"),
             QString::fromUtf8("「%1」这类语音系统控制被关掉了")
                 .arg(SystemControlSettings::categoryLabel(
                     SystemCommandText::category(command.kind)))}};
    }
    // 宿主 Agent 要自己负责确认：native 不替它攒 pending，缺 confirm 就回 409。
    const bool confirmed = body.value(QStringLiteral("confirm")).toBool(false);
    const SystemResult result = system_->execute(command, confirmed);
    const int code = result.ok ? 200 : (result.needsConfirm ? 409 : 400);
    appendEvent(result.ok ? QStringLiteral("system") : QStringLiteral("error"),
                QString(), QString(),
                QString::fromUtf8("桥接命令 %1：%2").arg(action, result.text), 0);
    if (result.ok && voicePet_ != nullptr) {
        voicePet_->speak(result.text);
    }
    QJsonObject reply{{QStringLiteral("ok"), result.ok},
                      {QStringLiteral("code"), code},
                      {QStringLiteral("message"), result.text},
                      {QStringLiteral("kind"), command.kind}};
    if (result.value >= 0) {
        reply.insert(QStringLiteral("value"), result.value);
    }
    if (result.needsConfirm) {
        reply.insert(QStringLiteral("needs_confirm"), true);
        reply.insert(QStringLiteral("confirm_phrase"),
                     SystemCommandText::confirmPhrase(command.kind));
    }
    return reply;
}

void MainWindow::onEngineEvent(const QString &kind, const QString &name,
                               const QString &profile, const QString &detail,
                               int vk) {
    if (kind == QStringLiteral("status")) {
        const bool on = detail.contains(QString::fromUtf8(" 开"));
        if (name == QString::fromUtf8("音量/亮度模式")) {
            applyRemoteMode(QString::fromUtf8("音量 · 亮度"),
                            QSet<int>{37, 38, 39, 40}, on);
            if (toast_ != nullptr) {
                toast_->showText(
                    on ? QString::fromUtf8(
                             "音量 / 亮度模式 · ↑↓ 音量，←→ 屏幕亮度；单击返回/菜单退出")
                       : QString::fromUtf8("已退出音量 / 亮度模式"));
            }
        } else if (name == QString::fromUtf8("鼠标模式")) {
            applyRemoteMode(QString::fromUtf8("鼠标模式"),
                            QSet<int>{37, 38, 39, 40, 13}, on);
            if (toast_ != nullptr) {
                toast_->showText(
                    on ? QString::fromUtf8(
                             "鼠标模式 · 方向键移动，OK 左键；单击菜单退出")
                       : QString::fromUtf8("已退出鼠标模式"));
            }
        } else if (name == QString::fromUtf8("导航模式")) {
            applyRemoteMode(QString::fromUtf8("导航模式"),
                            QSet<int>{37, 38, 39, 40}, on);
            if (toast_ != nullptr) {
                toast_->showText(
                    on ? QString::fromUtf8(
                             "导航模式 · ←→ 切换控件，↑↓ 翻页；单击主页退出")
                       : QString::fromUtf8("已退出导航模式"));
            }
        }
    }
    if (kind == QStringLiteral("key") && vk != 0) {
        if (remotePanel_ != nullptr) {
            remotePanel_->flash(vk);
        }
        if (workWeb_ != nullptr) {
            workWeb_->pushLive(QJsonObject{{QStringLiteral("lit"), vk}});
        }
        if (remoteState_ != nullptr && remotePanel_ != nullptr) {
            const QString text = name.isEmpty() ? detail : name;
            const QString label = remotePanel_->labelFor(vk);
            if (!text.isEmpty()) {
                remoteState_->setText(label.isEmpty()
                                          ? text
                                          : label + QStringLiteral(" · ") + text);
            }
        }
    }
    if (guide_ != nullptr && guide_->isVisible() &&
        kind == QStringLiteral("key") && vk != 0) {
        guide_->noteKey(Presets::remoteKeyName(vk));
    }
    appendEvent(kind, name, profile, detail, vk);
}

void MainWindow::onEngineRunning(bool running, const QString &message) {
    updateListenUi();
    refreshRemoteCard();
    if (remoteDot_ != nullptr) {
        remoteDot_->setStyleSheet(
            running ? QStringLiteral("background:%1; border-radius:3px;")
                          .arg(themeToken("ok"))
                    : QStringLiteral("background:%1; border-radius:3px;")
                          .arg(ThemeManager::instance().css(QStringLiteral("line"))));
    }
    if (remoteState_ != nullptr) {
        remoteState_->setText(
            running ? QString::fromUtf8("监听中 · ") +
                          voiceProfileLabel(store_->current(true).name)
                    : QString::fromUtf8("未监听"));
    }
    if (!running) {
        applyRemoteMode(QString(), QSet<int>(), false);
    }
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
    QColor color = ThemeManager::instance().color(QStringLiteral("text.dim"));
    if (kind == "error") {
        color = ThemeManager::instance().color(QStringLiteral("danger"));
    } else if (kind == "voice" || kind == "system") {
        // 系统命令的成功结果和听写同色：都是「按你说的做完了」。
        color = ThemeManager::instance().color(QStringLiteral("ok"));
    } else if (kind == "status") {
        color = ThemeManager::instance().color(QStringLiteral("muted"));
    }
    item->setForeground(color);
    logList_->addItem(item);
    logList_->scrollToBottom();
    // 空状态是一整块（标题 + 说明），藏在 logEmpty_ 的父控件上。
    if (QWidget *emptyBox = logEmpty_->parentWidget()) {
        emptyBox->hide();
    }
    logList_->show();
    logCount_->setText(QStringLiteral("%1 条").arg(logList_->count()));
    if (workWeb_ != nullptr) {
        // 外壳版另存一份带 kind 的账：页面按 kind 上色，规则和上面 Qt 版一致。
        webEvents_.append(QJsonObject{{QStringLiteral("kind"), kind},
                                      {QStringLiteral("text"), text}});
        workWeb_->refresh();
    }
}

void MainWindow::clearLog() {
    if (logList_ == nullptr) {
        return;
    }
    logList_->clear();
    logList_->hide();
    if (QWidget *emptyBox = logEmpty_->parentWidget()) {
        emptyBox->show();
    }
    logCount_->setText(QStringLiteral("0 条"));
    webEvents_ = QJsonArray();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message,
                             qintptr *result) {
    static const UINT wakeMessage =
        RegisterWindowMessageW(L"AgentRMC.Native.Wake");
    auto *msg = static_cast<MSG *>(message);
    // 无边框窗的边缘缩放：外壳形态下 WebView2 铺满整窗，Chromium 的子窗把命中测试
    // 全答成 HTCLIENT（实测），顶层的这条消息根本收不到——那条路走页面的 .rz 热区
    // + winResize。这里是**回退形态**（AGENTRMC_WORKWEB=0 或 WebView2 运行库缺失）
    // 的保障：Qt 控件自己铺满，没人抢命中测试，就在顶层把 5px 边框认成缩放区。
    if (msg != nullptr && msg->message == WM_NCHITTEST && workWeb_ == nullptr) {
        const POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        RECT win{};
        if (GetWindowRect(reinterpret_cast<HWND>(winId()), &win)) {
            const int border = 5;
            const bool left = pt.x >= win.left && pt.x < win.left + border;
            const bool right = pt.x < win.right && pt.x >= win.right - border;
            const bool top = pt.y >= win.top && pt.y < win.top + border;
            const bool bottom = pt.y < win.bottom && pt.y >= win.bottom - border;
            if (top && left) {
                *result = HTTOPLEFT;
                return true;
            }
            if (top && right) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (bottom && left) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (bottom && right) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (left) {
                *result = HTLEFT;
                return true;
            }
            if (right) {
                *result = HTRIGHT;
                return true;
            }
            if (top) {
                *result = HTTOP;
                return true;
            }
            if (bottom) {
                *result = HTBOTTOM;
                return true;
            }
        }
    }
    if (msg != nullptr && wakeMessage != 0 && msg->message == wakeMessage) {
        wake();
        *result = 0;
        return true;
    }
    // 全局语音热键（无遥控器触发）：一键开始/结束，复用耳机键的状态机。
    if (msg != nullptr && msg->message == WM_HOTKEY &&
        msg->wParam == kVoiceHotkeyId) {
        voice_->mediaToggle();
        *result = 0;
        return true;
    }
    // 插拔显示器/改分辨率后，缓存的物理屏句柄会失效，亮度命令要重新取。
    if (msg != nullptr && msg->message == WM_DISPLAYCHANGE) {
        if (system_ != nullptr) {
            system_->invalidateMonitors();
        }
        // 无边框窗口不会自己跟着新分辨率/新缩放缩：窗口比新屏大时顶栏右侧那排
        // （检查更新/主题/新手引导 + 最小化/最大化/关闭）就整块在屏幕外，用户
        // 看到的就是「顶栏显示不全」。延后一拍等 Windows 把工作区跟 DPI 更新完，
        // 再补一次更晚的——跨屏换倍率时 Qt 的几何更新实测会晚到。
        QTimer::singleShot(0, this, [this] { fitToScreen(); });
        QTimer::singleShot(350, this, [this] { fitToScreen(); });
        return QMainWindow::nativeEvent(eventType, message, result);
    }
    // 跨屏拖拽换 DPI：逻辑尺寸不变就没有 resizeEvent，widget 级 DPI 事件也
    // 不一定送到外壳控件（实测子 HWND 停在旧屏物理尺寸，页面被裁）。顶层的
    // 位置/DPI 消息最可靠；延后一拍等 Qt 把几何和倍率更新完再重喂物理边界。
    if (msg != nullptr && workWeb_ != nullptr &&
        (msg->message == WM_WINDOWPOSCHANGED || msg->message == WM_DPICHANGED)) {
        QTimer::singleShot(0, this, [this] {
            if (workWeb_ != nullptr) {
                workWeb_->syncBounds();
            }
        });
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    const QEvent::Type type = event->type();
    if (type == QEvent::DragEnter) {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        const QString file = modelPackageFromMime(drag->mimeData());
        if (!file.isEmpty()) {
            if (dropHint_ != nullptr) {
                // 主题包也是 zip：认出 theme.json 才说「换主题」，别把人吓成导入模型。
                dropHint_->setText(ThemeManager::isThemePackage(file)
                                       ? QString::fromUtf8("松手，换上这个主题")
                                       : QString::fromUtf8("松手，导入这个模型包"));
            }
            drag->acceptProposedAction();
            showDropHint(true);
            return true;
        }
    } else if (type == QEvent::DragLeave) {
        showDropHint(false);
    } else if (type == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event);
        const QString file = modelPackageFromMime(drop->mimeData());
        showDropHint(false);
        if (!file.isEmpty()) {
            drop->acceptProposedAction();
            if (ThemeManager::isThemePackage(file)) {
                importDroppedTheme(file);
            } else {
                importDroppedModel(file);
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::showDropHint(bool on) {
    if (dropHint_ == nullptr) {
        return;
    }
    if (!on) {
        dropHint_->hide();
        return;
    }
    dropHint_->setGeometry(rect());
    dropHint_->raise();
    dropHint_->show();
}

void MainWindow::importDroppedTheme(const QString &file) {
    QString error;
    QStringList warnings;
    const QString id =
        ThemeManager::instance().importThemePackage(file, &error, &warnings);
    if (id.isEmpty()) {
        appendEvent(QStringLiteral("error"),
                    QString::fromUtf8("主题导入失败"), QString(), error, 0);
        if (toast_ != nullptr) {
            toast_->showText(
                QString::fromUtf8("主题导入失败：%1").arg(error), 3200);
        }
        return;
    }
    ThemeManager::instance().setTheme(id);
    QString text = QString::fromUtf8("主题已导入并启用");
    if (!warnings.isEmpty()) {
        text += QString::fromUtf8("（提醒：%1%2）")
                    .arg(warnings.first(),
                         warnings.size() > 1 ? QStringLiteral("…") : QString());
    }
    appendEvent(QStringLiteral("status"), QString::fromUtf8("主题已导入"),
                QString(), text, 0);
    if (toast_ != nullptr) {
        toast_->showText(text, 3000);
    }
}

void MainWindow::importDroppedModel(const QString &file) {    const ModelImportResult result = importModelPackage(this, file);
    if (!result.ok) {
        appendEvent(QStringLiteral("error"), QString::fromUtf8("模型导入失败"),
                    QString(), result.error, 0);
        if (toast_ != nullptr) {
            toast_->showText(QString::fromUtf8("模型导入失败：%1").arg(result.error),
                             3200);
        }
        return;
    }
    const QString name = result.backend == QStringLiteral("qwen3")
                             ? QStringLiteral("Qwen3-ASR")
                             : QStringLiteral("SenseVoice");
    // 不擅自改识别方式：正在用的路线（默认转发）照旧，要用本地得自己切。
    appendEvent(QStringLiteral("voice"), QString::fromUtf8("模型已导入"),
                QString(),
                QString::fromUtf8("%1 已装好；要用本地识别，在「模型设置 → "
                                  "识别方式」里切过去")
                    .arg(name),
                0);
    refreshVoiceUi();
    if (toast_ != nullptr) {
        toast_->showText(
            QString::fromUtf8("%1 已装好，可在「模型设置」里切到本地识别").arg(name),
            3200);
    }
}

void MainWindow::checkUpdates(bool manual) {
    if (updating_) {
        return;
    }
    updating_ = true;
    if (updateButton_ != nullptr) {
        updateButton_->setText(QString::fromUtf8("检查中…"));
    }
    QString url = store_->config()
                      .value(QStringLiteral("ui"))
                      .toObject()
                      .value(QStringLiteral("update_url"))
                      .toString();
    if (url.trimmed().isEmpty()) {
        url = QStringLiteral("https://ligdesign.win/agentrmc-update.json");
    }
    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start([self, url, manual] {
        const UpdateInfo info = fetchUpdateInfo(url);
        if (self == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            self, [self, info, manual] { self->onUpdateChecked(info, manual); },
            Qt::QueuedConnection);
    });
}

void MainWindow::onUpdateChecked(const UpdateInfo &info, bool manual) {
    updating_ = false;
    if (updateButton_ != nullptr) {
        updateButton_->setText(QString::fromUtf8("检查更新"));
    }
    QSettings settings;
    if (!info.ok) {
        if (manual && toast_ != nullptr) {
            toast_->showText(QString::fromUtf8("检查更新失败：%1").arg(info.error),
                             3200);
        }
        return;
    }
    settings.setValue(QStringLiteral("update/last_check"),
                      QDateTime::currentSecsSinceEpoch());
    if (!versionIsNewer(info.version, appVersion())) {
        if (manual && toast_ != nullptr) {
            toast_->showText(
                QString::fromUtf8("已是最新版本 v%1").arg(appVersion()), 2400);
        }
        return;
    }
    if (!manual &&
        settings.value(QStringLiteral("update/skip_version")).toString() ==
            info.version) {
        return;
    }
    showUpdateDialog(info);
}

void MainWindow::showUpdateDialog(const UpdateInfo &info) {
    QDialog dialog(this);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setFixedWidth(560);
    const auto tk = [](const char *token) {
        return ThemeManager::instance().css(QString::fromLatin1(token));
    };
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: %1; border: 1px solid %2;"
        " border-radius: 12px; }"
        "QLabel { color: %3; font-size: %12px; }"
        "QLabel#newVer { color: %4; font-size: %13px; font-weight: 600; }"
        "QCheckBox { color: %5; font-size: %14px; }"
        "QPushButton { color: %3; background: %6;"
        " border: 1px solid %7; border-radius: 8px;"
        " padding: 5px 12px; }"
        "QPushButton:hover { background: %8; }"
        "QPushButton#primary { background: %9; color: %10;"
        " border: none; }"
        "QPushButton#primary:hover { background: %11; }")
        .arg(tk("dialog.bg"), tk("w24"), tk("text.dim"))
        .arg(tk("text"), tk("muted"), tk("w8"))
        .arg(tk("w20"), tk("w14"), tk("accent"))
        .arg(tk("on.accent")).arg(tk("accent.hover"))
        .arg(QString::number(ThemeManager::scaledPx(12)),
             QString::number(ThemeManager::scaledPx(15)),
             QString::number(ThemeManager::scaledPx(11))));

    auto *lay = new QVBoxLayout(&dialog);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(10);

    auto *title =
        new QLabel(QString::fromUtf8("发现新版本 v%1").arg(info.version));
    title->setObjectName("newVer");
    lay->addWidget(title);

    QString meta = QString::fromUtf8("当前 v%1").arg(appVersion());
    if (!info.date.isEmpty()) {
        meta = QString::fromUtf8("%1 · 发布于 %2").arg(meta, info.date);
    }
    lay->addWidget(new QLabel(meta));

    if (!info.title.isEmpty()) {
        auto *headline = new QLabel(info.title);
        headline->setStyleSheet(QStringLiteral("color:%1;")
                                    .arg(ThemeManager::instance().css(
                                        QStringLiteral("text"))));
        headline->setWordWrap(true);
        lay->addWidget(headline);
    }
    for (const QString &note : info.notes) {
        auto *line = new QLabel(QString::fromUtf8("· ") + note);
        line->setWordWrap(true);
        lay->addWidget(line);
    }

    auto *skip = new QCheckBox(QString::fromUtf8("跳过这个版本"));
    lay->addWidget(skip);

    auto *buttons = new QHBoxLayout;
    auto *later = new QPushButton(QString::fromUtf8("稍后"));
    connect(later, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto *quark = new QPushButton(QString::fromUtf8("夸克网盘"));
    quark->setVisible(!info.quark.isEmpty());
    connect(quark, &QPushButton::clicked, &dialog, [&dialog, info] {
        QDesktopServices::openUrl(QUrl(info.quark));
        dialog.accept();
    });
    auto *download = new QPushButton(QString::fromUtf8("去下载"));
    download->setObjectName("primary");
    connect(download, &QPushButton::clicked, &dialog, [&dialog, info] {
        const QString target =
            !info.baidu.isEmpty() ? info.baidu
                                  : (!info.site.isEmpty() ? info.site : info.quark);
        if (!target.isEmpty()) {
            QDesktopServices::openUrl(QUrl(target));
        }
        dialog.accept();
    });
    buttons->addWidget(skip);
    buttons->addStretch(1);
    buttons->addWidget(later);
    buttons->addWidget(quark);
    buttons->addWidget(download);
    lay->addLayout(buttons);

    dialog.exec();
    if (skip->isChecked()) {
        QSettings settings;
        settings.setValue(QStringLiteral("update/skip_version"), info.version);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (tray_ && !quitting_ && store_->uiBool("close_to_tray", true)) {
        hide();
        // 关窗进托盘：语音桌宠顶上，监听/录音状态一眼可见。
        voicePet_->presentIfEnabled();
        if (!trayHintShown_) {
            trayHintShown_ = true;
            tray_->showMessage(
                QString::fromUtf8("AgentRMC 仍在后台"),
                QString::fromUtf8("监听继续；点托盘图标叫回主界面"),
                QSystemTrayIcon::Information, 3000);
        }
        event->ignore();
        return;
    }
    quitting_ = true;
    QMainWindow::closeEvent(event);
    QApplication::quit();
}
