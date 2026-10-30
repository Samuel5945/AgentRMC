#include "KeyRouter.h"

#include "core/Presets.h"

#include <QJsonArray>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr double kTapWindow = 0.30;
constexpr double kDoubleWindow = 0.45;
constexpr double kModeIdle = 12.0;
constexpr double kMaxHold = 8.0;

const QHash<int, QString> &tapTargets() {
    static const QHash<int, QString> table = {
        {27, "media"}, {36, "nav"}, {93, "cursor"}};
    return table;
}

const QHash<QString, QJsonObject> &modeActions() {
    static const QHash<QString, QJsonObject> table = {
        {"volume_up",
         QJsonObject{{"type", "key"}, {"vk", 0xAF},
                     {"hold_repeat", 120}, {"label", QStringLiteral("音量 +")}}},
        {"volume_down",
         QJsonObject{{"type", "key"}, {"vk", 0xAE},
                     {"hold_repeat", 120}, {"label", QStringLiteral("音量 -")}}},
        {"brightness_up",
         QJsonObject{{"type", "brightness"}, {"delta", 10},
                     {"hold_repeat", 160}, {"label", QStringLiteral("亮度 +")}}},
        {"brightness_down",
         QJsonObject{{"type", "brightness"}, {"delta", -10},
                     {"hold_repeat", 160}, {"label", QStringLiteral("亮度 -")}}},
        {"mouse_up",
         QJsonObject{{"type", "mouse"}, {"dx", 0}, {"dy", -8},
                     {"hold_repeat", 45}, {"hold_accel", true},
                     {"label", QStringLiteral("鼠标 ↑")}}},
        {"mouse_down",
         QJsonObject{{"type", "mouse"}, {"dx", 0}, {"dy", 8},
                     {"hold_repeat", 45}, {"hold_accel", true},
                     {"label", QStringLiteral("鼠标 ↓")}}},
        {"mouse_left",
         QJsonObject{{"type", "mouse"}, {"dx", -8}, {"dy", 0},
                     {"hold_repeat", 45}, {"hold_accel", true},
                     {"label", QStringLiteral("鼠标 ←")}}},
        {"mouse_right",
         QJsonObject{{"type", "mouse"}, {"dx", 8}, {"dy", 0},
                     {"hold_repeat", 45}, {"hold_accel", true},
                     {"label", QStringLiteral("鼠标 →")}}},
        {"mouse_click",
         QJsonObject{{"type", "click"},
                     {"label", QStringLiteral("鼠标左键")}}},
    };
    return table;
}

QString modeLabel(const QString &mode) {
    if (mode == "media") return QStringLiteral("音量/亮度模式");
    if (mode == "cursor") return QStringLiteral("鼠标模式");
    if (mode == "nav") return QStringLiteral("导航模式");
    return mode;
}

}  // namespace

KeyRouter::KeyRouter(Injector *injector, Event eventSink)
    : injector_(injector), emit_(std::move(eventSink)) {}

KeyRouter::~KeyRouter() = default;

double KeyRouter::nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool KeyRouter::isTapTarget(int vk) const {
    return tapTargets().contains(vk);
}

bool KeyRouter::isModeTrigger(int vk) const {
    return tapTargets().contains(vk);
}

QString KeyRouter::triggerName(int vk) const {
    return tapTargets().value(vk);
}

QString KeyRouter::modeKey(int vk) const {
    if (mode_ == "media") {
        switch (vk) {
        case 38: return "volume_up";
        case 40: return "volume_down";
        case 37: return "brightness_down";
        case 39: return "brightness_up";
        default: return {};
        }
    }
    if (mode_ == "cursor") {
        switch (vk) {
        case 38: return "mouse_up";
        case 40: return "mouse_down";
        case 37: return "mouse_left";
        case 39: return "mouse_right";
        case 13: return "mouse_click";
        default: return {};
        }
    }
    return {};
}

QJsonObject KeyRouter::modeAction(const QString &name) const {
    return modeActions().value(name);
}

void KeyRouter::setMode(const QString &mode) {
    if (mode_.isEmpty()) {
        mode_ = mode;
        modeLast_ = nowSeconds();
        MessageBeep(MB_OK);
        if (emit_) {
            emit_("status", modeLabel(mode),
                  profile_ ? profile_->name : QString(),
                  modeLabel(mode) + QStringLiteral(" 开（单击返回/菜单退出）"), 0);
        }
    }
}

