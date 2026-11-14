#include "core/DshSessions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

QHash<QString, DshSessions::CacheEntry> DshSessions::cache_;

QString DshSessions::home() {
    const QString override = QString::fromLocal8Bit(qgetenv("AGENTRMC_DSH_HOME"));
    if (!override.isEmpty()) {
        return override;
    }
    return QDir::homePath() + QStringLiteral("/.dsh");
}

DshSessions::Info DshSessions::find(const QString &sessionId) {
    // 与派发哨兵同口径：只认 session- 前缀，其余字符限字母数字连字符——
    // 带路径分隔符/点号的 id 一律不碰盘，杜绝借 id 穿越目录。
    if (!sessionId.startsWith(QStringLiteral("session-")) ||
        sessionId.size() > 80) {
        return {};
    }
    for (const QChar c : sessionId) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('-')) {
            return {};
        }
    }

    const QString path = home() +
        QStringLiteral("/storages/session_projcache/sessions/") + sessionId +
        QStringLiteral(".json");
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        cache_.remove(sessionId);
        return {};
    }
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    const qint64 size = fi.size();
    const auto it = cache_.constFind(sessionId);
    if (it != cache_.constEnd() && it->mtimeMs == mtime && it->size == size) {
        return it->info;
    }

    Info info;
    // 投影文件实测 5~35KB；2MB 上限防病态文件把面板刷新拖死。
    if (size <= 2 * 1024 * 1024) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject root =
                QJsonDocument::fromJson(f.readAll()).object();
            const QJsonObject record = root.value(QStringLiteral("record")).toObject();
            if (!record.isEmpty()) {
                info.valid = true;
                const QJsonObject rows = record.value(QStringLiteral("rows")).toObject();
                info.title = rows.value(QStringLiteral("title"))
                                 .toObject()
                                 .value(QStringLiteral("val"))
                                 .toString()
                                 .trimmed();
                if (info.title.isEmpty()) {
                    // AI 标题是异步生成的，首轮后可能还没有：退首条用户正文。
                    const QString first =
                        rows.value(QStringLiteral("titleInput"))
                            .toObject()
                            .value(QStringLiteral("val"))
                            .toObject()
                            .value(QStringLiteral("first"))
                            .toObject()
                            .value(QStringLiteral("text"))
                            .toString();
                    info.title = first.simplified();
                    if (info.title.size() > 60) {
                        info.title = info.title.left(60) + QStringLiteral("…");
                    }
                }
                info.cwd = rows.value(QStringLiteral("workingDirectory"))
                               .toObject()
                               .value(QStringLiteral("val"))
                               .toString();
                if (info.cwd.isEmpty()) {
                    info.cwd = record.value(QStringLiteral("identity"))
                                   .toObject()
                                   .value(QStringLiteral("cwd"))
                                   .toString();
                }
            }
        }
    }

    if (cache_.size() > 512) {
        cache_.clear();
    }
    cache_.insert(sessionId, CacheEntry{mtime, size, info});
    return info;
}