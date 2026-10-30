#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();

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
