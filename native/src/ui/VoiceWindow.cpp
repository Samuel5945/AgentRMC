#include "VoiceWindow.h"

#include "Toast.h"
#include "VoiceSettingsDialog.h"
#include "core/Profiles.h"
#include "core/VoiceController.h"
#include "core/VoiceSettings.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWindow>

namespace {

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

        // 外环：录音时随电平加粗，给出「听到你了」的反馈。
        QPen ring(recording_ ? QColor(0x3f, 0xb9, 0x50) : QColor(79, 140, 255));
        ring.setWidth(recording_ ? int(3 + level_ * 5) : 4);
        painter.setPen(ring);
        painter.setBrush(Qt::NoBrush);
        const double radius = area.width() / 2.0 - ring.width();
        painter.drawEllipse(center, radius, radius);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x12, 0x17, 0x22));
        painter.drawEllipse(center, 44, 44);

        if (recording_) {
            painter.setBrush(QColor(0xf8, 0x51, 0x49));
            painter.drawRoundedRect(QRectF(center.x() - 13, center.y() - 13,
                                           26, 26), 5, 5);
            return;
        }

        // 麦克风：胶囊 + 拾音弧 + 底座。
        QPen pen(QColor(0xc9, 0xd1, 0xd9));
        pen.setWidth(3);
        painter.setPen(pen);
        painter.setBrush(QColor(0xc9, 0xd1, 0xd9));
        painter.drawRoundedRect(QRectF(center.x() - 9, center.y() - 22, 18,
                                       30), 9, 9);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(center.x() - 17, center.y() - 12, 34, 26),
                        180 * 16, 180 * 16);
        painter.drawLine(QPointF(center.x(), center.y() + 8),
                         QPointF(center.x(), center.y() + 15));
        painter.drawLine(QPointF(center.x() - 9, center.y() + 15),
                         QPointF(center.x() + 9, center.y() + 15));
    }

private:
    double level_ = 0.0;
    bool recording_ = false;
};

// 顶部拖拽条（同主窗 TitleBar 的做法）。
class DragStrip final : public QWidget {
public:
    explicit DragStrip(QWidget *parent) : QWidget(parent) {}

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (windowHandle() != nullptr && event->button() == Qt::LeftButton) {
            windowHandle()->startSystemMove();
        }
        QWidget::mousePressEvent(event);
    }
};

}  // namespace

