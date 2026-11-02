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
bool makeZip(const QString &zipPath, const QString &stageDir,
             const QString &version) {
    writeFile(stageDir + "/.qoder-plugin/plugin.json",
              QByteArray("{\"name\":\"agentrmc-bridge\",\"version\":\"") +
                  version.toUtf8() + "\"}");
    writeFile(stageDir + "/hooks/agentrmc-notify.js",
              QByteArray("// hook\n"));
    writeFile(stageDir + "/VERSION", version.toUtf8() + "\n");
    QProcess tar;
    tar.start("C:/Windows/System32/tar.exe",
              {"-cf", QDir::toNativeSeparators(zipPath), "-C",
               QDir::toNativeSeparators(stageDir), ".qoder-plugin", "hooks",
               "VERSION"});
    if (!tar.waitForStarted(3000) || !tar.waitForFinished(8000)) {
        return false;
    }
    return tar.exitCode() == 0 && QFile::exists(zipPath);
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

    if (failures == 0) {
        std::printf("\nALL PASS (%d/%d)\n", total, total);
    } else {
        std::printf("\n%d FAILED (of %d)\n", failures, total);
    }
    return failures == 0 ? 0 : 1;
}
