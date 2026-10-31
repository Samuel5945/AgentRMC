#include "AsrEngine.h"

#include "VoiceSettings.h"

#include <QFileInfo>

#include <cstring>
#include <mutex>
#include <vector>

#ifdef AGENTRMC_WITH_SHERPA
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace {

class UnavailableEngine final : public AsrEngine {
public:
    explicit UnavailableEngine(QString reason) : reason_(std::move(reason)) {}

    QString displayName() const override { return QStringLiteral("本地转写"); }

    bool available(QString *reason) override {
        if (reason != nullptr) *reason = reason_;
        return false;
    }

    QString transcribe(const QByteArray &, int, QString *error) override {
        if (error != nullptr) *error = reason_;
        return {};
    }

private:
    QString reason_;
};

#ifdef AGENTRMC_WITH_SHERPA

class SherpaEngine final : public AsrEngine {
public:
    explicit SherpaEngine(AsrConfig config) : config_(std::move(config)) {}

    ~SherpaEngine() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recognizer_ != nullptr) {
            SherpaOnnxDestroyOfflineRecognizer(recognizer_);
            recognizer_ = nullptr;
        }
    }

    QString displayName() const override {
        return config_.family == QStringLiteral("qwen3")
                   ? QStringLiteral("Qwen3-ASR")
                   : QStringLiteral("SenseVoice");
    }

    bool available(QString *reason) override {
        QString detail;
        if (config_.family == QStringLiteral("qwen3")) {
            if (!qwen3Ready(config_.modelDir, nullptr, nullptr, nullptr, nullptr)) {
                detail = QStringLiteral("未找到 Qwen3-ASR 模型");
            }
        } else if (!senseVoiceReady(config_.modelDir, nullptr, nullptr)) {
            detail = QStringLiteral("未找到 SenseVoice 模型");
        }
        if (reason != nullptr) *reason = detail;
        return detail.isEmpty();
    }

    QString transcribe(const QByteArray &pcm16, int sampleRate,
                       QString *error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureLoaded(error)) {
            return {};
        }
        if (pcm16.isEmpty() || sampleRate <= 0) {
            if (error != nullptr) *error = QStringLiteral("录音为空");
            return {};
        }

        std::vector<float> samples(size_t(pcm16.size() / 2));
        const auto *raw = reinterpret_cast<const qint16 *>(pcm16.constData());
        for (size_t i = 0; i < samples.size(); ++i) {
            samples[i] = float(raw[i]) / 32768.0f;
        }

        const SherpaOnnxOfflineStream *stream =
            SherpaOnnxCreateOfflineStream(recognizer_);
        if (stream == nullptr) {
            if (error != nullptr) *error = QStringLiteral("创建识别流失败");
            return {};
        }
        SherpaOnnxAcceptWaveformOffline(stream, sampleRate, samples.data(),
                                        int32_t(samples.size()));
        SherpaOnnxDecodeOfflineStream(recognizer_, stream);
        const SherpaOnnxOfflineRecognizerResult *result =
            SherpaOnnxGetOfflineStreamResult(stream);
        QString text;
        if (result != nullptr && result->text != nullptr) {
            text = cleanAsrText(QString::fromUtf8(result->text));
        }
        if (result != nullptr) {
            SherpaOnnxDestroyOfflineRecognizerResult(result);
        }
        SherpaOnnxDestroyOfflineStream(stream);
        return text;
    }

private:
    bool ensureLoaded(QString *error) {
        if (recognizer_ != nullptr) {
            return true;
        }
        QString detail;
        if (!available(&detail)) {
            if (error != nullptr) *error = detail;
            return false;
        }

        SherpaOnnxOfflineRecognizerConfig config;
        std::memset(&config, 0, sizeof(config));
        config.feat_config.sample_rate = 16000;
        // Qwen3-ASR 的 Mel 特征是 128 维，SenseVoice 是 80 维，混用会转出乱码。
        config.feat_config.feature_dim =
            config_.family == QStringLiteral("qwen3") ? 128 : 80;
        config.model_config.num_threads = qBound(1, config_.threads, 8);
        config.model_config.provider = "cpu";
        config.model_config.debug = 0;
        config.decoding_method = "greedy_search";

        QByteArray model;
        QByteArray tokens;
        QByteArray conv;
        QByteArray encoder;
        QByteArray decoder;
        QByteArray tokenizer;

        if (config_.family == QStringLiteral("qwen3")) {
            QString convPath;
            QString encoderPath;
            QString decoderPath;
            QString tokenizerPath;
            if (!qwen3Ready(config_.modelDir, &convPath, &encoderPath,
                            &decoderPath, &tokenizerPath)) {
                if (error != nullptr) *error = QStringLiteral("Qwen3-ASR 模型文件不完整");
                return false;
            }
            conv = convPath.toUtf8();
            encoder = encoderPath.toUtf8();
            decoder = decoderPath.toUtf8();
            tokenizer = tokenizerPath.toUtf8();
            config.model_config.qwen3_asr.conv_frontend = conv.constData();
            config.model_config.qwen3_asr.encoder = encoder.constData();
            config.model_config.qwen3_asr.decoder = decoder.constData();
            config.model_config.qwen3_asr.tokenizer = tokenizer.constData();
            config.model_config.qwen3_asr.max_total_len = 512;
            config.model_config.qwen3_asr.max_new_tokens = 128;
            config.model_config.qwen3_asr.temperature = 1e-6f;
            config.model_config.qwen3_asr.top_p = 0.8f;
            config.model_config.qwen3_asr.seed = 42;
        } else {
            QString modelPath;
            QString tokensPath;
            if (!senseVoiceReady(config_.modelDir, &modelPath, &tokensPath)) {
                if (error != nullptr) *error = QStringLiteral("SenseVoice 模型文件不完整");
                return false;
            }
            model = modelPath.toUtf8();
            tokens = tokensPath.toUtf8();
            config.model_config.sense_voice.model = model.constData();
            // auto = 中英混合：由模型按句判断语言，说英文应用名不会被硬翻成汉字。
            QString lang = config_.language.trimmed().toLower();
            if (lang.isEmpty() || lang == QStringLiteral("auto")) {
                lang = QStringLiteral("auto");
            }
            config.model_config.sense_voice.language = lang.toUtf8().constData();
            config.model_config.sense_voice.use_itn = 1;
            config.model_config.tokens = tokens.constData();
        }

        recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
        if (recognizer_ == nullptr) {
            if (error != nullptr) *error = QStringLiteral("模型加载失败（sherpa-onnx 返回空）");
            return false;
        }
        return true;
    }

    AsrConfig config_;
    std::mutex mutex_;
    const SherpaOnnxOfflineRecognizer *recognizer_ = nullptr;
};

#endif

}  // namespace

std::unique_ptr<AsrEngine> createAsrEngine(const AsrConfig &config) {
#ifdef AGENTRMC_WITH_SHERPA
    return std::make_unique<SherpaEngine>(config);
#else
    Q_UNUSED(config);
    return std::make_unique<UnavailableEngine>(
        QStringLiteral("当前构建未包含 sherpa-onnx 运行库"));
#endif
}
