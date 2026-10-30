#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QObject>
#include <QString>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "core/Injector.h"
#include "core/KeyRouter.h"
#include "core/Profiles.h"

class QTimer;
class InputEngine;

class InputEngineFilter final : public QAbstractNativeEventFilter {
public:
    explicit InputEngineFilter(InputEngine *engine) : engine_(engine) {}
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;

private:
    InputEngine *engine_;
};

class InputEngine final : public QObject {
    Q_OBJECT

public:
    explicit InputEngine(ProfileStore *store, QObject *parent = nullptr);
    ~InputEngine() override;

    bool start(QString *error = nullptr);
    void stop();
    bool isRunning() const { return running_.load(); }

signals:
    void eventReady(const QString &kind, const QString &name,
                    const QString &profile, const QString &detail, int vk);
    void runningChanged(bool running, const QString &message);

private:
    friend class InputEngineFilter;

    struct RawEvent {
        int vk = 0;
        bool down = false;
    };

    void handleRaw(void *lparam);
    void pushRaw(const RawEvent &event);
    void workerLoop();
    void refreshProfile();
    QString deviceName(void *handle);
    void cleanup();

    ProfileStore *store_ = nullptr;
    WinInjector injector_;
    std::unique_ptr<KeyRouter> router_;
    InputEngineFilter *filter_ = nullptr;
    void *hwnd_ = nullptr;
    void *mutex_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex queueMutex_;
    std::mutex profileMutex_;
    std::deque<RawEvent> queue_;
    std::shared_ptr<const Profile> profile_;
    QHash<void *, QString> deviceNames_;
    QString deviceMark_;
    QTimer *profileTimer_ = nullptr;
};
