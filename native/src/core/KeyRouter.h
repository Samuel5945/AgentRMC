#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <set>

#include "core/Injector.h"
#include "core/Profiles.h"

class KeyRouter {
public:
    using Event = std::function<void(const QString &kind, const QString &name,
                                     const QString &profile,
                                     const QString &detail, int vk)>;

    KeyRouter(Injector *injector, Event eventSink);
    ~KeyRouter();

    void setProfile(std::shared_ptr<const Profile> profile) { profile_ = std::move(profile); }
    void keyDown(int vk, double now = -1.0);
    void keyUp(int vk, double now = -1.0);
    void tick(double now = -1.0);
    void reset();
    void setTakeover(bool enabled, const std::set<int> &vks);
    void setCursorClocks(std::atomic<bool> *cursorMode,
                         std::atomic<double> *cursorLast);
    QString mode() const { return mode_; }
    bool navMode() const { return navMode_; }

    static double nowSeconds();

private:
    struct State {
        double started = 0;
        int vk = 0;
        QJsonObject action;
        QString name;
        QString profile;
        int count = 0;
        QString fired;
        QJsonObject holdAction;
        bool deferred = false;
        bool tapHold = false;
    };
    struct Tap {
        double due = 0;
        double last = 0;
        State st;
    };
    struct Deferred {
        double due = 0;
        State st;
    };
    struct Held {
        double due = 0;
        double interval = 0;
        bool once = false;
        bool accel = false;
        State st;
    };

    void begin(int vk, double now, const QString &name,
               const QJsonObject &action,
               const std::shared_ptr<const Profile> &profile, bool applyHold = true);
    void dispatch(const QString &profileName, const QString &name,
                  const QJsonObject &action, int vk);
    QJsonObject withHold(const Profile &profile, int vk,
                         const QJsonObject &action) const;
    void queueHold(int vk, double now, State &st);
    void exitMode();
    void setMode(const QString &mode);
    bool isTapTarget(int vk) const;
    bool isModeTrigger(int vk) const;
    QString triggerName(int vk) const;
    QString modeKey(int vk) const;
    QJsonObject modeAction(const QString &name) const;

    Injector *injector_ = nullptr;
    Event emit_;
    std::shared_ptr<const Profile> profile_;
    QHash<int, State> active_;
    QHash<int, Tap> tapPending_;
    QHash<int, Deferred> deferred_;
    QHash<int, Held> held_;
    bool takeover_ = false;
    std::set<int> takeoverVks_;
    std::atomic<bool> *cursorMode_ = nullptr;
    std::atomic<double> *cursorLast_ = nullptr;
    QString mode_;
    double modeLast_ = 0;
    bool navMode_ = false;
    int modeEnterVk_ = 0;
};
