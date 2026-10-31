#pragma once

#include <QDialog>

class ProfileStore;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class TtsClient;

// 语音反馈（TTS）设置：Base URL / API Key / 模型 / 音色全部用户自填，
// 语音主界面和语音设置共用这一个编辑器。任何 OpenAI 兼容的 TTS 都能接。
class TtsSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit TtsSettingsDialog(ProfileStore *store, QWidget *parent = nullptr);

signals:
    void saved();

private:
    void save();
    void preview();

    ProfileStore *store_ = nullptr;
    QCheckBox *enabled_ = nullptr;
    QComboBox *output_ = nullptr;
    QLineEdit *baseUrl_ = nullptr;
    QLineEdit *apiKey_ = nullptr;
    QComboBox *model_ = nullptr;
    QComboBox *voice_ = nullptr;
    QPushButton *preview_ = nullptr;
    QLabel *note_ = nullptr;
    TtsClient *tts_ = nullptr;
};
