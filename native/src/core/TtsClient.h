#pragma once

#include <QByteArray>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

#include "core/AudioOut.h"

class QJsonArray;
class QNetworkAccessManager;
class QTimer;

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
    // 清空队列并停止当前播放（waveOut reset）；桌宠收起/静音时用。
    void stop();

signals:
    void audioReady(const QByteArray &wav);
    void failed(const QString &error);

private:
    // 有空闲就取下一句去合成；合成完上一段还在播就先暂存。
    void pump();
    void play(const QByteArray &wav);
    void onIdleTick();

    QNetworkAccessManager *http_ = nullptr;
    bool enabled_ = true;
    bool synthesizing_ = false;  // 一次只合成一句
    static constexpr int kMaxPending = 3;  // 队列上限，连发提醒别刷屏
    QVector<QPair<QString, QString>> pending_;  // {text, voiceOverride} 排队
    QByteArray stashed_;  // 合成完但上一段还没播完，先存这儿
    int generation_ = 0;  // stop() 自增：在路上的合成回来直接扔
    QString baseUrl_ = QStringLiteral("https://api.xiaomimimo.com/v1");
    QString apiKey_;
    QString model_ = QStringLiteral("mimo-v2.5-tts");
    QString voice_ = QStringLiteral("Chloe");
    QString style_;
    QString outputDevice_;
    AudioOut audio_;
    QByteArray playing_;  // 异步播放期间内存必须活着
    QTimer *idleTimer_ = nullptr;  // 100ms 轮询上一段播没播完，衔接下一句
};
