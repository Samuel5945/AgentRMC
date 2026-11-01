#include "core/TtsClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
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
    idleTimer_ = new QTimer(this);
    idleTimer_->setInterval(100);
    connect(idleTimer_, &QTimer::timeout, this, [this] { onIdleTick(); });
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

void TtsClient::speak(const QString &text,
                      const QString &voiceOverride) {
    if (text.trimmed().isEmpty() || !ready()) {
        // 没就绪静默跳过（气泡由上层照常出）。
        return;
    }
    // 排队播报：上一句没念完不打断也不丢，念完接着念；队列满才丢最新的。
    if (pending_.size() >= kMaxPending) {
        return;
    }
    pending_.append({text, voiceOverride});
    pump();
}

void TtsClient::pump() {
    if (synthesizing_ || pending_.isEmpty()) {
        return;
    }
    const QPair<QString, QString> item = pending_.takeFirst();
    synthesizing_ = true;

    QJsonObject body;
    body["model"] = model_;
    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"},
                                {"content", style_.isEmpty()
                                                ? QString::fromUtf8(kDefaultStyle)
                                                : style_}});
    messages.append(QJsonObject{{"role", "assistant"},
                                {"content", item.first}});
    body["messages"] = messages;
    body["audio"] = QJsonObject{
        {"format", "wav"},
        {"voice", item.second.isEmpty() ? voice_ : item.second}};

    QNetworkRequest request{QUrl(baseUrl_ + QStringLiteral("/chat/completions"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey_.toUtf8());
    QNetworkReply *reply = http_->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation = generation_] {
                reply->deleteLater();
                synthesizing_ = false;
                if (generation != generation_) {
                    return;  // 中途 stop() 过：这句直接扔
                }
                const QByteArray bodyBytes = reply->readAll();
                if (reply->error() != QNetworkReply::NoError) {
                    // 4xx 把服务器的具体原因带上（无效音色名/额度不足一目了然）。
                    const QString serverMessage =
                        QJsonDocument::fromJson(bodyBytes)
                            .object()
                            .value("error")
                            .toObject()
                            .value("message")
                            .toString();
                    emit failed(serverMessage.isEmpty()
                                    ? reply->errorString()
                                    : QStringLiteral("%1（%2）").arg(
                                          reply->errorString(), serverMessage));
                    pump();
                    return;
                }
                const QJsonObject message = QJsonDocument::fromJson(bodyBytes)
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
                    pump();
                    return;
                }
                if (audio_.playing()) {
                    stashed_ = wav;  // 上一段播完由 onIdleTick 接上
                } else {
                    play(wav);
                }
                idleTimer_->start();
                pump();  // 排队还有就继续合成，跟播放并行
            });
}

void TtsClient::onIdleTick() {
    if (audio_.playing()) {
        return;
    }
    if (!stashed_.isEmpty()) {
        const QByteArray wav = stashed_;
        stashed_.clear();
        play(wav);  // 播完下一个 tick 再轮到排队的
        return;
    }
    idleTimer_->stop();
    pump();
}

void TtsClient::stop() {
    ++generation_;
    pending_.clear();
    stashed_.clear();
    audio_.stop();
    playing_.clear();
    idleTimer_->stop();
}

void TtsClient::play(const QByteArray &wav) {
    // 统一走 waveOut（空设备名 = WAVE_MAPPER 系统默认）：失败有错误码可报；
    // PlaySound 的失败是静默的，出了问题无从排查。
    QString error;
    if (!audio_.play(wav, outputDevice_, &error)) {
        emit failed(error.isEmpty() ? QStringLiteral("音频播放失败") : error);
        return;
    }
    // 缓冲留在成员里保证异步播放期间内存有效。
    playing_ = wav;
    emit audioReady(wav);
}
