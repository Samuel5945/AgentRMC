#pragma once

#include <QJsonArray>
#include <QPoint>
#include <QString>
#include <QWidget>

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

    // 气泡反馈 + TTS 出声（接小米 MiMo TTS，key 用户自填；没 key 只有气泡）。
    void speak(const QString &text, int durationMs = 0);

    // 桌宠对话闭环（ui.pet_chat 持久化）：开着时转写不再粘贴，改喂 LLM，
    // 回复用 TTS 念出来；收起桌宠自动退出对话。
    bool chatMode() const { return chatMode_; }
    void setChatMode(bool on);

signals:
    void fullUiRequested();
    void panelRequested();
    void listenToggleRequested();
    void hiddenByUser();

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
    void bubbleOnly(const QString &text, int durationMs);
    void speakAs(const QString &text);  // 对话回复朗读：tts.chat_voice 独立音色
    void askLlm(const QString &text);
    void openLlmSettings();
    void importThemePackage();
    void tick();
    QString chatPersona() const;  // llm_api.system → 主题人设 → 内置默认
    QString spriteState() const;  // 当前应显示的精灵状态名

    ProfileStore *store_ = nullptr;
    VoiceController *voice_ = nullptr;
    Card *card_ = nullptr;
    QLabel *bubble_ = nullptr;
    TtsClient *tts_ = nullptr;
    LlmClient *llm_ = nullptr;
    PetSprite *sprite_ = nullptr;  // 主题没精灵时为空帧，画圆球回落
    QString lastText_;  // 最近一次转写，右键菜单「朗读最近一句」用
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
