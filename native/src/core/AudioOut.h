#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>
#include <mutex>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

// 指定输出设备播放 WAV（winmm waveOut，无需 COM）。
// PlaySound 只能走系统默认输出，切不到耳机；这里按设备名定位 waveOut
// 设备直接灌 PCM。单路播放：新播放会停掉上一段。
class AudioOut {
public:
    AudioOut() { doneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~AudioOut() { stop(); CloseHandle(doneEvent_); }

    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;

    // 当前系统里的输出设备名列表（按 waveOut 顺序）。
    static QStringList deviceNames();

    // 播放整段 WAV；deviceName 为空或找不到时退回系统默认（WAVE_MAPPER）。
    // 返回 false = 起播失败（调用方可退回 PlaySound）。
    bool play(const QByteArray &wav, const QString &deviceName,
              QString *error = nullptr);
    void stop();

private:
    void drain();

    std::mutex mutex_;
    std::thread worker_;
    QByteArray buf_;        // 播放期间必须保持有效
    WAVEHDR hdr_ = {};
    HWAVEOUT hwo_ = nullptr;
    HANDLE doneEvent_ = nullptr;
    std::atomic_bool stop_{false};
};
