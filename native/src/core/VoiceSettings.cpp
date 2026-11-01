#include "VoiceSettings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

namespace {

int intValue(const QJsonObject &object, const QString &key, int fallback) {
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

bool boolValue(const QJsonObject &object, const QString &key, bool fallback) {
    const QJsonValue value = object.value(key);
    return value.isBool() ? value.toBool() : fallback;
}

QString stringValue(const QJsonObject &object, const QString &key,
                    const QString &fallback = QString()) {
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : fallback;
}

QStringList stringList(const QJsonValue &value) {
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) {
            out.append(text.toLower());
        }
    }
    return out;
}

void mergeHotkey(QJsonObject *target, const QJsonObject &source) {
    for (auto it = source.begin(); it != source.end(); ++it) {
        (*target)[it.key()] = it.value();
    }
}

QString firstFile(const QDir &dir, const QStringList &names) {
    for (const QString &name : names) {
        const QString path = dir.filePath(name);
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return {};
}

QString firstByPrefix(const QDir &dir, const QString &prefix,
                      const QStringList &suffixes) {
    const QStringList files = dir.entryList(QDir::Files);
    for (const QString &file : files) {
        if (!file.startsWith(prefix, Qt::CaseInsensitive)) {
            continue;
        }
        for (const QString &suffix : suffixes) {
            if (file.endsWith(suffix, Qt::CaseInsensitive)) {
                return dir.filePath(file);
            }
        }
    }
    return {};
}

QString absoluteDir(const QString &path, const QString &appDir) {
    if (path.isEmpty()) {
        return {};
    }
    QFileInfo info(path);
    if (info.isAbsolute()) {
        return QDir::cleanPath(path);
    }
    return QDir::cleanPath(appDir + "/" + path);
}

}  // namespace

VoiceBackend voiceBackendFromString(const QString &value) {
    const QString key = value.trimmed().toLower();
    if (key == "sensevoice" || key == "sense_voice" || key == "local") {
        return VoiceBackend::SenseVoice;
    }
    if (key == "qwen3" || key == "qwen3-asr" || key == "qwen3asr") {
        return VoiceBackend::Qwen3Asr;
    }
    if (key == "api" || key == "cloud" || key == "cloudasr") {
        return VoiceBackend::CloudAsr;
    }
    return VoiceBackend::Hotkey;
}

QString voiceBackendKey(VoiceBackend backend) {
    switch (backend) {
    case VoiceBackend::SenseVoice:
        return QStringLiteral("sensevoice");
    case VoiceBackend::Qwen3Asr:
        return QStringLiteral("qwen3");
    case VoiceBackend::CloudAsr:
        return QStringLiteral("api");
    case VoiceBackend::Hotkey:
    default:
        return QStringLiteral("hotkey");
    }
}

QString voiceBackendLabel(VoiceBackend backend) {
    switch (backend) {
    case VoiceBackend::SenseVoice:
        return QStringLiteral("本地 SenseVoice");
    case VoiceBackend::Qwen3Asr:
        return QStringLiteral("本地 Qwen3-ASR");
    case VoiceBackend::CloudAsr:
        return QStringLiteral("云端 ASR（联网）");
    case VoiceBackend::Hotkey:
    default:
        return QStringLiteral("转发热键（0 模型）");
    }
}

QString voiceProfileLabel(const QString &profileName) {
    const QString name = profileName.toLower();
    if (name == QStringLiteral("qoder")) {
        return QStringLiteral("Qoder");
    }
    if (name == QStringLiteral("agent")) {
        return QStringLiteral("WorkBuddy");
    }
    if (name.isEmpty() || name == QStringLiteral("default")) {
        return QStringLiteral("通用");
    }
    return profileName;
}

QString voiceComboText(const QStringList &mods, int vk) {
    QStringList parts;
    for (const QString &mod : mods) {
        parts.append(mod.toUpper());
    }
    if (vk >= 0x30 && vk <= 0x5A) {
        parts.append(QChar(vk));
    } else if (vk >= 0x70 && vk <= 0x7B) {
        parts.append(QStringLiteral("F%1").arg(vk - 0x6F));
    } else {
        parts.append(QStringLiteral("VK 0x") +
                     QString::number(vk, 16).toUpper().rightJustified(2, '0'));
    }
    return parts.join('+');
}

