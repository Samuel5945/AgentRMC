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

// 设备裁决：点名 → 默认通信端点 → WAVE_MAPPER，三档的选中、蓝牙判定、
// 降级说明都要能分开成因（「选了没用」和「设备拔了」是两回事）。
static int checkDeviceChoice() {
    int failures = 0;
    // WinMM 名字只有 31 个字符宽，列表里就是这种截断形态。
    const QStringList names = {
        QStringLiteral("Mic (Steam Streaming Microphone"),
        QStringLiteral("Headset (OPPO Enco Free3 Hands-Free "),
    };
    const QString oppo =
        QStringLiteral("Headset (OPPO Enco Free3 Hands-Free AG Audio)");
    const QString steam = QStringLiteral("麦克风 (Steam Streaming Microphone");
    const QString builtIn =
        QStringLiteral("麦克风阵列 (Realtek(R) Audio)");
    const auto expect = [&failures](const char *label, const MicChoice &got,
                                    const QString &wantSource, int wantIndex,
                                    bool wantBluetooth, bool wantNote) {
        const bool hasNote = !got.note.isEmpty();
        const bool bad = got.source != wantSource || got.index != wantIndex ||
                         got.bluetooth != wantBluetooth || hasNote != wantNote;
        if (bad) {
            std::fprintf(stderr,
                         "FAIL choice[%s]: source=%s(%s) index=%d(%d) "
                         "bt=%d(%d) note=%d(%d) '%s'\n",
                         label,
                         got.source.toUtf8().constData(),
                         wantSource.toUtf8().constData(), got.index, wantIndex,
                         got.bluetooth ? 1 : 0, wantBluetooth ? 1 : 0,
                         hasNote ? 1 : 0, wantNote ? 1 : 0,
                         got.note.toUtf8().constData());
            ++failures;
        }
    };

    // 1) 没点名：跟随默认通信端点（老行为，不能变）。
    expect("auto follows comms",
           AudioRecorder::chooseMicDevice(QString(), oppo, names),
           QStringLiteral("comms"), 1, true, false);
    // 2) 点名列表里的设备（截断名原样传进来）：用点名的那台，
    //    蓝牙判据也跟着换成点名的设备——这正是「换掉被占的耳机麦」的用法。
    expect("pin non-bluetooth",
           AudioRecorder::chooseMicDevice(names.at(0), oppo, names),
           QStringLiteral("pinned"), 0, false, false);
    // 3) 点名点到自己就是蓝牙耳机：照常判蓝牙（不许常开占死耳机通道）。
    expect("pin bluetooth", AudioRecorder::chooseMicDevice(oppo, builtIn, names),
           QStringLiteral("pinned"), 1, true, false);
    // 4) 点名的设备拔了：退回通信端点，但必须留下原因（静默退档＝假成功）。
    //    蓝牙判据跟着退回后的那台走——这里通信端点是耳机麦，所以仍判蓝牙。
    expect("pinned lost falls back",
           AudioRecorder::chooseMicDevice(builtIn, oppo, names),
           QStringLiteral("comms"), 1, true, true);
    // 5) 端点也匹配不到任何 WinMM 设备，且列表里只有虚拟麦 + 耳机麦：
    //    宁可走 WAVE_MAPPER，也不换到录不到人的虚拟麦上。
    expect("no comms match",
           AudioRecorder::chooseMicDevice(QString(), QStringLiteral("没人插着麦"),
                                          names),
           QStringLiteral("mapper"), -1, false, false);
    // 6) 一个设备都没有（列表空）：mapper 兜底，蓝牙按端点名判。
    expect("empty list",
           AudioRecorder::chooseMicDevice(QString(), oppo, QStringList{}),
           QStringLiteral("mapper"), -1, true, false);

    // 自动档躲占位：通信端点被耳机麦占着，但列表里有真麦 → 用真麦（prefer）。
    const QStringList withReal = QStringList()
        << names << QStringLiteral("麦克风阵列 (Realtek(R) Audio)");
    expect("auto dodges bluetooth",
           AudioRecorder::chooseMicDevice(QString(), oppo, withReal),
           QStringLiteral("prefer"), 2, false, false);
    // 端点被 Steam 虚拟麦占着，同样躲到真麦。
    expect("auto dodges virtual",
           AudioRecorder::chooseMicDevice(QString(), steam, withReal),
           QStringLiteral("prefer"), 2, false, false);
    // 端点名对不上列表，但有真麦：先用真麦，别急着退 WAVE_MAPPER。
    expect("unmatched comms prefers real",
           AudioRecorder::chooseMicDevice(QString(), QStringLiteral("没人插着麦"),
                                          withReal),
           QStringLiteral("prefer"), 2, false, false);
    // 用户点名了耳机麦：点名优先，自动躲占位不许再改（显式选择压过启发式）。
    expect("pinned beats prefer", AudioRecorder::chooseMicDevice(oppo, oppo, withReal),
           QStringLiteral("pinned"), 1, true, false);

    if (!AudioRecorder::nameIsBluetooth(QStringLiteral("Headset (X Hands-Free AG)"))) {
        std::fprintf(stderr, "FAIL nameIsBluetooth: Hands-Free 没认出来\n");
        ++failures;
    }
    if (AudioRecorder::nameIsBluetooth(builtIn)) {
        std::fprintf(stderr, "FAIL nameIsBluetooth: 内置麦被误判成蓝牙\n");
        ++failures;
    }
    if (!AudioRecorder::nameIsVirtualMic(steam) ||
        !AudioRecorder::nameIsVirtualMic(
            QStringLiteral("Microphone (VB-Cable Bypass)")) ||
        !AudioRecorder::nameIsVirtualMic(QStringLiteral("Cable Output (Virtual Audio Cable)"))) {
        std::fprintf(stderr, "FAIL nameIsVirtualMic: 虚拟麦没认出来\n");
        ++failures;
    }
    if (AudioRecorder::nameIsVirtualMic(builtIn) ||
        AudioRecorder::nameIsVirtualMic(QStringLiteral("Microphone (USB Audio Device)"))) {
        std::fprintf(stderr, "FAIL nameIsVirtualMic: 真麦被误判成虚拟麦\n");
        ++failures;
    }
    if (AudioRecorder::firstRealMic(withReal) != builtIn) {
        std::fprintf(stderr, "FAIL firstRealMic: got '%s'\n",
                     AudioRecorder::firstRealMic(withReal).toUtf8().constData());
        ++failures;
    }
    if (!AudioRecorder::firstRealMic(names).isEmpty()) {
        std::fprintf(stderr, "FAIL firstRealMic: 只有虚拟麦+耳机麦时不该给出设备\n");
        ++failures;
    }
    // sameDevice 是「设备变了要重开麦克风」的判据：按设备号认，不按来源认。
    const auto same = [](const QString &sa, int ia, const QString &sb, int ib) {
        MicChoice a;
        a.source = sa;
        a.index = ia;
        MicChoice b;
        b.source = sb;
        b.index = ib;
        return AudioRecorder::sameDevice(a, b);
    };
    struct SameCase {
        const char *label;
        bool got;
        bool want;
    };
    const SameCase cases[] = {
        {"同一台：点名命中 vs 自动命中同一号", same("pinned", 1, "comms", 1), true},
        {"同一台：自动 vs 躲占位命中同一号", same("comms", 2, "prefer", 2), true},
        {"换了台：耳机麦 vs 真麦", same("comms", 1, "prefer", 2), false},
        {"都没号：mapper vs mapper", same("mapper", -1, "mapper", -1), true},
        {"都没号：没开过 vs mapper", same("none", -1, "mapper", -1), false},
        {"一边有号一边没号", same("comms", 1, "mapper", -1), false},
    };
    for (const SameCase &c : cases) {
        if (c.got != c.want) {
            std::fprintf(stderr, "FAIL sameDevice[%s]: got %d want %d\n", c.label,
                         c.got ? 1 : 0, c.want ? 1 : 0);
            ++failures;
        }
    }
    std::printf("%s device choice\n", failures == 0 ? "PASS" : "FAIL");
    return failures;
}

