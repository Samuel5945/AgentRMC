#pragma once

#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSize>
#include <QTimer>

class QCloseEvent;
class QEvent;
class QMouseEvent;
class QResizeEvent;
class QShowEvent;
class DispatchPicker;
class ProfileStore;
class Toast;
class VoiceController;
class WaveformButton;

// 语音主界面：无遥控器的简洁窗口。大麦克风按钮一键开始/结束，
// 转写结果大字展示 + 最近历史（点击复制），底部引擎切换与模型设置。
class VoiceWindow final : public QWidget {
    Q_OBJECT

public:
    // embedded = 嵌进主窗左栏（无遥控器/手动切换时的「语音主界面」页）：
    // 不再是顶层窗口，✕/切换按钮改发 embedExitRequested 交回主窗处理。
    explicit VoiceWindow(VoiceController *voice, ProfileStore *store,
                         QWidget *parent = nullptr, bool embedded = false);

    // 跟随引擎监听状态；没在监听时麦克风按钮给出提示而不是空转。
    void setListeningUi(bool listening);

signals:
    // 打字对话：与说话同路由（转写展示 + 桌宠派活/对话判断）。
    void typedSubmitted(const QString &text);
    // 嵌入页的「遥控器」/✕：主窗把左栏切回遥控器卡。
    void embedExitRequested();
    // 用户点了「完整界面」：主窗负责把自己 wake 回来。
    void fullUiRequested();
    // 面板被收起（✕）：主窗负责把桌宠叫出来。
    void panelHidden();
    // 「朗读最近」：交给桌宠的 TTS 念，面板自己不持有 TTS。
    void speakRequested(const QString &text);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

public:
    // 桌宠收起时对话回复的落点：转写框显示 + 进「最近」历史。
    void showChatReply(const QString &text);
    // 派活模式被消费（一次性新任务交付后）时由主窗调用，下拉回到默认项。
    void refreshDispatchMode();

private:
    void copyText(const QString &text);
    void pushHistory(const QString &text, int costMs);
    // 转写框：全文另存，显示的那段按框宽折行、超出行数上限折成省略号，
    // 高度按当前字体现量（layoutTranscript），所以点击复制拿到的是全文。
    void setTranscript(const QString &text);
    void layoutTranscript();
    void refreshHint();
    void refreshHistoryEmpty();
    void refreshStatusDot();
    void submitTyped();

    VoiceController *voice_ = nullptr;
    ProfileStore *store_ = nullptr;
    Toast *toast_ = nullptr;

    QLabel *statusDot_ = nullptr;
    WaveformButton *mic_ = nullptr;
    QPushButton *testBtn_ = nullptr;
    QLabel *stateLabel_ = nullptr;
    QLabel *hintLabel_ = nullptr;
    QLabel *transcript_ = nullptr;
    QListWidget *history_ = nullptr;
    QLabel *historyEmpty_ = nullptr;
    QComboBox *backend_ = nullptr;
    DispatchPicker *picker_ = nullptr;
    QLineEdit *typed_ = nullptr;
    QPushButton *readLast_ = nullptr;
    QTimer *levelTimer_ = nullptr;
    QString lastText_;
    // 转写框的全文（显示的那段可能被折成省略号，复制/悬浮提示用这份）。
    QString transcriptFull_;

    bool listening_ = false;
    bool recording_ = false;
    bool embedded_ = false;
    // 操作说明只给首次使用的人看；用过一次收掉，悬浮提示兜底。
    bool hintSeen_ = false;

    // 主题面板背景的缩放缓存（4K 源图每帧现缩太贵）：按 (路径, 窗口尺寸)
    // 判失效，换主题换路径、拉窗口换尺寸才重算。
    QImage artScaled_;
    QString artScaledPath_;
    QSize artScaledSize_;
};
