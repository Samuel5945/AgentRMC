#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QSet>

#include <memory>

#include "core/Profiles.h"
#include "core/WorkItems.h"

class QCloseEvent;
class QAction;
class QComboBox;
class QDragEnterEvent;
struct UpdateInfo;
class QDropEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSystemTrayIcon;
class QTabWidget;
class QTableWidget;
class QTimer;
class QWidget;
class AgentBridge;
class InputEngine;
class AppPage;
class KeymapPage;
class GuideDialog;
class AgentBridgeDialog;
class RemotePanel;
class Toast;
class VoiceController;
class VoicePet;
class VoiceWindow;

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
    QWidget *buildWorkPage();
    QWidget *buildLogPage();
    QWidget *buildSystemPage();
    QWidget *buildGuidePage();
    void refreshWorkItems();
    void raiseSelectedWork();
    void ackSelectedWork();
    void raiseWorkItem(const QString &id);
    QString selectedWorkId() const;
    QJsonArray workersArray() const;
    QString workersJson() const;
    QString healthJson() const;
    void refreshHealth();
    void refreshSystemPage();
    void watchdogTick();
    int spoolPendingCount() const;
    int importSpool();
    void handleNotify(const QString &kind, const QString &title,
                      const QString &message, const QString &raise,
                      const QString &sessionId, const QString &project,
                      const QString &eventId, const QString &pluginVersion,
                      const QString &speech, bool replay);
    // POST /work-items/update 的处理：宿主 Agent 用 MCP 工具 pet_work_update
    // 主动汇报 progress / blocked / ready。progress 只落工单；blocked / ready
    // 是要人动手的，走和 hook 同一条提醒通道（气泡 + 语音）。
    QJsonObject reportWorkItem(const QJsonObject &payload);
    // 只喊人、不写工单：事件流一行 + 记宿主 + 气泡/语音/抢前台。
    void announceWork(const QString &kind, const QString &title,
                      const QString &message, const QString &spoken,
                      const QString &raise, const QString &sessionId);
    // 联动插件版本对账：{"bundled":内置版,"hosts":[每家 name/installed/loaded/
    // enabled/running/state],"bridges":[不走插件包的外部接入]}。loaded 是 hook
    // 报上来的「此刻真在跑的那版」，注册表只能说盘上装成哪版。需要动手的成因
    // 写进 problems，别压成一个词。
    QJsonObject pluginHealth(QStringList *problems) const;
    // 不走插件包的外部桥（OpenSquilla）：没有注册表可对账，只报装没装、进程在不在。
    // 别把它们塞进 Host 列表——那会让「读不出注册表」变成常驻故障。
    QJsonArray pluginBridges() const;
    void upgradePlugins();
    // 5 秒一拍：发现某家的已装版本在运行期间变过，就挂「等新版上报」标记。
    void trackPluginTransitions();
    void buildTray();
    void checkUpdates(bool manual);
    void onUpdateChecked(const UpdateInfo &info, bool manual);
    void showUpdateDialog(const UpdateInfo &info);
    void importDroppedModel(const QString &file);
    void showDropHint(bool on);
    void updateTrayState();
    bool autoStartEnabled() const;
    void setAutoStart(bool on);
    void showGuide();
    // 一键接入：复制安装话术 + 各家装没装。新手引导的「联动插件」和系统页都走这里。
    void showAgentBridgeDialog();
    // 直接跳到「使用引导 → 接入 AI Agent」那一节（弹窗里的「详细说明」）。
    void openPluginGuide();
    void toggleListening();
    void updateListenUi();
    void registerVoiceHotkey();
    void unregisterVoiceHotkey();
    void showVoicePanel();
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
    void openThemeDialog();
    void onVoiceState(const QString &state, const QString &detail);
    void onVoiceTranscribed(const QString &text, int costMs);
    void setVoiceStatus(const QString &detail, const QString &color,
                        bool showTranscript);

    std::unique_ptr<ProfileStore> store_;
    std::unique_ptr<WorkItemStore> workItems_;
    // pet_work_update 的播报节流：Agent 可能连着报几次 blocked，别喊成一串。
    // key = session_id（没有就用宿主名）；值 = 上次真的喊出去的时间。
    QHash<QString, QDateTime> attentionAnnounced_;
    // Agent 已经替 hook 报过 ready 并念过了：紧接着到的 stop 不再念第二遍。
    QHash<QString, QDateTime> readyAnnounced_;
    InputEngine *engine_ = nullptr;
    KeymapPage *keymapPage_ = nullptr;
    AppPage *appPage_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QListWidget *workList_ = nullptr;
    QLabel *workCount_ = nullptr;
    QLabel *chipNeeds_ = nullptr;
    QLabel *chipReview_ = nullptr;
    QLabel *chipWorking_ = nullptr;
    QWidget *workEmptyBox_ = nullptr;
    QLabel *workEmpty_ = nullptr;
    QLabel *workHint_ = nullptr;
    QComboBox *workFilter_ = nullptr;
    QLabel *workHealth_ = nullptr;
    QPushButton *workRaise_ = nullptr;
    QPushButton *workDone_ = nullptr;
    QLabel *sysBridge_ = nullptr;
    QLabel *sysUptime_ = nullptr;
    QLabel *sysSpool_ = nullptr;
    QLabel *sysLast_ = nullptr;
    QLabel *sysProblems_ = nullptr;
    QTableWidget *sysWorkers_ = nullptr;
    QLabel *sysPlugin_ = nullptr;
    QPushButton *pluginUpgrade_ = nullptr;
    // 「使用引导」页里的「接入 AI Agent」那一节：新手引导点了「联动插件」直接滚过去。
    QScrollArea *guideScroll_ = nullptr;
    QWidget *guideAgentSection_ = nullptr;
    QLabel *statusDot_ = nullptr;
    QLabel *statusText_ = nullptr;
    QPushButton *listenButton_ = nullptr;
    QPushButton *updateButton_ = nullptr;
    QListWidget *logList_ = nullptr;
    QLabel *logCount_ = nullptr;
    QLabel *logEmpty_ = nullptr;
    VoiceController *voice_ = nullptr;
    QComboBox *voiceBackend_ = nullptr;
    QLabel *voiceState_ = nullptr;
    QLabel *voiceFoot_ = nullptr;
    QPushButton *voiceTest_ = nullptr;
    // 宿主署名 → 它最近一次报上来的插件版本，也就是此刻真在跑的那一版。
    // 注册表只能说盘上装成哪版：宿主不重启，盘上是新版、内存里还是旧代码。
    QHash<QString, QString> pluginLoaded_;
    // 宿主署名 → 最近一次收到它 hook 的时刻。宿主开着却一条都没报过，和「报了旧版」
    // 是两种坏法：官方安装器会删掉上一版缓存目录，已开着的会话里 hook 路径直接失效，
    // 提醒是静默断掉而不是念出旧版本号，只有这个时间戳能把它分出来。
    QHash<QString, QDateTime> pluginLastReport_;
    // 注册表版本在运行期间换过、但这家还没用新版报过 hook → 挂起标记。
    // 收到任何一条 hook/MCP 上报就清掉，不靠超时猜。
    QHash<QString, bool> pluginAwaitingReport_;
    QHash<QString, QString> pluginSeenInstalled_;
    QString lastTranscript_;
    RemotePanel *remotePanel_ = nullptr;
    Toast *toast_ = nullptr;
    QLabel *remoteBadge_ = nullptr;
    QLabel *remoteDot_ = nullptr;
    QLabel *remoteState_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    AgentBridge *bridge_ = nullptr;
    bool bridgeRequested_ = false;
    QTimer *healthTimer_ = nullptr;
    QString spoolPath_;
    QDateTime startedAt_;
    QDateTime lastEventAt_;
    GuideDialog *guide_ = nullptr;
    AgentBridgeDialog *agentDialog_ = nullptr;
    VoiceWindow *voiceWindow_ = nullptr;
    VoicePet *voicePet_ = nullptr;
    QLabel *dropHint_ = nullptr;
    QAction *showAction_ = nullptr;
    QAction *listenAction_ = nullptr;
    QAction *petAction_ = nullptr;
    QAction *autoStartAction_ = nullptr;
    QString sceneName_;
    bool trayHintShown_ = false;
    bool quitting_ = false;
    bool updating_ = false;
    bool voiceHotkeyRegistered_ = false;
};
