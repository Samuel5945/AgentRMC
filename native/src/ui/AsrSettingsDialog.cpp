#include "ui/AsrSettingsDialog.h"

#include "core/Profiles.h"
#include "core/VoiceSettings.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

AsrSettingsDialog::AsrSettingsDialog(ProfileStore *store, QWidget *parent)
    : QDialog(parent), store_(store) {
    setObjectName("voiceDialog");  // 复用语音设置对话框的深色样式
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(460);

    const QJsonObject asrApi =
        store_->voiceConfig().value("asr_api").toObject();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("云端 ASR"));
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

    const auto addRow = [root](const QString &labelText, QWidget *field) {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(labelText);
        label->setObjectName("muted");
        label->setFixedWidth(98);
        row->addWidget(label);
        row->addWidget(field, 1);
        root->addLayout(row);
    };

    // 两种协议：MiMo 式（音频进 chat/completions）和 OpenAI 式（multipart 转写）。
    style_ = new QComboBox;
    style_->addItem(QString::fromUtf8("MiMo 式 · 音频进对话接口"), "chat");
    style_->addItem(QString::fromUtf8("OpenAI 式 · /audio/transcriptions"),
                    "transcriptions");
    const QString styleKey =
        asrApi.value("style").toString().trimmed().toLower() ==
                QStringLiteral("transcriptions")
            ? QStringLiteral("transcriptions")
            : QStringLiteral("chat");
    style_->setCurrentIndex(style_->findData(styleKey));
    style_->setToolTip(QString::fromUtf8(
        "小米 MiMo（mimo-v2.5-asr）选 MiMo 式；OpenAI/硅基流动/Groq 的 "
        "whisper 类选 OpenAI 式。"));
    addRow(QString::fromUtf8("协议"), style_);

    baseUrl_ = new QLineEdit(asrApi.value("base_url").toString());
    baseUrl_->setPlaceholderText(
        QString::fromUtf8("留空 = 用语音反馈（TTS）的 Base URL"));
    baseUrl_->setToolTip(QString::fromUtf8(
        "OpenAI 兼容根地址（/audio/transcriptions 会被自动拼在后面）。"
        "留空时和 TTS 用同一个平台。"));
    addRow(QString::fromUtf8("Base URL"), baseUrl_);

    apiKey_ = new QLineEdit(asrApi.value("api_key").toString());
    apiKey_->setEchoMode(QLineEdit::Password);
    apiKey_->setPlaceholderText(
        QString::fromUtf8("留空 = 用语音反馈（TTS）的 API Key"));
    apiKey_->setToolTip(QString::fromUtf8(
        "只保存在本机 config.json。不同平台才需要单独填。"));
    addRow(QString::fromUtf8("API Key"), apiKey_);

    // MiMo 平台免费注册入口（TTS/云端 ASR 同一个 Key）。
    // MiMo 平台免费注册入口：TTS/云端 ASR/对话模型都能用免费额度。
    auto *mimoReg = new QPushButton(QString::fromUtf8("免费注册小米 MiMo（有免费额度）"));
    mimoReg->setObjectName("guideLink");
    mimoReg->setCursor(Qt::PointingHandCursor);
    connect(mimoReg, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://platform.xiaomimimo.com?ref=5JPFPY")));
    });
    addRow(QString::fromUtf8("没有 Key？"), mimoReg);

    model_ = new QComboBox;
    model_->setEditable(true);
    model_->addItem(QStringLiteral("mimo-v2.5-asr"));
    model_->addItem(QStringLiteral("FunAudioLLM/SenseVoiceSmall"));
    model_->addItem(QStringLiteral("whisper-large-v3"));
    model_->addItem(QStringLiteral("whisper-1"));
    model_->addItem(QStringLiteral("gpt-4o-transcribe"));
    model_->setCurrentText(asrApi.value("model").toString());
    model_->setToolTip(QString::fromUtf8(
        "对应平台的 ASR 模型名，必填。MiMo 平台用 mimo-v2.5-asr（选 MiMo 式）；"
        "硅基流动 SenseVoiceSmall、Groq whisper-large-v3、OpenAI whisper-1 "
        "（选 OpenAI 式）。"));
    addRow(QString::fromUtf8("模型"), model_);

    note_ = new QLabel(QString::fromUtf8(
        "选到「云端 ASR」后：按住语音键说话，松手上传转写并粘贴，"
        "「试一句」同样可用。音频会上传到上面填的服务，本机不留存。"));
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
    connect(save, &QPushButton::clicked, this, &AsrSettingsDialog::save);
    foot->addStretch(1);
    foot->addWidget(cancel);
    foot->addWidget(save);
    root->addLayout(foot);
}

void AsrSettingsDialog::save() {
    QJsonObject patch = store_->voiceConfig().value("asr_api").toObject();
    patch["style"] = style_->currentData().toString();
    patch["base_url"] = baseUrl_->text().trimmed();
    patch["api_key"] = apiKey_->text().trimmed();
    patch["model"] = model_->currentText().trimmed();
    if (!store_->updateVoice(QJsonObject{{"asr_api", patch}})) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    emit saved();
    accept();
}
