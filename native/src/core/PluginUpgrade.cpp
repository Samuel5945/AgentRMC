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

constexpr char kPluginName[] = "agentrmc-bridge";
// DeepSeek Harness 的 bundle 包名：profile 的 dependencies 和 dsh.profile.bundles
// 两处都写这一个名字。
constexpr char kBundlePackage[] = "@local/agentrmc-bridge";
constexpr char kTar[] = "C:/Windows/System32/tar.exe";

struct FamilyEntry {
    const char *dir;          // 相对 home 的配置家
    const char *name;
    const char *exe;
    pluginupgrade::Kind kind;
    const char *registryRel;  // 相对配置家的注册表
    const char *pluginId;
    const char *cacheRel;     // 相对配置家的已装版本父目录
    const char *manifestRel;  // 包内清单
    const char *settingsRel;  // 启用状态所在的配置文件
    const char *mirrorRel;    // 目录市场源的整份镜像（nullptr = 没有）
    bool upgradable;          // false = 只报状态，不动它的注册表
};

// Qoder 国际版在前：CN 装在 .qoder-cn，两条正则不会互相命中。
// ZCode 的配置家是 ~/.zcode/cli（不是 ~/.zcode），注册表是 v1 格式。
// WorkBuddy 有两份安装：AI 版配置家在 ~/.workbuddy-ai，经典版在 ~/.workbuddy
// （后者是前者的子串，所以 AI 版必须排在前面判）。
const FamilyEntry kFamily[] = {
    {".qoder", "Qoder", "Qoder.exe", pluginupgrade::Kind::Qoder,
     "plugins/installed_plugins_v2.json", "agentrmc-bridge@local",
     "plugins/cache/local/agentrmc-bridge", ".qoder-plugin/plugin.json",
     "settings.json", nullptr, true},
    {".qoder-cn", "Qoder CN", "Qoder CN.exe", pluginupgrade::Kind::Qoder,
     "plugins/installed_plugins_v2.json", "agentrmc-bridge@local",
     "plugins/cache/local/agentrmc-bridge", ".qoder-plugin/plugin.json",
     "settings.json", nullptr, true},
    {".zcode/cli", "ZCode", "ZCode.exe", pluginupgrade::Kind::ZCode,
     "plugins/installed_plugins.json", "agentrmc-bridge@dev-agentrmc-local",
     "plugins/cache/dev-agentrmc-local/agentrmc-bridge",
     ".codebuddy-plugin/plugin.json", "config.json",
     "plugins/marketplaces/dev-agentrmc-local", true},
    {".workbuddy-ai", "WorkBuddy AI", "WorkBuddyAI.exe", pluginupgrade::Kind::WorkBuddy,
     "plugins/installed_plugins.json", "agentrmc-bridge@dev-agentrmc-local",
     "plugins/cache/dev-agentrmc-local/agentrmc-bridge",
     ".codebuddy-plugin/plugin.json", "settings.json", nullptr, false},
    {".workbuddy", "WorkBuddy", "WorkBuddy.exe", pluginupgrade::Kind::WorkBuddy,
     "plugins/installed_plugins.json", "agentrmc-bridge@dev-agentrmc-local",
     "plugins/cache/dev-agentrmc-local/agentrmc-bridge",
     ".codebuddy-plugin/plugin.json", "settings.json", nullptr, false},
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

// 注册表里这一条的位置：Qoder 是 plugins[id] 数组的第 0 条，ZCode 是 plugins 数组里
// 按 id 命中的那一条。两种形状都归到「row + 下标」上，后面改写只认这个。
struct RowRef {
    QJsonObject row;
    int index = -1;
};

bool findRow(const pluginupgrade::Host &host, const QJsonObject &registry,
             RowRef *ref) {
    const QJsonValue plugins = registry.value(QStringLiteral("plugins"));
    if (host.kind == pluginupgrade::Kind::ZCode) {
        if (!plugins.isArray()) {
            return false;
        }
        const QJsonArray rows = plugins.toArray();
        for (int i = 0; i < rows.size(); ++i) {
            const QJsonObject row = rows.at(i).toObject();
            const QString id = row.value(QStringLiteral("id")).toString();
            // 老条目可能只写了 name：id 缺省时按名字认，别把已装的说成没装。
            const QString name = row.value(QStringLiteral("name")).toString();
            if (id == host.pluginId ||
                (id.isEmpty() && name == QLatin1String(kPluginName))) {
                ref->row = row;
                ref->index = i;
                return true;
            }
        }
        return false;
    }
    const QJsonArray rows =
        plugins.toObject().value(host.pluginId).toArray();
    if (rows.isEmpty()) {
        return false;
    }
    ref->row = rows.first().toObject();
    ref->index = 0;
    return true;
}

// 把改好的 row 写回原位置：别家插件的条目一条都不能丢。
bool storeRow(const pluginupgrade::Host &host, QJsonObject *registry,
              const RowRef &ref, const QJsonObject &row, QString *error) {
    const QJsonValue plugins = registry->value(QStringLiteral("plugins"));
    if (host.kind == pluginupgrade::Kind::ZCode) {
        QJsonArray rows = plugins.toArray();
        if (!plugins.isArray() || ref.index < 0 || ref.index >= rows.size()) {
            if (error != nullptr) {
                *error = QString::fromUtf8("注册表里没有这条插件");
            }
            return false;
        }
        rows[ref.index] = row;
        registry->insert(QStringLiteral("plugins"), rows);
        return true;
    }
    QJsonObject byId = plugins.toObject();
    QJsonArray rows = byId.value(host.pluginId).toArray();
    if (rows.isEmpty()) {
        if (error != nullptr) {
            *error = QString::fromUtf8("注册表里没有这条插件");
        }
        return false;
    }
    rows[0] = row;
    byId.insert(host.pluginId, rows);
    registry->insert(QStringLiteral("plugins"), byId);
    return true;
}

// 启用状态在两种配置家里的位置不同：Qoder 是顶层 enabledPlugins，ZCode 挂在
// plugins.enabledPlugins 下。读不到一律当没开，不往 problem 里塞——这只影响展示。
bool readEnabled(const pluginupgrade::Host &host, const FamilyEntry &entry) {
    QString error;
    const QJsonObject settings =
        readObject(host.configHome + QLatin1Char('/') +
                       QString::fromLatin1(entry.settingsRel),
                   &error);
    if (!error.isEmpty()) {
        return false;
    }
    const QJsonObject scope =
        host.kind == pluginupgrade::Kind::ZCode
            ? settings.value(QStringLiteral("plugins")).toObject()
            : settings;
    return scope.value(QStringLiteral("enabledPlugins"))
        .toObject()
        .value(host.pluginId)
        .toBool();
}

// 把 zip 解到 dest。解完必须能看到清单，否则算失败——半个包比不升级更难查。
bool extractZip(const QString &zipPath, const QString &dest,
                const QString &manifestRel, QString *error) {
    QDir().mkpath(dest);
    QProcess tar;
    tar.start(QString::fromLatin1(kTar),
              {QStringLiteral("-xf"), QDir::toNativeSeparators(zipPath),
               QStringLiteral("-C"), QDir::toNativeSeparators(dest)});
    if (!tar.waitForStarted(3000)) {
        *error = QString::fromUtf8("起不了 tar.exe");
        return false;
    }
    if (!tar.waitForFinished(10000)) {
        tar.kill();
        tar.waitForFinished(1000);
        *error = QString::fromUtf8("解包超时");
        return false;
    }
    if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        *error = QString::fromUtf8("解包失败：%1")
                     .arg(QString::fromLocal8Bit(tar.readAllStandardError())
                              .trimmed());
        return false;
    }
    if (!QFile::exists(QDir(dest).filePath(manifestRel))) {
        *error = QString::fromUtf8("解完还是找不到清单");
        return false;
    }
    return true;
}

