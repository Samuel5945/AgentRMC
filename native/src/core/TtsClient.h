#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include "core/AudioOut.h"

class QJsonArray;
class QNetworkAccessManager;

// 通用 OpenAI 兼容 TTS 客户端：文本 → base64 wav，winmm 播放。
// 默认对准小米 MiMo（https://api.xiaomimimo.com/v1 + mimo-v2.5-tts），
// base_url / model / key 全部可配，任何 OpenAI 兼容的 TTS 都能接。
// 输出设备可指定（AudioOut 按设备名走 waveOut）；空 = 系统默认（PlaySound）。
// key 由用户自填（config.json 的 voice.tts.api_key 或环境变量
// MIMO_API_KEY），不在代码里落任何 key。
class TtsClient final : public QObject {
    Q_OBJECT

public:
    explicit TtsClient(QObject *parent = nullptr);

    void setOptions(bool enabled, const QString &baseUrl, const QString &apiKey,
                    const QString &model, const QString &voice,
                    const QString &style,
                    const QString &outputDevice = QString());
    // 有 key 且开关打开才算就绪；没就绪时 speak() 静默跳过（气泡照常显示）。
    bool ready() const { return enabled_ && !apiKey_.isEmpty(); }
    // voiceOverride 非空时本句用它（对话回复可单独选音色），空 = 全局音色。
    void speak(const QString &text, const QString &voiceOverride = QString());
    // 停止当前播放（SND_PURGE / waveOut reset），下次 speak 会覆盖缓冲。
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
    QString outputDevice_;
    AudioOut audio_;
    QByteArray playing_;  // SND_ASYNC 播放期间内存必须活着
};
