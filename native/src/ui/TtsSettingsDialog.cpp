#include "ui/TtsSettingsDialog.h"

#include "core/AudioOut.h"
#include "core/Profiles.h"
#include "core/TtsClient.h"
#include "core/VoiceSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

TtsSettingsDialog::TtsSettingsDialog(ProfileStore *store, QWidget *parent)
    : QDialog(parent), store_(store) {
    // 复用语音设置对话框的整套深色样式。
    setObjectName("voiceDialog");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(460);

    const QJsonObject voice = store_->voiceConfig();
    const QJsonObject tts = voice.value("tts").toObject();
    const VoiceOptions defaults = resolveVoiceOptions(
        store_->config(), QJsonObject{},
        QCoreApplication::applicationDirPath());

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("语音反馈（TTS）"));
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

    enabled_ = new QCheckBox(QString::fromUtf8("启用桌宠语音反馈"));
    enabled_->setChecked(tts.value("enabled").toBool(true));
    root->addWidget(enabled_);

    const auto addRow = [root](const QString &labelText, QWidget *field) {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(labelText);
        label->setObjectName("muted");
        label->setFixedWidth(98);
        row->addWidget(label);
        row->addWidget(field, 1);
        root->addLayout(row);
    };

    baseUrl_ = new QLineEdit(tts.value("base_url").toString());
    baseUrl_->setPlaceholderText(defaults.ttsBaseUrl);
    baseUrl_->setToolTip(QString::fromUtf8(
        "OpenAI 兼容的 API 根地址（不含 /chat/completions）。"
        "默认小米 MiMo；改成 OpenAI / 硅基流动 / DashScope 等同样可用。"));
    addRow(QString::fromUtf8("Base URL"), baseUrl_);

    apiKey_ = new QLineEdit(tts.value("api_key").toString());
    apiKey_->setEchoMode(QLineEdit::Password);
    apiKey_->setPlaceholderText(
        QString::fromUtf8("sk-… 在对应平台自填；留空则只有气泡不出声"));
    apiKey_->setToolTip(QString::fromUtf8(
        "只保存在本机 config.json，不会随配置上传。也可以设环境变量 "
        "MIMO_API_KEY，此处留空即可。"));
    addRow(QString::fromUtf8("API Key"), apiKey_);

    model_ = new QComboBox;
    model_->setEditable(true);
    model_->addItem(QStringLiteral("mimo-v2.5-tts"));
    model_->addItem(QStringLiteral("gpt-4o-mini-tts"));
    model_->addItem(QStringLiteral("tts-1"));
    model_->addItem(QStringLiteral("cosyvoice-v2"));
    model_->setCurrentText(tts.value("model").toString());
    if (model_->currentText().isEmpty()) {
        model_->setCurrentText(defaults.ttsModel);
    }
    model_->setToolTip(QString::fromUtf8(
        "对应平台的 TTS 模型名，可以直接改；默认 mimo-v2.5-tts（限时免费）。"));
    addRow(QString::fromUtf8("TTS 模型"), model_);

    voice_ = new QComboBox;
    voice_->setEditable(true);
    voice_->addItem(QStringLiteral("Chloe"));
    voice_->setCurrentText(tts.value("voice").toString());
    if (voice_->currentText().isEmpty()) {
        voice_->setCurrentText(defaults.ttsVoice);
    }
    voice_->setToolTip(QString::fromUtf8(
        "对应平台精品音色库的音色名；换模型/平台后记得换成它支持的音色。"));
    addRow(QString::fromUtf8("音色"), voice_);

    // 输出设备：软件只能从这里的列表选，系统默认还是耳机自己定。
    output_ = new QComboBox;
    output_->addItem(QString::fromUtf8("跟随系统默认"), QString());
    const QString savedDevice = tts.value("output_device").toString();
    for (const QString &name : AudioOut::deviceNames()) {
        output_->addItem(name, name);
        if (name.compare(savedDevice, Qt::CaseInsensitive) == 0) {
            output_->setCurrentIndex(output_->count() - 1);
        }
    }
    output_->setToolTip(QString::fromUtf8(
        "桌宠说话从哪个设备出声。列表来自当前插着的输出设备，"
        "插上耳机/音箱后重新打开这个设置刷新。"));
    addRow(QString::fromUtf8("输出设备"), output_);

    auto *previewRow = new QHBoxLayout;
    preview_ = new QPushButton(QString::fromUtf8("试听"));
    preview_->setObjectName("ghostSmall");
    preview_->setCursor(Qt::PointingHandCursor);
    connect(preview_, &QPushButton::clicked, this,
            &TtsSettingsDialog::preview);
    previewRow->addWidget(preview_);
    previewRow->addStretch(1);
    root->addLayout(previewRow);

    note_ = new QLabel(QString::fromUtf8(
        "桌宠可见时会念出反馈：切识别方式、复制、打开应用，"
        "右键菜单可「朗读最近一句」并随时开关语音反馈。"));
    note_->setObjectName("dialogNote");
    note_->setWordWrap(true);
    root->addWidget(note_);

    auto *foot = new QHBoxLayout;
    auto *cancel = new QPushButton(QString::fromUtf8("取消"));
    cancel->setObjectName("ghostSmall");
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto *save = new QPushButton(QString::fromUtf8("保存"));
    save->setObjectName("primary");
    save->setCursor(Qt::PointingHandCursor);
    save->setFixedWidth(86);
    connect(save, &QPushButton::clicked, this, &TtsSettingsDialog::save);
    foot->addStretch(1);
    foot->addWidget(cancel);
    foot->addWidget(save);
    root->addLayout(foot);

    // 试听用独立的客户端，字段没保存也能先验一遍。
    tts_ = new TtsClient(this);
    connect(tts_, &TtsClient::audioReady, this, [this] {
        preview_->setEnabled(true);
        note_->setStyleSheet(QStringLiteral("color:#3fb950;"));
        note_->setText(QString::fromUtf8("✓ 已合成并播放"));
    });
    connect(tts_, &TtsClient::failed, this, [this](const QString &error) {
        preview_->setEnabled(true);
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        note_->setText(QString::fromUtf8("试听失败：%1").arg(error));
    });
}

