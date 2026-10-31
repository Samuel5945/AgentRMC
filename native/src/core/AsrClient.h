#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// 云端 ASR，双协议：
// - "chat"（默认）：OpenAI 兼容 /chat/completions，音频作为 input_audio 进
//   user 消息，choices[0].message.content 即转写文本。小米 MiMo（mimo-v2.5-asr）
//   就是这种，和 TTS/对话共用一个平台。
// - "transcriptions"：multipart 上传 /audio/transcriptions，回 {"text"}。
//   OpenAI whisper / 硅基流动 / Groq 是这种。
// base_url / key 留空由调用方兜底成 TTS 的配置；一次只允许一个请求在途。
class AsrClient final : public QObject {
    Q_OBJECT

public:
    explicit AsrClient(QObject *parent = nullptr);

    void setOptions(const QString &baseUrl, const QString &apiKey,
                    const QString &model, const QString &language,
                    const QString &style);
    bool ready() const { return !apiKey_.isEmpty() && !model_.isEmpty(); }
    void transcribe(const QByteArray &wav);
    // 上传大小提示（KB），给事件流展示用。
    static QString sizeHint(const QByteArray &wav);

signals:
    void transcribed(const QString &text, qint64 costMs);
    void failed(const QString &error, qint64 costMs);

private:
    void transcribeViaChat(const QByteArray &wav);
    void transcribeViaTranscriptions(const QByteArray &wav);

    QNetworkAccessManager *http_ = nullptr;
    QString baseUrl_ = QStringLiteral("https://api.xiaomimimo.com/v1");
    QString apiKey_;
    QString model_;
    QString language_;
    QString style_ = QStringLiteral("chat");
    bool busy_ = false;
};
