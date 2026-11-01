#include "VoiceSettingsDialog.h"

#include "core/ModelImport.h"
#include "core/Profiles.h"
#include "core/ThemeManager.h"
#include "core/VoiceSettings.h"
#include "ui/AsrSettingsDialog.h"
#include "ui/LlmSettingsDialog.h"
#include "ui/TtsSettingsDialog.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

VoiceSettingsDialog::VoiceSettingsDialog(ProfileStore *store, QWidget *parent)
    : QDialog(parent), store_(store) {
    setObjectName("voiceDialog");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(460);
    setAcceptDrops(true);

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

    // 主题：切换即时生效（渲染 QSS + 广播），选择持久化在 ui.theme。
    auto *themeRow = new QHBoxLayout;
    auto *themeLabel = new QLabel(QString::fromUtf8("主题"));
    themeLabel->setObjectName("muted");
    themeLabel->setFixedWidth(98);
    theme_ = new QComboBox;
    theme_->setIconSize(QSize(22, 22));
    connect(theme_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString id = theme_->itemData(index).toString();
        if (!id.isEmpty()) {
            ThemeManager::instance().setTheme(id);
        }
    });
    reloadThemes();
    auto *themeImport = new QPushButton(QString::fromUtf8("导入主题包…"));
    themeImport->setObjectName("ghostSmall");
    themeImport->setCursor(Qt::PointingHandCursor);
    themeImport->setToolTip(QString::fromUtf8(
        "选一个主题包 .zip（theme.json + pet/ + backgrounds/），"
        "装进 themes 并立即换上；同名主题会被更新覆盖。"));
    connect(themeImport, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, QString::fromUtf8("选择主题包"),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
            QString::fromUtf8("主题包 (*.zip)"));
        if (file.isEmpty()) {
            return;
        }
        QString error;
        const QString id =
            ThemeManager::instance().importThemePackage(file, &error);
        if (id.isEmpty()) {
            note_->setStyleSheet(QStringLiteral("color:#f85149;"));
            note_->setText(QString::fromUtf8("导入失败：%1").arg(error));
            return;
        }
        reloadThemes();
        note_->setStyleSheet(QStringLiteral("color:#3fb950;"));
        note_->setText(QString::fromUtf8("主题包已导入并启用。"));
    });
    auto *themeDir = new QPushButton(QString::fromUtf8("打开主题目录"));
    themeDir->setObjectName("ghostSmall");
    themeDir->setCursor(Qt::PointingHandCursor);
    themeDir->setToolTip(QString::fromUtf8(
        "每个主题一个文件夹（theme.json + pet 状态帧 + backgrounds）。"
        "把新主题文件夹丢进去，重开设置就能选。"));
    connect(themeDir, &QPushButton::clicked, this, [] {
        QDir().mkpath(ThemeManager::themesRoot());
        QDesktopServices::openUrl(QUrl::fromLocalFile(ThemeManager::themesRoot()));
    });
    themeRow->addWidget(themeLabel);
    themeRow->addWidget(theme_, 1);
    themeRow->addWidget(themeImport);
    themeRow->addWidget(themeDir);
    root->addLayout(themeRow);

    auto *backendRow = new QHBoxLayout;
    auto *backendLabel = new QLabel(QString::fromUtf8("识别方式"));
    backendLabel->setObjectName("muted");
    backendLabel->setFixedWidth(98);
    backend_ = new QComboBox;
    backend_->addItem(QString::fromUtf8("转发当前应用热键（推荐 · 0 模型）"), "hotkey");
    backend_->addItem(QString::fromUtf8("本地 SenseVoice（离线 · 需导入模型）"),
                      "sensevoice");
    backend_->addItem(QString::fromUtf8("本地 Qwen3-ASR（离线 · 模型约 1GB）"), "qwen3");
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

    auto *langRow = new QHBoxLayout;
    auto *langLabel = new QLabel(QString::fromUtf8("识别语言"));
    langLabel->setObjectName("muted");
    langLabel->setFixedWidth(98);
    language_ = new QComboBox;
    language_->addItem(QString::fromUtf8("自动 · 中英混合（推荐）"), "auto");
    language_->addItem(QString::fromUtf8("中文"), "zh");
    language_->addItem(QString::fromUtf8("English"), "en");
    language_->addItem(QString::fromUtf8("日本語"), "ja");
    language_->addItem(QString::fromUtf8("한국어"), "ko");
    language_->addItem(QString::fromUtf8("粤语"), "yue");
    QString languageKey = voice.value("asr_language").toString().trimmed().toLower();
    if (languageKey.isEmpty()) {
        languageKey = QStringLiteral("auto");
    }
    const int langIndex = language_->findData(languageKey);
    language_->setCurrentIndex(langIndex >= 0 ? langIndex : 0);
    language_->setToolTip(QString::fromUtf8(
        "只对 SenseVoice 生效。说「打开 WorkBuddy」这类中英混的话选自动，"
        "英文应用名不会被硬翻成汉字。"));
    langRow->addWidget(langLabel);
    langRow->addWidget(language_, 1);
    root->addLayout(langRow);

    // 无遥控器的全局键盘热键：一键开始/结束说话。
    auto *kbRow = new QHBoxLayout;
    auto *kbLabel = new QLabel(QString::fromUtf8("键盘热键"));
    kbLabel->setObjectName("muted");
    kbLabel->setFixedWidth(98);
    kbHotkey_ = new QComboBox;
    kbHotkey_->addItem(QString::fromUtf8("关闭"), "off");
    kbHotkey_->addItem(QString::fromUtf8("Ctrl+Alt+V"), "ctrl+alt+86");
    kbHotkey_->addItem(QString::fromUtf8("Ctrl+Alt+B"), "ctrl+alt+66");
    kbHotkey_->addItem(QString::fromUtf8("Ctrl+Shift+Space"), "ctrl+shift+32");
    kbHotkey_->addItem(QString::fromUtf8("Alt+V"), "alt+86");
    kbHotkey_->setToolTip(QString::fromUtf8(
        "不插遥控器也能用：全局一键开始/结束说话，静音后自动收尾。"
        "组合键被别的程序占用时会注册失败，换一个即可。"));
    const QJsonObject kb = voice.value("keyboard_hotkey").toObject();
    if (!kb.value("enabled").toBool(true)) {
        kbHotkey_->setCurrentIndex(0);
    } else {
        QStringList mods;
        for (const QJsonValue &value : kb.value("mods").toArray()) {
            mods.append(value.toString().toLower());
        }
        const QString key = QString("%1+%2")
                                .arg(mods.join(QLatin1Char('+')))
                                .arg(int(kb.value("vk").toDouble(0x56)));
        const int kbIndex = kbHotkey_->findData(key);
        kbHotkey_->setCurrentIndex(kbIndex >= 0 ? kbIndex : 1);
    }
    kbRow->addWidget(kbLabel);
    kbRow->addWidget(kbHotkey_, 1);
    root->addLayout(kbRow);

    auto addModelRow = [&](const QString &labelText, QLabel **state,
                           QPushButton **import) {
        auto *line = new QHBoxLayout;
        auto *label = new QLabel(labelText);
        label->setObjectName("muted");
        label->setFixedWidth(98);
        *state = new QLabel;
        (*state)->setObjectName("muted");
        *import = new QPushButton(QString::fromUtf8("导入模型包…"));
        (*import)->setObjectName("ghostSmall");
        (*import)->setCursor(Qt::PointingHandCursor);
        line->addWidget(label);
        line->addWidget(*state, 1);
        line->addWidget(*import);
        root->addLayout(line);
    };

    addModelRow(QString::fromUtf8("SenseVoice"), &senseState_, &senseImport_);
    addModelRow(QString::fromUtf8("Qwen3-ASR"), &qwenState_, &qwenImport_);
    connect(senseImport_, &QPushButton::clicked, this,
            [this] { importModel(true); });
    connect(qwenImport_, &QPushButton::clicked, this,
            [this] { importModel(false); });

    // 云端 ASR：不是本地模型包，给状态 + 设置入口。
    asrState_ = new QLabel;
    asrState_->setObjectName("muted");
    auto *asrRow = new QHBoxLayout;
    auto *asrLabel = new QLabel(QString::fromUtf8("云端 ASR"));
    asrLabel->setObjectName("muted");
    asrLabel->setFixedWidth(98);
    auto *asrMore = new QPushButton(QString::fromUtf8("设置…"));
    asrMore->setObjectName("ghostSmall");
    asrMore->setCursor(Qt::PointingHandCursor);
    connect(asrMore, &QPushButton::clicked, this, [this] {
        auto *dialog = new AsrSettingsDialog(store_, this);
        connect(dialog, &AsrSettingsDialog::saved, this,
                [this] { refreshStatus(); emit optionsApplied(); });
        dialog->exec();
        dialog->deleteLater();
    });
    asrRow->addWidget(asrLabel);
    asrRow->addWidget(asrState_, 1);
    asrRow->addWidget(asrMore);
    root->addLayout(asrRow);

    // 桌宠对话（LLM）：和云端 ASR 同款的状态 + 设置入口。
    llmState_ = new QLabel;
    llmState_->setObjectName("muted");
    auto *llmRow = new QHBoxLayout;
    auto *llmLabel = new QLabel(QString::fromUtf8("桌宠对话"));
    llmLabel->setObjectName("muted");
    llmLabel->setFixedWidth(98);
    auto *llmMore = new QPushButton(QString::fromUtf8("设置…"));
    llmMore->setObjectName("ghostSmall");
    llmMore->setCursor(Qt::PointingHandCursor);
    connect(llmMore, &QPushButton::clicked, this, [this] {
        auto *dialog = new LlmSettingsDialog(store_, this);
        connect(dialog, &LlmSettingsDialog::saved, this,
                [this] { refreshStatus(); emit optionsApplied(); });
        dialog->exec();
        dialog->deleteLater();
    });
    llmRow->addWidget(llmLabel);
    llmRow->addWidget(llmState_, 1);
    llmRow->addWidget(llmMore);
    root->addLayout(llmRow);

    note_ = new QLabel(QString::fromUtf8(
        "本地识别要先装模型：点「导入模型包…」挑一个 .zip，或者把模型包直接"
        "拖进主窗口，程序会自己解压装好，不用填路径。"));
    note_->setObjectName("dialogNote");
    note_->setWordWrap(true);
    root->addWidget(note_);

    paste_ = new QCheckBox(QString::fromUtf8("转写后粘贴到前台窗口"));
    paste_->setChecked(voice.value("paste").toBool(true));
    autoEnter_ = new QCheckBox(QString::fromUtf8("粘贴后自动回车"));
    autoEnter_->setChecked(voice.value("auto_enter").toBool(false));
    root->addWidget(paste_);
    root->addWidget(autoEnter_);

    const QJsonObject media = store_->config().value("media_voice").toObject();
    headset_ = new QCheckBox(QString::fromUtf8("耳机播放键 = 语音开关"));
    headset_->setChecked(media.value("enabled").toBool(true));
    headset_->setToolTip(QString::fromUtf8(
        "按一次开始、再按一次结束。本地转写时静音 %1 秒自动收尾；"
        "转发热键模式 %2 秒自动释放组合键，免得 Ctrl+Shift 一直按住。")
        .arg(defaults.mediaSilenceMs / 1000)
        .arg(defaults.mediaAutoStopMs / 1000));
    root->addWidget(headset_);

    QStringList explicitScope;
    for (const QJsonValue &value : media.value("swallow_in").toArray()) {
        explicitScope << value.toString().toLower();
    }
    QStringList scope;
    for (const Profile &profile : store_->profiles()) {
        const bool hit = explicitScope.isEmpty()
                             ? !profile.voice.isEmpty()
                             : explicitScope.contains(profile.name.toLower());
        if (hit) {
            scope << profile.name;
        }
    }
    mediaHint_ = new QLabel(scope.isEmpty()
        ? QString::fromUtf8("当前没有场景配语音，耳机播放键保持系统媒体键；"
                            "在映射页给场景配上语音热键后会自动接管。")
        : QString::fromUtf8("只在 %1 场景接管，其余场景仍是系统媒体键。")
              .arg(scope.join(QStringLiteral(" / "))));
    mediaHint_->setObjectName("muted");
    mediaHint_->setWordWrap(true);
    root->addWidget(mediaHint_);

    // 桌宠语音反馈：详细配置（Base URL / API Key / 模型 / 音色）在共享的
    // TtsSettingsDialog 里编辑，语音主界面和这里开的是同一个。
    const QJsonObject tts = voice.value("tts").toObject();
    auto *ttsRow = new QHBoxLayout;
    ttsEnabled_ = new QCheckBox(QString::fromUtf8("桌宠语音反馈（TTS · 限时免费）"));
    ttsEnabled_->setChecked(tts.value("enabled").toBool(true));
    auto *ttsMore = new QPushButton(QString::fromUtf8("设置…"));
    ttsMore->setObjectName("ghostSmall");
    ttsMore->setCursor(Qt::PointingHandCursor);
    connect(ttsMore, &QPushButton::clicked, this, [this] {
        auto *dialog = new TtsSettingsDialog(store_, this);
        connect(dialog, &TtsSettingsDialog::saved, this, [this] {
            // 那边可能改了开关，拉回来别让本对话框保存时盖掉。
            ttsEnabled_->setChecked(store_->voiceConfig()
                                        .value("tts")
                                        .toObject()
                                        .value("enabled")
                                        .toBool(true));
            emit optionsApplied();
        });
        dialog->exec();
        dialog->deleteLater();
    });
    ttsRow->addWidget(ttsEnabled_);
    ttsRow->addStretch(1);
    ttsRow->addWidget(ttsMore);
    root->addLayout(ttsRow);

    auto *foot = new QHBoxLayout;
    auto *cancel = new QPushButton(QString::fromUtf8("取消"));
    cancel->setObjectName("ghostSmall");
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto *save = new QPushButton(QString::fromUtf8("保存"));
    save->setObjectName("primary");
    save->setCursor(Qt::PointingHandCursor);
    save->setFixedWidth(86);
    connect(save, &QPushButton::clicked, this, &VoiceSettingsDialog::save);
    foot->addStretch(1);
    foot->addWidget(cancel);
    foot->addWidget(save);
    root->addLayout(foot);

    connect(backend_, &QComboBox::currentIndexChanged, this,
            [this] { refreshStatus(); });
    for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
        button->setFocusPolicy(Qt::NoFocus);
    }
    refreshStatus();
}