void TtsSettingsDialog::preview() {
    // 试听直接用当前选中的输出设备，保存之前就能确认耳机里有没有声。
    tts_->setOptions(true, baseUrl_->text(), apiKey_->text(),
                     model_->currentText(), voice_->currentText(), QString(),
                     output_->currentData().toString());
    if (apiKey_->text().trimmed().isEmpty()) {
        note_->setStyleSheet(QStringLiteral("color:#d29922;"));
        note_->setText(QString::fromUtf8("先填 API Key 再试听。"));
        return;
    }
    preview_->setEnabled(false);
    note_->setStyleSheet(QString());
    note_->setText(QString::fromUtf8("合成中…（约几秒）"));
    tts_->speak(QString::fromUtf8("你好，我是你的语音桌宠"));
}

void TtsSettingsDialog::save() {
    // tts 对象整体读改写：updateVoice 是浅合并，直接塞新对象会把没改的字段冲掉。
    QJsonObject patch = store_->voiceConfig().value("tts").toObject();
    patch["enabled"] = enabled_->isChecked();
    patch["base_url"] = baseUrl_->text().trimmed();
    patch["api_key"] = apiKey_->text().trimmed();
    patch["model"] = model_->currentText().trimmed();
    if (voice_->currentText().trimmed().isEmpty()) {
        patch.remove("voice");
    } else {
        patch["voice"] = voice_->currentText().trimmed();
    }
    patch["output_device"] = output_->currentData().toString();
    if (!store_->updateVoice(QJsonObject{{"tts", patch}})) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    tts_->stop();
    emit saved();
    accept();
}
