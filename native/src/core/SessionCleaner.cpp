#include "core/SessionCleaner.h"

#include "core/SessionTranscript.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QVector>

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// 对齐 Magpie 的 ActiveWindow：一分钟内有写入的会话不碰（防止宿主还在写、
// 或刚写完索引没落定）。回收站入口本身还有「只接受 archived」第一道闸。
constexpr qint64 kActiveWindowMs = 60000;

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

// 会话 id 只认文件名安全字符：首字符必须字母数字（把「.」「..」「.hidden」
// 这类相对路径一刀切掉），字符集本身不含 / \ : 等分隔符。
bool safeSessionId(const QString &id) {
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return re.match(id).hasMatch();
}

// 跨卷退回复制时要保时间戳（Magpie copyAll 对每个文件 Chtimes）：否则
// 恢复回来的旧会话文件变成刚写的，下次又被活跃窗口拦住。Qt 的
// setFileTime 对未打开文件实测报「拒绝访问」，直接走 Win32。
void preserveTimes(const QString &src, const QString &dst) {
    HANDLE hs = CreateFileW(reinterpret_cast<LPCWSTR>(src.utf16()), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hs == INVALID_HANDLE_VALUE) {
        return;
    }
    FILETIME creation{}, access{}, modified{};
    const bool got = GetFileTime(hs, &creation, &access, &modified) != FALSE;
    CloseHandle(hs);
    if (!got) {
        return;
    }
    HANDLE hd = CreateFileW(reinterpret_cast<LPCWSTR>(dst.utf16()), FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hd == INVALID_HANDLE_VALUE) {
        return;
    }
    SetFileTime(hd, &creation, &access, &modified);
    CloseHandle(hd);
}

bool copyTree(const QString &src, const QString &dst, QString *error) {
    const QFileInfo info(src);
    if (info.isDir()) {
        if (!QDir().mkpath(dst)) {
            *error = QStringLiteral("建目录失败：") + dst;
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
        preserveTimes(src, dst);  // 目录时间戳也保，底部先铺完再铺外层
        return true;
    }
    if (!info.isFile()) {
        return true;
    }
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (QFileInfo::exists(dst) && !QFile::remove(dst)) {
        *error = QStringLiteral("覆盖旧文件失败：") + dst;
        return false;
    }
    if (!QFile::copy(src, dst)) {
        *error = QStringLiteral("复制失败：") + src;
        return false;
    }
    if (QFileInfo(dst).size() != info.size()) {
        *error = QStringLiteral("大小校验失败：") + src;
        return false;
    }
    preserveTimes(src, dst);
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

qint64 treeSize(const QString &path) {
    const QFileInfo info(path);
    if (info.isFile()) {
        return info.size();
    }
    if (!info.isDir()) {
        return 0;
    }
    qint64 n = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        n += it.fileInfo().size();
    }
    return n;
}

// 路径（或目录里任一文件）在 windowMs 内被写过 = 活跃，不碰。
bool recentWrite(const QString &path, qint64 windowMs, const QDateTime &now) {
    const auto hot = [&](const QDateTime &mt) {
        return mt.isValid() && mt.msecsTo(now) < windowMs;
    };
    const QFileInfo info(path);
    if (info.isFile()) {
        return hot(info.lastModified());
    }
    if (!info.isDir()) {
        return false;
    }
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (hot(it.fileInfo().lastModified())) {
            return true;
        }
    }
    return false;
}

// 移动一个文件或目录（Magpie move 的口径）：同卷 rename；跨卷退回
// 复制+删原件，且复制成功前绝不动原件、原件删不掉就把复制回滚——任何
// 失败姿势下原件都保持完整。目标已存在直接拒绝。
bool moveInto(const QString &from, const QString &to, QString *why) {
    const QFileInfo toInfo(to);
    if (toInfo.exists() || toInfo.isSymLink()) {
        *why = QStringLiteral("落点已被占用：") + to;
        return false;
    }
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (QFile::rename(from, to)) {
        return true;
    }
    // QFile::rename 对目录直接 false；同卷移动走 Win32 MoveFileW（目录、文件
    // 都行，保留时间戳），跨卷或再失败才退到复制+删。
    if (MoveFileW(reinterpret_cast<LPCWSTR>(from.utf16()),
                  reinterpret_cast<LPCWSTR>(to.utf16()))) {
        return true;
    }
    QString copyError;
    if (!copyTree(from, to, &copyError)) {
        *why = copyError;
        return false;
    }
    const bool gone = QFileInfo(from).isDir() ? QDir(from).removeRecursively()
                                              : QFile::remove(from);
    if (!gone) {
        if (QFileInfo(to).isDir()) {
            QDir(to).removeRecursively();
        } else {
            QFile::remove(to);
        }
        *why = QStringLiteral("原件被占用删不掉，已回滚：") + from;
        return false;
    }
    return true;
}

