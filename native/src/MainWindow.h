#pragma once

#include <QMainWindow>

#include <memory>

#include "core/Profiles.h"

class QCloseEvent;
class QSystemTrayIcon;
class KeymapPage;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(bool withTray, const QString &pin = QString(),
                        QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QWidget *buildHeader();
    QWidget *buildRemoteCard();
    QWidget *buildVoiceCard();
    QWidget *buildLogPage();
    QWidget *buildGuidePage();
    void buildTray();
    void showMilestones();

    std::unique_ptr<ProfileStore> store_;
    KeymapPage *keymapPage_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    bool quitting_ = false;
};
