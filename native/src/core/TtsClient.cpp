#include "core/TtsClient.h"

#include "core/VoiceSettings.h"

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

// 不设超时的话，走本机代理的连接可能永远停在 CloseWait：reply 不 finished，
// synthesizing_ 就一直是 true，之后每句只排队不合成，桌宠彻底哑掉。
constexpr int kTransferTimeoutMs = 30000;

// MiMo 流式 pcm16 固定 24kHz 单声道（实测：字节数/48000 与同文 wav 时长一致）。
constexpr int kStreamSampleRate = 24000;

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
    // 流式 pcm16：首块就能起播。服务端不认 stream 时回整段 JSON，走兼容路径。
    body["stream"] = true;
    body["audio"] = QJsonObject{
        {"format", "pcm16"},
        {"voice", item.second.isEmpty() ? voice_ : item.second}};

    QNetworkRequest request{QUrl(baseUrl_ + QStringLiteral("/chat/completions"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey_.toUtf8());
    request.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = http_->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    inFlight_ = reply;
    auto ctx = std::make_shared<StreamCtx>();
    ctx->generation = generation_;
    connect(reply, &QNetworkReply::readyRead, this,
            [this, reply, ctx] { consumeStream(reply, ctx); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctx] {
        reply->deleteLater();
        if (inFlight_ == reply) {
            inFlight_ = nullptr;
        }
        synthesizing_ = false;
        if (ctx->generation != generation_) {
            // 中途 stop() 过：这句直接扔。但开关重新打开后要能接着念。
            if (enabled_) {
                pump();
            }
            return;
        }
        consumeStream(reply, ctx);  // finished 前可能还有最后一截没读
        if (reply->error() != QNetworkReply::NoError) {
            if (ctx->toOut) {
                audio_.finishStream();  // 已收到的部分照播，别哑在半截
            }
            // 4xx 把服务器的具体原因带上（无效音色名/额度不足一目了然）。
            const QString serverMessage =
                QJsonDocument::fromJson(ctx->sseMode ? ctx->sse : ctx->raw)
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
        QByteArray wav;
        if (ctx->sseMode) {
            if (ctx->pcm.isEmpty()) {
                emit failed(QStringLiteral("TTS 返回内容不是音频"));
                pump();
                return;
            }
            wav = wavFromPcm16(ctx->pcm, kStreamSampleRate);
            if (ctx->toOut) {
                audio_.finishStream();
            }
        } else {
            const QJsonObject message =
                QJsonDocument::fromJson(ctx->raw)
                    .object()
                    .value("choices")
                    .toArray()
                    .at(0)
                    .toObject()
                    .value("message")
                    .toObject();
            wav = QByteArray::fromBase64(
                message.value("audio").toObject().value("data").toString().toUtf8());
            if (wav.size() < 44) {  // 连 WAV 头都不到，肯定不是音频
                emit failed(QStringLiteral("TTS 返回内容不是音频"));
                pump();
                return;
            }
        }
        if (ctx->toOut) {
            emit audioReady(wav);  // 首块时已经在播
        } else if (audio_.playing()) {
            stashed_ = wav;  // 上一段播完由 onIdleTick 接上
        } else {
            play(wav);
            emit audioReady(wav);
        }
        idleTimer_->start();
        pump();  // 排队还有就继续合成，跟播放并行
    });
}

void TtsClient::consumeStream(QNetworkReply *reply,
                              const std::shared_ptr<StreamCtx> &ctx) {
    if (ctx->generation != generation_) {
        return;
    }
    const QByteArray incoming = reply->readAll();
    if (incoming.isEmpty()) {
        return;
    }
    if (!ctx->ctypeChecked) {
        ctx->ctypeChecked = true;
        ctx->sseMode = reply->header(QNetworkRequest::ContentTypeHeader)
                           .toString()
                           .contains(QStringLiteral("event-stream"),
                                     Qt::CaseInsensitive);
    }
    if (!ctx->sseMode) {
        ctx->raw.append(incoming);
        return;
    }
    ctx->sse.append(incoming);
    int nl = 0;
    while ((nl = ctx->sse.indexOf('\n')) >= 0) {
        const QByteArray line = ctx->sse.left(nl).trimmed();
        ctx->sse.remove(0, nl + 1);
        if (!line.startsWith("data:")) {
            continue;
        }
        const QByteArray payload = line.mid(5).trimmed();
        if (payload.isEmpty() || payload == "[DONE]") {
            continue;
        }
        const QJsonArray choices = QJsonDocument::fromJson(payload)
                                       .object()
                                       .value("choices")
                                       .toArray();
        if (choices.isEmpty()) {
            continue;  // 收尾的 usage 块没有 choices
        }
        const QJsonObject audio = choices.at(0)
                                      .toObject()
                                      .value("delta")
                                      .toObject()
                                      .value("audio")
                                      .toObject();
        const QByteArray chunk = QByteArray::fromBase64(
            audio.value("data").toString().toUtf8());
        if (chunk.isEmpty()) {
            continue;  // delta.audio 为 null 的收尾块
        }
        const bool first = ctx->pcm.isEmpty();
        ctx->pcm.append(chunk);
        if (first && !audio_.playing()) {
            // 起播失败不在此报错：攒完整段走 play() 再报，避免双重失败提示。
            ctx->toOut = audio_.beginStream(kStreamSampleRate, 1, outputDevice_);
        }
        if (ctx->toOut) {
            audio_.feed(chunk);
        }
    }
}

void TtsClient::onIdleTick() {
    if (audio_.playing()) {
        return;
    }
    if (!stashed_.isEmpty()) {
        const QByteArray wav = stashed_;
        stashed_.clear();
        play(wav);  // 播完下一个 tick 再轮到排队的
        emit audioReady(wav);
        return;
    }
    idleTimer_->stop();
    pump();
}

void TtsClient::stop() {
    ++generation_;
    pending_.clear();
    stashed_.clear();
    idleTimer_->stop();
    // 在途请求必须掐掉：光清队列的话 synthesizing_ 还等着一个可能永远不回来的
    // reply，开关关掉再打开也救不回来。abort 会走 finished 回调复位状态。
    QNetworkReply *reply = inFlight_;
    inFlight_ = nullptr;
    if (reply != nullptr) {
        reply->abort();
    }
    audio_.stop();
}

void TtsClient::play(const QByteArray &wav) {
    // 统一走 waveOut（空设备名 = WAVE_MAPPER 系统默认）：失败有错误码可报；
    // PlaySound 的失败是静默的，出了问题无从排查。
    QString error;
    if (!audio_.play(wav, outputDevice_, &error)) {
        emit failed(error.isEmpty() ? QStringLiteral("音频播放失败") : error);
    }
}
