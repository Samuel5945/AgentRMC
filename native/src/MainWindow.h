#pragma once

#include <QMainWindow>

class QCloseEvent;
class QSystemTrayIcon;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(bool withTray, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QWidget *buildHeader();
    QWidget *buildRemoteCard();
    QWidget *buildVoiceCard();
    QWidget *buildKeymapPage();
    QWidget *buildLogPage();
    QWidget *buildGuidePage();
    void buildTray();
    void showMilestones();

    QSystemTrayIcon *tray_ = nullptr;
    bool quitting_ = false;
};