// ZCode 的目录市场源会整份镜像到 plugins/marketplaces/<id>/，安装时从镜像里取。
// 只把已装副本改新、镜像留着旧版，宿主下次重扫就会看到「已装 0.3.9 / 源里 0.3.8」
// 这种倒挂，可能直接把旧版装回去。所以镜像的文件和版本号一起更新。
bool syncMirror(const pluginupgrade::Host &host, const QString &zipPath,
                const QString &version, QString *error) {
    const QString manifestPath = host.mirrorRoot + QStringLiteral("/marketplace.json");
    QString readError;
    QJsonObject root = readObject(manifestPath, &readError);
    if (!readError.isEmpty()) {
        *error = readError;
        return false;
    }
    // 先落文件再改版本号：反过来的话，中途失败会留下「清单说新版、文件还是旧版」
    // 的镜像，那是宿主装出来会报错的组合。
    if (!extractZip(zipPath, host.mirrorRoot + QLatin1Char('/') +
                                 QString::fromLatin1(kPluginName),
                    host.manifestRel, error)) {
        return false;
    }
    QJsonArray rows = root.value(QStringLiteral("plugins")).toArray();
    bool hit = false;
    for (int i = 0; i < rows.size(); ++i) {
        QJsonObject row = rows.at(i).toObject();
        if (row.value(QStringLiteral("name")).toString() !=
            QLatin1String(kPluginName)) {
            continue;
        }
        row.insert(QStringLiteral("version"), version);
        rows[i] = row;
        hit = true;
        break;
    }
    if (!hit) {
        *error = QString::fromUtf8("市场清单里没有 %1").arg(QLatin1String(kPluginName));
        return false;
    }
    root.insert(QStringLiteral("plugins"), rows);
    return writeObject(manifestPath, root, error);
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
        host.kind = entry.kind;
        host.pluginId = QString::fromLatin1(entry.pluginId);
        host.manifestRel = QString::fromLatin1(entry.manifestRel);
        host.upgradable = entry.upgradable;
        host.configHome = home.absolutePath();
        host.cacheRoot = host.configHome + QLatin1Char('/') +
                         QString::fromLatin1(entry.cacheRel);
        host.registryPath = host.configHome + QLatin1Char('/') +
                            QString::fromLatin1(entry.registryRel);
        if (entry.mirrorRel != nullptr) {
            host.mirrorRoot = host.configHome + QLatin1Char('/') +
                              QString::fromLatin1(entry.mirrorRel);
        }

        QString error;
        const QJsonObject registry = readObject(host.registryPath, &error);
        if (!error.isEmpty()) {
            host.problem = error;
            hosts.append(host);
            continue;
        }
        RowRef ref;
        if (findRow(host, registry, &ref)) {
            host.installed = true;
            host.version = ref.row.value(QStringLiteral("version")).toString();
            host.installPath =
                ref.row.value(QStringLiteral("installPath")).toString();
        }

        // 启用状态按「插件 id」记，不按版本记，所以换版本不需要动配置文件。
        host.enabled = readEnabled(host, entry);
        hosts.append(host);
    }
    return hosts;
}

