#include "AppPage.h"

#include "core/AppLauncher.h"
#include "core/Profiles.h"
#include "ui/ToggleSwitch.h"

#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStandardPaths>
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
    auto *add = new QPushButton(QString::fromUtf8("添加应用"));
    add->setObjectName("ghostText");
    add->setCursor(Qt::PointingHandCursor);
    add->setToolTip(QString::fromUtf8(
        "加一个自定义应用：填名字和一个 exe/快捷方式/网址路径，"
        "之后说「打开 名字」就能启动。"));
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

        if (entry.id.startsWith(QStringLiteral("custom:"))) {
            auto *del = new QPushButton(QString::fromUtf8("删"));
            del->setObjectName("ghostSmall");
            del->setCursor(Qt::PointingHandCursor);
            del->setToolTip(QString::fromUtf8("从自定义应用里移除（不影响应用本身）"));
            const QString name = entry.name;
            const QString path = entry.customPath;
            connect(del, &QPushButton::clicked, this,
                    [this, name, path] { removeCustomApp(name, path); });
            r->addWidget(del);
        }

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
            note->setStyleSheet(QStringLiteral("color:#f85149;"));
            note->setText(QString::fromUtf8("名称和路径都要填。"));
            return;
        }
        QJsonArray apps = store_->uiArray(QStringLiteral("custom_apps"));
        for (const QJsonValue &value : apps) {
            if (value.toObject().value("name").toString().compare(
                    name, Qt::CaseInsensitive) == 0) {
                note->setStyleSheet(QStringLiteral("color:#f85149;"));
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
