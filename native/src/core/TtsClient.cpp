#include "core/TtsClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

namespace {

// user 消息是音色/语气指令，assistant 消息才是要念的文本（官方协议如此）。
constexpr const char *kDefaultStyle =
    "亲切自然的中文语音助手，语气轻快友好，吐字清晰，语速适中，简短利落";

}  // namespace

TtsClient::TtsClient(QObject *parent) : QObject(parent) {
    http_ = new QNetworkAccessManager(this);
}

void TtsClient::setOptions(bool enabled, const QString &baseUrl,
                           const QString &apiKey, const QString &model,
                           const QString &voice, const QString &style,
                           const QString &outputDevice) {
    enabled_ = enabled;
    baseUrl_ = baseUrl.trimmed();
    while (baseUrl_.endsWith(QLatin1Char('/'))) {
        baseUrl_.chop(1);
    }
    apiKey_ = apiKey.trimmed();
    if (!model.trimmed().isEmpty()) {
        model_ = model.trimmed();
    }
    if (!voice.trimmed().isEmpty()) {
        voice_ = voice.trimmed();
    }
    style_ = style.trimmed();
    outputDevice_ = outputDevice.trimmed();
}

void TtsClient::speak(const QString &text) {
    if (text.trimmed().isEmpty()) {
        return;
    }
    if (!ready() || busy_) {
        // 一次只合成一句：上一条还在路上时丢弃新的（气泡仍然照常出）。
        return;
    }
    busy_ = true;

    QJsonObject body;
    body["model"] = model_;
    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"},
                                {"content", style_.isEmpty()
                                                ? QString::fromUtf8(kDefaultStyle)
                                                : style_}});
    messages.append(QJsonObject{{"role", "assistant"}, {"content", text}});
    body["messages"] = messages;
    body["audio"] = QJsonObject{{"format", "wav"}, {"voice", voice_}};

    QNetworkRequest request{QUrl(baseUrl_ + QStringLiteral("/chat/completions"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey_.toUtf8());
    QNetworkReply *reply = http_->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        busy_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(reply->errorString());
            return;
        }
        const QJsonObject message = QJsonDocument::fromJson(reply->readAll())
                                        .object()
                                        .value("choices")
                                        .toArray()
                                        .at(0)
                                        .toObject()
                                        .value("message")
                                        .toObject();
        const QByteArray wav = QByteArray::fromBase64(
            message.value("audio").toObject().value("data").toString().toUtf8());
        if (wav.size() < 44) {  // 连 WAV 头都不到，肯定不是音频
            emit failed(QStringLiteral("TTS 返回内容不是音频"));
            return;
        }
        play(wav);
        emit audioReady(wav);
    });
}

void TtsClient::play(const QByteArray &wav) {
    // 指定了输出设备就走 waveOut 直连（切耳机用）；起不来再退回系统默认。
    if (!outputDevice_.isEmpty() &&
        audio_.play(wav, outputDevice_)) {
        return;
    }
    // 换新音频会自然顶掉上一段；缓冲留在成员里保证异步播放期间内存有效。
    playing_ = wav;
    PlaySoundW(reinterpret_cast<LPCWSTR>(playing_.constData()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void TtsClient::stop() {
    PlaySoundW(nullptr, nullptr, SND_PURGE);
    audio_.stop();
    playing_.clear();
}