QString bundleName() {
    return QStringLiteral("DeepSeek Harness");
}

QString bundleExe() {
    return QStringLiteral("DeepSeek Harness.exe");
}

// profile 里那条依赖长这样：`"@local/agentrmc-bridge": "link:D:/repo/plugins/deepseek-harness"`。
// pnpm 也可能写成 file: / 绝对路径 / 版本号（从 npm 装的情况）；只有前两种能指到盘上
// 某个目录，其余按「装是装了，但读不到目录里的版本」处理。
QString dependencyPath(const QString &spec) {
    QString value = spec.trimmed();
    for (const QString &prefix :
         {QStringLiteral("link:"), QStringLiteral("file:")}) {
        if (value.startsWith(prefix, Qt::CaseInsensitive)) {
            value = value.mid(prefix.size());
            break;
        }
    }
    if (value.isEmpty() || value.startsWith(QLatin1Char('^')) ||
        value.startsWith(QLatin1Char('~')) ||
        value.startsWith(QStringLiteral("npm:")) ||
        value.startsWith(QStringLiteral("workspace:"))) {
        return {};
    }
    return QDir::fromNativeSeparators(value);
}

QList<Bundle> scanBundles(const QString &homePath) {
    QList<Bundle> bundles;
    const QString dshHome = homePath + QStringLiteral("/.dsh");
    const QDir profiles(dshHome + QStringLiteral("/profiles"));
    if (!profiles.exists()) {
        return bundles;  // 本机没装 DSH，一个都不列
    }
    const QStringList names =
        profiles.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &profile : names) {
        const QString manifest =
            profiles.filePath(profile + QStringLiteral("/package.json"));
        Bundle bundle;
        bundle.name = bundleName();
        bundle.exe = bundleExe();
        bundle.home = dshHome;
        bundle.profile = profile;

        QString error;
        const QJsonObject root = readObject(manifest, &error);
        if (!error.isEmpty()) {
            bundle.problem = error;
            bundles.append(bundle);
            continue;
        }
        const QJsonObject dependencies =
            root.value(QStringLiteral("dependencies")).toObject();
        const QString packageName = QString::fromLatin1(kBundlePackage);
        const QString spec =
            dependencies.value(packageName).toString().trimmed();
        if (spec.isEmpty()) {
            // profile 存在但没装我们的 bundle：是一种状态，不是故障。
            bundles.append(bundle);
            continue;
        }
        bundle.installed = true;
        const QJsonArray enabled =
            root.value(QStringLiteral("dsh"))
                .toObject()
                .value(QStringLiteral("profile"))
                .toObject()
                .value(QStringLiteral("bundles"))
                .toArray();
        bundle.enabled = enabled.contains(QJsonValue(packageName));

        const QString path = dependencyPath(spec);
        if (path.isEmpty()) {
            bundle.problem =
                QString::fromUtf8("依赖写法读不出目录：%1").arg(spec);
            bundles.append(bundle);
            continue;
        }
        bundle.path = QDir(path).absolutePath();
        QFile pkg(QDir(bundle.path).filePath(QStringLiteral("package.json")));
        if (!pkg.open(QIODevice::ReadOnly)) {
            bundle.problem =
                QString::fromUtf8("链接指向的目录读不到：%1").arg(bundle.path);
            bundles.append(bundle);
            continue;
        }
        QJsonParseError parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(pkg.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            bundle.problem =
                QString::fromUtf8("%1 不是合法 JSON").arg(pkg.fileName());
            bundles.append(bundle);
            continue;
        }
        bundle.version =
            doc.object().value(QStringLiteral("version")).toString().trimmed();
        bundles.append(bundle);
    }
    return bundles;
}