void KeyRouter::exitMode() {
    if (mode_.isEmpty()) {
        return;
    }
    const QString label = modeLabel(mode_);
    mode_.clear();
    MessageBeep(MB_OK);
    if (emit_) {
        emit_("status", label, profile_ ? profile_->name : QString(),
              label + QStringLiteral(" 关"), 0);
    }
}

void KeyRouter::reset() {
    active_.clear();
    tapPending_.clear();
    deferred_.clear();
    held_.clear();
    mode_.clear();
    navMode_ = false;
    modeLast_ = 0;
    modeEnterVk_ = 0;
}

void KeyRouter::keyDown(int vk, double now) {
    if (now < 0) {
        now = nowSeconds();
    }
    if (active_.contains(vk)) {
        return;
    }
    const auto profile = profile_;
    if (!profile) {
        return;
    }
    const QString pname = profile->name;
    const QJsonObject actions = profile->actions;

    if (!mode_.isEmpty()) {
        if (isModeTrigger(vk)) {
            if (vk == modeEnterVk_) {
                return;
            }
            exitMode();
            return;
        }
        if (mode_ == "cursor") {
            const QString name = modeKey(vk);
            const QJsonObject action = modeAction(name);
            if (!name.isEmpty() && !action.isEmpty()) {
                modeLast_ = now;
                begin(vk, now, name, action, profile, false);
                return;
            }
        } else {
            const QString name = modeKey(vk);
            const QJsonObject action = modeAction(name);
            if (!name.isEmpty() && !action.isEmpty()) {
                modeLast_ = now;
                begin(vk, now, name, action, profile, false);
                return;
            }
        }
    }

    if (navMode_) {
        const QJsonObject nav = profile->nav;
        const QJsonObject navKeys = nav.value("keys").toObject();
        QString name = navKeys.value(QString::number(vk)).toString();
        if (QString::number(vk) == nav.value("exit_key").toString() &&
            vk != modeEnterVk_) {
            name = "nav_exit";
        }
        if (!name.isEmpty()) {
            const QJsonObject action = actions.value(name).toObject();
            if (!action.isEmpty()) {
                modeLast_ = now;
                begin(vk, now, name, action, profile, false);
                return;
            }
        }
    }

    QString sem = profile->keys.value(QString::number(vk)).toString();
    QJsonObject action = actions.value(sem).toObject();
    if (action.isEmpty() && isModeTrigger(vk)) {
        action = QJsonObject{{"type", "none"},
                             {"note", QStringLiteral("直通（不拦截）")}};
        sem = QStringLiteral("mode_") + triggerName(vk);
    }

    const QString doubleSem =
        profile->doubleMap.value(QString::number(vk)).toString();
    const bool doubleOff = doubleSem == "none";
    const QJsonObject doubleAction =
        doubleSem.isEmpty() || doubleOff ? QJsonObject{}
                                         : actions.value(doubleSem).toObject();
    const bool tapBuiltin = isTapTarget(vk) && !doubleOff;
    const bool needsTap = !doubleAction.isEmpty() || tapBuiltin;

    if (sem.isEmpty() || action.isEmpty()) {
        if (doubleAction.isEmpty()) {
            return;
        }
        action = QJsonObject{{"type", "none"},
                             {"note", QStringLiteral("直通（不拦截）")}};
        sem = QStringLiteral("double_wait_%1").arg(vk);
    }

    if (needsTap) {
        const auto pending = tapPending_.constFind(vk);
        if (pending != tapPending_.constEnd() &&
            now - pending->last <= kDoubleWindow) {
            tapPending_.remove(vk);
            active_.remove(vk);
            held_.remove(vk);
            deferred_.remove(vk);
            if (!doubleAction.isEmpty()) {
                if (doubleAction.value("type").toString() == "mode" ||
                    doubleAction.value("type").toString() == "nav_toggle") {
                    modeEnterVk_ = vk;
                }
                dispatch(pname, doubleSem, doubleAction, vk);
            } else {
                modeEnterVk_ = vk;
                const QString target = triggerName(vk);
                if (target == "nav") {
                    dispatch(pname, "nav_toggle",
                             QJsonObject{{"type", "nav_toggle"},
                                         {"label", QStringLiteral("导航模式")}},
                             vk);
                } else {
                    dispatch(pname, QStringLiteral("mode_") + target,
                             QJsonObject{{"type", "mode"}, {"mode", target},
                                         {"label", modeLabel(target)}},
                             vk);
                }
            }
            return;
        }

        const bool holdSpec = profile->hold.contains(QString::number(vk));
        if (holdSpec) {
            action = withHold(*profile, vk, action);
        }
        State st;
        st.started = now;
        st.vk = vk;
        st.action = action;
        st.name = sem;
        st.profile = pname;
        st.holdAction = action.value("hold_action").toObject();
        st.tapHold = holdSpec || !isTapTarget(vk);
        active_[vk] = st;
        tapPending_[vk] = Tap{now + kTapWindow, now, st};
        return;
    }
    begin(vk, now, sem, action, profile);
}

