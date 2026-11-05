#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

// 「这次从哪台录音设备收声」的裁决结果。选设备、判蓝牙、给 /state 报读数
// 三处都读同一个结构，免得判据和实际动作各算各的（以前就该一致的两处会不一致）。
struct MicChoice {
    QString name;                          // 选中的设备名；mapper 档没有名字，留空
    int index = -1;                        // waveIn 设备号；-1 = 走 WAVE_MAPPER
    // "none"=还没开过 / "pinned"=用户点名 / "comms"=默认通信端点 /
    // "prefer"=自动档躲开被占的端点换了真麦 / "mapper"=MME 默认
    QString source = QStringLiteral("none");
    bool bluetooth = false;                // 蓝牙耳机麦：占着会挂起 A2DP，不能常开
    QString note;                          // 降级原因（点名的设备没了等）；正常为空
};

class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();

    // 默认（voice.input_device 留空）时采集设备跟随 Windows 默认「通信」录音
    // 端点（微信/Teams 用的那一档），不用 MME 的默认录制设备：那一档可能被
    // Steam 之类虚拟麦占着，真人对着耳机说话也录不到。
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
    // 名字里带 Hands-Free 就算蓝牙耳机麦（设备名判据，和端点判据同一条）。
    static bool nameIsBluetooth(const QString &name);
    // 已知「不是真人麦」的虚拟麦：Steam 直播麦、VB-CABLE/Voicemeeter、
    // 各种 Virtual Audio Cable。名字判据，命中就当它不可用。
    static bool nameIsVirtualMic(const QString &name);
    // 纯函数：列表里第一台「真麦克风」（非蓝牙、非虚拟）。找不到返回空串。
    static QString firstRealMic(const QStringList &names);
    // 纯函数：按「用户点名 → 默认通信端点 → 真麦克风（躲开被占的档）→
    // WAVE_MAPPER」定这次开哪台。pinned 在列表里找不到时不静默改成自动，
    // 而是把原因写进 note。
    static MicChoice chooseMicDevice(const QString &pinned,
                                     const QString &commsName,
                                     const QStringList &names);

    // 点名要用的设备（空 = 自动跟随默认通信端点）；要在 open() 之前设。
    // 只在 GUI 线程读写（采集线程不碰这几个字段）。
    void setDeviceName(const QString &name);
    QString requestedDeviceName() const { return deviceName_; }
    // 上一次 open() 实际用的设备读数；没开过时 source=="none"。
    QString openedDeviceName() const { return opened_.name; }
    QString openedDeviceSource() const { return opened_.source; }
    QString openedDeviceNote() const { return opened_.note; }
    bool openedDeviceIsBluetooth() const { return opened_.bluetooth; }

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
    QString deviceName_;  // 点名的设备（空 = 自动）；GUI 线程读写
    MicChoice opened_;    // 上一次 open() 实际用的设备；close() 不清，留着好排查
    qint64 captureStartMs_ = 0;
    int sampleRate_ = 16000;
    int channels_ = 1;
};
