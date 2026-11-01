#pragma once

#include <QFrame>
#include <QJsonArray>

class QLabel;
class QVBoxLayout;
class ProfileStore;
class ToggleSwitch;

class AppPage final : public QFrame {
    Q_OBJECT

public:
    explicit AppPage(ProfileStore *store, QWidget *parent = nullptr);

signals:
    void launched(const QString &name, bool ok, const QString &error);

private:
    void rebuild();
    // 弹「名称 / 路径 / 别名」小对话框，存进 ui.custom_apps 并立即生效。
    void addCustomApp();
    void removeCustomApp(const QString &name, const QString &path);
    void persistCustomApps(const QJsonArray &apps);

    ProfileStore *store_ = nullptr;
    QVBoxLayout *rows_ = nullptr;
    QLabel *summary_ = nullptr;
    ToggleSwitch *voiceSwitch_ = nullptr;
};
