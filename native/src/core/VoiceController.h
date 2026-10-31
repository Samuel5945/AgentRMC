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

public slots:
    void voiceDown();
    void voiceUp();
    void mediaToggle();
    void tick();
    void startTest();
    void reload();
    void setListening(bool listening);

signals:
    void stateChanged(const QString &state, const QString &detail);
    void eventReady(const QString &kind, const QString &text);
    void transcribed(const QString &text, int costMs);

private:
    enum class State { Idle, Pending, Recording };

    void updateOptions();
    void startSession(bool media);
    void finishSession();
    void beginTestCapture();
    void runTranscription(const QByteArray &pcm);
    void deliver(const QString &text, int costMs, const QString &error);
    void setState(State state, const QString &detail);
    void beep(int frequency, int durationMs, bool enabled);
    QString appDir() const;

    ProfileStore *store_ = nullptr;
    WinInjector injector_;
    AudioRecorder recorder_;
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
