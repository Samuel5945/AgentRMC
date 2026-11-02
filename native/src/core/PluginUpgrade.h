#pragma once

#include <QList>
#include <QString>

// 桌宠联动插件（agentrmc-bridge）在 Qoder 家族两个宿主里的版本对账与升级。
//
// 为什么要这一层：Qoder 把 zip 上传的插件记成 agentrmc-bridge@local，插件页的
// 「更新」按钮不会去拉新包；Qoder 和 Qoder CN 又是两个独立配置家（各自的注册表，
// 装一家另一家没有）。结果是「插件其实没升级」在宿主界面上完全看不出来——本机就
// 出现过安装包带 0.3.6、两边已装的还是 0.3.1 的情况。
//
// 只管 Qoder 家族：两家读同一份 zip、注册表格式相同，而且这是唯一没有目录市场源
// 的宿主。ZCode / WorkBuddy 走各自的市场目录，这里不碰。
// 只做「把已装的升到新版的」：没装过的宿主不替它建注册表——首次安装交给宿主自己的
// 安装器，避免用私有格式造出一个宿主不认的条目。
namespace pluginupgrade {

struct Host {
    QString name;          // Qoder / Qoder CN
    QString exe;           // 宿主进程映像名，用来判断改完要不要重启
    QString configHome;    // 例：C:/Users/PC/.qoder
    QString registryPath;  // plugins/installed_plugins_v2.json
    bool installed = false;   // 注册表里有 agentrmc-bridge@local 条目
    bool enabled = false;     // settings.json 里这个插件是开着的
    QString version;          // 注册表记的已装版本
    QString installPath;
    // 非空 = 这一家读不出来的原因。必须和「没有条目」「已是最新」区分开，
    // 否则读盘失败会被念成「已最新」。
    QString problem;
};

struct Result {
    bool ok = false;
    QString host;
    QString from;
    QString to;
    QString detail;  // 失败原因或落点，直接进事件行/toast
    QString backupPath;
};

// 随安装包带出去的插件版本：读 <pluginDir>/VERSION（构建时由它同步进各清单）。
// 空串 = 读不到。
QString bundledVersion(const QString &pluginDir);

// 扫 homePath 下的 Qoder 家族配置家；目录不存在就不列（那个宿主本机没装）。
QList<Host> scanHomes(const QString &homePath);

// 把 zip 解到 <configHome>/plugins/cache/local/agentrmc-bridge/<version> 并把
// 注册表指过去（原件先备份进 backupDir）。宿主不重启不会加载新代码——调用方
// 按 host.exe 是否活着提醒重启。
Result upgrade(const Host &host, const QString &zipPath, const QString &version,
               const QString &backupDir);

}  // namespace pluginupgrade