QJsonObject KeyRouter::withHold(const Profile &profile, int vk,
                                const QJsonObject &action) const {
    const QString spec = profile.hold.value(QString::number(vk)).toString();
    if (spec.isEmpty()) {
        return action;
    }
    QJsonObject result = action;
    if (spec == "none") {
        for (const char *key : {"hold_action", "hold_repeat", "hold_accel",
                                "hold_defer"}) {
            result.remove(QString::fromLatin1(key));
        }
        return result;
    }
    if (spec == "repeat" || spec == "repeat_accel") {
        result.remove("hold_action");
        result.remove("hold_defer");
        result["hold_repeat"] = action.value("hold_repeat").toInt(100);
        result["hold_accel"] = spec == "repeat_accel";
        return result;
    }
    const QJsonObject hold = profile.actions.value(spec).toObject();
    if (hold.isEmpty()) {
        return action;
    }
    result["hold_action"] = hold;
    result["hold_ms"] = action.value("hold_ms").toInt(600);
    result.remove("hold_repeat");
    result.remove("hold_accel");
    result.remove("hold_defer");
    return result;
}

void KeyRouter::begin(int vk, double now, const QString &name,
                      const QJsonObject &action,
                      const std::shared_ptr<const Profile> &profile,
                      bool applyHold) {
    QJsonObject effective = action;
    if (applyHold) {
        effective = withHold(*profile, vk, action);
    }
    State st;
    st.started = now;
    st.vk = vk;
    st.action = effective;
    st.name = name;
    st.profile = profile->name;
    st.holdAction = effective.value("hold_action").toObject();
    active_[vk] = st;

    if (!st.holdAction.isEmpty() && effective.value("hold_defer").toBool()) {
        active_[vk].deferred = true;
        deferred_[vk] = Deferred{
            st.started + effective.value("hold_ms").toInt(500) / 1000.0,
            active_.value(vk)};
        return;
    }
    dispatch(st.profile, name, effective, vk);
    active_[vk].fired = "base";
    active_[vk].count = 1;
    queueHold(vk, st.started, active_[vk]);
}

void KeyRouter::queueHold(int vk, double now, State &st) {
    const QJsonObject action = st.action;
    const QJsonObject holdAction = st.holdAction;
    if (!holdAction.isEmpty()) {
        const double holdMs = action.value("hold_ms").toInt(600) / 1000.0;
        held_[vk] = Held{std::max(now, st.started + holdMs), 0, true, false, st};
        return;
    }
    const int repeatMs = action.value("hold_repeat").toInt(0);
    if (repeatMs > 0) {
        const double interval = repeatMs / 1000.0;
        held_[vk] = Held{std::max(now + interval, st.started + interval),
                         interval, false, action.value("hold_accel").toBool(), st};
    }
}

void KeyRouter::keyUp(int vk, double now) {
    if (now < 0) {
        now = nowSeconds();
    }
    if (vk == modeEnterVk_) {
        modeEnterVk_ = 0;
    }
    if (tapPending_.contains(vk)) {
        active_.remove(vk);
        held_.remove(vk);
        deferred_.remove(vk);
        return;
    }
    State st = active_.take(vk);
    held_.remove(vk);
    Deferred d;
    const bool hasDeferred = deferred_.contains(vk);
    if (hasDeferred) {
        d = deferred_.take(vk);
        st = d.st;
        dispatch(st.profile, st.name, st.action, vk);
        st.fired = "base";
        st.count += 1;
    }
}

