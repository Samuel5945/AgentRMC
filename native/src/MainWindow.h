#pragma once

#include <QByteArray>
#include <QMainWindow>
#include <QSet>

#include <memory>

#include "core/Profiles.h"

class QCloseEvent;
class QAction;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QSystemTrayIcon;
class InputEngine;
class KeymapPage;
class GuideDialog;
class RemotePanel;
class Toast;
class VoiceController;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(bool withTray, const QString &pin = QString(),
                        bool autoListen = true, QWidget *parent = nullptr);

    // 启动时的可见性：手动启动开窗，首次运行再弹引导；--tray（开机自启）只留托盘。
    void showOnStartup(bool startHidden);
    // 把主界面叫回来（托盘点击、第二次启动、托盘菜单）。
    void wake();

protected:
    void closeEvent(QCloseEvent *event) override;
    // 捕获「第二次启动」广播的自定义消息，把窗口叫回来。
    bool nativeEvent(const QByteArray &eventType, void *message,
                     qintptr *result) override;
    // 模型包直接拖进窗口就能装（窗口级拦截，落在哪个子控件上都行）。
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QWidget *buildHeader();
    QWidget *buildRemoteCard();
    QWidget *buildVoiceCard();
    QWidget *buildLogPage();
    QWidget *buildGuidePage();
    void buildTray();
    void importDroppedModel(const QString &file);
    void showDropHint(bool on);
    void updateTrayState();
    bool autoStartEnabled() const;
    void setAutoStart(bool on);
    void showGuide();
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
    Toast *toast_ = nullptr;
    QLabel *remoteBadge_ = nullptr;
    QLabel *remoteDot_ = nullptr;
    QLabel *remoteState_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    GuideDialog *guide_ = nullptr;
    QLabel *dropHint_ = nullptr;
    QAction *showAction_ = nullptr;
    QAction *listenAction_ = nullptr;
    QAction *autoStartAction_ = nullptr;
    QString sceneName_;
    bool trayHintShown_ = false;
    bool quitting_ = false;
};
