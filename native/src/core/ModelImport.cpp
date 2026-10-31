#include "ModelImport.h"

#include "core/VoiceSettings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMimeData>
#include <QProcess>
#include <QProgressDialog>
#include <QUrl>

namespace {

const QStringList kSuffixes = {
    QStringLiteral("zip"),   QStringLiteral("tar.bz2"), QStringLiteral("tbz2"),
    QStringLiteral("tar.gz"), QStringLiteral("tgz"),
};

QString stagePath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/models/.importing");
}

QString installPath(const QString &name) {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/models/") + name;
}

// 固定用系统自带 bsdtar：Git 的 GNU tar 解不了 zip，PATH 顺序不可靠。
QString tarExecutable() {
    const QString system =
        qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/tar.exe");
    return QFileInfo::exists(system) ? QDir::toNativeSeparators(system)
                                     : QStringLiteral("tar");
}

bool readyFor(const QString &dir, const QString &backend) {
    return backend == QStringLiteral("qwen3")
               ? qwen3Ready(dir, nullptr, nullptr, nullptr, nullptr)
               : senseVoiceReady(dir, nullptr, nullptr);
}

QString detectBackend(const QString &dir) {
    if (senseVoiceReady(dir, nullptr, nullptr)) {
        return QStringLiteral("sensevoice");
    }
    if (qwen3Ready(dir, nullptr, nullptr, nullptr, nullptr)) {
        return QStringLiteral("qwen3");
    }
    return QString();
}

}  // namespace

ModelImportResult importModelPackage(QWidget *parent, const QString &file) {
    ModelImportResult result;
    const QString suffix = [&file] {
        QString name = QFileInfo(file).fileName().toLower();
        for (const QString &candidate : kSuffixes) {
            if (name.endsWith(QLatin1Char('.') + candidate)) {
                return candidate;
            }
        }
        return QString();
    }();
    if (suffix.isEmpty()) {
        result.error = QStringLiteral("不支持的模型包格式：只认 zip / tar.bz2 / tar.gz");
        return result;
    }

    const QString stage = stagePath();
    QDir(stage).removeRecursively();
    QDir().mkpath(stage);

    QProcess tar;
    tar.setProcessChannelMode(QProcess::MergedChannels);
    tar.start(tarExecutable(),
              {QStringLiteral("-xf"), QDir::toNativeSeparators(file),
               QStringLiteral("-C"), QDir::toNativeSeparators(stage)});
    // 大包解压要几秒，泵事件保持界面响应。
    QProgressDialog progress(QString::fromUtf8("正在解压模型包，请稍候…"),
                             QString(), 0, 0, parent);
    progress.setWindowTitle(QString::fromUtf8("导入模型"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);
    progress.setAutoClose(false);
    progress.setStyleSheet(QStringLiteral(
        "QProgressDialog { background: #0f141d; }"
        "QProgressDialog QLabel { color: #e6edf3; font-size: 12px; }"
        "QProgressBar { border: 1px solid rgba(255,255,255,24);"
        " border-radius: 6px; background: rgba(255,255,255,7); height: 6px; }"
        "QProgressBar::chunk { background: #4f8cff; border-radius: 5px; }"));
    progress.show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    while (!tar.waitForFinished(200)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QApplication::restoreOverrideCursor();
    progress.close();

    if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        const QString out = QString::fromLocal8Bit(tar.readAll()).trimmed();
        QDir(stage).removeRecursively();
        result.error = QStringLiteral("解压失败：%1").arg(
            out.isEmpty() ? QStringLiteral("不是有效的模型包") : out);
        return result;
    }

    QString backend;
    QString found;
    for (const QFileInfo &info :
         QDir(stage).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        backend = detectBackend(info.absoluteFilePath());
        if (!backend.isEmpty()) {
            found = info.absoluteFilePath();
            break;
        }
    }
    if (found.isEmpty()) {
        backend = detectBackend(stage);
        if (!backend.isEmpty()) {
            found = stage;
        }
    }
    if (found.isEmpty()) {
        QDir(stage).removeRecursively();
        result.error = QStringLiteral("这个包里没找到可用的模型文件");
        return result;
    }

    const QString target = installPath(backend);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QDir(target).removeRecursively();
    const bool moved = found == stage ? QDir().rename(stage, target)
                                      : QDir().rename(found, target);
    QDir(stage).removeRecursively();
    if (!moved) {
        result.error = QStringLiteral("安装失败：写不进模型目录");
        return result;
    }
    if (!readyFor(target, backend)) {
        result.error = QStringLiteral("装好了但校验不过：模型文件不完整");
        return result;
    }
    result.ok = true;
    result.backend = backend;
    return result;
}

QString modelPackageFromMime(const QMimeData *mime) {
    if (mime == nullptr || !mime->hasUrls()) {
        return QString();
    }
    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        const QString name = QFileInfo(path).fileName().toLower();
        for (const QString &suffix : kSuffixes) {
            if (name.endsWith(QLatin1Char('.') + suffix)) {
                return path;
            }
        }
    }
    return QString();
}
