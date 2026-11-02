#include "PluginUpgrade.h"

#include "Updater.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>

namespace {

constexpr char kKey[] = "agentrmc-bridge@local";
constexpr char kManifest[] = ".qoder-plugin/plugin.json";
constexpr char kTar[] = "C:/Windows/System32/tar.exe";

struct FamilyEntry {
    const char *dir;
    const char *name;
    const char *exe;
};

// 国际版在前：CN 装在 .qoder-cn，两条正则不会互相命中。
const FamilyEntry kFamily[] = {
    {".qoder", "Qoder", "Qoder.exe"},
    {".qoder-cn", "Qoder CN", "Qoder CN.exe"},
};

QJsonObject readObject(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QString::fromUtf8("读不到 %1").arg(path);
        }
        return {};
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error != nullptr) {
            *error = QString::fromUtf8("%1 不是合法 JSON").arg(path);
        }
        return {};
    }
    return doc.object();
}

bool writeObject(const QString &path, const QJsonObject &root, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QString::fromUtf8("打不开写入 %1").arg(path);
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error != nullptr) {
            *error = QString::fromUtf8("写入 %1 失败").arg(path);
        }
        return false;
    }
    return true;
}

}  // namespace

namespace pluginupgrade {

QString bundledVersion(const QString &pluginDir) {
    QFile file(QDir(pluginDir).filePath(QStringLiteral("VERSION")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

QList<Host> scanHomes(const QString &homePath) {
    QList<Host> hosts;
    for (const FamilyEntry &entry : kFamily) {
        const QDir home(homePath + QLatin1Char('/') +
                        QString::fromLatin1(entry.dir));
        if (!home.exists()) {
            continue;  // 本机没装这个宿主，不列成「未安装」
        }
        Host host;
        host.name = QString::fromLatin1(entry.name);
        host.exe = QString::fromLatin1(entry.exe);
        host.configHome = home.absolutePath();
        host.registryPath = host.configHome +
                            QStringLiteral("/plugins/installed_plugins_v2.json");

        QString error;
        const QJsonObject registry = readObject(host.registryPath, &error);
        if (!error.isEmpty()) {
            host.problem = error;
            hosts.append(host);
            continue;
        }
        const QJsonArray rows = registry.value(QLatin1String("plugins"))
                                    .toObject()
                                    .value(QLatin1String(kKey))
                                    .toArray();
        if (!rows.isEmpty()) {
            const QJsonObject row = rows.first().toObject();
            host.installed = true;
            host.version = row.value(QStringLiteral("version")).toString();
            host.installPath =
                row.value(QStringLiteral("installPath")).toString();
        }

        // 启用状态按「插件 id」记，不按版本记，所以换版本不需要动 settings.json。
        QString settingsError;
        const QJsonObject settings = readObject(
            host.configHome + QStringLiteral("/settings.json"), &settingsError);
        host.enabled = settings.value(QStringLiteral("enabledPlugins"))
                           .toObject()
                           .value(QLatin1String(kKey))
                           .toBool();
        hosts.append(host);
    }
    return hosts;
}

Result upgrade(const Host &host, const QString &zipPath,
               const QString &version, const QString &backupDir) {
    Result result;
    result.host = host.name;
    result.from = host.version;
    result.to = version;
    auto fail = [&result](const QString &why) {
        result.detail = why;
        return result;
    };

    if (version.isEmpty()) {
        return fail(QString::fromUtf8("安装包里没有版本号（plugins/agentrmc-bridge/VERSION）"));
    }
    if (!host.installed) {
        return fail(QString::fromUtf8("这一家没装过插件，先在宿主里装一次"));
    }
    if (!QFile::exists(zipPath)) {
        return fail(QString::fromUtf8("找不到插件包 %1").arg(zipPath));
    }
    if (!versionIsNewer(version, host.version)) {
        return fail(QString::fromUtf8("已装 %1，不比它新").arg(host.version));
    }

    const QString cacheRoot = host.configHome +
                              QStringLiteral("/plugins/cache/local/agentrmc-bridge");
    const QString dest = cacheRoot + QLatin1Char('/') + version;
    const QString manifest = QDir(dest).filePath(QLatin1String(kManifest));

    // 1) 先问「这个 zip 里到底是哪一版」，再决定动不动盘。断言在前、写入在后：
    //    zip 内容和 VERSION 对不上时，解包已经把错的目录立起来了，注册表也就能
    //    指到一个名对实不对的位置——比不升级难查得多。
    //    目录已存在（上次解过）就直接读盘上那份，不再起 tar。
    QString packed;
    if (QFile::exists(manifest)) {
        QString error;
        const QJsonObject manifestObject = readObject(manifest, &error);
        if (!error.isEmpty()) {
            return fail(error);
        }
        packed = manifestObject.value(QStringLiteral("version")).toString();
    } else {
        if (!QFile::exists(QString::fromLatin1(kTar))) {
            return fail(QString::fromUtf8("系统里没有 %1，无法解包").arg(QString::fromLatin1(kTar)));
        }
        QProcess probe;
        probe.start(QString::fromLatin1(kTar),
                    {QStringLiteral("-xOf"), QDir::toNativeSeparators(zipPath),
                     QLatin1String(kManifest)});
        if (!probe.waitForStarted(3000)) {
            return fail(QString::fromUtf8("起不了 tar.exe"));
        }
        if (!probe.waitForFinished(8000)) {
            probe.kill();
            probe.waitForFinished(1000);
            return fail(QString::fromUtf8("读插件包超时"));
        }
        if (probe.exitStatus() != QProcess::NormalExit ||
            probe.exitCode() != 0) {
            return fail(QString::fromUtf8("插件包里没有 %1").arg(QLatin1String(kManifest)));
        }
        QJsonParseError parse{};
        const QJsonDocument doc =
            QJsonDocument::fromJson(probe.readAllStandardOutput(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            return fail(QString::fromUtf8("插件包里的清单不是合法 JSON"));
        }
        packed = doc.object().value(QStringLiteral("version")).toString();
    }
    if (packed != version) {
        return fail(QString::fromUtf8("包里的清单是 %1，和 %2 对不上").arg(packed, version));
    }

    // 2) 解包（幂等：已经解好就跳过，重跑不会把同一版解两遍）。
    if (!QFile::exists(manifest)) {
        QDir().mkpath(dest);
        QProcess tar;
        tar.start(QString::fromLatin1(kTar),
                  {QStringLiteral("-xf"), QDir::toNativeSeparators(zipPath),
                   QStringLiteral("-C"), QDir::toNativeSeparators(dest)});
        if (!tar.waitForStarted(3000)) {
            return fail(QString::fromUtf8("起不了 tar.exe"));
        }
        if (!tar.waitForFinished(10000)) {
            tar.kill();
            tar.waitForFinished(1000);
            return fail(QString::fromUtf8("解包超时"));
        }
        if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
            const QString err =
                QString::fromLocal8Bit(tar.readAllStandardError()).trimmed();
            return fail(QString::fromUtf8("解包失败：%1").arg(err));
        }
        if (!QFile::exists(manifest)) {
            return fail(QString::fromUtf8("解完还是找不到清单"));
        }
    }

    // 3) 备份原件，再改注册表里这一条（其余条目原样留着）。
    QString registryError;
    QJsonObject registry = readObject(host.registryPath, &registryError);
    if (!registryError.isEmpty()) {
        return fail(registryError);
    }
    QJsonObject plugins = registry.value(QLatin1String("plugins")).toObject();
    QJsonArray rows = plugins.value(QLatin1String(kKey)).toArray();
    if (rows.isEmpty()) {
        return fail(QString::fromUtf8("注册表里没有这条插件"));
    }
    QJsonObject row = rows.first().toObject();
    row.insert(QStringLiteral("version"), version);
    row.insert(QStringLiteral("installPath"), QDir::toNativeSeparators(dest));
    row.insert(QStringLiteral("lastUpdated"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    rows[0] = row;
    plugins.insert(QLatin1String(kKey), rows);
    registry.insert(QLatin1String("plugins"), plugins);

    if (!backupDir.isEmpty()) {
        QDir().mkpath(backupDir);
        // 毫秒戳：同一秒里连点两次升级也不能把备份覆盖掉。
        const QString stamp = QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMddTHHmmsszzz"));
        result.backupPath = QDir(backupDir).filePath(
            QStringLiteral("installed_plugins_%1.%2.bak").arg(QString::fromLatin1(kKey), stamp));
        QFile::copy(host.registryPath, result.backupPath);
    }

    QString writeError;
    if (!writeObject(host.registryPath, registry, &writeError)) {
        return fail(writeError);
    }

    // 4) 读回核对：写没落地不能算升级成功。
    QString rereadError;
    const QJsonObject after = readObject(host.registryPath, &rereadError);
    const QJsonObject afterRow = after.value(QLatin1String("plugins"))
                                     .toObject()
                                     .value(QLatin1String(kKey))
                                     .toArray()
                                     .first()
                                     .toObject();
    if (afterRow.value(QStringLiteral("version")).toString() != version ||
        QDir::toNativeSeparators(
            afterRow.value(QStringLiteral("installPath")).toString()) !=
            QDir::toNativeSeparators(dest)) {
        return fail(QString::fromUtf8("写完读回不一致（备份在 %1）").arg(result.backupPath));
    }

    result.ok = true;
    result.detail = QString::fromUtf8("%1 → %2").arg(result.from, result.to);
    return result;
}

}  // namespace pluginupgrade
