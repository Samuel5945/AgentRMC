#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QJsonArray;
class QNetworkAccessManager;

// 通用 OpenAI 兼容对话客户端（/chat/completions）：桌宠对话闭环的「脑子」。
// base_url / key / model 可配，留空由调用方兜底成 TTS 的平台配置。
class LlmClient final : public QObject {
    Q_OBJECT

public:
    explicit LlmClient(QObject *parent = nullptr);

    void setOptions(const QString &baseUrl, const QString &apiKey,
                    const QString &model, const QString &system);
    bool ready() const { return !apiKey_.isEmpty() && !model_.isEmpty(); }
    // messages 由调用方拼好（含 system 与最近历史）；一次一个请求在途。
    void chat(const QJsonArray &messages);

signals:
    void replied(const QString &text, qint64 costMs);
    void failed(const QString &error, qint64 costMs);

private:
    QNetworkAccessManager *http_ = nullptr;
    QString baseUrl_ = QStringLiteral("https://api.xiaomimimo.com/v1");
    QString apiKey_;
    QString model_;
    bool busy_ = false;
};