QString entriesRoot(const QString &trashOverride) {
    const QString base =
        trashOverride.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                  QStringLiteral("/session-trash")
            : trashOverride;
    return base + QStringLiteral("/entries");
}

// key = "<宿主安全名>/<条目文件夹>"。两段都过字符白名单、拒绝 . 与 ..、
// 解析后必须仍在回收目录内、且不是链接——越界与软链带出去一律拒（Magpie
// trashFolder 的防越界口径）。非法返回空串。
QString entryDirOfIn(const QString &key, const QString &root, QString *why) {
    const int cut = key.indexOf(QLatin1Char('/'));
    const QString exePart = cut < 0 ? QString() : key.left(cut);
    const QString folder = cut < 0 ? QString() : key.mid(cut + 1);
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9._-]{1,128}$"));
    const auto bad = [&](const QString &part) {
        return part.isEmpty() || part == QLatin1String(".") ||
               part == QLatin1String("..") || !ok.match(part).hasMatch();
    };
    if (cut < 0 || bad(exePart) || bad(folder)) {
        *why = QStringLiteral("回收条目 key 不合法");
        return QString();
    }
    const QString dir = QDir::cleanPath(
        root + QLatin1Char('/') + exePart + QLatin1Char('/') + folder);
    if (!dir.startsWith(QDir::cleanPath(root) + QLatin1Char('/'))) {
        *why = QStringLiteral("回收条目 key 不合法");
        return QString();
    }
    const QFileInfo fi(dir);
    if (!fi.isDir() || fi.isSymLink()) {
        *why = QStringLiteral("回收条目不存在");
        return QString();
    }
    return dir;
}

QJsonObject entryManifest(const QString &dir) {
    QFile f(dir + QStringLiteral("/entry.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeEntryJson(const QString &dir, const QJsonObject &entry) {
    QSaveFile f(dir + QStringLiteral("/entry.json"));
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(entry).toJson(QJsonDocument::Indented));
    return f.commit();
}

QJsonObject okObject(const QJsonObject &base = QJsonObject()) {
    QJsonObject out = base;
    out.insert(QStringLiteral("ok"), true);
    return out;
}

QJsonObject errObject(const QString &why) {
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), why}};
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

