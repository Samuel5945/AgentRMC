#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QTabWidget>
#include <QTimer>

#include "MainWindow.h"
#include "core/Profiles.h"
#include "ui/GuideDialog.h"
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
    int shotTab = -1;
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
        } else if (args.at(i) == "--tab") {
            shotTab = args.at(i + 1).toInt();
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

    MainWindow win(shot.isEmpty(), pin, shot.isEmpty());
    // 页签截图要看到整页内容，离屏时把窗口拉高。
    win.resize(1000, shotTab >= 0 ? 1400 : 640);

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
        win.show();
    }
    return app.exec();
}
