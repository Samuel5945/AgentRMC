#include "core/AsrClient.h"

#include <QDateTime>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
constexpr int kTransferTimeoutMs = 30000;
}

AsrClient::AsrClient(QObject *parent) : QObject(parent) {
    http_ = new QNetworkAccessManager(this);
}

void AsrClient::setOptions(const QString &baseUrl, const QString &apiKey,
                           const QString &model, const QString &language,
                           const QString &style) {
    baseUrl_ = baseUrl.trimmed();
    while (baseUrl_.endsWith(QLatin1Char('/'))) {
        baseUrl_.chop(1);
    }
    apiKey_ = apiKey.trimmed();
    model_ = model.trimmed();
    language_ = language.trimmed().toLower();
    style_ = style.trimmed().toLower();
    if (style_ != QStringLiteral("transcriptions")) {
        style_ = QStringLiteral("chat");
    }
}

QString AsrClient::sizeHint(const QByteArray &wav) {
    return QString::number(wav.size() / 1024);
}

void AsrClient::transcribe(const QByteArray &wav) {
    if (wav.isEmpty() || !ready() || busy_) {
        return;
    }
    if (style_ == QStringLiteral("transcriptions")) {
        transcribeViaTranscriptions(wav);
    } else {
        transcribeViaChat(wav);
    }
}

// MiMo 式：音频塞进 user 消息的 input_audio，文本从 message.content 出。
void AsrClient::transcribeViaChat(const QByteArray &wav) {
    busy_ = true;
    QJsonObject body;
    body["model"] = model_;
    QJsonObject audio{{"data", QString::fromLatin1(wav.toBase64())},
                      {"format", QStringLiteral("wav")}};
    QJsonArray content{QJsonObject{{"type", QStringLiteral("input_audio")},
                                   {"input_audio", audio}}};
    body["messages"] = QJsonArray{QJsonObject{{"role", QStringLiteral("user")},
                                              {"content", content}}};
    QNetworkRequest request{QUrl(baseUrl_ + QStringLiteral("/chat/completions"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey_.toUtf8());
    request.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = http_->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    connect(reply, &QNetworkReply::finished, this, [this, reply, started] {
        reply->deleteLater();
        busy_ = false;
        const qint64 costMs = QDateTime::currentMSecsSinceEpoch() - started;
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString serverMessage = QJsonDocument::fromJson(body)
                                              .object()
                                              .value("error")
                                              .toObject()
                                              .value("message")
                                              .toString();
            emit failed(serverMessage.isEmpty() ? reply->errorString()
                                                : serverMessage,
                        costMs);
            return;
        }
        const QString text = QJsonDocument::fromJson(body)
                                 .object()
                                 .value("choices")
                                 .toArray()
                                 .at(0)
                                 .toObject()
                                 .value("message")
                                 .toObject()
                                 .value("content")
                                 .toString()
                                 .trimmed();
        if (text.isEmpty()) {
            emit failed(QStringLiteral("云端返回了空转写"), costMs);
            return;
        }
        emit transcribed(text, costMs);
    });
}

// OpenAI 式：multipart 上传文件，回 {"text": "..."}。
void AsrClient::transcribeViaTranscriptions(const QByteArray &wav) {
    busy_ = true;
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral(
            "form-data; name=\"file\"; filename=\"recording.wav\"")));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                       QVariant(QStringLiteral("audio/wav")));
    filePart.setBody(wav);
    multi->append(filePart);
    QHttpPart modelPart;
    modelPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QVariant(QStringLiteral("form-data; name=\"model\"")));
    modelPart.setBody(model_.toUtf8());
    multi->append(modelPart);
    // OpenAI 系列接口接受 ISO-639-1 语言码；auto/粤语就别传了让它自己猜。
    if (language_ == "zh" || language_ == "en" || language_ == "ja" ||
        language_ == "ko") {
        QHttpPart languagePart;
        languagePart.setHeader(
            QNetworkRequest::ContentDispositionHeader,
            QVariant(QStringLiteral("form-data; name=\"language\"")));
        languagePart.setBody(language_.toUtf8());
        multi->append(languagePart);
    }

    QNetworkRequest request{QUrl(baseUrl_ + QStringLiteral("/audio/transcriptions"))};
    request.setTransferTimeout(kTransferTimeoutMs);
    request.setRawHeader("Authorization", "Bearer " + apiKey_.toUtf8());
    QNetworkReply *reply = http_->post(request, multi);
    multi->setParent(reply);

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    connect(reply, &QNetworkReply::finished, this, [this, reply, started] {
        reply->deleteLater();
        busy_ = false;
        const qint64 costMs = QDateTime::currentMSecsSinceEpoch() - started;
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString serverMessage = QJsonDocument::fromJson(body)
                                              .object()
                                              .value("error")
                                              .toObject()
                                              .value("message")
                                              .toString();
            emit failed(serverMessage.isEmpty() ? reply->errorString()
                                                : serverMessage,
                        costMs);
            return;
        }
        const QString text = QJsonDocument::fromJson(body)
                                 .object()
                                 .value("text")
                                 .toString()
                                 .trimmed();
        if (text.isEmpty()) {
            emit failed(QStringLiteral("云端返回了空转写"), costMs);
            return;
        }
        emit transcribed(text, costMs);
    });
}