QJsonObject SessionCleaner::removeSessions(const QJsonArray &items, QString *error,
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

    const QString root = entriesRoot(trashOverride);
    if (!QDir().mkpath(root)) {
        return failAll(QStringLiteral("建回收目录失败：") + root);
    }

    // 先去重；同一会话只处理一次，避免批量选择里重复项把移动跑两遍。
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

    // 复核会话真在该宿主会话库里（不要求归档，对齐 Magpie：库里没有就拒）。
    // 页面传来的字段不可信，一切以本机目录为准。
    QHash<QString, QSet<QString>> knownIds;
    for (const QJsonObject &item : work) {
        const QString exe = item.value(QStringLiteral("exe")).toString();
        if (!SessionCleaner::supportsHost(exe) || knownIds.contains(exe)) {
            continue;
        }
        QSet<QString> ids;
        const QString home = hostRoot(exe, homeOverride);
        for (const auto &info : SessionTranscript::listSessions(exe, 1000, home)) {
            ids.insert(info.id);
        }
        knownIds.insert(exe, ids);
    }

    QJsonArray failed;
    QJsonArray entries;
    int deleted = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString stamp = now.toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
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
        if (!safeSessionId(sid)) {
            fail(QStringLiteral("会话 id 不合法，拒绝删除"));
            continue;
        }
        if (!knownIds.value(exe).contains(sid)) {
            fail(QStringLiteral("会话不在该宿主会话库（可能已清理），未动"));
            continue;
        }

        const QString home = hostRoot(exe, homeOverride);
        QStringList paths;
        if (exe.compare(QStringLiteral("DeepSeek Harness.exe"),
                        Qt::CaseInsensitive) == 0) {
            const QString sessionDir = findDshSessionDir(home, sid);
            const QString projCache = home +
                QStringLiteral("/storages/session_projcache/sessions/") + sid +
                QStringLiteral(".json");
            if (sessionDir.isEmpty() && !QFileInfo::exists(projCache)) {
                fail(QStringLiteral("DSH 会话目录和投影缓存都不存在"));
                continue;
            }
            if (!sessionDir.isEmpty()) {
                paths.append(sessionDir);
            }
            if (QFileInfo::exists(projCache)) {
                paths.append(projCache);
            }
        } else {
            const QString jsonl = findJsonl(home + QStringLiteral("/projects"), sid);
            if (jsonl.isEmpty()) {
                fail(QStringLiteral("会话 jsonl 不在本机（可能已清理）"));
                continue;
            }
            paths.append(jsonl);
            const QString sidecar = jsonl.left(jsonl.size() - 6);  // 去 .jsonl
            if (QFileInfo::exists(sidecar)) {
                paths.append(sidecar);
            }
        }

        bool hot = false;
        for (const QString &p : paths) {
            if (recentWrite(p, kActiveWindowMs, now)) {
                hot = true;
                break;
            }
        }
        if (hot) {
            fail(QStringLiteral("该会话一分钟内有写入，可能正在运行，稍后再试"));
            continue;
        }

        // 条目目录：<entries>/<exe>/<UTC 时间>-<会话 id>；同秒重名加序号。
        const QString exeSafe = safeName(exe);
        QString folder = stamp + QLatin1Char('-') + safeName(sid);
        QString dir = root + QLatin1Char('/') + exeSafe + QLatin1Char('/') + folder;
        for (int n = 2; QFileInfo::exists(dir); ++n) {
            folder = stamp + QLatin1Char('-') + safeName(sid) + QLatin1Char('-') +
                     QString::number(n);
            dir = root + QLatin1Char('/') + exeSafe + QLatin1Char('/') + folder;
        }
        if (!QDir().mkpath(dir + QStringLiteral("/files"))) {
            fail(QStringLiteral("建回收条目目录失败：") + dir);
            continue;
        }

        QJsonArray moved;
        QStringList movedTos;
        qint64 size = 0;
        bool ok = true;
        QString why;
        for (int i = 0; i < paths.size(); ++i) {
            const QString name = QString::number(i) + QLatin1Char('-') +
                                 QFileInfo(paths.at(i)).fileName();
            size += treeSize(paths.at(i));
            const QString to = dir + QStringLiteral("/files/") + name;
            if (!moveInto(paths.at(i), to, &why)) {
                ok = false;
                break;
            }
            moved.append(QJsonObject{{QStringLiteral("from"), paths.at(i)},
                                     {QStringLiteral("name"), name}});
            movedTos.append(to);
            // 清单随移动增量落盘：中途断电/崩溃时，条目里已移动的文件都在
            // 清单上，恢复与排查都对得上（Magpie writeManifest 的做法）。
            if (!writeEntryJson(dir, QJsonObject{{QStringLiteral("kind"),
                                                  QStringLiteral("session-trash")},
                                                 {QStringLiteral("exe"), exe},
                                                 {QStringLiteral("session"), sid},
                                                 {QStringLiteral("project"), project},
                                                 {QStringLiteral("title"), title},
                                                 {QStringLiteral("deleted"),
                                                  now.toString(Qt::ISODate)},
                                                 {QStringLiteral("size"), size},
                                                 {QStringLiteral("items"), moved}})) {
                why = QStringLiteral("回收条目清单写入失败");
                ok = false;
                break;
            }
        }
        if (!ok) {
            // 已移动的原路搬回；会话保持完整（旧实现的「备份了但没删净」
            // 两不脏状态到此为止）。回滚本身再失败（原位置被占）时保留条目
            // 目录——宁可留个可见的孤儿条目，也绝不吞还没搬回的数据。
            bool undone = true;
            for (int j = movedTos.size() - 1; j >= 0; --j) {
                const QString back = moved.at(j).toObject()
                                         .value(QStringLiteral("from")).toString();
                QString undoWhy;
                if (!moveInto(movedTos.at(j), back, &undoWhy)) {
                    undone = false;
                }
            }
            if (undone) {
                QFile::remove(dir + QStringLiteral("/entry.json"));
                QDir(dir).removeRecursively();
            } else {
                why += QStringLiteral("，且部分文件没能搬回——剩余内容保留在回收目录：") + dir;
            }
            fail(why.isEmpty() ? QStringLiteral("移入回收站失败") : why);
            continue;
        }
        ++deleted;
        entries.append(exeSafe + QLatin1Char('/') + folder);
    }

    if (deleted == 0) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("deleted"), 0},
                           {QStringLiteral("failed"), failed},
                           {QStringLiteral("backup"), root},
                           {QStringLiteral("error"),
                            failed.isEmpty()
                                ? QStringLiteral("没有可删除的会话")
                                : QStringLiteral("没有会话被删除")}};
    }
    return QJsonObject{{QStringLiteral("ok"), failed.isEmpty()},
                       {QStringLiteral("deleted"), deleted},
                       {QStringLiteral("failed"), failed},
                       {QStringLiteral("entries"), entries},
                       {QStringLiteral("backup"), root}};
}

