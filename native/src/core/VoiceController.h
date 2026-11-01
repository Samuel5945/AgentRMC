#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>

#include "core/AudioRecorder.h"
#include "core/Injector.h"
#include "core/VoiceSettings.h"

class AsrClient;
class AsrEngine;
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

private:
    enum class State { Idle, Pending, Recording };

    void updateOptions();
    void startSession(bool media);
    void finishSession();
    void beginTestCapture();
    void runTranscription(const QByteArray &pcm);
    void runCloudTranscription(const QByteArray &pcm);
    void deliver(const QString &text, int costMs, const QString &error);
    void setState(State state, const QString &detail);
    void beep(int frequency, int durationMs, bool enabled);
    QString appDir() const;

    ProfileStore *store_ = nullptr;
    WinInjector injector_;
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
    int repeats_ = 0;
    qint64 pendingAt_ = 0;
    qint64 recordAt_ = 0;
    qint64 lastVoiceAt_ = 0;
    qint64 lastMediaAt_ = 0;
    QStringList activeMods_;
    int activeVk_ = 0;
    QString activeTrigger_;
    QString lastText_;
    std::atomic<bool> alive_{true};
};
