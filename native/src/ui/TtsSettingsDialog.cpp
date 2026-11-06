#include "ui/TtsSettingsDialog.h"

#include "core/AudioOut.h"
#include "core/AudioRecorder.h"
#include "core/Profiles.h"
#include "core/ThemeManager.h"
#include "core/TtsClient.h"
#include "core/VoiceSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
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
        "OpenAI 兼容的 API 根地址（不含 /chat/completions）\n"
        "默认小米 MiMo；改成 OpenAI / 硅基流动 / DashScope 等同样可用"));
    addRow(QString::fromUtf8("Base URL"), baseUrl_);

    apiKey_ = new QLineEdit(tts.value("api_key").toString());
    apiKey_->setEchoMode(QLineEdit::Password);
    apiKey_->setPlaceholderText(
        QString::fromUtf8("sk-… 在对应平台自填；留空则只有气泡不出声"));
    apiKey_->setToolTip(QString::fromUtf8(
        "只保存在本机 config.json，不会随配置上传\n"
        "也可以设环境变量 MIMO_API_KEY，此处留空即可"));
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
        "对应平台的 TTS 模型名，可以直接改\n"
        "默认 mimo-v2.5-tts（限时免费）"));
    addRow(QString::fromUtf8("TTS 模型"), model_);

    voice_ = new QComboBox;
    voice_->setEditable(true);
    voice_->addItem(QStringLiteral("Chloe（默认）"), QStringLiteral("Chloe"));
    // 第二档跟随形象：音色名与描述都取形象包带的（如鲸鱼娘），换形象自动跟着换。
    // 形象为空时跟随主题包自带的形象；两样都没带时该项不存在（跟随 = 退回
    // Chloe + 默认描述，没意义）。
    const Theme &activeTheme = ThemeManager::instance().current();
    if (!activeTheme.voiceStyle.isEmpty() || !activeTheme.voiceName.isEmpty()) {
        // 音色跟的是形象不是配色主题：形象没写名字时才退回主题显示名。
        const QString sourceName = activeTheme.characterName.isEmpty()
                                       ? activeTheme.displayName
                                       : activeTheme.characterName;
        voice_->addItem(QString::fromUtf8("跟随形象 · %1").arg(sourceName),
                        QStringLiteral("@theme"));
    }
    const QString savedVoice = tts.value("voice").toString().trimmed();
    const QString savedStyle = tts.value("style").toString().trimmed();
    if (isFollowThemeVoice(savedVoice) ||
        savedVoice.compare(QStringLiteral("Chloe"), Qt::CaseInsensitive) == 0) {
        // 旧版「跟随主题 · X」文案先归一成 @theme 再匹配。
        const QString normalized =
            isFollowThemeVoice(savedVoice) ? QStringLiteral("@theme") : savedVoice;
        const int saved = voice_->findData(normalized);
        // 没写自定义描述且形象带音色 = 形象音色正在生效，选回「跟随形象」。
        voice_->setCurrentIndex(
            saved >= 0
                ? saved
                : (savedStyle.isEmpty() && voice_->findData(QStringLiteral("@theme")) >= 0
                       ? voice_->findData(QStringLiteral("@theme"))
                       : 0));
    } else {
        const int saved = voice_->findData(savedVoice);
        if (saved >= 0) {
            voice_->setCurrentIndex(saved);
        } else {
            voice_->setCurrentText(savedVoice);  // 自定义音色名
        }
    }
    connect(voice_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (voice_->itemData(index).toString() == QStringLiteral("@theme")) {
            styleEdit_->clear();  // 空描述 = 交给形象音色
        }
    });
    voice_->setToolTip(QString::fromUtf8(
        "Chloe = 平台默认音色\n"
        "跟随形象 = 音色名与描述都用当前形象自带的（换形象自动跟着换）\n"
        "也可以直接手输平台支持的任意音色名\n"
        "换模型/平台后记得换配套音色"));
    addRow(QString::fromUtf8("音色"), voice_);

    // 文字音色设定：MiMo 这类 TTS 支持用一句描述定制嗓音和语气，空 = 默认。
    styleEdit_ = new QLineEdit(tts.value("style").toString());
    const QString themeStyle =
        ThemeManager::instance().current().voiceStyle;
    styleEdit_->setPlaceholderText(
        themeStyle.isEmpty()
            ? QString::fromUtf8("例：温柔的年轻女性，语速稍慢、声音清亮；"
                                "留空 = 内置默认（轻快清晰）")
            : themeStyle + QString::fromUtf8("（当前主题的音色，正在生效）"));
    styleEdit_->setToolTip(QString::fromUtf8(
        "用一句自然语言描述想要的嗓音、语气、语速，合成时作为指令发给模型\n"
        "想正式一点可以写「沉稳的男声，播音腔」\n"
        "对话多的话别写太长的句子"));
    addRow(QString::fromUtf8("音色描述"), styleEdit_);

    // 输入设备：默认档跟随系统「默认通信录音设备」，但那档被蓝牙耳机麦或虚拟麦
    // 占着、且还有真麦克风时会自动改用真麦；再不准就在下面点名。和「输出设备」配成一对。
    input_ = new QComboBox;
    input_->addItem(QString::fromUtf8("自动（优先真麦克风，躲开耳机麦/虚拟麦）"),
                    QString());
    const QStringList inputs = AudioRecorder::waveInNames();
    QString savedInput = voice.value("input_device").toString().trimmed();
    if (savedInput.isEmpty() && voice.value("input_device").isDouble()) {
        // 老写法填的是设备序号：换成当前列表里的名字，界面和运行时看到的是同一台。
        const int index = voice.value("input_device").toInt(-1);
        if (index >= 0 && index < inputs.size()) {
            savedInput = inputs.at(index).trimmed();
        }
    }
    for (const QString &raw : inputs) {
        // WinMM 名字只有 31 字符宽，截断后常留尾空格；savedInput 已 trim，
        // 这里不 trim 就认不出是同一台，会多出一条假的「现在没插着」。
        // 归一化口径跟运行时的 matchWaveInIndex 保持一致。
        const QString name = raw.trimmed();
        input_->addItem(name, name);
        if (name.compare(savedInput, Qt::CaseInsensitive) == 0) {
            input_->setCurrentIndex(input_->count() - 1);
        }
    }
    if (!savedInput.isEmpty() &&
        input_->currentData().toString().compare(savedInput, Qt::CaseInsensitive) != 0) {
        // 保存过的那台现在不在列表里（拔了/改名，或压根没连这台电脑）：
        // 不再列成可选项——留着就等人误选一台打不开的设备，说完没反应。
        // 回到自动就行，不用额外解释：设备连回来时它自己会重新出现在列表里。
        input_->setCurrentIndex(0);
    }
    const QString autoName = AudioRecorder::communicationsDeviceName();
    input_->setToolTip(QString::fromUtf8(
        "说话时从哪个麦克风收声\n"
        "自动 = 跟着系统「默认通信录音设备」（现在是「%1」）；那一档被蓝牙耳机麦\n"
        "或虚拟麦占着、列表里还有真麦克风时，自动改用真麦克风\n"
        "列表来自当前插着的输入设备，插上新麦后重开这里刷新\n"
        "点名哪台就用哪台，保存后立即生效，不用重启")
                           .arg(autoName.isEmpty() ? QString::fromUtf8("没有")
                                                   : autoName));
    addRow(QString::fromUtf8("输入设备"), input_);

    // 输出设备：软件只能从这里的列表选，系统默认还是耳机自己定。
    output_ = new QComboBox;
    output_->addItem(QString::fromUtf8("跟随系统默认"), QString());
    const QString savedDevice = tts.value("output_device").toString().trimmed();
    for (const QString &raw : AudioOut::deviceNames()) {
        const QString name = raw.trimmed();
        output_->addItem(name, name);
        if (name.compare(savedDevice, Qt::CaseInsensitive) == 0) {
            output_->setCurrentIndex(output_->count() - 1);
        }
    }
    output_->setToolTip(QString::fromUtf8(
        "桌宠说话从哪个设备出声\n"
        "列表来自当前插着的输出设备\n"
        "插上耳机/音箱后重新打开这个设置刷新"));
    addRow(QString::fromUtf8("输出设备"), output_);

    auto *previewRow = new QHBoxLayout;
    preview_ = new QPushButton(QString::fromUtf8("试听"));
    preview_->setObjectName("ghostSmall");
    preview_->setCursor(Qt::PointingHandCursor);
    connect(preview_, &QPushButton::clicked, this,
            &TtsSettingsDialog::preview);
    previewRow->addWidget(preview_);
    previewRow->addStretch(1);
    auto *mimoReg = new QPushButton(QString::fromUtf8("没有 Key？免费注册小米 MiMo"));
    mimoReg->setObjectName("guideLink");
    mimoReg->setCursor(Qt::PointingHandCursor);
    connect(mimoReg, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://platform.xiaomimimo.com?ref=5JPFPY")));
    });
    previewRow->addWidget(mimoReg);
    root->addLayout(previewRow);

    note_ = new QLabel(QString::fromUtf8(
        "桌宠可见时会念出反馈：切识别方式、复制、打开应用。"
        "语音面板可「朗读最近」；语音反馈开关就在上面。"));
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