QJsonArray SessionCleaner::listTrash(const QString &trashOverride) {
    QVector<QJsonObject> rows;
    const QString root = entriesRoot(trashOverride);
    const QFileInfoList exeDirs =
        QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo &exeFi : exeDirs) {
        if (exeFi.isSymLink()) {
            continue;
        }
        const QFileInfoList folders = QDir(exeFi.absoluteFilePath())
                                          .entryInfoList(QDir::Dirs |
                                                             QDir::NoDotAndDotDot,
                                                         QDir::Time);
        for (const QFileInfo &fi : folders) {
            if (fi.isSymLink()) {
                continue;
            }
            QJsonObject row;
            row.insert(QStringLiteral("key"),
                       exeFi.fileName() + QLatin1Char('/') + fi.fileName());
            const QJsonObject entry = entryManifest(fi.absoluteFilePath());
            if (entry.isEmpty()) {
                // 清单丢了也不让它躲在回收站里删不掉：列成损坏条目，只能
                // 彻底删除，不能恢复。
                row.insert(QStringLiteral("damaged"), true);
                row.insert(QStringLiteral("exe"), fi.dir().dirName());
                row.insert(QStringLiteral("title"),
                           QStringLiteral("（清单损坏的条目）"));
                row.insert(QStringLiteral("deleted"), fi.fileTime(QFileDevice::FileModificationTime).toString(Qt::ISODate));
                row.insert(QStringLiteral("size"),
                           treeSize(fi.absoluteFilePath() + QStringLiteral("/files")));
            } else {
                row.insert(QStringLiteral("damaged"), false);
                for (const QString &k : {QStringLiteral("exe"),
                                         QStringLiteral("session"),
                                         QStringLiteral("project"),
                                         QStringLiteral("title"),
                                         QStringLiteral("deleted")}) {
                    row.insert(k, entry.value(k));
                }
                row.insert(QStringLiteral("size"), entry.value(QStringLiteral("size")).toDouble());
            }
            rows.append(row);
        }
    }
    // 最新删除在前（按 ISO 串字典序即可，deleted 统一 UTC ISO）。
    // QJsonArray 的迭代器是 QJsonValueRef 代理，std::sort  swap 不了，
    // 在 QJsonObject 向量上排完再装成数组。
    std::sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("deleted")).toString() >
               b.value(QStringLiteral("deleted")).toString();
    });
    QJsonArray out;
    for (const QJsonObject &row : rows) {
        out.append(row);
    }
    return out;
}

