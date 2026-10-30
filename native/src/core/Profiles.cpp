#include "Profiles.h"

#include "Presets.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

QJsonObject mergeObjects(const QJsonObject &base, const QJsonObject &over) {
    QJsonObject out = base;
    for (auto it = over.begin(); it != over.end(); ++it) {
        out[it.key()] = it.value();
    }
    return out;
}

QStringList stringList(const QJsonArray &arr) {
    QStringList out;
    for (const auto &v : arr) {
        out << v.toString();
    }
    return out;
}

}  // namespace

ProfileStore::ProfileStore(QString profilesPath)
    : path_(std::move(profilesPath)) {}

bool ProfileStore::load(QString *error) {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("打不开 ") + path_;
        }
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (doc.isNull() || !doc.isObject()) {
        if (error) {
            *error = QStringLiteral("profiles.json 解析失败: ") + parse.errorString();
        }
        return false;
    }

    root_ = doc.object();
    rawProfiles_ = root_.value("profiles").toArray();

    QHash<QString, QJsonObject> rawByName;
    for (const auto &value : rawProfiles_) {
        const QJsonObject obj = value.toObject();
        const QString name = obj.value("name").toString();
        if (!name.isEmpty()) {
            rawByName[name] = obj;
        }
    }

    QHash<QString, QJsonObject> cache;
    std::function<QJsonObject(const QJsonObject &, int)> resolve =
        [&](const QJsonObject &rawObject, int depth) -> QJsonObject {
        const QString name = rawObject.value("name").toString();
        if (!name.isEmpty() && cache.contains(name)) {
            return cache.value(name);
        }
        QJsonObject base;
        const QString parentName = rawObject.value("extends").toString();
        if (depth < 4 && !parentName.isEmpty() && parentName != name &&
            rawByName.contains(parentName)) {
            base = resolve(rawByName.value(parentName), depth + 1);
        }
        QJsonObject merged = base;
        for (auto it = rawObject.begin(); it != rawObject.end(); ++it) {
            merged[it.key()] = it.value();
        }
        merged["keys"] = mergeObjects(base.value("keys").toObject(),
                                      rawObject.value("keys").toObject());
        merged["actions"] = mergeObjects(base.value("actions").toObject(),
                                         rawObject.value("actions").toObject());
        merged["double"] = mergeObjects(base.value("double").toObject(),
                                        rawObject.value("double").toObject());
        merged["hold"] = mergeObjects(base.value("hold").toObject(),
                                      rawObject.value("hold").toObject());
        if (base.contains("voice") && rawObject.contains("voice")) {
            merged["voice"] = mergeObjects(base.value("voice").toObject(),
                                           rawObject.value("voice").toObject());
        }
        if (!name.isEmpty()) {
            cache[name] = merged;
        }
        return merged;
    };

    profiles_.clear();
    for (const auto &value : rawProfiles_) {
        const QJsonObject raw = value.toObject();
        Profile profile;
        profile.name = raw.value("name").toString();
        profile.raw = raw;
        const QJsonObject merged = resolve(raw, 0);
        profile.keys = merged.value("keys").toObject();
        profile.actions = merged.value("actions").toObject();
        profile.doubleMap = merged.value("double").toObject();
        profile.hold = merged.value("hold").toObject();
        profile.nav = merged.value("nav").toObject();
        profile.voice = merged.value("voice").toObject();
        const QJsonObject match = merged.value("match").toObject();
        profile.processMatch = stringList(match.value("process").toArray());
        profile.titleMatch = stringList(match.value("title").toArray());
        profiles_.push_back(profile);
    }

    fallback_ = Profile{};
    fallback_.name = QStringLiteral("default");
    for (const auto &profile : profiles_) {
        if (profile.name == QStringLiteral("default")) {
            fallback_ = profile;
            break;
        }
    }
    if (fallback_.keys.isEmpty() && !profiles_.isEmpty()) {
        fallback_ = profiles_.last();
    }

    current_ = nullptr;
    cacheMs_ = 0;
    return true;
}

bool ProfileStore::loadConfig(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("打不开 ") + path;
        }
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (doc.isNull() || !doc.isObject()) {
        if (error) {
            *error = QStringLiteral("config.json 解析失败: ") + parse.errorString();
        }
        return false;
    }
    config_ = doc.object();
    configPath_ = path;
    return true;
}

