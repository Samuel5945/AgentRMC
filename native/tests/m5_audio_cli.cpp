#include "core/AudioRecorder.h"
#include "core/VoiceSettings.h"

#include <QFile>
#include <QThread>

#include <cstdio>

int main(int argc, char **argv) {
    const int seconds = argc > 1 ? qMax(1, QString::fromLocal8Bit(argv[1]).toInt())
                                 : 2;
    const QString path = argc > 2 ? QString::fromLocal8Bit(argv[2])
                                  : QStringLiteral("audio_test.wav");

    AudioRecorder recorder;
    QString error;
    if (!recorder.open(&error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.toLocal8Bit().constData());
        return 1;
    }
    // WinMM 某些设备要 0.5-1 秒才送第一块数据；监听时是常开的，这里模拟预热。
    QThread::sleep(1);
    recorder.startCapture();
    QThread::sleep(unsigned(seconds));
    int durationMs = 0;
    const QByteArray pcm = recorder.stopCapture(&durationMs);
    recorder.close();
    if (pcm.isEmpty()) {
        std::fprintf(stderr, "FAIL: no PCM captured (%d ms)\n", durationMs);
        return 1;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "FAIL: cannot write %s\n",
                     path.toLocal8Bit().constData());
        return 1;
    }
    file.write(wavFromPcm16(pcm, 16000, 1));
    file.close();
    std::printf("PASS native audio: %d ms, %lld bytes -> %s\n", durationMs,
                static_cast<long long>(pcm.size()),
                path.toLocal8Bit().constData());
    return 0;
}