void KeyRouter::tick(double now) {
    if (now < 0) {
        now = nowSeconds();
    }
    if (!mode_.isEmpty() && now - modeLast_ > kModeIdle) {
        exitMode();
    }

    for (auto it = tapPending_.begin(); it != tapPending_.end();) {
        if (now < it->due) {
            ++it;
            continue;
        }
        const int vk = it.key();
        State st = it->st;
        it = tapPending_.erase(it);
        dispatch(st.profile, st.name, st.action, vk);
        if (active_.contains(vk)) {
            active_[vk].fired = "base";
            active_[vk].count = 1;
            if (st.tapHold) {
                queueHold(vk, now, active_[vk]);
            }
        }
    }

    for (auto it = deferred_.begin(); it != deferred_.end();) {
        if (now < it->due) {
            ++it;
            continue;
        }
        const int vk = it.key();
        State st = it->st;
        const QJsonObject holdAction = st.holdAction;
        it = deferred_.erase(it);
        dispatch(st.profile, st.name, holdAction, vk);
        if (active_.contains(vk)) {
            active_[vk].fired = "hold";
            active_[vk].count += 1;
        }
    }

    for (auto it = held_.begin(); it != held_.end();) {
        State &st = it->st;
        if (now - st.started > kMaxHold) {
            it = held_.erase(it);
            continue;
        }
        if (now < it->due) {
            ++it;
            continue;
        }
        if (it->once) {
            const QJsonObject holdAction = st.holdAction;
            const QString profileName = st.profile;
            const QString name = st.name;
            it = held_.erase(it);
            dispatch(profileName, name, holdAction, st.vk);
            if (active_.contains(st.vk)) {
                active_[st.vk].fired = "hold";
                active_[st.vk].count += 1;
            }
            continue;
        }
        int n = 0;
        while (now >= it->due && n < 3) {
            dispatch(st.profile, st.name, st.action, st.vk);
            if (active_.contains(st.vk)) {
                active_[st.vk].count += 1;
            }
            ++n;
            if (it->accel) {
                it->interval = std::max(0.045, it->interval * 0.86);
            }
            it->due += it->interval;
        }
        ++it;
    }

    for (auto it = active_.begin(); it != active_.end();) {
        if (now - it->started > kMaxHold) {
            it = active_.erase(it);
            continue;
        }
        ++it;
    }
}

void KeyRouter::dispatch(const QString &profileName, const QString &name,
                         const QJsonObject &action, int vk) {
    const QString kind = action.value("type").toString();
    QString detail = Presets::describeAction(action, false);
    try {
        if (kind == "key") {
            QStringList mods;
            for (const auto &m : action.value("mods").toArray()) {
                mods << m.toString();
            }
            injector_->key(mods, action.value("vk").toInt(),
                           action.value("repeat").toInt(1));
        } else if (kind == "scroll") {
            const int reps = std::max(1, action.value("repeat").toInt(1));
            for (int i = 0; i < reps; ++i) {
                injector_->scroll(action.value("delta").toInt());
                if (i + 1 < reps) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        } else if (kind == "run") {
            injector_->run(action.value("cmd").toString());
        } else if (kind == "nav_toggle") {
            navMode_ = !navMode_;
            MessageBeep(MB_OK);
            if (emit_) {
                emit_("status", QStringLiteral("导航模式"), profileName,
                      navMode_ ? QStringLiteral("导航模式 开")
                               : QStringLiteral("导航模式 关"),
                      0);
            }
            return;
        } else if (kind == "nav_exit") {
            navMode_ = false;
            if (emit_) {
                emit_("status", QStringLiteral("导航模式"), profileName,
                      QStringLiteral("导航模式 关"), 0);
            }
            return;
        } else if (kind == "mode") {
            const QString mode = action.value("mode").toString();
            if (mode_ == mode) {
                exitMode();
            } else {
                exitMode();
                if (mode == "media" || mode == "cursor") {
                    mode_ = mode;
                    modeLast_ = nowSeconds();
                    MessageBeep(MB_OK);
                    if (emit_) {
                        emit_("status", modeLabel(mode), profileName,
                              modeLabel(mode) +
                                  QStringLiteral(" 开（单击返回/菜单退出）"),
                              0);
                    }
                }
            }
            return;
        } else if (kind == "brightness") {
            injector_->brightness(action.value("delta").toInt(10));
        } else if (kind == "mouse") {
            injector_->mouseMove(action.value("dx").toInt(),
                                 action.value("dy").toInt());
        } else if (kind == "click") {
            injector_->mouseClick();
        } else if (kind == "none") {
            // 默认叠加模式下什么都不做；原生键自己会到前台。
        } else {
            return;
        }
    } catch (...) {
        if (emit_) {
            emit_("error", name, profileName,
                  QStringLiteral("动作执行失败"), vk);
        }
        return;
    }
    if (emit_) {
        emit_("key", name, profileName, detail, vk);
    }
}