bool ProfileStore::uiBool(const QString &key, bool fallback) const {
    const QJsonObject ui = config_.value("ui").toObject();
    return ui.contains(key) ? ui.value(key).toBool(fallback) : fallback;
}

bool ProfileStore::setUiBool(const QString &key, bool value) {
    if (configPath_.isEmpty()) {
        configPath_ = QFileInfo(path_).absolutePath() + "/config.json";
    }
    QJsonObject root = config_;
    QJsonObject ui = root.value("ui").toObject();
    ui[key] = value;
    root["ui"] = ui;

    QSaveFile out(configPath_);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        return false;
    }
    config_ = root;
    return true;
}

const Profile *ProfileStore::find(const QString &name) const {
    for (const auto &profile : profiles_) {
        if (profile.name == name) {
            return &profile;
        }
    }
    return nullptr;
}

const Profile &ProfileStore::current(bool force) {
    if (!pinned_.isEmpty()) {
        if (const Profile *pinned = find(pinned_)) {
            return *pinned;
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && current_ != nullptr && now - cacheMs_ < 300) {
        return *current_;
    }

    QString exe;
    QString title;
    foregroundInfo(&exe, &title);
    const QString exeLower = exe.toLower();
    const QString titleLower = title.toLower();

    const Profile *matched = nullptr;
    for (const auto &profile : profiles_) {
        for (const QString &process : profile.processMatch) {
            if (process.compare(exeLower, Qt::CaseInsensitive) == 0) {
                matched = &profile;
                break;
            }
        }
        if (matched) {
            break;
        }
        for (const QString &needle : profile.titleMatch) {
            if (titleLower.contains(needle, Qt::CaseInsensitive)) {
                matched = &profile;
                break;
            }
        }
        if (matched) {
            break;
        }
    }
    current_ = matched ? matched : &fallback_;
    cacheMs_ = now;
    return *current_;
}

QStringList ProfileStore::names() const {
    QStringList out;
    for (const auto &profile : profiles_) {
        if (!profile.name.isEmpty()) {
            out << profile.name;
        }
    }
    return out;
}

bool ProfileStore::apply(const QString &profileName, int vk,
                         const QString &gesture, const QString &kind,
                         const QJsonObject &custom) {
    int index = -1;
    for (int i = 0; i < rawProfiles_.size(); ++i) {
        if (rawProfiles_.at(i).toObject().value("name").toString() ==
            profileName) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return false;
    }

    QJsonObject raw = rawProfiles_.at(index).toObject();
    QJsonObject actions = raw.value("actions").toObject();
    QJsonObject keys = raw.value("keys").toObject();

    auto nativeIfSame = [&](QJsonObject act) {
        const bool plain = act.value("mods").toArray().isEmpty();
        if (act.value("type").toString() == "key" && plain &&
            act.value("vk").toInt(-1) == vk) {
            return QJsonObject{
                {"type", "none"},
                {"label", act.value("label").toString()},
                {"note", QStringLiteral("原生 ") + Presets::nativeLabel(vk)},
            };
        }
        return act;
    };

    if (gesture == "single") {
        QJsonObject action = custom.isEmpty()
                                 ? Presets::actionFor(kind, vk, "single")
                                 : custom;
        if (action.isEmpty()) {
            return false;
        }
        if (!custom.isEmpty()) {
            action = nativeIfSame(action);
        }
        const QString sem = QString("k%1_%2").arg(vk).arg(kind);
        keys[QString::number(vk)] = sem;
        actions[sem] = action;
        raw["keys"] = keys;
        raw["actions"] = actions;
    } else if (gesture == "double") {
        QJsonObject doubleMap = raw.value("double").toObject();
        if (kind == "none") {
            doubleMap[QString::number(vk)] = "none";
        } else {
            QJsonObject action = custom.isEmpty()
                                     ? Presets::actionFor(kind, vk, "double")
                                     : custom;
            if (action.isEmpty()) {
                return false;
            }
            const QString sem = QString("k%1_d_%2").arg(vk).arg(kind);
            doubleMap[QString::number(vk)] = sem;
            actions[sem] = action;
        }
        raw["double"] = doubleMap;
        raw["actions"] = actions;
    } else if (gesture == "hold") {
        QJsonObject hold = raw.value("hold").toObject();
        if (kind == "none" || kind == "repeat" || kind == "repeat_accel") {
            hold[QString::number(vk)] = kind;
        } else {
            QJsonObject action = custom.isEmpty()
                                     ? Presets::actionFor(kind, vk, "hold")
                                     : custom;
            if (action.isEmpty()) {
                return false;
            }
            const QString sem = QString("k%1_h_%2").arg(vk).arg(kind);
            hold[QString::number(vk)] = sem;
            actions[sem] = action;
        }
        raw["hold"] = hold;
        raw["actions"] = actions;
    } else {
        return false;
    }

    rawProfiles_[index] = raw;
    root_["profiles"] = rawProfiles_;

    const QString backup = path_ + ".bak";
    QFile::remove(backup);
    if (QFile::exists(path_)) {
        QFile::copy(path_, backup);
    }

    QSaveFile out(path_);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    out.write(QJsonDocument(root_).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        return false;
    }
    load();
    return true;
}

void ProfileStore::foregroundInfo(QString *exe, QString *title) {
    if (exe) exe->clear();
    if (title) title->clear();
#ifdef Q_OS_WIN
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (handle) {
            wchar_t path[1024];
            DWORD size = 1024;
            if (QueryFullProcessImageNameW(handle, 0, path, &size)) {
                if (exe) {
                    *exe = QFileInfo(
                               QString::fromWCharArray(path, int(size)))
                               .fileName();
                }
            }
            CloseHandle(handle);
        }
    }
    const int len = GetWindowTextLengthW(hwnd);
    if (len > 0 && title) {
        wchar_t buffer[1024];
        const int got = GetWindowTextW(hwnd, buffer, 1024);
        if (got > 0) {
            *title = QString::fromWCharArray(buffer, got);
        }
    }
#endif
}

