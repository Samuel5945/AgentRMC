#pragma once

#include <QDialog>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QLabel;
class QKeyEvent;
class QMouseEvent;
class QPushButton;
class QStackedWidget;
class QWidget;

// 引导页与引导弹窗共用的「键位 + 说明」行。
namespace guide {

QWidget *heading(const QString &text);
QWidget *row(const QString &key, const QString &text, int keyWidth);

}  // namespace guide

// 首次启动的四步引导：配对 → 试按键 → 选语音 → 手势。
class GuideDialog final : public QDialog {
    Q_OBJECT

public:
    explicit GuideDialog(QWidget *parent = nullptr);

    void showStep(int index);
    // 遥控器真的按下去时喂进来，第二步实时显示「已收到」。
    void noteKey(const QString &keyLabel);
    // 标记当前语音路线（hotkey / sensevoice / qwen3）。
    void setBackend(const QString &backend);

signals:
    void useHotkeyRequested();
    void openVoiceSettingsRequested();
    void modelPackageDropped(const QString &file);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    // 引导是独立置顶窗口，拖到它上面时主窗口收不到，这里也接。
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QWidget *buildPairingPage();
    QWidget *buildKeyTestPage();
    QWidget *buildVoicePage();
    QWidget *buildGesturePage();
    void updateStepUi();

    QStackedWidget *pages_ = nullptr;
    QLabel *stepLabel_ = nullptr;
    QLabel *keyStatus_ = nullptr;
    QLabel *hotkeyMark_ = nullptr;
    QLabel *localMark_ = nullptr;
    QLabel *dropHint_ = nullptr;
    int keyCount_ = 0;
    QPushButton *prev_ = nullptr;
    QPushButton *next_ = nullptr;
};
