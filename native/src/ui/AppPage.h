#pragma once

#include <QFrame>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

class QLabel;
class QLayout;
class ProfileStore;
class ToggleSwitch;

class AppPage final : public QFrame {
    Q_OBJECT

public:
    explicit AppPage(ProfileStore *store, QWidget *parent = nullptr);

    // WebView 外壳：显示就绪状态 + 动作入口。添加/移除仍弹 Qt 原生对话框，
    // 网页只把点击送回来。
    QJsonObject webState();
    void webLaunch(const QString &id);
    void webAddCustom();
    void webRemoveCustom(const QString &name, const QString &path);
    void webRescan();
    void webSetVoiceLaunch(bool on);
    void webSetSystemControl(bool on);

signals:
    void launched(const QString &name, bool ok, const QString &error);

private:
    void rebuild();
    // 弹「名称 / 路径 / 别名」小对话框，存进 ui.custom_apps 并立即生效。
    void addCustomApp();
    void removeCustomApp(const QString &name, const QString &path);
    void persistCustomApps(const QJsonArray &apps);

    ProfileStore *store_ = nullptr;
    QLayout *rows_ = nullptr;
    QLabel *summary_ = nullptr;
    ToggleSwitch *voiceSwitch_ = nullptr;
    ToggleSwitch *systemSwitch_ = nullptr;
    // app id -> 图标。真实图标只解析一次；首帧解析不到时画占位（不缓存）。
    QHash<QString, QIcon> iconCache_;
    // app id -> 图标 data URL，给 WebView 外壳当 <img src>；rebuild 时刷新。
    QHash<QString, QString> iconUrls_;
};
