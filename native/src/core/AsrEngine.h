#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

struct AsrConfig {
    QString family = QStringLiteral("sensevoice");
    QString modelDir;
    QString language = QStringLiteral("zh");
    int threads = 4;
};

class AsrEngine {
public:
    virtual ~AsrEngine() = default;
    virtual QString displayName() const = 0;
    virtual bool available(QString *reason = nullptr) = 0;
    virtual QString transcribe(const QByteArray &pcm16, int sampleRate,
                               QString *error = nullptr) = 0;
};

std::unique_ptr<AsrEngine> createAsrEngine(const AsrConfig &config);
