#include "core/SessionCleaner.h"

#include "core/SessionTranscript.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QVector>

namespace {

QString hostSubdir(const QString &exe) {
    if (exe.compare(QStringLiteral("WorkBuddy.exe"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("/.workbuddy");
    }
    if (exe.compare(QStringLiteral("WorkBuddyAI.exe"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("/.workbuddy-ai");
    }
    if (exe.compare(QStringLiteral("Qoder.exe"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("/.qoder");
    }
    if (exe.compare(QStringLiteral("Qoder CN.exe"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("/.qoder-cn");
    }
    if (exe.compare(QStringLiteral("DeepSeek Harness.exe"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("/.dsh");
    }
    return QString();
}

QString hostRoot(const QString &exe, const QString &homeOverride) {
    const QString sub = hostSubdir(exe);
    return sub.isEmpty() ? QString() : (homeOverride.isEmpty() ? QDir::homePath()
                                                               : homeOverride) +
                                           sub;
}

QString safeName(QString name) {
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                 QStringLiteral("_"));
    return name.isEmpty() ? QStringLiteral("session") : name;
}

bool copyTree(const QString &src, const QString &dst, QString *error) {
    const QFileInfo info(src);
    if (info.isDir()) {
        if (!QDir().mkpath(dst)) {
            *error = QStringLiteral("建备份目录失败：") + dst;
            return false;
        }
        const QFileInfoList children =
            QDir(src).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &child : children) {
            if (!copyTree(child.absoluteFilePath(),
                          dst + QLatin1Char('/') + child.fileName(), error)) {
                return false;
            }
        }
        return true;
    }
    if (!info.isFile()) {
        return true;
    }
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (QFileInfo::exists(dst) && !QFile::remove(dst)) {
        *error = QStringLiteral("覆盖旧备份失败：") + dst;
        return false;
    }
    if (!QFile::copy(src, dst)) {
        *error = QStringLiteral("复制备份失败：") + src;
        return false;
    }
    if (QFileInfo(dst).size() != info.size()) {
        *error = QStringLiteral("备份大小校验失败：") + src;
        return false;
    }
    return true;
}

QString findJsonl(const QString &root, const QString &sessionId) {
    QDirIterator it(root, QStringList{QStringLiteral("*.jsonl")}, QDir::Files,
                    QDirIterator::Subdirectories);
    const QString name = sessionId + QStringLiteral(".jsonl");
    while (it.hasNext()) {
        it.next();
        if (it.fileName() == name) {
            return it.filePath();
        }
    }
    return QString();
}

QString findDshSessionDir(const QString &root, const QString &sessionId) {
    QDirIterator it(root + QStringLiteral("/sessions"),
                    QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileName() == sessionId) {
            return it.filePath();
        }
    }
    return QString();
}

}  // namespace

bool SessionCleaner::supportsHost(const QString &agentExe) {
    return agentExe.compare(QStringLiteral("WorkBuddy.exe"), Qt::CaseInsensitive) == 0 ||
           agentExe.compare(QStringLiteral("WorkBuddyAI.exe"), Qt::CaseInsensitive) == 0 ||
           agentExe.compare(QStringLiteral("Qoder.exe"), Qt::CaseInsensitive) == 0 ||
           agentExe.compare(QStringLiteral("Qoder CN.exe"), Qt::CaseInsensitive) == 0 ||
           agentExe.compare(QStringLiteral("DeepSeek Harness.exe"),
                            Qt::CaseInsensitive) == 0;
}

QJsonObject SessionCleaner::removeArchived(const QJsonArray &items, QString *error,
                                           const QString &homeOverride,
                                           const QString &trashOverride) {
    const auto failAll = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("deleted"), 0},
                           {QStringLiteral("failed"), QJsonArray()},
                           {QStringLiteral("error"), why}};
    };
    if (items.isEmpty()) {
        return failAll(QStringLiteral("没有选择会话"));
    }

    const QString defaultTrash =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/session-trash");
    const QString baseTrash = trashOverride.isEmpty() ? defaultTrash : trashOverride;
    const QString trashRoot =
        baseTrash + QLatin1Char('/') +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    if (!QDir().mkpath(trashRoot)) {
        return failAll(QStringLiteral("建会话回收目录失败：") + trashRoot);
    }

    // 先去重；同一会话只处理一次，避免批量选择里重复项把备份/删除跑两遍。
    QSet<QString> seen;
    QVector<QJsonObject> work;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QString exe = item.value(QStringLiteral("exe")).toString();
        const QString sid = item.value(QStringLiteral("session")).toString();
        if (exe.isEmpty() || sid.isEmpty()) {
            continue;
        }
        const QString key = exe.toLower() + QLatin1Char('|') + sid;
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        work.append(item);
    }
    if (work.isEmpty()) {
        return failAll(QStringLiteral("选择项缺少宿主或会话 id"));
    }

    // 只信“当前目录里仍标为 archived”的会话。页面传来的 archived 字段不可信。
    QHash<QString, QSet<QString>> archivedIds;
    for (const QJsonObject &item : work) {
        const QString exe = item.value(QStringLiteral("exe")).toString();
        if (!SessionCleaner::supportsHost(exe)) {
            continue;
        }
        if (archivedIds.contains(exe)) {
            continue;
        }
        QSet<QString> ids;
        const QString root = hostRoot(exe, homeOverride);
        for (const auto &info : SessionTranscript::listSessions(exe, 1000, root)) {
            if (info.archived) {
                ids.insert(info.id);
            }
        }
        archivedIds.insert(exe, ids);
    }

    QJsonArray failed;
    QJsonArray manifestItems;
    int deleted = 0;
    for (const QJsonObject &item : work) {
        const QString exe = item.value(QStringLiteral("exe")).toString();
        const QString sid = item.value(QStringLiteral("session")).toString();
        const QString project = item.value(QStringLiteral("project")).toString();
        const QString title = item.value(QStringLiteral("title")).toString();
        const auto fail = [&](const QString &why) {
            failed.append(QJsonObject{{QStringLiteral("exe"), exe},
                                      {QStringLiteral("session"), sid},
                                      {QStringLiteral("error"), why}});
        };
        if (!SessionCleaner::supportsHost(exe)) {
            fail(QStringLiteral("该宿主没有安全的会话删除接口"));
            continue;
        }
        if (!archivedIds.value(exe).contains(sid)) {
            fail(QStringLiteral("不是当前归档会话，拒绝删除"));
            continue;
        }

        const QString root = hostRoot(exe, homeOverride);
        const QString backupDir = trashRoot + QLatin1Char('/') + safeName(exe) +
                                  QLatin1Char('/') + safeName(sid);
        QString backupError;
        QStringList backedFiles;

        if (exe.compare(QStringLiteral("DeepSeek Harness.exe"),
                        Qt::CaseInsensitive) == 0) {
            const QString sessionDir = findDshSessionDir(root, sid);
            const QString projCache = root +
                QStringLiteral("/storages/session_projcache/sessions/") + sid +
                QStringLiteral(".json");
            if (sessionDir.isEmpty() && !QFileInfo::exists(projCache)) {
                fail(QStringLiteral("DSH 会话目录和投影缓存都不存在"));
                continue;
            }
            if (!sessionDir.isEmpty()) {
                const QString dst = backupDir + QStringLiteral("/session");
                if (!copyTree(sessionDir, dst, &backupError)) {
                    fail(backupError);
                    continue;
                }
                backedFiles.append(dst);
            }
            if (QFileInfo::exists(projCache)) {
                const QString dst = backupDir + QStringLiteral("/projcache.json");
                if (!copyTree(projCache, dst, &backupError)) {
                    fail(backupError);
                    continue;
                }
                backedFiles.append(dst);
            }
            // 先落 manifest，再动宿主原件；中途异常至少能找到备份。
            QSaveFile mf(trashRoot + QStringLiteral("/manifest.json"));
            if (!mf.open(QIODevice::WriteOnly)) {
                fail(QStringLiteral("无法写备份清单"));
                continue;
            }
            QJsonObject row{{QStringLiteral("exe"), exe},
                            {QStringLiteral("session"), sid},
                            {QStringLiteral("project"), project},
                            {QStringLiteral("title"), title},
                            {QStringLiteral("files"), QJsonArray::fromStringList(backedFiles)}};
            manifestItems.append(row);
            mf.write(QJsonDocument(QJsonObject{
                 {QStringLiteral("kind"), QStringLiteral("session-trash")},
                 {QStringLiteral("items"), manifestItems}})
                         .toJson(QJsonDocument::Indented));
            if (!mf.commit()) {
                fail(QStringLiteral("备份清单写入失败"));
                continue;
            }
            bool removed = true;
            if (!sessionDir.isEmpty() &&
                !QDir(sessionDir).removeRecursively()) {
                removed = false;
            }
            if (removed && QFileInfo::exists(projCache) && !QFile::remove(projCache)) {
                removed = false;
            }
            if (!removed) {
                fail(QStringLiteral("备份完成，但宿主目录正被占用，未删净"));
                continue;
            }
            ++deleted;
            continue;
        }

        // WorkBuddy / Qoder 系：jsonl + 同 id 的 sidecar 目录。
        const QString jsonl = findJsonl(root + QStringLiteral("/projects"), sid);
        if (jsonl.isEmpty()) {
            fail(QStringLiteral("会话 jsonl 不在本机（可能已清理）"));
            continue;
        }
        const QString sidecar = jsonl.left(jsonl.size() - 6);  // 去 .jsonl
        const QString dstFile = backupDir + QLatin1Char('/') + QFileInfo(jsonl).fileName();
        if (!copyTree(jsonl, dstFile, &backupError)) {
            fail(backupError);
            continue;
        }
        backedFiles.append(dstFile);
        if (QFileInfo::exists(sidecar)) {
            const QString dstDir = backupDir + QStringLiteral("/sidecar");
            if (!copyTree(sidecar, dstDir, &backupError)) {
                fail(backupError);
                continue;
            }
            backedFiles.append(dstDir);
        }
        QSaveFile mf(trashRoot + QStringLiteral("/manifest.json"));
        if (!mf.open(QIODevice::WriteOnly)) {
            fail(QStringLiteral("无法写备份清单"));
            continue;
        }
        manifestItems.append(QJsonObject{
            {QStringLiteral("exe"), exe},
            {QStringLiteral("session"), sid},
            {QStringLiteral("project"), project},
            {QStringLiteral("title"), title},
            {QStringLiteral("files"), QJsonArray::fromStringList(backedFiles)}});
        mf.write(QJsonDocument(QJsonObject{
             {QStringLiteral("kind"), QStringLiteral("session-trash")},
             {QStringLiteral("items"), manifestItems}})
                     .toJson(QJsonDocument::Indented));
        if (!mf.commit()) {
            fail(QStringLiteral("备份清单写入失败"));
            continue;
        }
        bool removed = QFile::remove(jsonl);
        if (removed && QFileInfo::exists(sidecar)) {
            removed = QDir(sidecar).removeRecursively();
        }
        if (!removed) {
            fail(QStringLiteral("备份完成，但宿主文件正被占用，未删净"));
            continue;
        }
        ++deleted;
    }

    if (deleted == 0) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("deleted"), 0},
                           {QStringLiteral("failed"), failed},
                           {QStringLiteral("backup"), trashRoot},
                           {QStringLiteral("error"),
                            failed.isEmpty()
                                ? QStringLiteral("没有可删除的归档会话")
                                : QStringLiteral("没有会话被删除")}};
    }
    return QJsonObject{{QStringLiteral("ok"), failed.isEmpty()},
                       {QStringLiteral("deleted"), deleted},
                       {QStringLiteral("failed"), failed},
                       {QStringLiteral("backup"), trashRoot}};
}