int main(int argc, char **argv) {
    const int seconds = argc > 1 ? qMax(1, QString::fromLocal8Bit(argv[1]).toInt())
                                 : 2;
    const QString path = argc > 2 ? QString::fromLocal8Bit(argv[2])
                                  : QStringLiteral("audio_test.wav");
    // 第三个参数：点名要从哪台录（用来采「你说话时到底录到了什么」的真音频）。
    const QString pinArg =
        argc > 3 ? QString::fromLocal8Bit(argv[3]).trimmed() : QString();

    int failures = checkNameMatching() + checkDeviceChoice();
    if (failures != 0) {
        std::fprintf(stderr, "FAIL offline checks: %d\n", failures);
        return 1;
    }
    // 真机读数：不点名时这次会打开哪台（-1 = 退回 WAVE_MAPPER）。
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

    // 自动档（没点名）：open 走的必须是通信端点那一档。
    AudioRecorder recorder;
    QString error;
    if (!recorder.open(&error)) {
        std::fprintf(stderr, "FAIL auto open: %s\n", error.toLocal8Bit().constData());
        return 1;
    }
    std::printf("auto open -> device='%s' source=%s\n",
                recorder.openedDeviceName().toUtf8().constData(),
                recorder.openedDeviceSource().toUtf8().constData());
    recorder.close();

    // 点名档：验证「点名的就是实际打开的那台」。没给参数就拿列表第一台试。
    const QString pinned = pinArg.isEmpty()
                               ? (names.isEmpty() ? QString() : names.at(0).trimmed())
                               : pinArg;
    if (!pinned.isEmpty()) {
        recorder.setDeviceName(pinned);
        QString pinError;
        if (!recorder.open(&pinError)) {
            std::fprintf(stderr, "FAIL pinned open: %s\n",
                         pinError.toLocal8Bit().constData());
            return 1;
        }
        const QString got = recorder.openedDeviceName();
        const QString source = recorder.openedDeviceSource();
        std::printf("pinned '%s' -> device='%s' source=%s note='%s'\n",
                    pinned.toUtf8().constData(), got.toUtf8().constData(),
                    source.toUtf8().constData(),
                    recorder.openedDeviceNote().toUtf8().constData());
        if (source != QStringLiteral("pinned")) {
            std::fprintf(stderr, "FAIL: 点名没生效（source=%s）\n",
                         source.toUtf8().constData());
            recorder.close();
            return 1;
        }
    } else {
        std::printf("note: 这台机器没有 waveIn 输入设备，跳过点名档\n");
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
    // 正面通过标记：缺这行就是没跑完，别把「没有 FAIL」当成过了。
    std::printf("ALL PASS audio\n");
    return 0;
}