void VoiceSettingsDialog::reloadThemes() {
    const QSignalBlocker block(theme_);
    theme_->clear();
    const ThemeManager &themes = ThemeManager::instance();
    for (const ThemeInfo &info : themes.themes()) {
        // 不放 preview 缩略图：整图缩到 22px 只剩一坨深色，毫无辨识度。
        theme_->addItem(info.displayName, info.id);
        const int index = theme_->count() - 1;
        if (!info.valid) {
            theme_->setItemText(
                index, QStringLiteral("%1（清单损坏）").arg(info.displayName));
            theme_->setItemData(index,
                                QStringLiteral("解析失败：%1").arg(info.error),
                                Qt::ToolTipRole);
            theme_->setItemData(index, false, Qt::UserRole - 1);  // 置灰
        }
    }
    const int index = theme_->findData(themes.currentId());
    theme_->setCurrentIndex(index >= 0 ? index : 0);
}

void VoiceSettingsDialog::refreshStatus() {
    auto state = [](QLabel *label, const QString &dir, bool sense) {
        const bool ready = sense ? senseVoiceReady(dir, nullptr, nullptr)
                                 : qwen3Ready(dir, nullptr, nullptr, nullptr, nullptr);
        label->setText(ready ? QString::fromUtf8("✓ 已安装")
                             : QString::fromUtf8("未安装"));
        label->setStyleSheet(ready ? QStringLiteral("color:#3fb950;")
                                   : QStringLiteral("color:#d29922;"));
    };
    state(senseState_, senseDefault_, true);
    state(qwenState_, qwenDefault_, false);

    const VoiceOptions current = resolveVoiceOptions(
        store_->config(), QJsonObject{},
        QCoreApplication::applicationDirPath());
    const bool cloudReady =
        !current.asrApiKey.isEmpty() && !current.asrApiModel.isEmpty();
    asrState_->setText(cloudReady ? QStringLiteral("✓ %1").arg(current.asrApiModel)
                                  : QString::fromUtf8("未配置"));
    asrState_->setStyleSheet(cloudReady ? QStringLiteral("color:#3fb950;")
                                        : QStringLiteral("color:#d29922;"));
    const bool llmReady =
        !current.llmApiKey.isEmpty() && !current.llmApiModel.isEmpty();
    llmState_->setText(llmReady ? QStringLiteral("✓ %1").arg(current.llmApiModel)
                                : QString::fromUtf8("未配置"));
    llmState_->setStyleSheet(llmReady ? QStringLiteral("color:#3fb950;")
                                      : QStringLiteral("color:#d29922;"));

    const QString backend = backend_->currentData().toString();
    if (backend == "sensevoice") {
        note_->setText(QString::fromUtf8(
            "SenseVoiceSmall int8：中文/粤语/英日韩，自带标点，CPU 即可。"
            "没装的话点上面的「导入模型包…」选 .tar.bz2，程序自己解压装好。"));
    } else if (backend == "qwen3") {
        note_->setText(QString::fromUtf8(
            "Qwen3-ASR 0.6B int8：52 种语言/22 种方言，中英混合和中文识别更准，"
            "CPU 转写会比 SenseVoice 慢。模型包（约 880MB）在网盘/官网下载，"
            "用「导入模型包…」装好，保存后生效。"));
    } else {
        note_->setText(QString::fromUtf8(
            "默认路线：不装模型，把语音键转成当前前台场景的热键"
            "（Qoder Ctrl+Shift+V / WorkBuddy Ctrl+D）。其他应用在 profiles.json "
            "配 voice.hotkey，或改用本地转写。"));
    }
}

