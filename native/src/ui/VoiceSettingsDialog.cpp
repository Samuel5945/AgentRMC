#include "VoiceSettingsDialog.h"

#include "core/Profiles.h"
#include "core/VoiceSettings.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

VoiceSettingsDialog::VoiceSettingsDialog(ProfileStore *store, QWidget *parent)
    : QDialog(parent), store_(store) {
    setObjectName("voiceDialog");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(460);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("语音设置"));
    title->setObjectName("captureTitle");
    auto *close = new QPushButton(QString::fromUtf8("✕"));
    close->setObjectName("ghostText");
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedWidth(24);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(close);
    root->addLayout(head);

    const QJsonObject voice = store_->voiceConfig();
    const VoiceOptions defaults = resolveVoiceOptions(
        store_->config(), QJsonObject{},
        QCoreApplication::applicationDirPath());
    senseDefault_ = defaults.senseVoiceDir;
    qwenDefault_ = defaults.qwen3Dir;

    auto *backendRow = new QHBoxLayout;
    auto *backendLabel = new QLabel(QString::fromUtf8("识别方式"));
    backendLabel->setObjectName("muted");
    backendLabel->setFixedWidth(98);
    backend_ = new QComboBox;
    backend_->addItem(QString::fromUtf8("转发当前应用热键（0 模型）"), "hotkey");
    backend_->addItem(QString::fromUtf8("本地 SenseVoice（约 230MB）"), "sensevoice");
    backend_->addItem(QString::fromUtf8("本地 Qwen3-ASR（约 1GB）"), "qwen3");
    QString backendKey = voice.value("backend").toString();
    if (backendKey.isEmpty()) {
        backendKey = voice.value("mode").toString() == QStringLiteral("local")
                         ? QStringLiteral("sensevoice")
                         : QStringLiteral("hotkey");
    }
    const int index = backend_->findData(backendKey);
    backend_->setCurrentIndex(index >= 0 ? index : 0);
    backendRow->addWidget(backendLabel);
    backendRow->addWidget(backend_, 1);
    root->addLayout(backendRow);

    auto addDirRow = [&](const QString &labelText, const QString &value,
                         const QString &placeholder, QLineEdit **edit,
                         QLabel **state) {
        auto *row = new QVBoxLayout;
        row->setSpacing(4);
        auto *line = new QHBoxLayout;
        auto *label = new QLabel(labelText);
        label->setObjectName("muted");
        label->setFixedWidth(98);
        *edit = new QLineEdit(value);
        (*edit)->setPlaceholderText(placeholder);
        auto *browse = new QPushButton(QString::fromUtf8("浏览"));
        browse->setObjectName("ghostSmall");
        browse->setCursor(Qt::PointingHandCursor);
        connect(browse, &QPushButton::clicked, this,
                [this, edit] { this->browse(*edit); });
        line->addWidget(label);
        line->addWidget(*edit, 1);
        line->addWidget(browse);
        row->addLayout(line);
        auto *statusRow = new QHBoxLayout;
        statusRow->addSpacing(98);
        *state = new QLabel;
        (*state)->setObjectName("muted");
        statusRow->addWidget(*state);
        statusRow->addStretch(1);
        row->addLayout(statusRow);
        root->addLayout(row);
    };

    addDirRow(QString::fromUtf8("SenseVoice 目录"),
              voice.value("sensevoice_dir").toString(), senseDefault_, &senseDir_,
              &senseState_);
    addDirRow(QString::fromUtf8("Qwen3-ASR 目录"),
              voice.value("qwen3_dir").toString(), qwenDefault_, &qwenDir_,
              &qwenState_);

    note_ = new QLabel(QString::fromUtf8(
        "模型不随程序打包。运行 tools\\download_voice_models.bat sensevoice / qwen3 下载；"
        "也可以把模型目录指到已下载的位置。"));
    note_->setObjectName("dialogNote");
    note_->setWordWrap(true);
    root->addWidget(note_);

    paste_ = new QCheckBox(QString::fromUtf8("转写后粘贴到前台窗口"));
    paste_->setChecked(voice.value("paste").toBool(true));
    autoEnter_ = new QCheckBox(QString::fromUtf8("粘贴后自动回车"));
    autoEnter_->setChecked(voice.value("auto_enter").toBool(false));
    root->addWidget(paste_);
    root->addWidget(autoEnter_);

    auto *foot = new QHBoxLayout;
    auto *download = new QPushButton(QString::fromUtf8("下载脚本"));
    download->setObjectName("ghostSmall");
    download->setCursor(Qt::PointingHandCursor);
    connect(download, &QPushButton::clicked, this,
            &VoiceSettingsDialog::openDownloadScript);
    auto *cancel = new QPushButton(QString::fromUtf8("取消"));
    cancel->setObjectName("ghostSmall");
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto *save = new QPushButton(QString::fromUtf8("保存"));
    save->setObjectName("primary");
    save->setCursor(Qt::PointingHandCursor);
    save->setFixedWidth(86);
    connect(save, &QPushButton::clicked, this, &VoiceSettingsDialog::save);
    foot->addWidget(download);
    foot->addStretch(1);
    foot->addWidget(cancel);
    foot->addWidget(save);
    root->addLayout(foot);

    connect(backend_, &QComboBox::currentIndexChanged, this,
            [this] { refreshStatus(); });
    connect(senseDir_, &QLineEdit::textChanged, this,
            [this] { refreshStatus(); });
    connect(qwenDir_, &QLineEdit::textChanged, this,
            [this] { refreshStatus(); });
    for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
        button->setFocusPolicy(Qt::NoFocus);
    }
    refreshStatus();
}