QString TtsSettingsDialog::resolvedVoiceName() const {
    // 预设项的显示文案（「Chloe（默认）」）不是真实音色名，真实名在 userData；
    // 自定义手输的项没有 data，取输入文本本身。
    const QString data = voice_->currentData().toString();
    if (!data.isEmpty() && data != QStringLiteral("@theme")) {
        return data;
    }
    return voice_->currentText().trimmed();
}

void TtsSettingsDialog::preview() {
    // 试听直接用当前选中的输出设备，保存之前就能确认耳机里有没有声。
    QString voiceName = resolvedVoiceName();
    QString style = styleEdit_->text();
    if (voice_->currentData().toString() == QStringLiteral("@theme")) {
        const Theme &theme = ThemeManager::instance().current();
        voiceName = theme.voiceName.isEmpty() ? QStringLiteral("Chloe")
                                              : theme.voiceName;
        style = theme.voiceStyle;
    }
    tts_->setOptions(true, baseUrl_->text(), apiKey_->text(),
                     model_->currentText(), voiceName, style,
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
    if (voice_->currentData().toString() == QStringLiteral("@theme")) {
        styleEdit_->clear();  // 跟随形象：不落自定义描述
        patch["voice"] = QStringLiteral("@theme");  // 落哨兵，运行时按形象解析
    } else {
        const QString resolved = resolvedVoiceName();
        if (resolved.isEmpty()) {
            patch.remove("voice");
        } else {
            patch["voice"] = resolved;
        }
    }
    if (styleEdit_->text().trimmed().isEmpty()) {
        patch.remove("style");
    } else {
        patch["style"] = styleEdit_->text().trimmed();
    }
    patch["output_device"] = output_->currentData().toString();
    // 录音设备写在 voice 级（voice.input_device），不塞进 tts：tts 那组只管出声。
    // 空串 = 自动跟随系统默认通信录音设备。
    if (!store_->updateVoice(QJsonObject{{QStringLiteral("tts"), patch},
                                         {QStringLiteral("input_device"),
                                          input_->currentData().toString()}})) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    tts_->stop();
    emit saved();
    accept();
}