VoiceOptions resolveVoiceOptions(const QJsonObject &config,
                                 const QJsonObject &profileVoice,
                                 const QString &appDir) {
    VoiceOptions options;
    const QJsonObject voice = config.value("voice").toObject();
    const QJsonObject voiceKey = config.value("voice_key").toObject();
    const QJsonObject media = config.value("media_voice").toObject();

    const QString backend = stringValue(voice, "backend");
    if (!backend.isEmpty()) {
        options.backend = voiceBackendFromString(backend);
    } else if (stringValue(voice, "mode") == QStringLiteral("local")) {
        options.backend = VoiceBackend::SenseVoice;
    }

    options.senseVoiceDir = resolveModelDir(
        stringValue(voice, "sensevoice_dir"), appDir, QStringLiteral("sensevoice"),
        {QStringLiteral("sensevoice"), QStringLiteral("sherpa-onnx-sense-voice")});
    // 兜底目录名要和 ModelImport 的 installPath(backend) 一致（models/qwen3），
    // 不然「导入模型包」装好了这里却找不到。
    options.qwen3Dir = resolveModelDir(
        stringValue(voice, "qwen3_dir"), appDir, QStringLiteral("qwen3"),
        {QStringLiteral("qwen3"), QStringLiteral("qwen3-asr"),
         QStringLiteral("sherpa-onnx-qwen3-asr")});

    QJsonObject hotkey = voice.value("hotkey").toObject();
    const QJsonObject profileHotkey = profileVoice.value("hotkey").toObject();
    mergeHotkey(&hotkey, profileHotkey);

    options.hotkeyMods = stringList(hotkey.value("mods"));
    if (options.hotkeyMods.isEmpty()) {
        options.hotkeyMods = {QStringLiteral("ctrl"), QStringLiteral("shift")};
    }
    options.hotkeyVk = intValue(hotkey, "vk", 0x56);

    // 无遥控器的全局键盘热键；热键模式注入前台用的组合是另一回事，
    // 默认给 Ctrl+Alt+V 避开它。
    const QJsonObject kbHotkey = voice.value("keyboard_hotkey").toObject();
    options.kbHotkeyEnabled = boolValue(kbHotkey, "enabled", true);
    options.kbHotkeyMods = stringList(kbHotkey.value("mods"));
    if (options.kbHotkeyMods.isEmpty()) {
        options.kbHotkeyMods = {QStringLiteral("ctrl"), QStringLiteral("alt")};
    }
    options.kbHotkeyVk = intValue(kbHotkey, "vk", 0x56);

    QString trigger = stringValue(profileVoice, "trigger");
    if (trigger.isEmpty()) {
        trigger = stringValue(hotkey, "trigger", QStringLiteral("hold"));
    }
    trigger = trigger.toLower();
    options.hotkeyTrigger =
        trigger == QStringLiteral("toggle") ? QStringLiteral("toggle")
                                            : QStringLiteral("hold");

    options.paste = boolValue(voice, "paste", true);
    options.autoEnter = boolValue(voice, "auto_enter", false);
    options.sampleRate = qBound(8000, intValue(voice, "sample_rate", 16000), 48000);
    options.language = stringValue(voice, "asr_language", QStringLiteral("auto"));
    options.threads = qBound(1, intValue(voice, "threads", 4), 8);

    options.minRecordMs =
        qBound(100, intValue(voiceKey, "min_record_ms", 300), 10000);
    options.confirmWindowMs =
        qBound(50, intValue(voiceKey, "confirm_window_ms", 250), 2000);
    options.minRepeats =
        qBound(1, intValue(voiceKey, "min_repeats", 2), 10);
    options.silenceStopMs =
        qBound(200, intValue(voiceKey, "silence_stop_ms", 700), 10000);
    options.voiceVk = intValue(voiceKey, "vkey", 0x74);

    options.mediaEnabled = boolValue(media, "enabled", true);
    options.mediaVk = intValue(media, "vkey", 0xB3);
    options.mediaProfiles = stringList(media.value("swallow_in"));
    options.mediaScopeExplicit = !options.mediaProfiles.isEmpty();
    options.mediaSilenceMs = intValue(media, "silence_ms", 6000);
    options.mediaAutoStopMs = intValue(media, "auto_stop_ms", 30000);

    const QJsonObject tts = voice.value("tts").toObject();
    options.ttsEnabled = boolValue(tts, "enabled", true);
    options.ttsApiKey = stringValue(tts, "api_key").trimmed();
    if (options.ttsApiKey.isEmpty()) {
        // 自用部署不想把 key 落盘时走环境变量。
        options.ttsApiKey =
            qEnvironmentVariable("MIMO_API_KEY").trimmed();
    }
    options.ttsBaseUrl = stringValue(
        tts, "base_url", QStringLiteral("https://api.xiaomimimo.com/v1"));
    if (options.ttsBaseUrl.endsWith(QLatin1Char('/'))) {
        options.ttsBaseUrl.chop(1);
    }
    options.ttsModel =
        stringValue(tts, "model", QStringLiteral("mimo-v2.5-tts"));
    options.ttsVoice = stringValue(tts, "voice", QStringLiteral("Chloe"));
    options.ttsStyle = stringValue(tts, "style");
    options.ttsOutputDevice = stringValue(tts, "output_device");
    options.ttsChatVoice = stringValue(tts, "chat_voice").trimmed();

    // 云端 ASR：base_url / key 留空就沿用 TTS 的，一个平台填一次两处能用。
    const QJsonObject asrApi = voice.value("asr_api").toObject();
    options.asrApiBaseUrl = stringValue(asrApi, "base_url").trimmed();
    if (options.asrApiBaseUrl.isEmpty()) {
        options.asrApiBaseUrl = options.ttsBaseUrl;
    }
    while (options.asrApiBaseUrl.endsWith(QLatin1Char('/'))) {
        options.asrApiBaseUrl.chop(1);
    }
    options.asrApiKey = stringValue(asrApi, "api_key").trimmed();
    if (options.asrApiKey.isEmpty()) {
        options.asrApiKey = options.ttsApiKey;
    }
    options.asrApiModel = stringValue(asrApi, "model").trimmed();
    options.asrApiStyle = stringValue(asrApi, "style", QStringLiteral("chat"));

    // 桌宠对话闭环：同样的兜底链。
    const QJsonObject llmApi = voice.value("llm_api").toObject();
    options.llmApiBaseUrl = stringValue(llmApi, "base_url").trimmed();
    if (options.llmApiBaseUrl.isEmpty()) {
        options.llmApiBaseUrl = options.ttsBaseUrl;
    }
    while (options.llmApiBaseUrl.endsWith(QLatin1Char('/'))) {
        options.llmApiBaseUrl.chop(1);
    }
    options.llmApiKey = stringValue(llmApi, "api_key").trimmed();
    if (options.llmApiKey.isEmpty()) {
        options.llmApiKey = options.ttsApiKey;
    }
    options.llmApiModel =
        stringValue(llmApi, "model", QStringLiteral("mimo-v2.6-flash"));
    options.llmApiSystem = stringValue(llmApi, "system");
    return options;
}

