#pragma once

#include <QDialog>

class ProfileStore;
class QComboBox;
class QLabel;
class QLineEdit;

// 云端 ASR 设置：Base URL / API Key 留空 = 沿用语音反馈（TTS）的配置，
// 模型必填。任何 OpenAI 兼容的 /audio/transcriptions 服务都能接。
class AsrSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AsrSettingsDialog(ProfileStore *store, QWidget *parent = nullptr);

signals:
    void saved();

private:
    void save();

    ProfileStore *store_ = nullptr;
    QComboBox *style_ = nullptr;
    QLineEdit *baseUrl_ = nullptr;
    QLineEdit *apiKey_ = nullptr;
    QComboBox *model_ = nullptr;
    QLabel *note_ = nullptr;
};
