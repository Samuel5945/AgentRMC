#pragma once

#include <QDialog>

class ProfileStore;
class QCheckBox;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QPushButton;

class VoiceSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit VoiceSettingsDialog(ProfileStore *store, QWidget *parent = nullptr);

signals:
    void saved();
    // 子对话框（TTS / 云端 ASR / 桌宠对话）保存了配置：外层就算点「取消」，
    // 持有 VoiceController 的一侧也要 reload，新配置才立即生效。
    void optionsApplied();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void save();
    void refreshStatus();
    void reloadThemes();
    // 让小白只挑一个模型包：解压、找模型目录、装到程序自己的 models\ 下。
    void importModel(bool senseVoice);
    void importFromPath(const QString &file, const QString &expectedBackend);

    ProfileStore *store_ = nullptr;
    QString senseDefault_;
    QString qwenDefault_;
    QComboBox *theme_ = nullptr;
    QComboBox *backend_ = nullptr;
    QComboBox *language_ = nullptr;
    QComboBox *kbHotkey_ = nullptr;
    QLabel *senseState_ = nullptr;
    QLabel *qwenState_ = nullptr;
    QLabel *asrState_ = nullptr;
    QLabel *llmState_ = nullptr;
    QPushButton *senseImport_ = nullptr;
    QPushButton *qwenImport_ = nullptr;
    QLabel *note_ = nullptr;
    QCheckBox *paste_ = nullptr;
    QCheckBox *autoEnter_ = nullptr;
    QCheckBox *headset_ = nullptr;
    QLabel *mediaHint_ = nullptr;
    QCheckBox *ttsEnabled_ = nullptr;
};
