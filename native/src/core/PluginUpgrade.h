#pragma once

#include <QList>
#include <QString>

// 桌宠联动插件（agentrmc-bridge）在各宿主里的版本对账与升级。
//
// 为什么要这一层：宿主的插件页不一定会去拉新包。Qoder 把 zip 上传的插件记成
// agentrmc-bridge@local，插件页的「更新」按钮不会去拉新包；Qoder 和 Qoder CN 又是
// 两个独立配置家（各自的注册表，装一家另一家没有）。结果是「插件其实没升级」在宿主
// 界面上完全看不出来——本机就出现过安装包带 0.3.6、两边已装的还是 0.3.1 的情况。
//
// 覆盖四家：
//   - Qoder / Qoder CN：读同一份 zip，注册表是 plugins[id] = [row]；
//   - ZCode：目录市场源（dev-agentrmc-local）只在宿主自己重扫时才重新拉，
//     重启宿主并不会重扫（2026-11-02 实测：仓库 17:34 升到 0.3.9，17:40 启动的
//     ZCode 仍停在 16:59 拉下来的 0.3.8），于是它也会落后一整版。
//     ZCode 的注册表是 plugins = [row...]，按 id 找。
//   - WorkBuddy AI（~/.workbuddy-ai）/ WorkBuddy（~/.workbuddy）：注册表与 Qoder
//     同形，但市场源条目带 autoUpdate，宿主自己会拉新版，所以**只检测不代升级**
//     （Host::upgradable = false）——和它抢注册表不值当。
//
// 只做「把已装的升到新版的」：没装过的宿主不替它建注册表——首次安装交给宿主自己的
// 安装器，避免用私有格式造出一个宿主不认的条目。
namespace pluginupgrade {

// 注册表格式差异，只影响「怎么找到并改写这一条」。
enum class Kind {
    Qoder,      // <home>/plugins/installed_plugins_v2.json，plugins 是 id→[row] 的对象
    ZCode,      // <home>/plugins/installed_plugins.json，plugins 是 [row...] 数组
    WorkBuddy,  // 与 Qoder 同形（id→[row]），只是文件名和 id 不同；只做检测不代升级
};

struct Host {
    QString name;          // Qoder / Qoder CN / ZCode
    QString exe;           // 宿主进程映像名，用来判断改完要不要重启
    QString configHome;    // 例：C:/Users/PC/.qoder、C:/Users/PC/.zcode/cli
    QString registryPath;  // 例：plugins/installed_plugins_v2.json
    Kind kind = Kind::Qoder;
    QString pluginId;      // agentrmc-bridge@local / agentrmc-bridge@dev-agentrmc-local
    QString cacheRoot;     // 已装版本的父目录，版本号是它的子目录名
    QString manifestRel;   // 包内清单路径（.qoder-plugin / .codebuddy-plugin）
    // 目录市场源在宿主里的整份镜像（只有 ZCode 有）。空 = 这家没有镜像要跟。
    QString mirrorRoot;
    bool installed = false;   // 注册表里有这一条
    bool enabled = false;     // 宿主配置里这个插件是开着的
    // 这一家由 AgentRMC 代升级吗？WorkBuddy 自己带 autoUpdate，只报状态不动它的盘。
    bool upgradable = true;
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

// 装在宿主**自己 profile** 里的联动插件包（目前只有 DeepSeek Harness）。
//
// DSH 的插件不是一个 zip、也没有 installed_plugins 注册表：装法是 profile
// （~/.dsh/profiles/<name>/package.json）里一条 `link:` 依赖，再加进
// `dsh.profile.bundles` 列表。所以它不能走 Host/registry 那套对账，需要单独扫。
//
// **不代升级**：升它要 pnpm 重装 + 重启 Host 进程（Host 换了 JavaScript 代际），
// 不是改注册表能完成的；AgentRMC 只报状态，并在「使用引导」里给出让 DSH 里的
// Agent 自己装/升的说法。
struct Bundle {
    QString name;     // DeepSeek Harness
    QString exe;      // 宿主进程映像名，用来判断在不在跑
    QString home;     // 配置家：<home>/.dsh
    QString profile;  // profile 名，例如 desktop
    QString path;     // link 指向的 bundle 目录（空 = 没装）
    QString version;  // 那个目录 package.json 里的 version
    bool installed = false;
    bool enabled = false;  // profile 的 dsh.profile.bundles 里有没有它
    // 非空 = 这一份读不出来的原因；必须和「没装」「已最新」分开。
    QString problem;
};

// 扫 homePath/.dsh/profiles/*/package.json。没有 ~/.dsh 就返回空列表。
QList<Bundle> scanBundles(const QString &homePath);

// 随安装包带出去的 bundle 版本：读 <bundleDir>/package.json 的 version。
// 空串 = 读不到（安装包里没带这个目录，或文件坏了）。
QString bundledBundleVersion(const QString &bundleDir);

// bundle 的名字与进程映像名（写死在这，UI 与扫描两边共用一个来源）。
QString bundleName();
QString bundleExe();

// 「复制安装话术」里那句话：一句话覆盖所有宿主，Agent 只看自己在哪一行。
// appDir = exe 所在目录；插件都在 <appDir>/plugins 里，所以话术只报这一个落点。
//
// 放在这里而不是散在 UI 里，是因为它会被整段复制进别人的对话框——路径写错、
// 占位符漏替换都看不出来。有测试盯着（plugin_upgrade_test）。
QString installSentence(const QString &appDir);

// 随安装包带出去的插件版本：读 <pluginDir>/VERSION（构建时由它同步进各清单）。
// 空串 = 读不到。
QString bundledVersion(const QString &pluginDir);

// 扫 homePath 下的宿主配置家；目录不存在就不列（那个宿主本机没装）。
QList<Host> scanHomes(const QString &homePath);

// 把 zip 解到 <cacheRoot>/<version> 并把注册表指过去（原件先备份进 backupDir）。
// 宿主不重启不会加载新代码——调用方按 host.exe 是否活着提醒重启。
Result upgrade(const Host &host, const QString &zipPath, const QString &version,
               const QString &backupDir);

}  // namespace pluginupgrade
