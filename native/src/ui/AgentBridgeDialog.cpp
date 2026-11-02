#include "ui/AgentBridgeDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 一行状态里的名字列宽：宿主名最长是「DeepSeek Harness（desktop）」。
constexpr int kNameWidth = 210;

QString hostLine(const QJsonObject &row) {
    const QString installed = row.value(QStringLiteral("installed")).toString();
    if (installed.isEmpty()) {
        return QStringLiteral("未装");
    }
    QString text = QString::fromUtf8("已装 %1").arg(installed);
    if (!row.value(QStringLiteral("enabled")).toBool()) {
        text += QString::fromUtf8(" · 未启用");
    }
    const QString loaded = row.value(QStringLiteral("loaded")).toString();
    if (!loaded.isEmpty() && loaded != installed) {
        text += QString::fromUtf8(" · 进程 %1（重启宿主才加载）").arg(loaded);
    }
    return text;
}

}  // namespace

AgentBridgeDialog::AgentBridgeDialog(QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("voiceDialog"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setFixedWidth(560);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("接入 Agent（桌宠提醒）"));
    title->setObjectName(QStringLiteral("captureTitle"));
    auto *close = new QPushButton(QString::fromUtf8("✕"));
    close->setObjectName(QStringLiteral("ghostText"));
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedWidth(24);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(close);
    root->addLayout(head);

    auto *intro = new QLabel(QString::fromUtf8(
        "装一次就够：等工具授权、等选方案、一轮结束时，桌宠自己冒气泡念一句。"
        "下面这句话通用——复制它，粘给任意一个正在干活的 Agent，"
        "它会认出自己在哪个宿主里，按那一宿主的方式装好。"));
    intro->setObjectName(QStringLiteral("secBody"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *statusTitle = new QLabel(QString::fromUtf8("本机现状"));
    statusTitle->setObjectName(QStringLiteral("muted"));
    root->addWidget(statusTitle);

    auto *statusBox = new QWidget;
    statusLay_ = new QVBoxLayout(statusBox);
    statusLay_->setContentsMargins(0, 0, 0, 0);
    statusLay_->setSpacing(6);
    root->addWidget(statusBox);

    hint_ = new QLabel(QString::fromUtf8(
        "粘过去就不用管了；装完让那个 Agent 重启对应宿主，新代码才会加载。"
        "OpenSquilla 不走插件（靠外部桥），单独装。"));
    hint_->setObjectName(QStringLiteral("muted"));
    hint_->setWordWrap(true);
    root->addWidget(hint_);

    // 手动装的人只要认这一个目录：zip、目录市场源、DSH 的 bundle 都在里面。
    path_ = new QLabel;
    path_->setObjectName(QStringLiteral("muted"));
    path_->setWordWrap(true);
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(path_);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *details = new QPushButton(QString::fromUtf8("详细说明"));
    details->setObjectName(QStringLiteral("ghostSmall"));
    details->setCursor(Qt::PointingHandCursor);
    connect(details, &QPushButton::clicked, this,
            [this] { emit detailsRequested(); });
    auto *folder = new QPushButton(QString::fromUtf8("打开插件目录"));
    folder->setObjectName(QStringLiteral("ghostSmall"));
    folder->setCursor(Qt::PointingHandCursor);
    connect(folder, &QPushButton::clicked, this,
            [this] { emit openFolderRequested(); });

    copy_ = new QPushButton(QString::fromUtf8("复制安装话术"));
    copy_->setObjectName(QStringLiteral("primary"));
    copy_->setCursor(Qt::PointingHandCursor);
    copy_->setMinimumWidth(170);
    connect(copy_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(sentence_);
        copy_->setText(QString::fromUtf8("已复制 ✓ 去粘给 Agent"));
        QTimer::singleShot(2600, copy_, [this] {
            copy_->setText(QString::fromUtf8("复制安装话术"));
        });
    });

    actions->addWidget(details);
    actions->addWidget(folder);
    actions->addStretch(1);
    actions->addWidget(copy_);
    root->addLayout(actions);
}

void AgentBridgeDialog::clearRows() {
    while (QLayoutItem *item = statusLay_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

void AgentBridgeDialog::addRow(const QString &name, const QString &detail,
                               bool ok) {
    auto *row = new QWidget;
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto *dot = new QLabel(ok ? QStringLiteral("●") : QStringLiteral("○"));
    dot->setStyleSheet(ok ? QStringLiteral("color:#3fb950;")
                          : QStringLiteral("color:#8b949e;"));
    auto *nameLabel = new QLabel(name);
    nameLabel->setFixedWidth(kNameWidth);
    auto *detailLabel = new QLabel(detail);
    detailLabel->setObjectName(QStringLiteral("muted"));
    detailLabel->setWordWrap(true);
    lay->addWidget(dot);
    lay->addWidget(nameLabel);
    lay->addWidget(detailLabel, 1);
    statusLay_->addWidget(row);
}

void AgentBridgeDialog::setData(const QJsonObject &health,
                                const QString &sentence,
                                const QString &pluginsDir) {
    sentence_ = sentence;
    if (path_ != nullptr) {
        path_->setText(QString::fromUtf8("插件目录：%1").arg(pluginsDir));
        path_->setToolTip(QString::fromUtf8(
            "手动装就从这个目录拿东西：\n"
            "agentrmc-bridge.zip —— Qoder / Qoder CN 上传用\n"
            "marketplace.json —— ZCode / WorkBuddy 加目录市场源用\n"
            "deepseek-harness\\ —— DeepSeek Harness 用 plugin_manager 装这个目录"));
    }
    clearRows();

    const auto addPlain = [this](const QString &name, const QString &detail,
                                 bool ok) { addRow(name, detail, ok); };

    for (const QJsonValue &value :
         health.value(QStringLiteral("hosts")).toArray()) {
        const QJsonObject row = value.toObject();
        addPlain(row.value(QStringLiteral("name")).toString(), hostLine(row),
                 !row.value(QStringLiteral("installed")).toString().isEmpty());
    }
    // DSH 的 bundle：多 profile 时只报装了的那些，和系统页保持同一口径。
    QList<QJsonObject> bundles;
    for (const QJsonValue &value :
         health.value(QStringLiteral("bundles")).toArray()) {
        const QJsonObject row = value.toObject();
        if (!row.value(QStringLiteral("installed")).toString().isEmpty()) {
            bundles.append(row);
        }
    }
    const QJsonArray allBundles =
        health.value(QStringLiteral("bundles")).toArray();
    if (bundles.isEmpty() && !allBundles.isEmpty()) {
        addPlain(allBundles.first()
                     .toObject()
                     .value(QStringLiteral("name"))
                     .toString(),
                 QString::fromUtf8("未装（DSH 里用 plugin_manager 装，见话术）"),
                 false);
    }
    for (const QJsonObject &row : bundles) {
        QString name = row.value(QStringLiteral("name")).toString();
        const QString profile = row.value(QStringLiteral("profile")).toString();
        if (allBundles.size() > 1 && !profile.isEmpty()) {
            name += QString::fromUtf8("（%1）").arg(profile);
        }
        addPlain(name, hostLine(row), true);
    }
    // 外部桥（OpenSquilla）不占插件版本，但要让人知道它已经在。
    for (const QJsonValue &value :
         health.value(QStringLiteral("bridges")).toArray()) {
        const QJsonObject row = value.toObject();
        const bool running = row.value(QStringLiteral("running")).toBool();
        addPlain(row.value(QStringLiteral("name")).toString(),
                 QString::fromUtf8("外部桥 · %1（不用装插件）")
                     .arg(running ? QString::fromUtf8("运行中")
                                  : QString::fromUtf8("未启动")),
                 running);
    }
}
