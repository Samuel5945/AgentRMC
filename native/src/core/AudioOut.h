#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

// 指定输出设备播放 WAV / PCM 流（winmm waveOut，无需 COM）。
// PlaySound 只能走系统默认输出，切不到耳机；这里按设备名定位 waveOut
// 设备直接灌 PCM。单路播放：新播放会停掉上一段。
class AudioOut {
public:
    // 自动复位：每个缓冲播完 signaled 一次，drain 醒来全量扫，信号合并不丢块。
    AudioOut() { doneEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    ~AudioOut() { stop(); CloseHandle(doneEvent_); }

    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;

    // 当前系统里的输出设备名列表（按 waveOut 顺序）。
    static QStringList deviceNames();

    // 播放整段 WAV；deviceName 为空或找不到时退回系统默认（WAVE_MAPPER）。
    // 返回 false = 起播失败（调用方可退回 PlaySound）。
    bool play(const QByteArray &wav, const QString &deviceName,
              QString *error = nullptr);
    // 流式播放（边合成边播）：beginStream 开设备，feed 追加 16bit PCM 块，
    // finishStream 封口；最后一块播完自动关设备。feed 在封口/停止后拒收。
    bool beginStream(int sampleRate, int channels, const QString &deviceName,
                     QString *error = nullptr);
    bool feed(const QByteArray &pcm);
    void finishStream();
    void stop();

    // 是否正在出声；上层用它衔接下一句，别把没念完的掐了。
    bool playing() const { return playing_; }

private:
    void drain();

    struct StreamBuf {
        QByteArray data;  // 播放期间必须活着
        WAVEHDR hdr = {};
    };

    std::mutex mutex_;
    std::thread worker_;
    std::vector<std::unique_ptr<StreamBuf>> bufs_;  // mutex_ 保护
    HWAVEOUT hwo_ = nullptr;
    HANDLE doneEvent_ = nullptr;
    bool finishing_ = false;  // mutex_ 保护
    bool streaming_ = false;  // mutex_ 保护
    std::atomic_bool stop_{false};
    std::atomic_bool playing_{false};
};
