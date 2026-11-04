#include "core/AudioRecorder.h"
#include "core/VoiceSettings.h"

#include <QFile>
#include <QThread>

#include <cstdio>

// 设备名匹配是纯字符串活，先离线断言，再拿真机做端到端录音。
static int checkNameMatching() {
    int failures = 0;
    const auto expect = [&failures](const QString &endpoint, int got, int want,
                                    const char *label) {
        if (got != want) {
            std::fprintf(stderr, "FAIL match '%s' -> %d, want %d (%s)\n",
                         endpoint.toUtf8().constData(), got, want, label);
            ++failures;
        }
    };

    const QString oppo =
        QStringLiteral("Headset (OPPO Enco Free3 Hands-Free AG Audio)");
    const QString steam = QStringLiteral("Mic (Steam Streaming Microphone)");
    // WinMM 只留 31 个字符：OPPO 那条截到半截还带尾空格，Steam 那条少了右括号。
    QStringList truncated;
    truncated << QStringLiteral("Mic (Steam Streaming Microphone")
              << QStringLiteral("Headset (OPPO Enco Free3 Hands-Free ");
    expect(oppo, AudioRecorder::matchWaveInIndex(oppo, truncated), 1,
           "truncated tail space");
    expect(steam, AudioRecorder::matchWaveInIndex(steam, truncated), 0,
           "truncated head");

    QStringList exact;
    exact << QStringLiteral("Mic A") << oppo;
    expect(oppo, AudioRecorder::matchWaveInIndex(oppo, exact), 1, "exact");

    // 同系列里两个都能前缀命中时，要挑截得最长的那个。
    QStringList both;
    both << QStringLiteral("Headset (OPPO Enco Free3")
         << QStringLiteral("Headset (OPPO Enco Free3 Hands-Free ");
    expect(oppo, AudioRecorder::matchWaveInIndex(oppo, both), 1, "longest wins");

    expect(QStringLiteral("No Such Mic"),
           AudioRecorder::matchWaveInIndex(QStringLiteral("No Such Mic"),
                                           truncated),
           -1, "no match");
    expect(QString(), AudioRecorder::matchWaveInIndex(QString(), truncated), -1,
           "empty name");
    std::printf("%s name matching\n", failures == 0 ? "PASS" : "FAIL");
    return failures;
}

int main(int argc, char **argv) {
    const int seconds = argc > 1 ? qMax(1, QString::fromLocal8Bit(argv[1]).toInt())
                                 : 2;
    const QString path = argc > 2 ? QString::fromLocal8Bit(argv[2])
                                  : QStringLiteral("audio_test.wav");

    if (checkNameMatching() != 0) {
        return 1;
    }
    // 真机读数：这次要打开的到底是哪台设备（-1 = 退回 WAVE_MAPPER）。
    const QString endpoint = AudioRecorder::communicationsDeviceName();
    std::printf("comms capture = '%s' waveInIndex=%d bluetooth=%d\n",
                endpoint.isEmpty()
                    ? "(none)"
                    : endpoint.toUtf8().constData(),
                AudioRecorder::communicationsWaveInIndex(),
                AudioRecorder::communicationsDeviceIsBluetooth() ? 1 : 0);
    const QStringList names = AudioRecorder::waveInNames();
    for (int i = 0; i < names.size(); ++i) {
        std::printf("  waveIn[%d] = '%s'\n", i, names.at(i).toUtf8().constData());
    }

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
