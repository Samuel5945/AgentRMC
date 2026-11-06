#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>

#include "core/AudioRecorder.h"
#include "core/Injector.h"
#include "core/SystemCommand.h"
#include "core/SystemConfirm.h"
#include "core/VoiceSettings.h"

class AsrClient;
class AsrEngine;
class ISystemControl;
class ProfileStore;
class QTimer;

class VoiceController final : public QObject {
    Q_OBJECT

public:
    explicit VoiceController(ProfileStore *store, QObject *parent = nullptr);
    ~VoiceController() override;

    VoiceOptions options();
    QString backendLabel();
    QString statusText();
    QString hotkeyHint();
    bool modelReady(QString *detail = nullptr);
    bool busy() const { return state_ != State::Idle || transcribing_; }
    bool recording() const { return state_ == State::Recording; }
    bool chatMode() const { return chatMode_; }
    // 麦克风实时电平（0~1，未开麦时为 0），给语音主界面画电平环。
    double micLevel() const { return recorder_.level(); }
    // 麦克风读数（进 /state）：想要哪台、实际开的哪台、走的是点名还是自动、
    // 为什么退档。「语音没声」要先能分清是没设备、被占、还是选错了档。
    QJsonObject micStatus() const;
    // 把一句话直接粘到当前前台窗口（桌宠「交给 Agent」用，不做前台判断）。
    // 调用方负责先把目标窗口拉到前台。
    bool injectText(const QString &text, bool autoEnter = true);
    // 向前台窗口发一个组合键（派活时先按宿主的「聚焦聊天输入框」键）。
    // SendInput 被拦时返回 false，调用方据此回落剪贴板。
    bool sendHotkey(const QStringList &mods, int vk);
    // 打字对话（语音主界面输入框）：与对话模式下的说话同路由——
    // transcribed 展示 + chatRequested 交桌宠判派活/对话；不走 deliver 的
    // 「粘进前台」路（打字时前台往往就是自己这个窗口）。
    void submitTyped(const QString &text);
    // 系统控制（音量/亮度/媒体/电源）的执行器，主窗建好后注入，与桥共用一份。
    void setSystemControl(ISystemControl *control);

public slots:
    void voiceDown();
    void voiceUp();
    void mediaToggle();
    void tick();
    void startTest();
    void reload();
    void setListening(bool listening);
    // 桌宠对话模式：转写不再粘贴/启动应用，改发给 chatRequested 喂 LLM。
    void setChatMode(bool on);

signals:
    void stateChanged(const QString &state, const QString &detail);
    void eventReady(const QString &kind, const QString &text);
    void transcribed(const QString &text, int costMs);
    // reload() 后发：识别方式等选项变了，让主窗/语音面板/桌宠同步自己的模式控件。
    void optionsChanged();
    // setChatMode() 后发：对话模式可能在桌宠右键/语音面板任一处开关，
    // 两边的勾选态和缓存都靠它互相同步。
    void chatModeChanged(bool on);
    // 对话模式下的一句转写（chatMode 才会发）。
    void chatRequested(const QString &text);
    // 系统命令的确认口令与执行结果：主窗接到桌宠的 speak 上念出来。
    // 只有语音来源才发——打字时用户就在键盘前，不必再念一遍。
    void spokenFeedback(const QString &text);

private:
    enum class State { Idle, Pending, Recording };

    void updateOptions();
    // 监听期间是否常开麦克风：蓝牙耳机永远 false（占麦克风会挂起 A2DP）。
    bool shouldHoldMic();
    // 配置里点名的录音设备（voice.input_device：名字，或老的序号形态）
    // 换成当前 waveIn 列表里的设备名；空串 = 自动跟随默认通信端点。
    QString pinnedMicName() const;
    // 所有开麦入口都走这里：先按当前配置点名，再 open，
    // 保证「判据看的那台」和「真打开的那台」不会分叉。
    bool openMic(QString *error);
    // 盯着设备变化：默认通信档被换掉（插拔耳机、串流起停、Steam 虚拟麦抢档）时，
    // 常开的麦克风还挂在旧设备上，判据和实际录的那台会分叉，必须重开。
    void syncMicDevice();
    // /state 用的当前可打开录音设备清单：一次 curl 就能对账「列不列得到那台麦」
    // 和「实际用的是哪台」。
    static QJsonArray micDeviceList();
    void startSession(bool media);
    void finishSession();
    void beginTestCapture();
    void runTranscription(const QByteArray &pcm);
    void runCloudTranscription(const QByteArray &pcm);
    void deliver(const QString &text, int costMs, const QString &error);
    // 命中系统命令返回 true（调用方就此收手，不再听写）；没命中返回 false，
    // 这句话照原样往下走。typed=true 时不许触发关机/睡眠这类不可逆动作。
    bool deliverSystem(const QString &text, int costMs, bool typed);
    void dispatchSystem(const SystemCommand &command, bool confirmed, bool typed);
    void expireConfirm(qint64 now);
    void setState(State state, const QString &detail);
    void beep(int frequency, int durationMs, bool enabled);
    QString appDir() const;

    ProfileStore *store_ = nullptr;
    WinInjector injector_;
    ISystemControl *system_ = nullptr;
    AudioRecorder recorder_;
    AsrClient *asr_ = nullptr;  // 云端 ASR 路线；本地路线仍是 engine_
    std::unique_ptr<AsrEngine> engine_;
    std::mutex engineMutex_;
    QString engineFamily_;
    QString engineDir_;
    QString engineLang_;
    VoiceOptions options_;
    QTimer *tick_ = nullptr;
    State state_ = State::Idle;
    bool mediaStart_ = false;
    bool test_ = false;
    bool transcribing_ = false;
    bool listening_ = false;
    bool chatMode_ = false;
    bool toggleCombo_ = false;
    bool holdCombo_ = false;
    bool micAlwaysOn_ = false;  // true = 监听期间常开（非蓝牙，保 preroll）
    qint64 micSyncAt_ = 0;      // 上次盯设备变化的时刻（tick 是 50ms，这里 2s 一次）
    int repeats_ = 0;
    qint64 pendingAt_ = 0;
    qint64 recordAt_ = 0;
    qint64 lastVoiceAt_ = 0;
    qint64 lastMediaAt_ = 0;
    QStringList activeMods_;
    int activeVk_ = 0;
    QString activeTrigger_;
    QString lastText_;
    // 危险动作的二次确认：只认完整口令（「确认关机」），超时或说别的都作废。
    SystemConfirm confirm_;
    bool systemBusy_ = false;
    std::atomic<bool> alive_{true};
};
