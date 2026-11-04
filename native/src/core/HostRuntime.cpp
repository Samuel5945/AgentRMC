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

// 只认 Qoder 家族：它的 hooks 是唯一的消费者，别的 Electron 宿主能不能当 Node
// 用没有实测过，不往里塞。
QString runningQoderExe() {
    for (const QString &exe :
         {QStringLiteral("Qoder.exe"), QStringLiteral("Qoder CN.exe")}) {
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
    const QString exe = runningQoderExe();
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
