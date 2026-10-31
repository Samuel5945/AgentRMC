#include "AppPage.h"

#include "core/AppLauncher.h"
#include "core/Profiles.h"
#include "ui/ToggleSwitch.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

AppPage::AppPage(ProfileStore *store, QWidget *parent)
    : QFrame(parent), store_(store) {
    setObjectName("card");

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(18, 15, 16, 13);
    lay->setSpacing(9);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("应用启动"));
    title->setObjectName("cardTitle");
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
    rows_ = new QVBoxLayout(holder);
    rows_->setContentsMargins(0, 0, 6, 0);
    rows_->setSpacing(3);
    scroll->setWidget(holder);
    lay->addWidget(scroll, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(8);
    summary_ = new QLabel;
    summary_->setObjectName("muted");
    summary_->setWordWrap(true);
    const QString tip = QStringLiteral(
        "开着时，本地识别出来的「打开 xx」会直接启动应用，不再贴进输入框；"
        "想把这句话原样打给 Agent，就关掉它。");
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
    lay->addLayout(foot);

    rebuild();
}

void AppPage::rebuild() {
    while (QLayoutItem *item = rows_->takeAt(0)) {
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    int installed = 0;
    const QVector<AppEntry> &catalog = AppLauncher::catalog();
    for (const AppEntry &entry : catalog) {
        const AppTarget target = AppLauncher::resolve(entry);
        if (target.found) {
            ++installed;
        }

        auto *row = new QWidget;
        row->setObjectName("appRow");
        row->setFixedHeight(31);
        auto *r = new QHBoxLayout(row);
        r->setContentsMargins(9, 0, 5, 0);
        r->setSpacing(10);

        auto *name = new QLabel(entry.name);
        name->setObjectName("appName");
        name->setFixedWidth(92);
        r->addWidget(name);

        auto *state = new QLabel(target.found ? target.detail
                                              : QString::fromUtf8("没装"));
        state->setObjectName(target.found ? "appState" : "appStateMiss");
        state->setToolTip(target.found ? target.target
                                       : QString::fromUtf8(
                                             "开始菜单里没有这个应用的快捷方式"));
        r->addWidget(state, 1);

        auto *open = new QPushButton(QString::fromUtf8("打开"));
        open->setObjectName("ghostSmall");
        open->setCursor(target.found ? Qt::PointingHandCursor
                                     : Qt::ArrowCursor);
        open->setEnabled(target.found);
        open->setToolTip(QString::fromUtf8("现在打开一次（语音说「打开 %1」同样有效）")
                             .arg(entry.name));
        const QString appId = entry.id;
        const QString appName = entry.name;
        connect(open, &QPushButton::clicked, this, [this, appId, appName] {
            QString error;
            const bool ok = AppLauncher::launch(appId, &error);
            emit launched(appName, ok, error);
        });
        r->addWidget(open);

        rows_->addWidget(row);
    }
    rows_->addStretch(1);

    summary_->setText(
        QString::fromUtf8("本机找到 %1 / %2 个 · 语音启动要用本地识别（SenseVoice / Qwen3）；"
                          "转发热键时语音交给前台应用。想用手上的遥控器，"
                          "可以在「按键映射」里把「打开 xx」设成某个键的单击 / 双击 / 长按。")
            .arg(installed)
            .arg(catalog.size()));
}
