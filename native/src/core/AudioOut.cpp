#include "core/AudioOut.h"

#include <QVector>

namespace {

struct WavView {
    const char *data = nullptr;
    DWORD size = 0;
    WAVEFORMATEX wfx = {};
};

// 最小 RIFF 解析：够读 TTS 返回的 PCM WAV 就行。
bool parseWav(const QByteArray &wav, WavView *out) {
    const uchar *p = reinterpret_cast<const uchar *>(wav.constData());
    const int n = wav.size();
    if (n < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool haveFmt = false;
    int pos = 12;
    while (pos + 8 <= n) {
        const quint32 size =
            quint32(p[pos + 4]) | (quint32(p[pos + 5]) << 8) |
            (quint32(p[pos + 6]) << 16) | (quint32(p[pos + 7]) << 24);
        if (memcmp(p + pos, "fmt ", 4) == 0 && size >= 16) {
            const uchar *f = p + pos + 8;
            auto rd16 = [&f](int offset) {
                return quint16(f[offset]) | (quint16(f[offset + 1]) << 8);
            };
            auto rd32 = [&f](int offset) {
                return quint32(f[offset]) | (quint32(f[offset + 1]) << 8) |
                       (quint32(f[offset + 2]) << 16) |
                       (quint32(f[offset + 3]) << 24);
            };
            // 只支持线性 PCM；TTS 返回的就是这种。
            if (rd16(0) != WAVE_FORMAT_PCM) {
                return false;
            }
            out->wfx.wFormatTag = WAVE_FORMAT_PCM;
            out->wfx.nChannels = rd16(2);
            out->wfx.nSamplesPerSec = rd32(4);
            out->wfx.nAvgBytesPerSec = rd32(8);
            out->wfx.nBlockAlign = rd16(12);
            out->wfx.wBitsPerSample = rd16(14);
            out->wfx.cbSize = 0;
            haveFmt = out->wfx.nChannels > 0 &&
                      out->wfx.nSamplesPerSec > 0 && out->wfx.wBitsPerSample > 0;
        } else if (memcmp(p + pos, "data", 4) == 0) {
            const quint32 avail = quint32(n - pos - 8);
            out->data = reinterpret_cast<const char *>(p + pos + 8);
            out->size = size < avail ? size : avail;
        }
        pos += int(8 + size + (size & 1));
    }
    return haveFmt && out->data != nullptr && out->size > 0;
}

int resolveDevice(const QString &deviceName) {
    if (deviceName.trimmed().isEmpty()) {
        return WAVE_MAPPER;
    }
    const UINT count = waveOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEOUTCAPSW caps = {};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        if (QString::fromWCharArray(caps.szPname).compare(deviceName.trimmed(),
                                                         Qt::CaseInsensitive) == 0) {
            return int(i);
        }
    }
    return WAVE_MAPPER;  // 设备可能被拔了：退回系统默认
}

}  // namespace

QStringList AudioOut::deviceNames() {
    QStringList names;
    const UINT count = waveOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEOUTCAPSW caps = {};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        // 蓝牙耳机会同时出「Stereo」和「Hands-Free AG Audio」两个端点；
        // 往 Hands-Free 放声音同样会把耳机切进通话通道（A2DP 挂起），
        // TTS 播报用不上它，列表里直接滤掉防误选。
        const QString name = QString::fromWCharArray(caps.szPname);
        if (!name.contains(QStringLiteral("hands-free"), Qt::CaseInsensitive)) {
            names.append(name);
        }
    }
    return names;
}

bool AudioOut::play(const QByteArray &wav, const QString &deviceName,
                    QString *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    stop();

    WavView view;
    if (!parseWav(wav, &view)) {
        if (error != nullptr) {
            *error = QStringLiteral("返回的不是可播放的 PCM WAV");
        }
        return false;
    }
    if (waveOutOpen(&hwo_, UINT(resolveDevice(deviceName)), &view.wfx,
                    reinterpret_cast<DWORD_PTR>(doneEvent_), 0,
                    CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        hwo_ = nullptr;
        if (error != nullptr) {
            *error = QStringLiteral("打不开音频输出设备 %1（可能已拔出或被独占）")
                         .arg(deviceName.isEmpty() ? QStringLiteral("系统默认")
                                                   : deviceName);
        }
        return false;
    }

    buf_ = wav;  // view.data 指向这块内存，播放期间必须活着
    ZeroMemory(&hdr_, sizeof(hdr_));
    hdr_.lpData = const_cast<LPSTR>(view.data);
    hdr_.dwBufferLength = view.size;
    if (waveOutPrepareHeader(hwo_, &hdr_, sizeof(hdr_)) != MMSYSERR_NOERROR) {
        waveOutClose(hwo_);
        hwo_ = nullptr;
        return false;
    }

    stop_ = false;
    ResetEvent(doneEvent_);
    if (waveOutWrite(hwo_, &hdr_, sizeof(hdr_)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(hwo_, &hdr_, sizeof(hdr_));
        waveOutClose(hwo_);
        hwo_ = nullptr;
        return false;
    }
    worker_ = std::thread([this] { drain(); });
    return true;
}

void AudioOut::drain() {
    // 等播完（WOM_DONE 会触发事件）；stop 时先 reset 再等 unprepare 成功。
    for (;;) {
        WaitForSingleObject(doneEvent_, 400);
        const MMRESULT result =
            waveOutUnprepareHeader(hwo_, &hdr_, sizeof(hdr_));
        if (result != WAVERR_STILLPLAYING) {
            break;
        }
        if (stop_) {
            waveOutReset(hwo_);
        }
    }
    waveOutClose(hwo_);
    hwo_ = nullptr;
}

void AudioOut::stop() {
    stop_ = true;
    SetEvent(doneEvent_);
    if (worker_.joinable()) {
        worker_.join();
    }
    buf_.clear();
    stop_ = false;
}
