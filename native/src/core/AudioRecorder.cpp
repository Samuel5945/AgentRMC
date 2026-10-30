#include "AudioRecorder.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmeapi.h>

namespace {

constexpr int kBufferCount = 4;
constexpr int kTargetRate = 16000;
constexpr int kPrerollMs = 400;

struct CaptureFormat {
    int rate;
    int channels;
};

qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QByteArray to16kMono(const QByteArray &pcm, int rate, int channels) {
    if (rate == kTargetRate && channels == 1) {
        return pcm;
    }
    const int bytesPerFrame = channels * 2;
    if (bytesPerFrame <= 0 || pcm.size() < bytesPerFrame) {
        return {};
    }
    const int frames = pcm.size() / bytesPerFrame;
    const int outFrames = int(qint64(frames) * kTargetRate / rate);
    if (outFrames <= 0) {
        return {};
    }
    QByteArray out(outFrames * 2, '\0');
    const auto *src = reinterpret_cast<const qint16 *>(pcm.constData());
    auto *dst = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < outFrames; ++i) {
        const double pos = double(i) * rate / kTargetRate;
        int index = int(pos);
        const double frac = pos - index;
        if (index >= frames - 1) {
            index = frames - 1;
        }
        const int next = qMin(index + 1, frames - 1);
        double a = 0.0;
        double b = 0.0;
        for (int ch = 0; ch < channels; ++ch) {
            a += src[index * channels + ch];
            b += src[next * channels + ch];
        }
        a /= channels;
        b /= channels;
        dst[i] = qint16(qBound(-32768.0, a + (b - a) * frac, 32767.0));
    }
    return out;
}

double chunkLevel(const qint16 *samples, int count) {
    if (count <= 0) {
        return 0.0;
    }
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        const double value = samples[i] / 32768.0;
        sum += value * value;
    }
    return std::sqrt(sum / count);
}

void trimFront(QByteArray *data, int keepBytes) {
    if (data->size() > keepBytes) {
        data->remove(0, data->size() - keepBytes);
    }
}

}  // namespace

struct AudioRecorder::Impl {
    HWAVEIN handle = nullptr;
    WAVEFORMATEX format{};
    WAVEHDR headers[kBufferCount]{};
    BYTE *buffers[kBufferCount]{};
    bool consumed[kBufferCount]{};
    int chunkBytes = 0;
    int rate = kTargetRate;
    int channels = 1;
    int prerollBytes = 0;
    int capturedChunks = 0;
};

AudioRecorder::AudioRecorder() : impl_(std::make_unique<Impl>()) {}

AudioRecorder::~AudioRecorder() {
    close();
}

bool AudioRecorder::open(QString *error) {
    if (streaming_.load()) {
        return true;
    }
    closeDevice();

    const std::vector<CaptureFormat> formats = {
        {16000, 1}, {44100, 1}, {48000, 1}, {44100, 2}, {48000, 2},
    };

    bool opened = false;
    for (const CaptureFormat &candidate : formats) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = WORD(candidate.channels);
        format.nSamplesPerSec = DWORD(candidate.rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = WORD(candidate.channels * 2);
        format.nAvgBytesPerSec = DWORD(candidate.rate * format.nBlockAlign);
        HWAVEIN handle = nullptr;
        const MMRESULT result = waveInOpen(&handle, WAVE_MAPPER, &format, 0, 0,
                                           CALLBACK_NULL);
        if (result == MMSYSERR_NOERROR) {
            impl_->handle = handle;
            impl_->format = format;
            impl_->rate = candidate.rate;
            impl_->channels = candidate.channels;
            opened = true;
            break;
        }
    }

    if (!opened) {
        if (error != nullptr) {
            *error = QStringLiteral("没有可用的麦克风（WinMM 无法打开默认输入设备）");
        }
        return false;
    }

    // 50ms 一块：松手时最多再等一块，句尾不会丢 100-200ms。
    impl_->chunkBytes = qMax(800, impl_->rate * impl_->channels * 2 / 20);
    impl_->prerollBytes =
        qMax(3200, impl_->rate * impl_->channels * 2 * kPrerollMs / 1000);
    impl_->capturedChunks = 0;
    for (int i = 0; i < kBufferCount; ++i) {
        impl_->buffers[i] = new BYTE[size_t(impl_->chunkBytes)];
        std::memset(&impl_->headers[i], 0, sizeof(WAVEHDR));
        impl_->headers[i].lpData = reinterpret_cast<LPSTR>(impl_->buffers[i]);
        impl_->headers[i].dwBufferLength = DWORD(impl_->chunkBytes);
        impl_->consumed[i] = false;
        const MMRESULT prepare =
            waveInPrepareHeader(impl_->handle, &impl_->headers[i], sizeof(WAVEHDR));
        if (prepare != MMSYSERR_NOERROR) {
            closeDevice();
            if (error != nullptr) {
                *error = QStringLiteral("麦克风缓冲区初始化失败");
            }
            return false;
        }
        waveInAddBuffer(impl_->handle, &impl_->headers[i], sizeof(WAVEHDR));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pcm_.clear();
    }
    level_.store(0.0);
    capturing_.store(false);
    sampleRate_ = impl_->rate;
    channels_ = impl_->channels;

    if (waveInStart(impl_->handle) != MMSYSERR_NOERROR) {
        closeDevice();
        if (error != nullptr) {
            *error = QStringLiteral("麦克风启动失败");
        }
        return false;
    }

    streaming_.store(true);
    thread_ = std::thread(&AudioRecorder::pollLoop, this);
    return true;
}

