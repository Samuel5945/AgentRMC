#pragma once

#include <QJsonArray>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <functional>

class QContextMenuEvent;
class QEvent;
class QHideEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;
class QTimer;
class LlmClient;
class PetSprite;
class ProfileStore;
class TtsClient;
class VoiceController;
class Theme;

// 语音桌宠：主窗/语音面板关掉之后留在屏幕上的小圆球。
// - 状态一眼可见：未监听灰环 / 监听蓝环呼吸 / 录音绿环随电平加粗 / 转写黄圈旋转
// - 悬停展开卡片：切识别方式、看最近转写（点击复制）、回主界面、开语音面板、贴边、关闭
// - 可拖动；拖到屏幕边缘（或点「贴边」）就缩成一条边条藏着，悬停弹出
// - 单击球体：没监听先开监听，监听中就是开始/结束说话
// - speak() 是给 TTS 预留的口：之后接小米免费 TTS，把合成文本喂进来就冒气泡“说话”
class VoicePet final : public QWidget {
    Q_OBJECT

public:
    explicit VoicePet(ProfileStore *store, VoiceController *voice,
                      QWidget *parent = nullptr);

    // 跟随监听开关（颜色环），录音/转写状态自己听 VoiceController 的信号。
    void setListening(bool listening);

    // 桌宠开关，持久化在 ui.pet_enabled（默认开）；关掉时连气泡一起收起。
    bool petEnabled() const;
    void setPetEnabled(bool on);

    // 主窗关掉进托盘、开机自启只留托盘时叫出桌宠。
    void present();
    void presentIfEnabled();

    // 悬停展开/收起卡片（也供截图模式用）。
    void expandCard();

    // 会话级静音（不落盘）：截图/演示模式不想真发 TTS 请求时用。
    void setVoiceFeedbackMuted(bool muted);

    // TTS 是否就绪（key 已填）；决定 Agent 等确认的通知要不要托盘兜底。
    bool ttsReady() const;

    // 气泡反馈 + TTS 出声（接小米 MiMo TTS，key 用户自填；没 key 只有气泡）。
    // raiseExe 非空时气泡可点击，点了把该进程的窗口拉回前台。
    void speak(const QString &text, int durationMs = 0,
               const QString &raiseExe = QString());

    // Agent 普通进度通知：只冒气泡不出声，别打断在放的东西（等确认走 speak）。
    void notify(const QString &text, const QString &raiseExe = QString());

    // 对话人设里注入当前待办摘要；主窗提供，避免模型编造工具调用去「查任务」。
    void setWorkSummaryProvider(std::function<QString()> provider);

    // 收起当前气泡（点击拉回窗口后调用）。
    void hideBubble();

    // 桌宠状态快照（AgentBridge GET /state 的应答），给 Agent 决策用。
    QString stateJson() const;

    // 桌宠对话闭环（ui.pet_chat 持久化）：开着时转写不再粘贴，改喂 LLM，
    // 回复用 TTS 念出来；收起桌宠自动退出对话。
    bool chatMode() const { return chatMode_; }
    void setChatMode(bool on);

signals:
    void fullUiRequested();
    void panelRequested();
    void listenToggleRequested();
    void hiddenByUser();
    // 对话模式的一条回复（桌宠可见时有气泡，收起时靠它落到面板/事件流）。
    void chatReplied(const QString &text);
    // 用户点了气泡：把这条提醒对应的宿主窗口拉回前台。
    void bubbleActivated(const QString &raiseExe);
    // 语音派活成功：主窗落一条 working Work Item。
    void workHandedOff(const QString &agentExe, const QString &text);
    void voiceFeedbackError(const QString &error);

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    class Card;

    enum class Dock { None, Left, Right, Top, Bottom };

