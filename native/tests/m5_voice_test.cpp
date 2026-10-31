#include "core/VoiceSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::fputs("FAIL: ", stderr);
        std::fputs(message, stderr);
        std::fputc(10, stderr);
    }
}

quint32 le32(const QByteArray &data, int offset) {
    return quint32(quint8(data[offset])) |
           (quint32(quint8(data[offset + 1])) << 8) |
           (quint32(quint8(data[offset + 2])) << 16) |
           (quint32(quint8(data[offset + 3])) << 24);
}

quint16 le16(const QByteArray &data, int offset) {
    return quint16(quint8(data[offset])) |
           quint16(quint16(quint8(data[offset + 1])) << 8);
}

void touch(const QString &path) {
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write("x");
}

QJsonObject baseConfig() {
    return QJsonObject{
        {"voice_key", QJsonObject{{"vkey", 0x74},
                                  {"confirm_window_ms", 250},
                                  {"min_repeats", 2},
                                  {"silence_stop_ms", 700},
                                  {"min_record_ms", 300}}},
        {"media_voice", QJsonObject{{"enabled", true},
                                    {"vkey", 0xB3},
                                    {"swallow_in", QJsonArray{"qoder", "agent"}}}},
        {"voice", QJsonObject{{"backend", "hotkey"},
                              {"paste", true},
                              {"auto_enter", false},
                              {"sample_rate", 16000},
                              {"asr_language", "zh"},
                              {"hotkey", QJsonObject{
                                             {"vk", 0x56},
                                             {"mods", QJsonArray{"ctrl", "shift"}},
                                             {"trigger", "hold"}}}}},
    };
}

}  // namespace

