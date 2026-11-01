#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

enum class VoiceBackend {
    Hotkey = 0,
    SenseVoice = 1,
    Qwen3Asr = 2,
    CloudAsr = 3,
};

struct VoiceOptions {
    VoiceBackend backend = VoiceBackend::Hotkey;
    QString senseVoiceDir;
    QString qwen3Dir;
    QStringList hotkeyMods;
    int hotkeyVk = 0x56;  // V
    QString hotkeyTrigger = QStringLiteral("hold");
    bool kbHotkeyEnabled = true;   // 无遥控器的全局键盘热键（一键说话/收尾）
    QStringList kbHotkeyMods = {QStringLiteral("ctrl"), QStringLiteral("alt")};
    int kbHotkeyVk = 0x56;  // V → Ctrl+Alt+V
    bool paste = true;
    bool autoEnter = false;
    int sampleRate = 16000;
    int minRecordMs = 300;
    int confirmWindowMs = 250;
    int minRepeats = 2;
    int silenceStopMs = 700;
    int maxRecordMs = 60000;
    int voiceVk = 0x74;  // F5
    int mediaVk = 0xB3;  // VK_MEDIA_PLAY_PAUSE
    bool mediaEnabled = true;
    QStringList mediaProfiles;  // 显式覆盖；留空时按场景是否配了语音判定
    bool mediaScopeExplicit = false;
    int mediaSilenceMs = 6000;   // 耳机起的本地会话：静音这么久就收尾
    int mediaAutoStopMs = 30000;  // 耳机起的热键会话：没有录音信号，到时释放组合键
    int threads = 4;
    QString language = QStringLiteral("zh");
    // 桌宠语音反馈（OpenAI 兼容 TTS，默认小米 MiMo）。key 由用户自填：
    // config.json 的 voice.tts.api_key，或环境变量 MIMO_API_KEY（不进仓库）。
    // base_url/model 开放可配，任何 OpenAI 兼容的 TTS 都能接。
    bool ttsEnabled = true;
    QString ttsApiKey;
    QString ttsBaseUrl = QStringLiteral("https://api.xiaomimimo.com/v1");
    QString ttsModel = QStringLiteral("mimo-v2.5-tts");
    QString ttsVoice = QStringLiteral("Chloe");
    QString ttsStyle;  // 音色/语气指令；空 = 用 TtsClient 里的内置默认
    QString ttsOutputDevice;  // 输出设备名；空 = 跟随系统默认（切耳机在这里选）
    QString ttsChatVoice;  // 对话回复朗读的音色；空 = 跟随 ttsVoice
    // 云端 ASR（base_url / api_key 留空时沿用 TTS 的配置——同一平台填一次
    // 就能两用；model 必填）。style: "chat"=MiMo 式音频进 chat/completions，
    // "transcriptions"=OpenAI 式 multipart /audio/transcriptions。
    QString asrApiBaseUrl;  // 空 = 用 ttsBaseUrl
    QString asrApiKey;      // 空 = 用 ttsApiKey（再退到 MIMO_API_KEY）
    QString asrApiModel;
    QString asrApiStyle = QStringLiteral("chat");
    // 桌宠对话闭环（LLM，OpenAI 兼容 chat/completions）。同样留空沿用 TTS。
    QString llmApiBaseUrl;  // 空 = 用 ttsBaseUrl
    QString llmApiKey;      // 空 = 用 ttsApiKey
    QString llmApiModel = QStringLiteral("mimo-v2.6-flash");
    QString llmApiSystem;   // 人设/风格；空 = 用 VoicePet 的内置默认
};

// voice.tts.voice 的「跟随主题」哨兵与历史形态：空 / "@theme" / 旧版误存的
// 「跟随主题 · X」界面文案都算跟随主题；其它值一律视为用户自选的音色名。
inline bool isFollowThemeVoice(const QString &voice) {
    const QString trimmed = voice.trimmed();
    return trimmed.isEmpty() || trimmed == QStringLiteral("@theme") ||
           trimmed.contains(QStringLiteral("跟随主题"));
}

VoiceBackend voiceBackendFromString(const QString &value);
QString voiceBackendKey(VoiceBackend backend);
QString voiceBackendLabel(VoiceBackend backend);
QString voiceProfileLabel(const QString &profileName);
QString voiceComboText(const QStringList &mods, int vk);

VoiceOptions resolveVoiceOptions(const QJsonObject &config,
                                 const QJsonObject &profileVoice,
                                 const QString &appDir);

QString resolveModelDir(const QString &configured, const QString &appDir,
                        const QString &fallbackName,
                        const QStringList &nameHints);
bool senseVoiceReady(const QString &dir, QString *modelPath = nullptr,
                     QString *tokensPath = nullptr);
bool qwen3Ready(const QString &dir, QString *convFrontend = nullptr,
                QString *encoder = nullptr, QString *decoder = nullptr,
                QString *tokenizer = nullptr);

QByteArray wavFromPcm16(const QByteArray &pcm, int sampleRate,
                        int channels = 1);
QString cleanAsrText(const QString &text);
