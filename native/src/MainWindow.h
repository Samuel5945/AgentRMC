#pragma once

#include <QMainWindow>

#include <memory>

#include "core/Profiles.h"

class QCloseEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QSystemTrayIcon;
class InputEngine;
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
    void toggleListening();
    void updateListenUi();
    void onEngineEvent(const QString &kind, const QString &name,
                       const QString &profile, const QString &detail, int vk);
    void onEngineRunning(bool running, const QString &message);
    void appendEvent(const QString &kind, const QString &name,
                     const QString &profile, const QString &detail, int vk);
    void clearLog();

    std::unique_ptr<ProfileStore> store_;
    InputEngine *engine_ = nullptr;
    KeymapPage *keymapPage_ = nullptr;
    QLabel *statusDot_ = nullptr;
    QLabel *statusText_ = nullptr;
    QPushButton *listenButton_ = nullptr;
    QListWidget *logList_ = nullptr;
    QLabel *logCount_ = nullptr;
    QLabel *logEmpty_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    bool quitting_ = false;
};
