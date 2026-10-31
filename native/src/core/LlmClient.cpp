#include "core/LlmClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

LlmClient::LlmClient(QObject *parent) : QObject(parent) {
    http_ = new QNetworkAccessManager(this);
}

void LlmClient::setOptions(const QString &baseUrl, const QString &apiKey,
                           const QString &model, const QString &) {
    baseUrl_ = baseUrl.trimmed();
    while (baseUrl_.endsWith(QLatin1Char('/'))) {
        baseUrl_.chop(1);
    }
    apiKey_ = apiKey.trimmed();
    model_ = model.trimmed();
}

void LlmClient::chat(const QJsonArray &messages) {
    if (messages.isEmpty() || !ready() || busy_) {
        return;
    }
    busy_ = true;

    QJsonObject body;
    body["model"] = model_;
    body["messages"] = messages;
    QNetworkRequest request{QUrl(baseUrl_ + QStringLiteral("/chat/completions"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey_.toUtf8());
    request.setTransferTimeout(45000);
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
        const QJsonObject message = QJsonDocument::fromJson(body)
                                        .object()
                                        .value("choices")
                                        .toArray()
                                        .at(0)
                                        .toObject()
                                        .value("message")
                                        .toObject();
        // 推理型模型可能把正文放在 reasoning_content，兜一手。
        QString text = message.value("content").toString().trimmed();
        if (text.isEmpty()) {
            text = message.value("reasoning_content").toString().trimmed();
        }
        if (text.isEmpty()) {
            emit failed(QStringLiteral("模型返回了空回复"), costMs);
            return;
        }
        emit replied(text, costMs);
    });
}
