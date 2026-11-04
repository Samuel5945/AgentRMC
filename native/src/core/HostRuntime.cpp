#include "core/HostRuntime.h"

#include "core/AgentWindow.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QString runtimeDir() {
    QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty()) {
        local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    return local + QStringLiteral("/AgentRMC/runtime");
}

// 只列实测过 ELECTRON_RUN_AS_NODE=1 可用的宿主（2026-11-04）；CodeBuddy 未实测故不列。
QString runningHostExe() {
    for (const QString &exe :
         {QStringLiteral("Qoder.exe"), QStringLiteral("Qoder CN.exe"),
          QStringLiteral("ZCode.exe"), QStringLiteral("WorkBuddy.exe"),
          QStringLiteral("WorkBuddyAI.exe"), QStringLiteral("DeepSeek Harness.exe"),
          QStringLiteral("OpenSquilla.exe")}) {
        const QString path = AgentWindow::imagePath(exe);
        if (!path.isEmpty()) {
            return path;
        }
    }
    return QString();
}

}  // namespace

namespace HostRuntime {

QString hintPath() {
    return runtimeDir() + QStringLiteral("/node.txt");
}

void refreshHint() {
    const QString exe = runningHostExe();
    const QString path = hintPath();
    if (exe.isEmpty()) {
        QFile::remove(path);
        return;
    }

    QFile current(path);
    if (current.open(QIODevice::ReadOnly)) {
        const QString existing = QString::fromUtf8(current.readAll()).trimmed();
        if (existing.compare(exe, Qt::CaseInsensitive) == 0) {
            return;
        }
    }

    QDir().mkpath(runtimeDir());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        return;
    }
    out.write(exe.toUtf8());
    out.write("\n");
    out.commit();
}

}  // namespace HostRuntime
