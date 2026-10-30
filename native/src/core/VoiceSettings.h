#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

enum class VoiceBackend {
    Hotkey = 0,
    SenseVoice = 1,
    Qwen3Asr = 2,
};

struct VoiceOptions {
    VoiceBackend backend = VoiceBackend::Hotkey;
    QString senseVoiceDir;
    QString qwen3Dir;
    QStringList hotkeyMods;
    int hotkeyVk = 0x56;  // V
    QString hotkeyTrigger = QStringLiteral("hold");
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
    QStringList mediaProfiles;
    int threads = 4;
    QString language = QStringLiteral("zh");
};

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