void VoiceSettingsDialog::importModel(bool senseVoice) {
    // 用系统文件对话框：Qt 自绘的那种会继承深色主题的浅灰字，落在白底上看不清。
    const QString file = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("选择模型包"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        QString::fromUtf8("模型包 (*.zip *.tar.bz2 *.tbz2 *.tar.gz *.tgz)"));
    if (file.isEmpty()) {
        return;
    }
    importFromPath(file, senseVoice ? QStringLiteral("sensevoice")
                                    : QStringLiteral("qwen3"));
}

void VoiceSettingsDialog::importFromPath(const QString &file,
                                         const QString &expectedBackend) {
    const ModelImportResult result = importModelPackage(this, file);
    refreshStatus();
    note_->setStyleSheet(QString());
    if (!result.ok) {
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        note_->setText(result.error);
        return;
    }
    const QString name = result.backend == QStringLiteral("qwen3")
                             ? QStringLiteral("Qwen3-ASR")
                             : QStringLiteral("SenseVoice");
    if (!expectedBackend.isEmpty() && expectedBackend != result.backend) {
        note_->setStyleSheet(QStringLiteral("color:#d29922;"));
        note_->setText(QString::fromUtf8(
                           "装的是 %1 模型，可在上面「识别方式」里切过去。")
                           .arg(name));
        return;
    }
    note_->setStyleSheet(QStringLiteral("color:#3fb950;"));
    note_->setText(
        QString::fromUtf8("%1 模型已装好，保存后生效。").arg(name));
}