void AudioRecorder::close() {
    capturing_.store(false);
    if (!streaming_.exchange(false)) {
        closeDevice();
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (impl_->handle != nullptr) {
        waveInStop(impl_->handle);
        waveInReset(impl_->handle);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pcm_.clear();
    }
    closeDevice();
    level_.store(0.0);
}

void AudioRecorder::startCapture() {
    if (!streaming_.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    trimFront(&pcm_, impl_->prerollBytes);
    impl_->capturedChunks = 0;
    capturing_.store(true);
    captureStartMs_ = nowMs();
    level_.store(0.0);
}

QByteArray AudioRecorder::stopCapture(int *durationMs) {
    if (!capturing_.load()) {
        if (durationMs != nullptr) *durationMs = 0;
        return {};
    }
    QByteArray pcm;
    int rate = sampleRate_;
    int channels = channels_;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const int chunks = impl_->capturedChunks;
        // 等当前正在录的那一块回来（最多 250ms），保住句尾。
        chunkCv_.wait_for(lock, std::chrono::milliseconds(250),
                          [this, chunks] {
                              return impl_->capturedChunks > chunks;
                          });
        capturing_.store(false);
        if (durationMs != nullptr) {
            *durationMs = int(qMax<qint64>(0, nowMs() - captureStartMs_));
        }
        pcm.swap(pcm_);
    }
    level_.store(0.0);
    return to16kMono(pcm, rate, channels);
}

void AudioRecorder::pollLoop() {
    while (streaming_.load()) {
        for (int i = 0; i < kBufferCount && streaming_.load(); ++i) {
            WAVEHDR &header = impl_->headers[i];
            if ((header.dwFlags & WHDR_DONE) == 0 || impl_->consumed[i]) {
                continue;
            }
            waveInUnprepareHeader(impl_->handle, &header, sizeof(WAVEHDR));
            const DWORD bytes = header.dwBytesRecorded;
            if (bytes > 0) {
                const auto *samples = reinterpret_cast<const qint16 *>(header.lpData);
                const double level = chunkLevel(samples, int(bytes / 2));
                const double previous = level_.load();
                level_.store(previous * 0.6 + level * 0.4);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pcm_.append(reinterpret_cast<const char *>(header.lpData),
                                int(bytes));
                    if (!capturing_.load()) {
                        trimFront(&pcm_, impl_->prerollBytes);
                    } else {
                        ++impl_->capturedChunks;
                    }
                }
                chunkCv_.notify_all();
            }
            header.dwBytesRecorded = 0;
            header.dwFlags = 0;
            if (streaming_.load()) {
                const MMRESULT prepare =
                    waveInPrepareHeader(impl_->handle, &header, sizeof(WAVEHDR));
                const MMRESULT add =
                    prepare == MMSYSERR_NOERROR
                        ? waveInAddBuffer(impl_->handle, &header, sizeof(WAVEHDR))
                        : prepare;
                impl_->consumed[i] = (prepare != MMSYSERR_NOERROR ||
                                      add != MMSYSERR_NOERROR);
            } else {
                impl_->consumed[i] = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void AudioRecorder::closeDevice() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->handle != nullptr) {
        for (int i = 0; i < kBufferCount; ++i) {
            WAVEHDR &header = impl_->headers[i];
            if (header.dwFlags & WHDR_PREPARED) {
                waveInUnprepareHeader(impl_->handle, &header, sizeof(WAVEHDR));
            }
            delete[] impl_->buffers[i];
            impl_->buffers[i] = nullptr;
            std::memset(&header, 0, sizeof(WAVEHDR));
            impl_->consumed[i] = false;
        }
        waveInClose(impl_->handle);
        impl_->handle = nullptr;
    }
}
