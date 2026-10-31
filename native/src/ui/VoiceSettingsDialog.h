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

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void save();
    void refreshStatus();
    // 让小白只挑一个模型包：解压、找模型目录、装到程序自己的 models\ 下。
    void importModel(bool senseVoice);
    void importFromPath(const QString &file, const QString &expectedBackend);

    ProfileStore *store_ = nullptr;
    QString senseDefault_;
    QString qwenDefault_;
    QComboBox *backend_ = nullptr;
    QComboBox *language_ = nullptr;
    QLabel *senseState_ = nullptr;
    QLabel *qwenState_ = nullptr;
    QPushButton *senseImport_ = nullptr;
    QPushButton *qwenImport_ = nullptr;
    QLabel *note_ = nullptr;
    QCheckBox *paste_ = nullptr;
    QCheckBox *autoEnter_ = nullptr;
    QCheckBox *headset_ = nullptr;
    QLabel *mediaHint_ = nullptr;
};