std::pair<QString, QString> ProfileStore::matchSummary(const Profile &profile) {
    const auto few = [](const QStringList &items) -> QString {
        if (items.isEmpty()) return {};
        if (items.size() == 1) return items.first();
        const QString joined = items.join(QStringLiteral(" / "));
        if (items.size() == 2 && joined.size() <= 26) return joined;
        return QStringLiteral("%1 等 %2 个").arg(items.first()).arg(items.size());
    };

    QStringList bits;
    if (!profile.processMatch.isEmpty()) {
        bits << few(profile.processMatch);
    } else if (!profile.titleMatch.isEmpty()) {
        bits << QStringLiteral("标题 ") + few(profile.titleMatch);
    }
    QString shortText = bits.join(QStringLiteral(" · "));
    if (shortText.isEmpty()) {
        shortText = QStringLiteral("兜底 · 无匹配条件");
    }

    QStringList full;
    if (!profile.processMatch.isEmpty()) {
        full << QStringLiteral("进程：") +
                    profile.processMatch.join(QStringLiteral(" / "));
    }
    if (!profile.titleMatch.isEmpty()) {
        full << QStringLiteral("标题：") +
                    profile.titleMatch.join(QStringLiteral(" / "));
    }
    const QString fullText = full.isEmpty()
                                 ? QStringLiteral("没有匹配条件，作为兜底场景")
                                 : full.join('\n');
    return {shortText, fullText};
}

QString ProfileStore::voiceMode(const QJsonObject &config) {
    const QString mode =
        config.value("voice").toObject().value("mode").toString();
    return mode.isEmpty() ? QStringLiteral("hotkey") : mode.toLower();
}

QString ProfileStore::voiceCombo(const QJsonObject &config,
                                 const Profile &profile) {
    QJsonObject hotkey =
        config.value("voice").toObject().value("hotkey").toObject();
    if (hotkey.isEmpty()) {
        hotkey = QJsonObject{{"vk", 0x56},
                             {"mods", QJsonArray{"ctrl", "shift"}}};
    }
    const QJsonObject override =
        profile.voice.value("hotkey").toObject();
    for (auto it = override.begin(); it != override.end(); ++it) {
        hotkey[it.key()] = it.value();
    }
    QStringList mods;
    for (const auto &m : hotkey.value("mods").toArray()) {
        mods << m.toString().toLower();
    }
    return Presets::comboText(mods, hotkey.value("vk").toInt(0x56));
}