void VoiceSettingsDialog::refreshStatus() {
    auto state = [](QLabel *label, const QString &dir, bool sense) {
        const bool ready = sense ? senseVoiceReady(dir, nullptr, nullptr)
                                 : qwen3Ready(dir, nullptr, nullptr, nullptr, nullptr);
        label->setText(ready ? QString::fromUtf8("✓ 已找到模型")
                             : QString::fromUtf8("未找到模型文件"));
        label->setStyleSheet(ready ? QStringLiteral("color:#3fb950;")
                                   : QStringLiteral("color:#d29922;"));
    };
    const QString senseDir = senseDir_->text().trimmed().isEmpty()
                                 ? senseDefault_
                                 : senseDir_->text().trimmed();
    const QString qwenDir = qwenDir_->text().trimmed().isEmpty()
                                ? qwenDefault_
                                : qwenDir_->text().trimmed();
    state(senseState_, senseDir, true);
    state(qwenState_, qwenDir, false);

    const QString backend = backend_->currentData().toString();
    if (backend == "sensevoice") {
        note_->setText(QString::fromUtf8(
            "SenseVoiceSmall int8：中文/粤语/英日韩，自带标点，CPU 即可。"
            "下载后约 230MB。"));
    } else if (backend == "qwen3") {
        note_->setText(QString::fromUtf8(
            "Qwen3-ASR 0.6B int8：52 种语言/22 种方言，体积约 1GB，"
            "CPU 转写会比 SenseVoice 慢。"));
    } else {
        note_->setText(QString::fromUtf8(
            "默认路线：不打包模型，把语音键转成当前前台场景的热键"
            "（Qoder Ctrl+Shift+V / WorkBuddy Ctrl+D）。其他应用在 profiles.json "
            "配 voice.hotkey，或改用本地转写。"));
    }
}

void VoiceSettingsDialog::browse(QLineEdit *edit) {
    const QString start = edit->text().trimmed().isEmpty()
                              ? edit->placeholderText()
                              : edit->text();
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选择模型目录"), start);
    if (!dir.isEmpty()) {
        edit->setText(QDir::toNativeSeparators(dir));
    }
}

void VoiceSettingsDialog::save() {
    QJsonObject patch;
    patch["backend"] = backend_->currentData().toString();
    patch["sensevoice_dir"] =
        QDir::fromNativeSeparators(senseDir_->text().trimmed());
    patch["qwen3_dir"] = QDir::fromNativeSeparators(qwenDir_->text().trimmed());
    patch["paste"] = paste_->isChecked();
    patch["auto_enter"] = autoEnter_->isChecked();
    if (!store_->updateVoice(patch)) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    emit saved();
    accept();
}

void VoiceSettingsDialog::openDownloadScript() {
    const QString appDir = QCoreApplication::applicationDirPath();
    QString script = appDir + "/tools/download_voice_models.bat";
    if (!QFileInfo::exists(script)) {
        script = appDir + "/../../tools/download_voice_models.bat";
    }
    if (!QFileInfo::exists(script)) {
        note_->setText(QString::fromUtf8("没找到 tools\\download_voice_models.bat"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    const QString backend = backend_->currentData().toString();
    if (backend == "hotkey") {
        note_->setText(QString::fromUtf8(
            "先在上面选择 SenseVoice 或 Qwen3-ASR，再点“下载脚本”。"));
        return;
    }
    const QString size = backend == "qwen3" ? QString::fromUtf8("约 838MB")
                                            : QString::fromUtf8("约 158MB");
    if (QMessageBox::question(
            this, QString::fromUtf8("下载模型"),
            QString::fromUtf8("将运行下载脚本，模型包 %1，保存到 native\\models。继续？")
                .arg(size)) != QMessageBox::Yes) {
        return;
    }
    QStringList args{"/c", "start", "", QDir::toNativeSeparators(script)};
    args.append(backend);
    QProcess::startDetached("cmd.exe", args);
}