VoiceWindow::VoiceWindow(VoiceController *voice, ProfileStore *store,
                         QWidget *parent)
    : QWidget(parent), voice_(voice), store_(store) {
    setObjectName("voiceWindow");
    setWindowTitle(QString::fromUtf8("AgentRMC 语音"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setFixedSize(380, 560);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *header = new DragStrip(this);
    header->setObjectName("voiceHeader");
    header->setFixedHeight(44);
    auto *head = new QHBoxLayout(header);
    head->setContentsMargins(14, 0, 10, 0);
    head->setSpacing(8);
    statusDot_ = new QLabel;
    statusDot_->setFixedSize(6, 6);
    statusDot_->setStyleSheet(QStringLiteral("background:#4f8cff;"));
    auto *title = new QLabel(QString::fromUtf8("语音"));
    title->setObjectName("brand");
    auto *full = new QPushButton(QString::fromUtf8("完整界面"));
    full->setObjectName("ghostText");
    full->setCursor(Qt::PointingHandCursor);
    full->setToolTip(QString::fromUtf8("切回完整主界面"));
    connect(full, &QPushButton::clicked, this, &VoiceWindow::fullUiRequested);
    auto *minBtn = new QPushButton(QString::fromUtf8("—"));
    minBtn->setObjectName("winBtn");
    minBtn->setCursor(Qt::PointingHandCursor);
    connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    auto *closeBtn = new QPushButton(QString::fromUtf8("✕"));
    closeBtn->setObjectName("closeBtn");
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::hide);
    head->addWidget(statusDot_);
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(full);
    head->addWidget(minBtn);
    head->addWidget(closeBtn);
    root->addWidget(header);

    auto *body = new QVBoxLayout;
    body->setContentsMargins(20, 10, 20, 14);
    body->setSpacing(8);

    mic_ = new MicButton;
    connect(mic_, &QPushButton::clicked, this, [this] {
        if (!listening_) {
            toast_->showText(QString::fromUtf8(
                "还没开始监听：托盘菜单或主界面里点「开始监听」"));
            return;
        }
        voice_->mediaToggle();
    });
    auto *micRow = new QHBoxLayout;
    micRow->addStretch(1);
    micRow->addWidget(mic_);
    micRow->addStretch(1);
    body->addLayout(micRow);

    stateLabel_ = new QLabel(QString::fromUtf8("点击开始 · 再点结束"));
    stateLabel_->setObjectName("voiceStateLine");
    stateLabel_->setAlignment(Qt::AlignHCenter);
    body->addWidget(stateLabel_);

    hintLabel_ = new QLabel;
    hintLabel_->setObjectName("muted");
    hintLabel_->setAlignment(Qt::AlignHCenter);
    refreshHint();
    body->addWidget(hintLabel_);

    transcript_ = new QLabel(QString::fromUtf8("说了话会显示在这里，点击可复制"));
    transcript_->setObjectName("voiceTranscript");
    transcript_->setWordWrap(true);
    transcript_->setMinimumHeight(64);
    transcript_->setCursor(Qt::PointingHandCursor);
    transcript_->installEventFilter(this);
    body->addWidget(transcript_);

    auto *historyTitle = new QLabel(QString::fromUtf8("最近"));
    historyTitle->setObjectName("muted");
    body->addWidget(historyTitle);

    history_ = new QListWidget;
    history_->setObjectName("voiceHistory");
    history_->setFrameShape(QFrame::NoFrame);
    history_->setSelectionMode(QAbstractItemView::NoSelection);
    history_->setFocusPolicy(Qt::NoFocus);
    history_->setWordWrap(true);
    history_->setSpacing(2);
    connect(history_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { copyText(item->data(Qt::UserRole).toString()); });
    body->addWidget(history_, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(6);
    backend_ = new QComboBox;
    backend_->addItem(QString::fromUtf8("转发热键 · 0 模型"), "hotkey");
    backend_->addItem(QString::fromUtf8("SenseVoice"), "sensevoice");
    backend_->addItem(QString::fromUtf8("Qwen3-ASR"), "qwen3");
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
                toast_->showText(QString::fromUtf8("识别引擎已切换"));
            });
    auto *settings = new QPushButton(QString::fromUtf8("模型设置"));
    settings->setObjectName("ghostSmall");
    settings->setCursor(Qt::PointingHandCursor);
    connect(settings, &QPushButton::clicked, this, [this] {
        auto *dialog = new VoiceSettingsDialog(store_, this);
        connect(dialog, &VoiceSettingsDialog::saved, this, [this] {
            voice_->reload();
            refreshHint();
        });
        dialog->exec();
        dialog->deleteLater();
    });
    foot->addWidget(backend_, 1);
    foot->addWidget(settings);
    body->addLayout(foot);

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
                statusDot_->setStyleSheet(recording_
                                              ? QStringLiteral("background:#3fb950;")
                                              : QStringLiteral("background:#4f8cff;"));
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

void VoiceWindow::copyText(const QString &text) {
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    toast_->showText(QString::fromUtf8("已复制到剪贴板"));
}

void VoiceWindow::pushHistory(const QString &text, int costMs) {
    auto *item = new QListWidgetItem(history_);
    item->setText(QStringLiteral("%1    · %2s")
                      .arg(text,
                           QString::number(costMs / 1000.0, 'f', 1)));
    item->setData(Qt::UserRole, text);
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

void VoiceWindow::closeEvent(QCloseEvent *event) {
    // ✕ 的语义是回托盘，不是退出；真退出走托盘菜单。
    event->ignore();
    hide();
}
