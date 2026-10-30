#pragma once

#include <QMainWindow>
#include <QSet>

#include <memory>

#include "core/Profiles.h"

class QCloseEvent;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSystemTrayIcon;
class InputEngine;
class KeymapPage;
class RemotePanel;
class VoiceController;

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
    void refreshVoiceUi();
    void refreshRemoteCard();
    void applyRemoteMode(const QString &badge, const QSet<int> &keys, bool on);
    void openVoiceSettings();
    void onVoiceState(const QString &state, const QString &detail);
    void onVoiceTranscribed(const QString &text, int costMs);
    void setVoiceStatus(const QString &detail, const QString &color,
                        bool showTranscript);

    std::unique_ptr<ProfileStore> store_;
    InputEngine *engine_ = nullptr;
    KeymapPage *keymapPage_ = nullptr;
    QLabel *statusDot_ = nullptr;
    QLabel *statusText_ = nullptr;
    QPushButton *listenButton_ = nullptr;
    QListWidget *logList_ = nullptr;
    QLabel *logCount_ = nullptr;
    QLabel *logEmpty_ = nullptr;
    VoiceController *voice_ = nullptr;
    QComboBox *voiceBackend_ = nullptr;
    QLabel *voiceState_ = nullptr;
    QLabel *voiceFoot_ = nullptr;
    QPushButton *voiceTest_ = nullptr;
    QString lastTranscript_;
    RemotePanel *remotePanel_ = nullptr;
    QLabel *remoteBadge_ = nullptr;
    QLabel *remoteDot_ = nullptr;
    QLabel *remoteState_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    bool quitting_ = false;
};
