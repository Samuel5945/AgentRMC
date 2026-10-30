#pragma once

#include <QDialog>

class ProfileStore;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class VoiceSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit VoiceSettingsDialog(ProfileStore *store, QWidget *parent = nullptr);

signals:
    void saved();

private:
    void browse(QLineEdit *edit);
    void save();
    void refreshStatus();
    void openDownloadScript();

    ProfileStore *store_ = nullptr;
    QString senseDefault_;
    QString qwenDefault_;
    QComboBox *backend_ = nullptr;
    QLineEdit *senseDir_ = nullptr;
    QLineEdit *qwenDir_ = nullptr;
    QLabel *senseState_ = nullptr;
    QLabel *qwenState_ = nullptr;
    QLabel *note_ = nullptr;
    QCheckBox *paste_ = nullptr;
    QCheckBox *autoEnter_ = nullptr;
};
