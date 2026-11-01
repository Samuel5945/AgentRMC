#pragma once

#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QVector>

struct AppEntry {
    QString id;
    QString name;
    QStringList aliases;
    QStringList shortcuts;
    QString aumid;
    QString customPath;  // 自定义应用的可执行/快捷方式/URL；空 = 内置表条目
};

struct AppTarget {
    bool found = false;
    QString kind;     // url / shortcut / aumid
    QString target;
    QString detail;   // 给人看的来源说明
};

struct AppVoiceMatch {
    bool matched = false;    // 这句话是「打开某应用」的指令
    bool resolved = false;   // 找到了能启动的东西
    bool builtin = false;    // 命中内置应用表
    QString appId;
    QString name;
    AppTarget target;
};

struct AppLaunchResult {
    bool matched = false;
    bool ok = false;
    QString appId;
    QString name;
    QString error;
};

class AppLauncher {
public:
    static const QVector<AppEntry> &catalog();
    static QStringList appIds();
    static const AppEntry *findApp(const QString &id);
    static QString nameOf(const QString &id);

    // 自定义应用（ui.custom_apps）：MainWindow 启动时、应用页增删后调用。
    // 目录 = 内置表 + 自定义，keymap 的应用动作下拉也会跟着带上。
    static void setCustomApps(const QVector<AppEntry> &apps);
    static QVector<AppEntry> parseCustomApps(const QJsonArray &raw);

    static AppTarget resolve(const QString &id);
    static AppTarget resolve(const AppEntry &entry);
    static bool launch(const QString &id, QString *error = nullptr);

    static AppVoiceMatch matchVoice(const QString &text);
    static AppLaunchResult runVoice(const QString &text);

    // 开始菜单是运行时扫描的，装/删应用后用它重新扫一遍。
    static void refreshCache();

    static QString normalize(const QString &text);
};
