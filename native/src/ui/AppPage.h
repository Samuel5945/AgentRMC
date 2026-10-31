#pragma once

#include <QFrame>

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

    ProfileStore *store_ = nullptr;
    QVBoxLayout *rows_ = nullptr;
    QLabel *summary_ = nullptr;
    ToggleSwitch *voiceSwitch_ = nullptr;
};