    void ensureCard();
    void ensureBubble();
    void collapseCard();
    void collapseSoon();
    void setBackend(int backend);
    void openPanel();
    void openFullUi();
    void closeFromUi();
    void dockAndPeek();
    void popOut();
    void applyDockPos(bool peek);
    void maybeSnap();
    void refreshState();
    void applyTtsOptions();
    void applyLlmOptions();
    void setTtsEnabled(bool on);
    void bubbleOnly(const QString &text, int durationMs,
                    const QString &raiseExe = QString());
    void speakAs(const QString &text);  // 对话回复朗读：tts.chat_voice 独立音色
    void askLlm(const QString &text);
    // 对话模式的一句话：先按本地规则判断该谁接，再分流给宿主 Agent 或自带 LLM。
    void handleUtterance(const QString &text);
    // 这句话像「让 Agent 干活」而不是闲聊（纯本地规则，零延迟，不调模型）。
    static bool looksLikeTask(const QString &text);
    // 这句话该交给哪个宿主：显式指定 > 说话时前台的那个 > 最近发通知的那个 > 探测。
    // 返回空 = 一个 Agent 都不在。
    QString onlineAgentExe() const;
    // 把话交给宿主 Agent：拉窗口 → 轮询等前台切过去 → 聚焦聊天输入框 → 粘贴 + 回车。
    bool handOffToAgent(const QString &text, const QString &exe);
    // 轮询等目标宿主真正到前台（Electron 切焦点不是瞬时的）；到位后先按
    // 聚焦键（或按名激活「新建任务」）再粘贴。attempts 用完还没到位 →
    // 剪贴板兜底，绝不硬注入别的窗口。
    void handOffPollForeground(const QString &text, const QString &exe,
                               const QString &shortName,
                               const QStringList &focusMods, int focusVk,
                               bool hasFocus, const QStringList &newTaskNames,
                               int attempts);
    // 没有聚焦键的宿主：UIA 把焦点交给「新建任务」→ 回车 → 验焦点落进输入框
    // → 验证粘贴。任一环节不满足就不粘那一步（见 .cpp 里的兜底）。
    void handOffNewTask(const QString &text, const QString &exe,
                        const QString &shortName, const QStringList &names);
    // 两段读回粘贴：粘不带 Enter 读回证明字进输入框 → Enter 读回证明真发送；
    // 证明不了重试一轮再回落剪贴板。WorkBuddy 系唤醒后视图切换会吞键的对策。
    void handOffVerifiedPaste(const QString &text, const QString &exe,
                              const QString &shortName);
    // 交付收尾：气泡 + 记 last_used + 落 working 工单（粘贴路与验证路共用）。
    void handOffDelivered(const QString &text, const QString &exe,
                          const QString &shortName);
    // 前台与输入框都就绪后的最后一粘：再验一次前台，注入失败诚实回落剪贴板。
    void handOffPaste(const QString &text, const QString &exe,
                      const QString &shortName);
    void openLlmSettings();
    void tick();
    QString chatPersona() const;  // llm_api.system → 主题人设 → 内置默认
    QString spriteState() const;  // 当前应显示的精灵状态名

    ProfileStore *store_ = nullptr;
    VoiceController *voice_ = nullptr;
    Card *card_ = nullptr;
    QLabel *bubble_ = nullptr;
    TtsClient *tts_ = nullptr;
    LlmClient *llm_ = nullptr;
    std::function<QString()> workSummaryProvider_;
    PetSprite *sprite_ = nullptr;  // 主题没精灵时为空帧，画圆球回落
    QString lastText_;  // 最近一次转写，右键菜单「朗读最近一句」用
    QString pendingRaise_;  // 当前气泡点了要拉回的宿主进程名，空 = 气泡不可点
    bool ttsErrorShown_ = false;  // TTS 失败提示一次就够，不刷屏

    bool chatMode_ = false;
    bool llmBusy_ = false;
    QJsonArray chatHistory_;  // user/assistant 轮次，system 每次请求现拼

    bool listening_ = false;
    bool recording_ = false;
    bool transcribing_ = false;
    QString detail_;
    double level_ = 0.0;
    int phase_ = 0;  // 呼吸 / 旋转动画相位

    Dock dock_ = Dock::None;
    bool peeked_ = false;  // 贴边后是否已缩成边条
    bool pressMoved_ = false;
    QPoint dragGlobal_;
    QPoint dragOffset_;

    QTimer *animTimer_ = nullptr;
    QTimer *collapseTimer_ = nullptr;
    QTimer *bubbleTimer_ = nullptr;
};
