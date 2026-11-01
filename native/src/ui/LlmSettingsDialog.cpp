#include "ui/LlmSettingsDialog.h"

#include "core/LlmClient.h"
#include "core/Profiles.h"
#include "core/VoiceSettings.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

LlmSettingsDialog::LlmSettingsDialog(ProfileStore *store, QWidget *parent)
    : QDialog(parent), store_(store) {
    setObjectName("voiceDialog");  // 复用语音设置对话框的深色样式
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(460);

    const QJsonObject llmApi =
        store_->voiceConfig().value("llm_api").toObject();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QString::fromUtf8("桌宠对话（LLM）"));
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

    baseUrl_ = new QLineEdit(llmApi.value("base_url").toString());
    baseUrl_->setPlaceholderText(
        QString::fromUtf8("留空 = 用语音反馈（TTS）的 Base URL"));
    baseUrl_->setToolTip(QString::fromUtf8(
        "OpenAI 兼容根地址（/chat/completions 会被自动拼在后面）。"));
    addRow(QString::fromUtf8("Base URL"), baseUrl_);

    apiKey_ = new QLineEdit(llmApi.value("api_key").toString());
    apiKey_->setEchoMode(QLineEdit::Password);
    apiKey_->setPlaceholderText(
        QString::fromUtf8("留空 = 用语音反馈（TTS）的 API Key"));
    apiKey_->setToolTip(QString::fromUtf8(
        "只保存在本机 config.json。不同平台才需要单独填。"));
    addRow(QString::fromUtf8("API Key"), apiKey_);

    model_ = new QComboBox;
    model_->setEditable(true);
    model_->addItem(QStringLiteral("mimo-v2.6-flash"));
    model_->addItem(QStringLiteral("mimo-v2.6-pro"));
    model_->addItem(QStringLiteral("gpt-4o-mini"));
    model_->addItem(QStringLiteral("deepseek-chat"));
    model_->setCurrentText(llmApi.value("model").toString());
    if (model_->currentText().isEmpty()) {
        model_->setCurrentText(QStringLiteral("mimo-v2.6-flash"));
    }
    model_->setToolTip(QString::fromUtf8(
        "对话用的模型，必填。对话闭环要快，建议 flash/mini 档。"));
    addRow(QString::fromUtf8("模型"), model_);

    system_ = new QLineEdit(llmApi.value("system").toString());
    system_->setPlaceholderText(QString::fromUtf8(
        "人设/风格指令；留空用内置默认（口语化、两句话以内）"));
    addRow(QString::fromUtf8("人设"), system_);

    // 回复音色：对话模式的朗读可独立于「语音反馈」选音色；空 = 跟随。
    chatVoice_ = new QComboBox;
    chatVoice_->setEditable(true);
    chatVoice_->addItem(QStringLiteral("Chloe"));
    const QJsonObject ttsObj = store_->voiceConfig().value("tts").toObject();
    chatVoice_->setCurrentText(ttsObj.value("chat_voice").toString().trimmed());
    if (chatVoice_->lineEdit() != nullptr) {
        chatVoice_->lineEdit()->setPlaceholderText(
            QString::fromUtf8("留空 = 跟随语音反馈的音色"));
    }
    chatVoice_->setToolTip(QString::fromUtf8(
        "对话模式朗读回复用的音色；留空 = 跟随「语音反馈 → 设置」里的音色。"
        "换 TTS 平台/模型后记得换成它支持的音色名。"));
    addRow(QString::fromUtf8("回复音色"), chatVoice_);

    auto *testRow = new QHBoxLayout;
    test_ = new QPushButton(QString::fromUtf8("测试"));
    test_->setObjectName("ghostSmall");
    test_->setCursor(Qt::PointingHandCursor);
    connect(test_, &QPushButton::clicked, this, &LlmSettingsDialog::test);
    testRow->addWidget(test_);
    testRow->addStretch(1);
    root->addLayout(testRow);

    note_ = new QLabel(QString::fromUtf8(
        "右键桌宠开「对话模式」后：说话 → 转写 → 模型回答 → TTS 念出来，"
        "回复不再粘贴到输入框。桌宠收起即退出对话模式。"));
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
    connect(save, &QPushButton::clicked, this, &LlmSettingsDialog::save);
    foot->addStretch(1);
    foot->addWidget(cancel);
    foot->addWidget(save);
    root->addLayout(foot);

    llm_ = new LlmClient(this);
    connect(llm_, &LlmClient::replied, this, [this](const QString &text, qint64) {
        test_->setEnabled(true);
        QString preview = text;
        if (preview.size() > 80) {
            preview = preview.left(80) + QString::fromUtf8("…");
        }
        note_->setStyleSheet(QStringLiteral("color:#3fb950;"));
        note_->setText(QString::fromUtf8("✓ 模型回复：%1").arg(preview));
    });
    connect(llm_, &LlmClient::failed, this, [this](const QString &error, qint64) {
        test_->setEnabled(true);
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        note_->setText(QString::fromUtf8("测试失败：%1").arg(error));
    });
}

void LlmSettingsDialog::test() {
    llm_->setOptions(baseUrl_->text(), apiKey_->text(), model_->currentText(),
                     system_->text());
    if (!llm_->ready()) {
        note_->setStyleSheet(QStringLiteral("color:#d29922;"));
        note_->setText(QString::fromUtf8(
            "还差配置：模型必填，Key 可以留空沿用语音反馈的。"));
        return;
    }
    test_->setEnabled(false);
    note_->setStyleSheet(QString());
    note_->setText(QString::fromUtf8("问模型中…（几秒）"));
    QJsonArray messages{
        QJsonObject{{"role", "system"}, {"content", system_->text().trimmed().isEmpty()
                                                     ? QString::fromUtf8("你是语音桌宠，用中文口语简短回答。")
                                                     : system_->text()}},
        QJsonObject{{"role", "user"}, {"content", QString::fromUtf8("用一句话打个招呼")}}};
    llm_->chat(messages);
}

void LlmSettingsDialog::save() {
    // llm_api 对象整体读改写：updateVoice 是浅合并。
    QJsonObject patch = store_->voiceConfig().value("llm_api").toObject();
    patch["base_url"] = baseUrl_->text().trimmed();
    patch["api_key"] = apiKey_->text().trimmed();
    patch["model"] = model_->currentText().trimmed();
    const QString persona = system_->text().trimmed();
    if (persona.isEmpty()) {
        patch.remove("system");
    } else {
        patch["system"] = persona;
    }
    if (!store_->updateVoice(QJsonObject{{"llm_api", patch}})) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    // 回复音色存在 voice.tts 里（TTS 合成配置一处管），空 = 删掉跟随全局。
    QJsonObject tts = store_->voiceConfig().value("tts").toObject();
    const QString chatVoice = chatVoice_->currentText().trimmed();
    if (chatVoice.isEmpty()) {
        tts.remove("chat_voice");
    } else {
        tts["chat_voice"] = chatVoice;
    }
    if (!store_->updateVoice(QJsonObject{{"tts", tts}})) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    emit saved();
    accept();
}
