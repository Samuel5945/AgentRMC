#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();

    // 采集设备一律跟随 Windows 默认「通信」录音端点（微信/Teams 用的那一档），
    // 不再用 MME 的默认录制设备：那一档可能被 Steam 之类虚拟麦占着，
    // 真人对着耳机说话也录不到。
    // 通信端点友好名（取不到返回空串）。
    static QString communicationsDeviceName();
    // 通信端点是不是蓝牙耳机麦（名字含 Hands-Free：占着会把 A2DP 挂起，不能常开）。
    static bool communicationsDeviceIsBluetooth();
    // 通信端点对应的 WinMM 输入设备索引；-1 = 匹配不到，退回 WAVE_MAPPER。
    static int communicationsWaveInIndex();
    // WinMM 输入设备名列表，下标即 waveInOpen 的设备号。
    static QStringList waveInNames();
    // 纯函数：在 WinMM 设备名列表里找端点名。WinMM 名字只有 31 个字符宽，
    // 长端点名会被截断（还可能留个尾空格），所以精确匹配之后还要认前缀。
    static int matchWaveInIndex(const QString &endpointName,
                                const QStringList &waveInNames);

    // 打开并持续采集：设备启动有 0.5-1s 延迟，监听开始时先预热。
    bool open(QString *error = nullptr);
    void close();

    // 从当前时刻开始截取；未截取时只保留最后 400ms 作为预录。
    void startCapture();
    QByteArray stopCapture(int *durationMs = nullptr);

    bool isOpen() const { return streaming_.load(); }
    bool isCapturing() const { return capturing_.load(); }
    double level() const { return level_.load(); }

private:
    struct Impl;
    void pollLoop();
    void closeDevice();

    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
    std::condition_variable chunkCv_;
    QByteArray pcm_;
    std::thread thread_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> capturing_{false};
    std::atomic<double> level_{0.0};
    qint64 captureStartMs_ = 0;
    int sampleRate_ = 16000;
    int channels_ = 1;
};
