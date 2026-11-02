// PluginUpgrade 自测：全程在临时目录里造一个假的宿主配置家，绝不碰真实的
// ~/.qoder(-cn)。zip 用系统 tar 现造，覆盖「先断言再写盘」这条主线。
#include "core/PluginUpgrade.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;
int total = 0;

void check(const char *name, bool ok) {
    ++total;
    if (ok) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

void writeFile(const QString &path, const QByteArray &text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.write(text);
    file.close();
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray registryText(const QString &version, const QString &installPath) {
    QJsonObject row;
    row.insert("scope", "user");
    row.insert("installPath", installPath);
    row.insert("version", version);
    row.insert("displayName", "AgentRMC 桌宠联动");
    row.insert("appRowId", "local:plugin:agentrmc-bridge");
    QJsonObject ours;
    ours.insert("agentrmc-bridge@local", QJsonArray{row});
    // 陪跑条目：验证改写不会把别人家的插件记录弄丢。
    QJsonObject other;
    other.insert("scope", "user");
    other.insert("installPath", "C:/other/cloudflare/1.0.0");
    other.insert("version", "1.0.0");
    ours.insert("cloudflare@qoder-marketplace", QJsonArray{other});
    QJsonObject root;
    root.insert("version", 2);
    root.insert("plugins", ours);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// 造一个和 zip-plugin.ps1 同构的包：条目用 / 分隔，清单里带 version。
// 两家宿主的清单路径不同（Qoder 读 .qoder-plugin，ZCode 读 .codebuddy-plugin），
// 包里本来就各带一份，这里也照做。
bool makeZip(const QString &zipPath, const QString &stageDir,
             const QString &version) {
    const QByteArray manifest =
        QByteArray("{\"name\":\"agentrmc-bridge\",\"version\":\"") +
        version.toUtf8() + "\"}";
    writeFile(stageDir + "/.qoder-plugin/plugin.json", manifest);
    writeFile(stageDir + "/.codebuddy-plugin/plugin.json", manifest);
    writeFile(stageDir + "/hooks/agentrmc-notify.js",
              QByteArray("// hook\n"));
    writeFile(stageDir + "/VERSION", version.toUtf8() + "\n");
    QProcess tar;
    tar.start("C:/Windows/System32/tar.exe",
              {"-cf", QDir::toNativeSeparators(zipPath), "-C",
               QDir::toNativeSeparators(stageDir), ".qoder-plugin",
               ".codebuddy-plugin", "hooks", "VERSION"});
    if (!tar.waitForStarted(3000) || !tar.waitForFinished(8000)) {
        return false;
    }
    return tar.exitCode() == 0 && QFile::exists(zipPath);
}

// ZCode 的注册表是 v1 形状：plugins 是数组，按 id 找条目。
QByteArray zcodeRegistryText(const QString &version,
                             const QString &installPath) {
    QJsonObject ours;
    ours.insert("id", "agentrmc-bridge@dev-agentrmc-local");
    ours.insert("name", "agentrmc-bridge");
    ours.insert("marketplace", "dev-agentrmc-local");
    ours.insert("version", version);
    ours.insert("installPath", installPath);
    ours.insert("updatedAt", "2026-11-02T08:07:31.153Z");
    ours.insert("source", "./agentrmc-bridge");
    QJsonObject other;
    other.insert("id", "github@zcode-plugins-official");
    other.insert("name", "github");
    other.insert("version", "0.1.4");
    other.insert("installPath", "C:/other/github/0.1.4");
    QJsonObject root;
    root.insert("version", 1);
    root.insert("plugins", QJsonArray{other, ours});
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir base;
    if (!base.isValid()) {
        std::printf("FAIL temp dir\n");
        return 1;
    }
    const QString home = base.path() + "/home";
    const QString intl = home + "/.qoder";
    const QString cn = home + "/.qoder-cn";
    const QString backup = base.path() + "/backup";
    const QString zip = base.path() + "/plugin.zip";
    const QString oldPath = intl + "/plugins/cache/local/agentrmc-bridge/0.3.1";

    writeFile(intl + "/plugins/installed_plugins_v2.json",
              registryText("0.3.1", QDir::toNativeSeparators(oldPath)));
    writeFile(intl + "/settings.json",
              QByteArray("{\"enabledPlugins\":{\"agentrmc-bridge@local\":true}}"));
    // CN：注册表在，但没有我们这一条（本机只装过别的插件）。
    QJsonObject cnPlugins;
    cnPlugins.insert("other@market", QJsonArray{QJsonObject{{"version", "1.0.0"}}});
    QJsonObject cnRoot;
    cnRoot.insert("plugins", cnPlugins);
    writeFile(cn + "/plugins/installed_plugins_v2.json",
              QJsonDocument(cnRoot).toJson(QJsonDocument::Indented));
    if (!makeZip(zip, base.path() + "/stage", "0.3.7")) {
        std::printf("FAIL zip\n");
        return 1;
    }

    const QList<pluginupgrade::Host> hosts = pluginupgrade::scanHomes(home);
    check("两家配置家都扫到", hosts.size() == 2);
    const pluginupgrade::Host qoder = hosts.at(0);
    const pluginupgrade::Host qoderCn = hosts.at(1);
    check("国际版按注册表报已装版本且启用中",
          qoder.name == "Qoder" && qoder.installed &&
              qoder.version == "0.3.1" && qoder.enabled && qoder.problem.isEmpty());
    check("CN 没装过 = installed false，不当成读取失败",
          qoderCn.name == "Qoder CN" && !qoderCn.installed &&
              qoderCn.problem.isEmpty());
    check("bundledVersion 读包根 VERSION",
          pluginupgrade::bundledVersion(base.path() + "/stage") == "0.3.7" &&
              pluginupgrade::bundledVersion(base.path() + "/nothing").isEmpty());

    const QByteArray registryBefore =
        readAll(intl + "/plugins/installed_plugins_v2.json");

    const pluginupgrade::Result sameVersion =
        pluginupgrade::upgrade(qoder, zip, "0.3.1", backup);
    check("同版不写盘：断言「不比它新」",
          !sameVersion.ok && sameVersion.detail.contains("不比它新") &&
              readAll(intl + "/plugins/installed_plugins_v2.json") ==
                  registryBefore);

    const pluginupgrade::Result mismatch =
        pluginupgrade::upgrade(qoder, zip, "9.9.9", backup);
    check("清单版本对不上时先断言失败，一个目录都不建",
          !mismatch.ok && mismatch.detail.contains("对不上") &&
              !QDir().exists(intl + "/plugins/cache/local/agentrmc-bridge/9.9.9") &&
              readAll(intl + "/plugins/installed_plugins_v2.json") ==
                  registryBefore);

    const pluginupgrade::Result absent =
        pluginupgrade::upgrade(qoderCn, zip, "0.3.7", backup);
    check("没装过的宿主不替它造注册表",
          !absent.ok && absent.detail.contains("没装过") &&
              !QDir().exists(cn + "/plugins/cache/local/agentrmc-bridge"));

    const pluginupgrade::Result done =
        pluginupgrade::upgrade(qoder, zip, "0.3.7", backup);
    const QJsonObject afterRow =
        QJsonDocument::fromJson(
            readAll(intl + "/plugins/installed_plugins_v2.json"))
            .object()
            .value("plugins")
            .toObject()
            .value("agentrmc-bridge@local")
            .toArray()
            .first()
            .toObject();
    check("升级后注册表指向新版目录",
          done.ok && afterRow.value("version").toString() == "0.3.7" &&
              QDir::toNativeSeparators(afterRow.value("installPath").toString()) ==
                  QDir::toNativeSeparators(
                      intl + "/plugins/cache/local/agentrmc-bridge/0.3.7"));
    check("新版清单真的解出来了",
          QFile::exists(intl + "/plugins/cache/local/agentrmc-bridge/0.3.7/hooks/"
                        "agentrmc-notify.js"));
    check("别家插件条目没被动",
          QJsonDocument::fromJson(readAll(intl + "/plugins/installed_plugins_v2.json"))
              .object()
              .value("plugins")
              .toObject()
              .contains("cloudflare@qoder-marketplace"));
    check("改写前留了原件备份",
          done.backupPath.endsWith(".bak") &&
              readAll(done.backupPath).contains("0.3.1"));

    // 重扫一次再试：upgrade() 信调用方给的 Host.version，拿升级前那份旧快照去调
    // 等于自欺——真机上的「升级」按钮每次都是重扫后才决定要不要动盘。
    const pluginupgrade::Host current = pluginupgrade::scanHomes(home).at(0);
    const pluginupgrade::Result again =
        pluginupgrade::upgrade(current, zip, "0.3.7", backup);
    check("重复升级不报错也不重复解包",
          !again.ok && again.detail.contains("不比它新") &&
              current.version == "0.3.7");

    writeFile(intl + "/plugins/installed_plugins_v2.json", QByteArray("{ not json"));
    const QList<pluginupgrade::Host> broken = pluginupgrade::scanHomes(home);
    check("注册表坏了要报成因，不能念成已最新",
          broken.at(0).problem.contains("JSON") && !broken.at(0).installed);

    // ZCode：配置家在 .zcode/cli，注册表是 plugins 数组，市场源还有一份整份镜像。
    const QString zhome = base.path() + "/zhome";
    const QString zhomeCli = zhome + "/.zcode/cli";
    writeFile(zhomeCli + "/plugins/installed_plugins.json",
              zcodeRegistryText("0.3.1",
                                QDir::toNativeSeparators(
                                    zhomeCli +
                                    "/plugins/cache/dev-agentrmc-local/"
                                    "agentrmc-bridge/0.3.1")));
    writeFile(zhomeCli + "/config.json",
              QByteArray("{\"plugins\":{\"enabledPlugins\":"
                         "{\"agentrmc-bridge@dev-agentrmc-local\":true}}}"));
    const QString zmirror = zhomeCli + "/plugins/marketplaces/dev-agentrmc-local";
    writeFile(zmirror + "/marketplace.json",
              QByteArray("{\"name\":\"dev-agentrmc-local\",\"plugins\":[{\"name\":"
                         "\"agentrmc-bridge\",\"source\":\"./agentrmc-bridge\","
                         "\"version\":\"0.3.1\"}]}"));

    const QList<pluginupgrade::Host> zhosts = pluginupgrade::scanHomes(zhome);
    check("ZCode 按数组注册表扫到，启用状态取自 config.json.plugins",
          zhosts.size() == 1 && zhosts.at(0).name == "ZCode" &&
              zhosts.at(0).installed && zhosts.at(0).version == "0.3.1" &&
              zhosts.at(0).enabled && zhosts.at(0).problem.isEmpty());

    const pluginupgrade::Result zdone =
        pluginupgrade::upgrade(zhosts.at(0), zip, "0.3.7", backup);
    const QJsonArray zrows = QJsonDocument::fromJson(
                                 readAll(zhomeCli +
                                         "/plugins/installed_plugins.json"))
                                 .object()
                                 .value("plugins")
                                 .toArray();
    check("ZCode 升级后数组里那一条指向新版目录",
          zdone.ok && zrows.size() == 2 &&
              zrows.at(1).toObject().value("version").toString() == "0.3.7" &&
              QDir::toNativeSeparators(
                  zrows.at(1).toObject().value("installPath").toString()) ==
                  QDir::toNativeSeparators(
                      zhomeCli + "/plugins/cache/dev-agentrmc-local/"
                                 "agentrmc-bridge/0.3.7"));
    check("ZCode 别家插件条目没被动",
          zrows.at(0).toObject().value("id").toString() ==
              "github@zcode-plugins-official");
    check("ZCode 清单按 .codebuddy-plugin 解出来",
          QFile::exists(zhomeCli + "/plugins/cache/dev-agentrmc-local/"
                                   "agentrmc-bridge/0.3.7/hooks/"
                                   "agentrmc-notify.js"));
    check("ZCode 市场镜像的文件和版本号一起跟到新版",
          QFile::exists(zmirror + "/agentrmc-bridge/hooks/agentrmc-notify.js") &&
              QJsonDocument::fromJson(readAll(zmirror + "/marketplace.json"))
                      .object()
                      .value("plugins")
                      .toArray()
                      .at(0)
                      .toObject()
                      .value("version")
                      .toString() == "0.3.7");
    check("ZCode 重扫后报已装齐",
          pluginupgrade::scanHomes(zhome).at(0).version == "0.3.7");

    // WorkBuddy：注册表与 Qoder 同形（id→[row]），但只做检测，不代升级。
    const QString whome = base.path() + "/whome";
    const QString wbAi = whome + "/.workbuddy-ai";
    const QString wbClassic = whome + "/.workbuddy";
    const QByteArray wbRegistry =
        QByteArray("{\"version\":2,\"plugins\":{\"agentrmc-bridge@dev-agentrmc-local\":"
                   "[{\"scope\":\"user\",\"installPath\":\"C:/wb/0.3.1\","
                   "\"version\":\"0.3.1\",\"lastUpdated\":\"2026-11-02T00:05:03Z\"}]}}");
    const QByteArray wbSettings =
        QByteArray("{\"enabledPlugins\":{\"agentrmc-bridge@dev-agentrmc-local\":true}}");
    writeFile(wbAi + "/plugins/installed_plugins.json", wbRegistry);
    writeFile(wbAi + "/settings.json", wbSettings);
    // 经典版没装过这个插件，但配置家存在——要报「未装」，不能报读不出。
    writeFile(wbClassic + "/plugins/installed_plugins.json",
              QByteArray("{\"version\":2,\"plugins\":{}}"));
    writeFile(wbClassic + "/settings.json", QByteArray("{}"));

    const QList<pluginupgrade::Host> whosts = pluginupgrade::scanHomes(whome);
    check("两家 WorkBuddy 都扫到，AI 版在前",
          whosts.size() == 2 && whosts.at(0).name == "WorkBuddy AI" &&
              whosts.at(1).name == "WorkBuddy");
    check("WorkBuddy AI 按 Qoder 同形注册表报已装且启用",
          whosts.at(0).installed && whosts.at(0).version == "0.3.1" &&
              whosts.at(0).enabled && whosts.at(0).problem.isEmpty());
    check("WorkBuddy 经典版没装过 = installed false，不当成读取失败",
          !whosts.at(1).installed && whosts.at(1).problem.isEmpty());
    const pluginupgrade::Result wbDone =
        pluginupgrade::upgrade(whosts.at(0), zip, "0.3.7", backup);
    check("WorkBuddy 只检测不代升级，一个目录都不建",
          !wbDone.ok && wbDone.detail.contains("由宿主自己更新") &&
              !QDir().exists(wbAi + "/plugins/cache/dev-agentrmc-local/agentrmc-bridge") &&
              readAll(wbAi + "/plugins/installed_plugins.json") == wbRegistry);

    // DeepSeek Harness：装在宿主 profile 里的 bundle，没有注册表可读，
    // 只能从 ~/.dsh/profiles/<profile>/package.json 的 link 依赖反推。
    const QString dhome = base.path() + "/dhome";
    const QString bundleDir = base.path() + "/bundle";
    writeFile(bundleDir + "/package.json",
              QByteArray("{\"name\":\"@local/agentrmc-bridge\",\"version\":\"0.3.9\"}"));
    const QByteArray profileText =
        QByteArray("{\"name\":\"dsh-profile-desktop\",\"dependencies\":"
                   "{\"@local/agentrmc-bridge\":\"link:") +
        QDir::fromNativeSeparators(bundleDir).toUtf8() +
        "\"},\"dsh\":{\"profile\":{\"bundles\":[\"@deepseek-ai/dsh-base\","
        "\"@local/agentrmc-bridge\"]}}}";
    writeFile(dhome + "/.dsh/profiles/desktop/package.json", profileText);
    // 另一个 profile 没装：要报「未装」，不能报读不出。
    writeFile(dhome + "/.dsh/profiles/other/package.json",
              QByteArray("{\"name\":\"dsh-profile-other\",\"dependencies\":{}}"));

    const QList<pluginupgrade::Bundle> bundles =
        pluginupgrade::scanBundles(dhome);
    check("DSH 的两个 profile 都扫到，按名字排",
          bundles.size() == 2 && bundles.at(0).profile == "desktop" &&
              bundles.at(1).profile == "other");
    check("读到 link 指向目录里的版本，且认得出它已启用",
          bundles.at(0).installed && bundles.at(0).version == "0.3.9" &&
              bundles.at(0).enabled && bundles.at(0).problem.isEmpty() &&
              QDir::fromNativeSeparators(bundles.at(0).path) ==
                  QDir::fromNativeSeparators(bundleDir));
    check("profile 里没装 = installed false，不当成读取失败",
          !bundles.at(1).installed && bundles.at(1).problem.isEmpty());
    check("bundle 名字与进程映像名对齐原生白名单",
          pluginupgrade::bundleName() == "DeepSeek Harness" &&
              pluginupgrade::bundleExe() == "DeepSeek Harness.exe");
    check("bundledBundleVersion 读 package.json 的 version",
          pluginupgrade::bundledBundleVersion(bundleDir) == "0.3.9" &&
              pluginupgrade::bundledBundleVersion(base.path() + "/nothing").isEmpty());
    check("本机没装 DSH 时一条都不列",
          pluginupgrade::scanBundles(base.path() + "/nohome").isEmpty());

    // enabled 只看 dsh.profile.bundles：依赖装了但没列进去 = 装了没启用。
    writeFile(dhome + "/.dsh/profiles/desktop/package.json",
              QByteArray("{\"dependencies\":{\"@local/agentrmc-bridge\":\"link:") +
                  QDir::fromNativeSeparators(bundleDir).toUtf8() +
                  "\"},\"dsh\":{\"profile\":{\"bundles\":[]}}}");
    const pluginupgrade::Bundle listed = pluginupgrade::scanBundles(dhome).at(0);
    check("依赖在但 bundles 里没有 = 已装未启用",
          listed.installed && !listed.enabled && listed.version == "0.3.9");

    // 链接指向的目录被删了：要报成因，不能念成已最新。
    writeFile(dhome + "/.dsh/profiles/desktop/package.json",
              QByteArray("{\"dependencies\":{\"@local/agentrmc-bridge\":"
                         "\"link:D:/gone/deepseek-harness\"}}"));
    const pluginupgrade::Bundle brokenBundle =
        pluginupgrade::scanBundles(dhome).at(0);
    check("链接指向的目录不存在时报成因",
          brokenBundle.installed && brokenBundle.problem.contains("读不到") &&
              brokenBundle.version.isEmpty());

    // 一句话安装话术：整段会被复制进别人的对话框，路径错一个字都看不出来，
    // 所以这里把它当数据验——尤其防「占位符错位」（QString::arg 的多参重载按
    // 编号最小者替换，串里少一个 %N 后面全整体前移）。
    const QString appDir = base.path() + "/app";
    writeFile(appDir + "/plugins/agentrmc-bridge/VERSION", QByteArray("0.3.7\n"));
    const QString sentence = pluginupgrade::installSentence(appDir);
    const QString plugins = QDir::toNativeSeparators(appDir + "/plugins");
    check("话术里只有一个落点：安装目录的 plugins",
          sentence.contains(QString::fromUtf8("插件都在这个目录里：") + plugins));
    check("话术带内置版本",
          sentence.contains(QString::fromUtf8("内置版本 0.3.7")));
    check("Qoder 那行的 filePath 是 plugins 里的 zip",
          sentence.contains(QString::fromUtf8("filePath=%1\\agentrmc-bridge.zip")
                                .arg(plugins)));
    check("ZCode / WorkBuddy 那行给的是 plugins 目录本身",
          sentence.contains(QString::fromUtf8("加进应用市场：%1（").arg(plugins)));
    check("DSH 那行给的是 plugins 里的 deepseek-harness 子目录",
          sentence.contains(plugins + QString::fromUtf8("\\deepseek-harness")) &&
              sentence.contains(QStringLiteral("install_bundle")));
    check("没有漏替换的占位符",
          !sentence.contains(QLatin1Char('%')) &&
              !sentence.contains(QStringLiteral("agentrmc-bridge.zip\\agentrmc-bridge.zip")));

    if (failures == 0) {
        std::printf("\nALL PASS (%d/%d)\n", total, total);
    } else {
        std::printf("\n%d FAILED (of %d)\n", failures, total);
    }
    return failures == 0 ? 0 : 1;
}