QString resolveModelDir(const QString &configured, const QString &appDir,
                        const QString &fallbackName,
                        const QStringList &nameHints) {
    if (!configured.trimmed().isEmpty()) {
        return absoluteDir(configured.trimmed(), appDir);
    }

    QDir app(appDir);
    const QString direct = app.filePath(QStringLiteral("models/") + fallbackName);
    if (QFileInfo(direct).isDir()) {
        return QDir::cleanPath(direct);
    }

    // 开发时 exe 在 native/build/Release，模型放在 native/models。
    QDir repo(app.filePath(QStringLiteral("../..")));
    const QString repoDirect =
        repo.filePath(QStringLiteral("models/") + fallbackName);
    if (QFileInfo(repoDirect).isDir()) {
        return QDir::cleanPath(repoDirect);
    }

    QDir repoModels(repo.filePath(QStringLiteral("models")));
    if (repoModels.exists()) {
        const QStringList dirs = repoModels.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &name : dirs) {
            const QString lower = name.toLower();
            for (const QString &hint : nameHints) {
                if (lower.contains(hint.toLower())) {
                    return QDir::cleanPath(repoModels.filePath(name));
                }
            }
        }
    }
    return QDir::cleanPath(direct);
}

bool senseVoiceReady(const QString &dir, QString *modelPath,
                     QString *tokensPath) {
    const QDir root(dir);
    const QString model =
        firstFile(root, {QStringLiteral("model.int8.onnx"),
                         QStringLiteral("model.onnx")});
    const QString tokens = firstFile(root, {QStringLiteral("tokens.txt")});
    if (modelPath != nullptr) *modelPath = model;
    if (tokensPath != nullptr) *tokensPath = tokens;
    return !model.isEmpty() && !tokens.isEmpty();
}