QJsonObject SessionCleaner::restore(const QString &key, QString *error,
                                    const QString &homeOverride,
                                    const QString &trashOverride) {
    QString why;
    const QString dir = entryDirOfIn(key, entriesRoot(trashOverride), &why);
    if (dir.isEmpty()) {
        if (error != nullptr) {
            *error = why;
        }
        return errObject(why);
    }
    const QJsonObject entry = entryManifest(dir);
    const QJsonArray items = entry.value(QStringLiteral("items")).toArray();
    if (items.isEmpty()) {
        why = QStringLiteral("条目没有清单记录，无法恢复；可彻底删除");
        if (error != nullptr) {
            *error = why;
        }
        return errObject(why);
    }
    // 先全量检查落点：任一原位又被占了就整体拒绝，一个文件都不动
    //（Magpie Restore「Nothing is moved when any of those places is
    // taken again」——半恢复比不恢复更糟）。
    for (const QJsonValue &value : items) {
        const QString from = value.toObject().value(QStringLiteral("from")).toString();
        if (from.isEmpty() || QFileInfo::exists(from)) {
            why = QStringLiteral("原位已有东西（%1），未恢复任何文件").arg(from);
            if (error != nullptr) {
                *error = why;
            }
            return errObject(why);
        }
    }
    int done = 0;
    for (const QJsonValue &value : items) {
        const QJsonObject m = value.toObject();
        const QString from = m.value(QStringLiteral("from")).toString();
        const QString name = m.value(QStringLiteral("name")).toString();
        if (name != QFileInfo(name).fileName() || name.isEmpty()) {
            why = QStringLiteral("条目清单记录不合法");
            break;
        }
        QString moveWhy;
        if (!moveInto(dir + QStringLiteral("/files/") + name, from, &moveWhy)) {
            why = moveWhy;
            break;
        }
        ++done;
    }
    if (done != items.size()) {
        // 已放回的原路收回条目里，回收站保持完整。
        for (int j = done - 1; j >= 0; --j) {
            const QJsonObject m = items.at(j).toObject();
            const QString from = m.value(QStringLiteral("from")).toString();
            QString undoWhy;
            moveInto(from,
                     dir + QStringLiteral("/files/") +
                         m.value(QStringLiteral("name")).toString(),
                     &undoWhy);
        }
        if (error != nullptr) {
            *error = why.isEmpty() ? QStringLiteral("恢复失败") : why;
        }
        return errObject(why.isEmpty() ? QStringLiteral("恢复失败") : why);
    }
    QFile::remove(dir + QStringLiteral("/entry.json"));
    QDir(dir).removeRecursively();
    QJsonObject out = okObject();
    out.insert(QStringLiteral("exe"), entry.value(QStringLiteral("exe")));
    out.insert(QStringLiteral("session"), entry.value(QStringLiteral("session")));
    out.insert(QStringLiteral("title"), entry.value(QStringLiteral("title")));
    Q_UNUSED(homeOverride);
    return out;
}

QJsonObject SessionCleaner::purge(const QString &key, QString *error,
                                  const QString &trashOverride) {
    QString why;
    const QString dir = entryDirOfIn(key, entriesRoot(trashOverride), &why);
    if (dir.isEmpty()) {
        if (error != nullptr) {
            *error = why;
        }
        return errObject(why);
    }
    if (!QDir(dir).removeRecursively()) {
        why = QStringLiteral("条目被占用，没删净");
        if (error != nullptr) {
            *error = why;
        }
        return errObject(why);
    }
    QJsonObject out = okObject();
    out.insert(QStringLiteral("key"), key);
    return out;
}

QJsonObject SessionCleaner::purgeAll(QString *error, const QString &trashOverride) {
    int purged = 0;
    QStringList leftovers;
    const QJsonArray rows = listTrash(trashOverride);
    for (const QJsonValue &value : rows) {
        const QString key = value.toObject().value(QStringLiteral("key")).toString();
        QString why;
        if (purge(key, &why, trashOverride).value(QStringLiteral("ok")).toBool()) {
            ++purged;
        } else {
            leftovers.append(key + QStringLiteral("：") + why);
        }
    }
    if (purged == 0 && error != nullptr) {
        *error = leftovers.isEmpty() ? QStringLiteral("回收站本来就是空的")
                                     : leftovers.join(QStringLiteral("；"));
        return errObject(*error);
    }
    QJsonObject out = okObject();
    out.insert(QStringLiteral("purged"), purged);
    if (!leftovers.isEmpty()) {
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("error"), leftovers.join(QStringLiteral("；")));
    }
    return out;
}
