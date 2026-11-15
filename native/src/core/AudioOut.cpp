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
    WavView view;
    if (!parseWav(wav, &view)) {
        if (error != nullptr) {
            *error = QStringLiteral("返回的不是可播放的 PCM WAV");
        }
        return false;
    }
    if (!beginStream(int(view.wfx.nSamplesPerSec), int(view.wfx.nChannels),
                     deviceName, error)) {
        return false;
    }
    if (!feed(QByteArray(view.data, int(view.size)))) {
        stop();
        if (error != nullptr) {
            *error = QStringLiteral("音频写入失败");
        }
        return false;
    }
    finishStream();
    return true;
}

bool AudioOut::beginStream(int sampleRate, int channels,
                           const QString &deviceName, QString *error) {
    // 先 join 旧线程再拿锁：drain 循环里要锁 mutex_，持锁 join 会死锁。
    stop();
    std::lock_guard<std::mutex> lock(mutex_);

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = WORD(channels);
    wfx.nSamplesPerSec = DWORD(sampleRate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = WORD(channels * 2);
    wfx.nAvgBytesPerSec = DWORD(sampleRate * wfx.nBlockAlign);
    if (waveOutOpen(&hwo_, UINT(resolveDevice(deviceName)), &wfx,
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
    stop_ = false;
    finishing_ = false;
    streaming_ = true;
    ResetEvent(doneEvent_);
    playing_ = true;
    worker_ = std::thread([this] { drain(); });
    return true;
}

bool AudioOut::feed(const QByteArray &pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!streaming_ || finishing_ || stop_ || pcm.isEmpty()) {
        return false;
    }
    auto buf = std::make_unique<StreamBuf>();
    buf->data = pcm;
    buf->hdr.lpData = buf->data.data();
    buf->hdr.dwBufferLength = DWORD(buf->data.size());
    if (waveOutPrepareHeader(hwo_, &buf->hdr, sizeof(buf->hdr)) !=
        MMSYSERR_NOERROR) {
        return false;
    }
    if (waveOutWrite(hwo_, &buf->hdr, sizeof(buf->hdr)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(hwo_, &buf->hdr, sizeof(buf->hdr));
        return false;
    }
    bufs_.push_back(std::move(buf));
    return true;
}

void AudioOut::finishStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!streaming_) {
        return;
    }
    finishing_ = true;
    SetEvent(doneEvent_);  // 一块都没喂就封口时，drain 靠这个醒来收尾
}

void AudioOut::drain() {
    // 等播完（WOM_DONE 触发事件）；stop 时先 reset 再回收全部缓冲。
    for (;;) {
        WaitForSingleObject(doneEvent_, 400);
        bool end = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                waveOutReset(hwo_);
            }
            for (auto it = bufs_.begin(); it != bufs_.end();) {
                if ((*it)->hdr.dwFlags & WHDR_DONE) {
                    waveOutUnprepareHeader(hwo_, &(*it)->hdr,
                                           sizeof((*it)->hdr));
                    it = bufs_.erase(it);
                } else {
                    ++it;
                }
            }
            end = stop_ || (finishing_ && bufs_.empty());
        }
        if (end) {
            break;
        }
    }
    waveOutClose(hwo_);
    hwo_ = nullptr;
    streaming_ = false;
    playing_ = false;
}

void AudioOut::stop() {
    stop_ = true;
    SetEvent(doneEvent_);
    if (worker_.joinable()) {
        worker_.join();
    }
    bufs_.clear();
    stop_ = false;
}
