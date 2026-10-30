#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QTimer>

#include "MainWindow.h"
#include "core/Profiles.h"
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
    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) == "--shot") {
            shot = args.at(i + 1);
        } else if (args.at(i) == "--pin") {
            pin = args.at(i + 1);
        } else if (args.at(i) == "--shot-settings") {
            settingsShot = args.at(i + 1);
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

    MainWindow win(shot.isEmpty(), pin, shot.isEmpty());
    win.resize(1000, 640);

    if (!shot.isEmpty()) {
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
