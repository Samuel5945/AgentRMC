#pragma once

#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>

class QCloseEvent;
class QEvent;
class QMouseEvent;
class ProfileStore;
class Toast;
class VoiceController;

// 语音主界面：无遥控器的简洁窗口。大麦克风按钮一键开始/结束，
// 转写结果大字展示 + 最近历史（点击复制），底部引擎切换与模型设置。
class VoiceWindow final : public QWidget {
    Q_OBJECT

public:
    explicit VoiceWindow(VoiceController *voice, ProfileStore *store,
                         QWidget *parent = nullptr);

    // 跟随引擎监听状态；没在监听时麦克风按钮给出提示而不是空转。
    void setListeningUi(bool listening);

signals:
    // 用户点了「完整界面」：主窗负责把自己 wake 回来。
    void fullUiRequested();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void copyText(const QString &text);
    void pushHistory(const QString &text, int costMs);
    void refreshHint();

    VoiceController *voice_ = nullptr;
    ProfileStore *store_ = nullptr;
    Toast *toast_ = nullptr;

    QLabel *statusDot_ = nullptr;
    QPushButton *mic_ = nullptr;
    QLabel *stateLabel_ = nullptr;
    QLabel *hintLabel_ = nullptr;
    QLabel *transcript_ = nullptr;
    QListWidget *history_ = nullptr;
    QComboBox *backend_ = nullptr;
    QTimer *levelTimer_ = nullptr;

    bool listening_ = false;
    bool recording_ = false;
};
