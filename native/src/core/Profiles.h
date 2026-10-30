#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <utility>

struct Profile {
    QString name;
    QJsonObject raw;
    QJsonObject keys;
    QJsonObject actions;
    QJsonObject doubleMap;
    QJsonObject hold;
    QJsonObject nav;
    QJsonObject voice;
    QStringList processMatch;
    QStringList titleMatch;
};

class ProfileStore {
public:
    explicit ProfileStore(QString profilesPath);

    QString path() const { return path_; }
    bool load(QString *error = nullptr);
    bool loadConfig(const QString &path, QString *error = nullptr);

    const QJsonObject &config() const { return config_; }
    bool uiBool(const QString &key, bool fallback = false) const;
    bool setUiBool(const QString &key, bool value);
    QJsonObject voiceConfig() const;
    bool updateVoice(const QJsonObject &patch);
    bool updateMediaVoice(const QJsonObject &patch);
    const QVector<Profile> &profiles() const { return profiles_; }
    const Profile &fallback() const { return fallback_; }
    const Profile *find(const QString &name) const;
    const Profile &current(bool force = false);

    QString pinned() const { return pinned_; }
    void setPinned(const QString &name) { pinned_ = name; }
    bool isPinned() const { return !pinned_.isEmpty(); }
    QStringList names() const;

    bool apply(const QString &profileName, int vk, const QString &gesture,
               const QString &kind,
               const QJsonObject &custom = QJsonObject());

    static void foregroundInfo(QString *exe, QString *title);
    static std::pair<QString, QString> matchSummary(const Profile &profile);
    static QString voiceMode(const QJsonObject &config);
    static QString voiceCombo(const QJsonObject &config, const Profile &profile);

private:
    bool saveConfig(const QJsonObject &root);

    QString path_;
    QString configPath_;
    QJsonObject root_;
    QJsonArray rawProfiles_;
    QVector<Profile> profiles_;
    Profile fallback_;
    QJsonObject config_;
    QString pinned_;
    const Profile *current_ = nullptr;
    qint64 cacheMs_ = 0;
};
