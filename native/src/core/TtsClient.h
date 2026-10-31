#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// 通用 OpenAI 兼容 TTS 客户端：文本 → base64 wav，winmm PlaySound 内存播放。
// 默认对准小米 MiMo（https://api.xiaomimimo.com/v1 + mimo-v2.5-tts），
// base_url / model / key 全部可配，任何 OpenAI 兼容的 TTS 都能接。
// key 由用户自填（config.json 的 voice.tts.api_key 或环境变量
// MIMO_API_KEY），不在代码里落任何 key。
class TtsClient final : public QObject {
    Q_OBJECT

public:
    explicit TtsClient(QObject *parent = nullptr);

    void setOptions(bool enabled, const QString &baseUrl, const QString &apiKey,
                    const QString &model, const QString &voice,
                    const QString &style);
    // 有 key 且开关打开才算就绪；没就绪时 speak() 静默跳过（气泡照常显示）。
    bool ready() const { return enabled_ && !apiKey_.isEmpty(); }
    void speak(const QString &text);
    // 停止当前播放（SND_PURGE），下次 speak 会覆盖缓冲。
    void stop();

signals:
    void audioReady(const QByteArray &wav);
    void failed(const QString &error);

private:
    void play(const QByteArray &wav);

    QNetworkAccessManager *http_ = nullptr;
    bool enabled_ = true;
    bool busy_ = false;
    QString baseUrl_ = QStringLiteral("https://api.xiaomimimo.com/v1");
    QString apiKey_;
    QString model_ = QStringLiteral("mimo-v2.5-tts");
    QString voice_ = QStringLiteral("Chloe");
    QString style_;
    QByteArray playing_;  // SND_ASYNC 播放期间内存必须活着
};
