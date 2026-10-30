#include "core/AsrEngine.h"

#include <QElapsedTimer>
#include <QFile>

#include <cstdio>

namespace {

quint32 le32(const QByteArray &data, int offset) {
    return quint32(quint8(data[offset])) |
           (quint32(quint8(data[offset + 1])) << 8) |
           (quint32(quint8(data[offset + 2])) << 16) |
           (quint32(quint8(data[offset + 3])) << 24);
}

quint16 le16(const QByteArray &data, int offset) {
    return quint16(quint8(data[offset])) |
           quint16(quint16(quint8(data[offset + 1])) << 8);
}

bool readWav(const QString &path, QByteArray *pcm, int *sampleRate,
             QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法打开 ") + path;
        return false;
    }
    const QByteArray data = file.readAll();
    if (data.size() < 44 || data.left(4) != "RIFF" || data.mid(8, 4) != "WAVE") {
        *error = QStringLiteral("不是 WAV 文件");
        return false;
    }
    int channels = 0;
    int bits = 0;
    int rate = 0;
    int format = 0;
    int offset = 12;
    QByteArray payload;
    while (offset + 8 <= data.size()) {
        const QByteArray id = data.mid(offset, 4);
        const quint32 size = le32(data, offset + 4);
        const int body = offset + 8;
        if (id == "fmt " && body + 16 <= data.size()) {
            format = le16(data, body);
            channels = le16(data, body + 2);
            rate = int(le32(data, body + 4));
            bits = le16(data, body + 14);
        } else if (id == "data") {
            const int available = qMin<int>(int(size), data.size() - body);
            payload = data.mid(body, available);
        }
        offset = body + int(size) + (int(size) & 1);
    }
    if (format != 1 || bits != 16 || channels < 1 || rate <= 0) {
        *error = QStringLiteral("只支持 16-bit PCM WAV");
        return false;
    }
    if (rate != 16000) {
        *error = QStringLiteral("测试音频需要 16kHz");
        return false;
    }
    if (channels == 1) {
        *pcm = payload;
    } else {
        const int frames = payload.size() / (channels * 2);
        QByteArray mono(frames * 2, '\0');
        const auto *src = reinterpret_cast<const qint16 *>(payload.constData());
        auto *dst = reinterpret_cast<qint16 *>(mono.data());
        for (int i = 0; i < frames; ++i) {
            int sum = 0;
            for (int ch = 0; ch < channels; ++ch) {
                sum += src[i * channels + ch];
            }
            dst[i] = qint16(sum / channels);
        }
        *pcm = mono;
    }
    *sampleRate = rate;
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::puts("usage: AgentRMCAsrTest <wav> <model-dir> [sensevoice|qwen3]");
        return 1;
    }
    const QString wav = QString::fromLocal8Bit(argv[1]);
    const QString modelDir = QString::fromLocal8Bit(argv[2]);
    const QString family =
        argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("sensevoice");

    QByteArray pcm;
    int sampleRate = 0;
    QString error;
    if (!readWav(wav, &pcm, &sampleRate, &error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.toLocal8Bit().constData());
        return 1;
    }

    AsrConfig config;
    config.family = family;
    config.modelDir = modelDir;
    const auto engine = createAsrEngine(config);
    if (!engine->available(&error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.toLocal8Bit().constData());
        return 1;
    }

    QString text;
    qint64 firstMs = 0;
    qint64 secondMs = 0;
    for (int pass = 0; pass < 2; ++pass) {
        QElapsedTimer timer;
        timer.start();
        text = engine->transcribe(pcm, sampleRate, &error);
        const qint64 cost = timer.elapsed();
        if (pass == 0) {
            firstMs = cost;
        } else {
            secondMs = cost;
        }
        if (!error.isEmpty()) {
            std::fprintf(stderr, "FAIL: %s\n", error.toLocal8Bit().constData());
            return 1;
        }
    }
    std::printf("RESULT first=%lldms warm=%lldms: %s\n",
                static_cast<long long>(firstMs),
                static_cast<long long>(secondMs), text.toUtf8().constData());
    return text.isEmpty() ? 2 : 0;
}