bool qwen3Ready(const QString &dir, QString *convFrontend, QString *encoder,
                QString *decoder, QString *tokenizer) {
    const QDir root(dir);
    const QString conv =
        firstByPrefix(root, QStringLiteral("conv_frontend"),
                      {QStringLiteral(".onnx")});
    const QString enc =
        firstByPrefix(root, QStringLiteral("encoder"),
                      {QStringLiteral(".onnx")});
    const QString dec =
        firstByPrefix(root, QStringLiteral("decoder"),
                      {QStringLiteral(".onnx")});

    QString tok;
    const QString tokenizerDir = root.filePath(QStringLiteral("tokenizer"));
    if (QFileInfo(tokenizerDir).isDir()) {
        tok = tokenizerDir;
    } else {
        tok = firstFile(root, {QStringLiteral("tokens.txt")});
    }

    if (convFrontend != nullptr) *convFrontend = conv;
    if (encoder != nullptr) *encoder = enc;
    if (decoder != nullptr) *decoder = dec;
    if (tokenizer != nullptr) *tokenizer = tok;
    return !conv.isEmpty() && !enc.isEmpty() && !dec.isEmpty() && !tok.isEmpty();
}

QByteArray wavFromPcm16(const QByteArray &pcm, int sampleRate, int channels) {
    const int safeChannels = channels > 0 ? channels : 1;
    const int safeRate = sampleRate > 0 ? sampleRate : 16000;
    const int byteRate = safeRate * safeChannels * 2;
    const quint32 dataSize = quint32(pcm.size());

    QByteArray wav;
    wav.reserve(44 + pcm.size());
    wav.append("RIFF", 4);
    auto appendLe32 = [&wav](quint32 value) {
        wav.append(char(value & 0xFF));
        wav.append(char((value >> 8) & 0xFF));
        wav.append(char((value >> 16) & 0xFF));
        wav.append(char((value >> 24) & 0xFF));
    };
    auto appendLe16 = [&wav](quint16 value) {
        wav.append(char(value & 0xFF));
        wav.append(char((value >> 8) & 0xFF));
    };
    appendLe32(36 + dataSize);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    appendLe32(16);
    appendLe16(1);  // PCM
    appendLe16(quint16(safeChannels));
    appendLe32(quint32(safeRate));
    appendLe32(quint32(byteRate));
    appendLe16(quint16(safeChannels * 2));
    appendLe16(16);
    wav.append("data", 4);
    appendLe32(dataSize);
    wav.append(pcm);
    return wav;
}

QString cleanAsrText(const QString &text) {
    static const QRegularExpression tag(QStringLiteral("<\\|[^|]*\\|>"));
    QString out = text;
    out.remove(tag);
    out.replace(QStringLiteral("\r"), QString());
    return out.trimmed();
}