QString bundledBundleVersion(const QString &bundleDir) {
    QFile file(QDir(bundleDir).filePath(QStringLiteral("package.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object().value(QStringLiteral("version")).toString().trimmed();
}

QString installSentence(const QString &appDir) {
    // 一个落点：安装目录里的 plugins。zip、目录市场源、DSH 的 bundle 都在它里面，
    // 所以先报它，再报「它里面的哪个东西」——手动装的人不用记互不相干的具体文件名。
    const QString root =
        QDir::toNativeSeparators(appDir + QStringLiteral("/plugins"));
    const QString zip = root + QStringLiteral("\\agentrmc-bridge.zip");
    const QString bundle = root + QStringLiteral("\\deepseek-harness");
    const QString version =
        bundledVersion(appDir + QStringLiteral("/plugins/agentrmc-bridge"));
    // 占位符：%1 插件目录、%2 版本、%3 zip、%4 bundle 目录。
    // 注意 QString::arg(a,b,c,d) 的语义是「每次替换剩下编号最小的那个占位符」，
    // 并不按位置对应：串里少一个 %N，后面所有参数就整体前移一格——这里四个必须都在。
    return QString::fromUtf8(
               "帮我装或升级本机的 AgentRMC 桌宠联动插件 agentrmc-bridge（内置版本 %2）。"
               "插件都在这个目录里：%1 —— 下面按你自己所在的宿主只看对应那一行，"
               "做完把版本报给我。\n"
               "- Qoder / Qoder CN：用 Qoder 设置安装器装本地包 —— install_extension，"
               "参数 source=local、kind=plugin、filePath=%3；"
               "国际版和 Qoder CN 是两个独立 app，各装一次，装完重启那个 app 才真加载。"
               "没有安装器时才手工：侧边栏 扩展 → 插件 → 添加插件 → 上传插件，"
               "选 %1 目录里那个 agentrmc-bridge.zip。\n"
               "- ZCode / WorkBuddy：把这个目录本身加进应用市场：%1"
               "（它就是目录市场源，里面有 marketplace.json）。\n"
               "- DeepSeek Harness：它不是 zip，是 %1 里的 deepseek-harness 子目录 —— "
               "用 plugin_manager 的 install_bundle，target 给 %4；"
               "装完必须重启 DeepSeek Harness（Host 换新代码只能重启）。\n"
               "装好并启用之后，在我等确认、等选方案或任务结束时用桌宠提醒我。")
        .arg(root, version.isEmpty() ? QStringLiteral("未知") : version, zip,
             bundle);
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
    if (!host.upgradable) {
        return fail(QString::fromUtf8("这一家由宿主自己更新，不动它的注册表"));
    }
    if (!QFile::exists(zipPath)) {
        return fail(QString::fromUtf8("找不到插件包 %1").arg(zipPath));
    }
    if (!versionIsNewer(version, host.version)) {
        return fail(QString::fromUtf8("已装 %1，不比它新").arg(host.version));
    }

    const QString dest = host.cacheRoot + QLatin1Char('/') + version;
    const QString manifest = QDir(dest).filePath(host.manifestRel);

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
                     host.manifestRel});
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
            return fail(QString::fromUtf8("插件包里没有 %1").arg(host.manifestRel));
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
        QString extractError;
        if (!extractZip(zipPath, dest, host.manifestRel, &extractError)) {
            return fail(extractError);
        }
    }

    // 3) 备份原件，再改注册表里这一条（其余条目原样留着）。
    QString registryError;
    QJsonObject registry = readObject(host.registryPath, &registryError);
    if (!registryError.isEmpty()) {
        return fail(registryError);
    }
    RowRef ref;
    if (!findRow(host, registry, &ref)) {
        return fail(QString::fromUtf8("注册表里没有这条插件"));
    }
    QJsonObject row = ref.row;
    row.insert(QStringLiteral("version"), version);
    row.insert(QStringLiteral("installPath"), QDir::toNativeSeparators(dest));
    // 两家的时间字段不同名：Qoder 用 lastUpdated，ZCode 用 updatedAt。
    row.insert(host.kind == Kind::ZCode ? QStringLiteral("updatedAt")
                                        : QStringLiteral("lastUpdated"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    QString storeError;
    if (!storeRow(host, &registry, ref, row, &storeError)) {
        return fail(storeError);
    }

    if (!backupDir.isEmpty()) {
        QDir().mkpath(backupDir);
        // 毫秒戳：同一秒里连点两次升级也不能把备份覆盖掉。
        const QString stamp = QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMddTHHmmsszzz"));
        result.backupPath = QDir(backupDir).filePath(
            QStringLiteral("installed_plugins_%1.%2.bak").arg(host.pluginId, stamp));
        QFile::copy(host.registryPath, result.backupPath);
    }

    QString writeError;
    if (!writeObject(host.registryPath, registry, &writeError)) {
        return fail(writeError);
    }

    // 4) 读回核对：写没落地不能算升级成功。
    QString rereadError;
    const QJsonObject after = readObject(host.registryPath, &rereadError);
    RowRef afterRef;
    if (!findRow(host, after, &afterRef) ||
        afterRef.row.value(QStringLiteral("version")).toString() != version ||
        QDir::toNativeSeparators(
            afterRef.row.value(QStringLiteral("installPath")).toString()) !=
            QDir::toNativeSeparators(dest)) {
        return fail(QString::fromUtf8("写完读回不一致（备份在 %1）").arg(result.backupPath));
    }

    result.ok = true;
    result.detail = QString::fromUtf8("%1 → %2").arg(result.from, result.to);

    // 5) 目录市场源的镜像跟着一起走。放在注册表写成功之后：已装副本已经对了，
    //    镜像没跟上只影响宿主自己「有没有新版」的判断，不该把这次升级判成失败，
    //    但得让人看见——否则下次宿主重扫会莫名其妙把旧版装回来。
    if (!host.mirrorRoot.isEmpty() && QDir(host.mirrorRoot).exists()) {
        QString mirrorError;
        if (!syncMirror(host, zipPath, version, &mirrorError)) {
            result.detail += QString::fromUtf8("（市场镜像没同步：%1）")
                                 .arg(mirrorError);
        }
    }
    return result;
}

}  // namespace pluginupgrade
