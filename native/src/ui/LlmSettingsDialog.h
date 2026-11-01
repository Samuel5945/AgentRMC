#pragma once

#include <QDialog>

class ProfileStore;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class LlmClient;

// 桌宠对话（LLM）设置：Base URL / API Key 留空 = 沿用语音反馈（TTS）的配置，
// model 必填。任何 OpenAI 兼容的 chat/completions 服务都能接。
class LlmSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit LlmSettingsDialog(ProfileStore *store, QWidget *parent = nullptr);

signals:
    void saved();

private:
    void save();
    void test();

    ProfileStore *store_ = nullptr;
    QLineEdit *baseUrl_ = nullptr;
    QLineEdit *apiKey_ = nullptr;
    QComboBox *model_ = nullptr;
    QLineEdit *system_ = nullptr;
    QComboBox *chatVoice_ = nullptr;  // 回复音色；空 = 跟随语音反馈
    QPushButton *test_ = nullptr;
    QLabel *note_ = nullptr;
    LlmClient *llm_ = nullptr;
};