void VoiceSettingsDialog::dragEnterEvent(QDragEnterEvent *event) {
    if (modelPackageFromMime(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void VoiceSettingsDialog::dropEvent(QDropEvent *event) {
    const QString file = modelPackageFromMime(event->mimeData());
    if (file.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    importFromPath(file, QString());
}

void VoiceSettingsDialog::save() {
    const QString backend = backend_->currentData().toString();
    if (backend != QStringLiteral("hotkey")) {
        const bool ready =
            backend == QStringLiteral("qwen3")
                ? qwen3Ready(qwenDefault_, nullptr, nullptr, nullptr, nullptr)
                : senseVoiceReady(senseDefault_, nullptr, nullptr);
        if (!ready) {
            note_->setStyleSheet(QStringLiteral("color:#f85149;"));
            note_->setText(QString::fromUtf8(
                "这个模型还没导入，语音会不可用：先点上面的「导入模型包…」，"
                "或把识别方式改回转发。"));
            return;
        }
    }
    QJsonObject patch;
    patch["backend"] = backend;
    patch["asr_language"] = language_->currentData().toString();
    const QString kbKey = kbHotkey_->currentData().toString();
    if (kbKey == QStringLiteral("off")) {
        patch["keyboard_hotkey"] = QJsonObject{{"enabled", false}};
    } else {
        const QStringList parts = kbKey.split(QLatin1Char('+'));
        QJsonArray mods;
        for (int i = 0; i < parts.size() - 1; ++i) {
            mods.append(parts.at(i));
        }
        patch["keyboard_hotkey"] =
            QJsonObject{{"enabled", true},
                        {"mods", mods},
                        {"vk", parts.last().toInt()}};
    }
    patch["paste"] = paste_->isChecked();
    patch["auto_enter"] = autoEnter_->isChecked();
    if (!store_->updateVoice(patch)) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    QJsonObject mediaPatch;
    mediaPatch["enabled"] = headset_->isChecked();
    if (!store_->updateMediaVoice(mediaPatch)) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    // tts 对象整体读改写：本对话框只管开关，详细字段归 TtsSettingsDialog。
    QJsonObject tts = store_->voiceConfig().value("tts").toObject();
    tts["enabled"] = ttsEnabled_->isChecked();
    if (!store_->updateVoice(QJsonObject{{"tts", tts}})) {
        note_->setText(QString::fromUtf8("保存失败：无法写入 config.json"));
        note_->setStyleSheet(QStringLiteral("color:#f85149;"));
        return;
    }
    emit saved();
    accept();
}