int main() {
    QTemporaryDir temp;
    check(temp.isValid(), "temporary dir");
    const QString appDir = temp.path();

    {
        const VoiceOptions options =
            resolveVoiceOptions(baseConfig(), QJsonObject{}, appDir);
        check(options.backend == VoiceBackend::Hotkey, "default backend hotkey");
        check(options.hotkeyVk == 0x56, "default hotkey vk");
        check(options.hotkeyMods == QStringList({"ctrl", "shift"}),
              "default hotkey mods");
        check(options.hotkeyTrigger == "hold", "default trigger hold");
        check(options.voiceVk == 0x74, "default voice key");
        check(options.mediaVk == 0xB3, "default media key");
        check(options.mediaScopeExplicit, "explicit swallow_in list");
        check(options.mediaProfiles == QStringList({"qoder", "agent"}),
              "media scope from config");
        check(options.mediaSilenceMs == 6000, "media silence default");
        check(options.mediaAutoStopMs == 30000, "media auto stop default");
        check(options.minRepeats == 2, "default min repeats");
        check(options.senseVoiceDir.endsWith("models/sensevoice"),
              "default sensevoice dir");
        check(voiceComboText(options.hotkeyMods, options.hotkeyVk) ==
                  "CTRL+SHIFT+V",
              "combo text");
        // 无遥控器的全局键盘热键：默认开、Ctrl+Alt+V，可关可换。
        check(options.kbHotkeyEnabled, "kb hotkey default on");
        check(options.kbHotkeyMods == QStringList({"ctrl", "alt"}),
              "kb hotkey default mods");
        check(options.kbHotkeyVk == 0x56, "kb hotkey default vk");
    }

    {
        QJsonObject config = baseConfig();
        QJsonObject voice = config.value("voice").toObject();
        voice["keyboard_hotkey"] = QJsonObject{{"enabled", false}};
        config["voice"] = voice;
        const VoiceOptions options =
            resolveVoiceOptions(config, QJsonObject{}, appDir);
        check(!options.kbHotkeyEnabled, "kb hotkey disabled");
    }

    {
        QJsonObject config = baseConfig();
        QJsonObject voice = config.value("voice").toObject();
        voice["keyboard_hotkey"] = QJsonObject{
            {"enabled", true}, {"mods", QJsonArray{"alt"}}, {"vk", 66}};
        config["voice"] = voice;
        const VoiceOptions options =
            resolveVoiceOptions(config, QJsonObject{}, appDir);
        check(options.kbHotkeyMods == QStringList({"alt"}), "kb hotkey alt");
        check(options.kbHotkeyVk == 66, "kb hotkey vk b");
    }

    {
        // 不写 swallow_in 时不再退回固定名单，改由场景是否配了语音决定。
        QJsonObject config = baseConfig();
        QJsonObject media = config.value("media_voice").toObject();
        media.remove("swallow_in");
        config["media_voice"] = media;
        const VoiceOptions options =
            resolveVoiceOptions(config, QJsonObject{}, appDir);
        check(!options.mediaScopeExplicit, "scope derived when unset");
        check(options.mediaProfiles.isEmpty(), "no hardcoded scene list");
    }

    {
        const QJsonObject profileVoice{
            {"hotkey", QJsonObject{{"vk", 68},
                                   {"mods", QJsonArray{"ctrl"}},
                                   {"trigger", "toggle"}}}};
        const VoiceOptions options =
            resolveVoiceOptions(baseConfig(), profileVoice, appDir);
        check(options.hotkeyVk == 68, "profile hotkey vk");
        check(options.hotkeyMods == QStringList({"ctrl"}),
              "profile hotkey mods");
        check(options.hotkeyTrigger == "toggle", "profile trigger toggle");
    }

    {
        QJsonObject config = baseConfig();
        QJsonObject voice = config.value("voice").toObject();
        voice["backend"] = "sensevoice";
        voice["sensevoice_dir"] = "models/custom";
        voice["paste"] = false;
        config["voice"] = voice;
        const VoiceOptions options =
            resolveVoiceOptions(config, QJsonObject{}, appDir);
        check(options.backend == VoiceBackend::SenseVoice, "sensevoice backend");
        check(options.senseVoiceDir == QDir::cleanPath(appDir + "/models/custom"),
              "configured model dir");
        check(!options.paste, "paste false");
    }

    {
        QJsonObject config = baseConfig();
        QJsonObject voice = config.value("voice").toObject();
        voice.remove("backend");
        voice["mode"] = "local";
        config["voice"] = voice;
        const VoiceOptions options =
            resolveVoiceOptions(config, QJsonObject{}, appDir);
        check(options.backend == VoiceBackend::SenseVoice, "mode local fallback");
    }

    {
        check(voiceBackendFromString("qwen3-asr") == VoiceBackend::Qwen3Asr,
              "qwen3 alias");
        check(voiceBackendFromString("LOCAL") == VoiceBackend::SenseVoice,
              "local alias");
        check(voiceBackendFromString("") == VoiceBackend::Hotkey,
              "unknown backend");
        check(voiceBackendKey(VoiceBackend::Qwen3Asr) == "qwen3",
              "qwen3 key");
        check(voiceProfileLabel("agent") == QStringLiteral("WorkBuddy"),
              "agent label");
        check(voiceProfileLabel("default") == QStringLiteral("通用"),
              "default label");
    }

    {
        const QByteArray pcm(3200, '\0');
        const QByteArray wav = wavFromPcm16(pcm, 16000, 1);
        check(wav.size() == 44 + pcm.size(), "wav size");
        check(wav.left(4) == "RIFF", "wav riff");
        check(wav.mid(8, 4) == "WAVE", "wav wave");
        check(wav.mid(12, 4) == "fmt ", "wav fmt");
        check(le16(wav, 20) == 1, "wav pcm");
        check(le16(wav, 22) == 1, "wav channels");
        check(le32(wav, 24) == 16000, "wav rate");
        check(le32(wav, 28) == 32000, "wav byte rate");
        check(le16(wav, 34) == 16, "wav bits");
        check(wav.mid(36, 4) == "data", "wav data");
        check(le32(wav, 40) == quint32(pcm.size()), "wav data size");
        check(le32(wav, 4) == quint32(pcm.size() + 36), "wav riff size");
    }

    {
        check(cleanAsrText("<|zh|><|NEUTRAL|>你好，世界<|end|>") ==
                  QString::fromUtf8("你好，世界"),
              "clean sensevoice tags");
        check(cleanAsrText("  hello  ") == "hello", "clean trim");
    }

    {
        const QString dir = appDir + "/sv";
        QDir().mkpath(dir);
        touch(dir + "/model.int8.onnx");
        touch(dir + "/tokens.txt");
        QString model;
        QString tokens;
        check(senseVoiceReady(dir, &model, &tokens), "sensevoice ready");
        check(model.endsWith("model.int8.onnx"), "sensevoice model path");
        check(tokens.endsWith("tokens.txt"), "sensevoice tokens path");
        QFile::remove(dir + "/tokens.txt");
        check(!senseVoiceReady(dir, nullptr, nullptr), "sensevoice missing tokens");
    }

    {
        const QString dir = appDir + "/qwen3";
        QDir().mkpath(dir + "/tokenizer");
        touch(dir + "/conv_frontend.onnx");
        touch(dir + "/encoder.int8.onnx");
        touch(dir + "/decoder.int8.onnx");
        QString conv;
        QString encoder;
        QString decoder;
        QString tokenizer;
        check(qwen3Ready(dir, &conv, &encoder, &decoder, &tokenizer),
              "qwen3 ready");
        check(conv.endsWith("conv_frontend.onnx"), "qwen3 conv path");
        check(encoder.endsWith("encoder.int8.onnx"), "qwen3 encoder path");
        check(decoder.endsWith("decoder.int8.onnx"), "qwen3 decoder path");
        check(tokenizer.endsWith("tokenizer"), "qwen3 tokenizer path");
    }

    if (failures == 0) {
        std::puts("PASS native voice: options / wav / model discovery");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
