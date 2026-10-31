#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QTabWidget>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MainWindow.h"
#include "core/Profiles.h"
#include "core/VoiceController.h"
#include "ui/GuideDialog.h"
#include "ui/VoicePet.h"
#include "ui/VoiceSettingsDialog.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("AgentRMCNative");
    QApplication::setOrganizationName("LIG");
    QApplication::setApplicationVersion(AGENTRMC_VERSION);

    QFile qss(":/style.qss");
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
    }

    QString shot;
    QString pin;
    QString settingsShot;
    QString guideShot;
    QString petShot;
    int shotTab = -1;
    int guideStep = -1;
    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) == "--shot") {
            shot = args.at(i + 1);
        } else if (args.at(i) == "--pin") {
            pin = args.at(i + 1);
        } else if (args.at(i) == "--shot-settings") {
            settingsShot = args.at(i + 1);
        } else if (args.at(i) == "--shot-guide") {
            guideShot = args.at(i + 1);
        } else if (args.at(i) == "--shot-pet") {
            petShot = args.at(i + 1);
        } else if (args.at(i) == "--tab") {
            shotTab = args.at(i + 1).toInt();
        } else if (args.at(i) == "--guide-step") {
            guideStep = args.at(i + 1).toInt();
        }
    }

    // 开机自启带 --tray：只留托盘；手动双击图标时不带，直接开窗口。
    const bool trayStart = app.arguments().contains(QStringLiteral("--tray"));
    const QString ipcName = QStringLiteral("AgentRMC.Native.Wake");
    const bool shotMode = !shot.isEmpty() || !settingsShot.isEmpty() ||
                          !guideShot.isEmpty() || !petShot.isEmpty();
    if (!shotMode) {
        if (HWND existing = FindWindowW(nullptr, L"AgentRMC Native")) {
            if (!trayStart) {
                PostMessageW(existing,
                             RegisterWindowMessageW(L"AgentRMC.Native.Wake"), 0, 0);
            }
            return 0;
        }
    }

    if (!settingsShot.isEmpty()) {
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        VoiceSettingsDialog dialog(&store);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QTimer::singleShot(350, &dialog, [&dialog, settingsShot] {
            const bool ok = dialog.grab().save(settingsShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed")
                              << settingsShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (!guideShot.isEmpty()) {
        GuideDialog dialog;
        if (guideStep >= 0) {
            dialog.showStep(guideStep);
        }
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QTimer::singleShot(350, &dialog, [&dialog, guideShot] {
            const bool ok = dialog.grab().save(guideShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed")
                              << guideShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (!petShot.isEmpty()) {
        // 桌宠验收图：球体 + 悬停卡片 + 气泡拼一张。
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        VoiceController voice(&store);
        VoicePet pet(&store, &voice);
        pet.setAttribute(Qt::WA_DontShowOnScreen);
        pet.present();
        pet.expandCard();
        pet.speak(QString::fromUtf8("你好，我是你的语音桌宠"));
        QTimer::singleShot(450, &pet, [&pet, petShot] {
            QPixmap canvas(320, 420);
            canvas.fill(Qt::transparent);
            QPainter painter(&canvas);
            painter.drawPixmap(30, 8, pet.grab());
            QWidget *card = pet.findChild<QWidget *>(QStringLiteral("petCard"));
            if (card != nullptr) {
                painter.drawPixmap(30, 110, card->grab());
            }
            QWidget *bubble =
                pet.findChild<QWidget *>(QStringLiteral("petBubble"));
            if (bubble != nullptr) {
                painter.drawPixmap(30, 340, bubble->grab());
            }
            const bool ok = canvas.save(petShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed")
                              << petShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    MainWindow win(shot.isEmpty(), pin, shot.isEmpty());
    // 页签截图要看到整页内容，离屏时把窗口拉高。
    win.resize(1000, shotTab >= 0 ? 1400 : 640);

    if (!shotMode) {
        // 托盘常驻：窗口隐藏/关闭不该让进程退出。
        QApplication::setQuitOnLastWindowClosed(false);
    }

    if (!shot.isEmpty()) {
        if (shotTab >= 0) {
            if (auto *tabs = win.findChild<QTabWidget *>()) {
                tabs->setCurrentIndex(shotTab);
            }
        }
        win.setAttribute(Qt::WA_DontShowOnScreen);
        win.show();
        QTimer::singleShot(450, &win, [&win, shot] {
            const bool ok = win.grab().save(shot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed") << shot;
            QCoreApplication::quit();
        });
    } else {
        win.showOnStartup(trayStart);
    }
    return app.exec();
}
