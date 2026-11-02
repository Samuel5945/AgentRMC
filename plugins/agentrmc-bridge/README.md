# AgentRMC Bridge（ZCode / Qoder 插件）

把 [AgentRMC](https://github.com/AgentRMC/Agentrmc) 桌宠接进你的 Agent：

- **自动提醒（hooks）**：三类提醒都出声，等确认 / 等选方案时直接把该 Agent 主
  窗口拉到前台——等工具授权（`PermissionRequest`，念「等你确认 Bash：命令…」）、
  等选方案（念「等你选方案」；Qoder 和 WorkBuddy 里走 `PreToolUse` +
  `matcher:"AskUserQuestion"`，见下）、一轮回答结束（`Stop`，念「X：<最终回复>」，
  取不到 transcript 时退回「X 已完成任务，等你回来验收」）。完成提醒**不自动抢
  前台**，但气泡可点，点一下把 Agent 窗口叫回来。AgentRMC 没开就静默跳过（<100ms），
  不拖慢会话。
- **主动交互（MCP）**：三个工具——`pet_notify`（阶段进度气泡）、`pet_speak`
  （让桌宠 TTS 说一句话）、`pet_state`（查桌宠状态），配 `agentrmc-pet` skill
  教模型何时用、怎么不刷屏。
- **传输**：全部走 AgentRMC 本机回环口 `127.0.0.1:45915`（AgentBridge），
  无密钥、无外部依赖（hooks 与 MCP server 都是零依赖 node 脚本）。

## 宿主差异

**Qoder 和 Qoder CN 是两个独立 app**：各自的配置家（`~/.qoder` 与 `~/.qoder-cn`）、各自的插件注册表、
不同的进程映像名（`Qoder.exe` 与 `Qoder CN.exe`）。在一个里装好，另一个不会自动有；同一个 zip
两边通用，装完各落各家。

|              | ZCode                                | WorkBuddy / WorkBuddy AI             | Qoder                                | Qoder CN                             |
| ------------ | ------------------------------------ | ------------------------------------ | ------------------------------------ | ------------------------------------ |
| 清单         | `.zcode-plugin/plugin.json`          | `.codebuddy-plugin/plugin.json`      | `.qoder-plugin/plugin.json`          | 同 Qoder（装在 `~/.qoder-cn`）       |
| hooks 配置   | `hooks/hooks.json`（manifest 给目录）| 根 `hooks/`（永远加载）+ manifest 追加 `hooks-workbuddy/hooks.json` | `hooks-qoder/hooks.json`（要给到 .json 文件）| 同 Qoder      |
| 拉前台       | 原生端缺省找 `ZCode.exe`             | 自检 → `WorkBuddy.exe` / `WorkBuddyAI.exe` | 自检 → `Qoder.exe`                   | 自检 → `Qoder CN.exe`                |
| 宿主署名     | 缺省 ZCode                           | 自检 → `WorkBuddy` / `WorkBuddy AI`  | 自检 → 气泡念「Qoder」               | 自检 → 气泡念「Qoder CN」            |

### 署名与拉前台靠自检，不写死

0.2.0 把 Qoder 那份 hooks 的 `raise` 写死成 `Qoder CN.exe`，于是在国际版里桌宠会把**另一个 app**
的窗口拽到前台；手工改已装副本又会被下一次更新/重装覆盖回去。0.3.0 起 `lib/host.js` 自己认宿主，
判据按可靠性排序：

1. argv 显式给了「宿主名 + 进程映像名」→ 照办（老的手写配置行为不变）；
2. 脚本自身安装路径落在哪个配置家（每个 app 只会把插件解到自己的 `~/.qoder(-cn)/plugins/cache/` 下，
   这是最硬的证据，不依赖宿主注入什么）；
3. 宿主注入的 `QODER_CONFIG_DIR` / `QODERCN_CONFIG_DIR` / `QODER_PLUGIN_ROOT` / `QODER_PRODUCT_ID`；
4. 认不出变体但确认是 Qoder 家族 → 按国际版（绝不退化成去找 `ZCode.exe`）；都不是才按 ZCode / WorkBuddy。

原生端按进程映像名**精确**匹配窗口，`Qoder.exe` 不会误命中 `Qoder CN.exe`，两个 app 各拉各的窗。
`AGENTRMC_DRY_RUN=1` 让 hook 只打印将要发送的 payload（含判定来源 `source`）而不真发，用来验判据。

### WorkBuddy / WorkBuddy AI 也是两个独立 app（0.3.3）

`WorkBuddy`（`WorkBuddy.exe`，配置家 `~/.workbuddy`）和 `WorkBuddy AI`（`WorkBuddyAI.exe`，
配置家 `~/.workbuddy-ai`）是两个独立安装：插件缓存、市场注册、进程名都不同。`lib/host.js`
从插件安装路径判变体——`/.workbuddy-ai/` → AI，`/.workbuddy/` → 旧版；路径判不出才按产品名
兜底 AI，并在 DRY_RUN 里标 `workbuddy-unknown-variant`，不再把「任意 WorkBuddy 都拉 AI」
当成正常。两个 app 需要**各自安装/升级**本插件；同一个 zip 两边通用，装完各落各的缓存，
`raise` 只拉自己那个 exe，找不到就静默，不会去拉另一个版本。

### 提醒分别走哪些事件

| 提醒       | Qoder / Qoder CN                                  | WorkBuddy                                          | ZCode                                 |
| ---------- | ------------------------------------------------- | -------------------------------------------------- | ------------------------------------- |
| 等工具授权 | `PermissionRequest`                               | `PermissionRequest`                                | `PermissionRequest`                   |
| 等选方案   | `PreToolUse` + `matcher:"AskUserQuestion"`        | `PreToolUse` + `matcher:"AskUserQuestion"`         | `PermissionRequest`（tool_name 命中） |
| 一轮结束   | `Stop`                                            | `Stop`                                             | `Stop`                                |

「等选方案」在 Qoder 里必须挂 `PreToolUse`：拿 `~/.qoder/logs/runs/<run>/qodercli.log` 的
`hook.started` 对账实测，Qoder 对 `AskUserQuestion` **不发** `PermissionRequest`（只有一条
`source="runtime"` 的 SDK `PreToolUse:AskUserQuestion`），所以 0.2.0 里那个按 `tool_name`
判定的分支在这条链路上永远进不去——桌宠选方案时不出声就是这么来的。脚本收到 `question`
后仍按 `permission` 发给原生端：原生端只对 `stop` / `permission` 出语音并拉前台
（`MainWindow.cpp` 的 notify 分支），别的 kind 只冒气泡，这样不用动原生。`matcher` 万一不被
宿主执行，脚本会再判一次 `tool_name`，认不出来直接静默——`PreToolUse` 每个工具都触发，
宁可少一条也不能把提醒刷成屏。

**WorkBuddy 是同一个病**（0.3.2 修）：给已装副本的 hook 脚本插探针后跑一次
`AskUserQuestion`，探针日志一条都没有——文件型 hook 根本没被调用。原因在 CLI 源码里：
插件 hook（`hookManager.executeHooks`）只在工具真的要走授权决策时才派发
`PermissionRequest`，而 `AskUserQuestion` 走的是「问一句」的审批队列，那条路径只查 SDK 注册的
回调（`sdkHooksManager`），不查 settings / 插件里的 hooks。所以 WorkBuddy 也改成挂
`PreToolUse` + `matcher:"AskUserQuestion"`。

为什么不直接写进共用的根 `hooks/hooks.json`：那份 ZCode 也在用，ZCode 对未知事件的处理没验过，
不拿它冒险。WorkBuddy 的 `hooks/hooks.json` **永远加载**、manifest 里的 `hooks` 再合并一次，
所以把 `PreToolUse` 单独放 `hooks-workbuddy/hooks.json` 由 manifest 声明，两边都拿到、
也不会重复触发（根那份里没有 `PreToolUse`）。


Qoder 同时导出 `${QODER_PLUGIN_ROOT}` 和 `${CLAUDE_PLUGIN_ROOT}`，也认 manifest 里的
`mcpServers`，所以只有清单目录名和 hooks 那份必须分开。两条链路的 hook 事件名一致
（`PermissionRequest` / `Stop`）。

## 前提

- ZCode 或 Qoder（桌面版）+ 本插件；
- AgentRMC 安装包需带 AgentBridge；**按 `raise` 拉前台要 2026-11-02 起的构建**，
  旧版只提醒不拉窗口；
- `node` 在 PATH（hooks 与 MCP 都用 `node` 调起）。

## 安装

对外的交付形态：本插件随 AgentRMC 安装包一起落到 `安装目录\plugins\`（源目录 +
`agentrmc-bridge.zip`），用户在 App 的「使用引导 → 接入 AI Agent」里点「复制安装话术」，
把那句话发给正在干活的 Agent，由 Agent 自己装。手工路径如下。

- **ZCode**：应用市场添加本地目录（本仓库 `plugins/`，内含 `marketplace.json`），
  安装「AgentRMC 桌宠联动」即可；hooks 与 MCP 随插件自动生效。
- **Qoder Desktop**（国际版与 Qoder CN 各装一次）：侧边栏 扩展 → 插件 → 添加插件 → 上传插件（ZIP）。
  Desktop 只有「官方市场」和「本地 ZIP」两种来源，不认「本地目录市场源」（那条路只有 CLI 的代码
  里在找 `marketplace.json`）。把 `plugins/agentrmc-bridge/` 打成 zip 上传即可，装完落在
  `~/.qoder/plugins/cache/local/agentrmc-bridge/<version>`（Qoder CN 落在 `~/.qoder-cn/...`）。
  两个 app 的注册表互不相干，**在一个里装好另一个仍然没有**；装对之后两边的气泡署名和拉前台
  目标都由自检给出，不用再改任何文件。
- 打 Qoder 用的 zip 有两个坑：条目名必须用 `/` 分隔（PowerShell `Compress-Archive`
  写成 `\`，Qoder 解不出清单，会报 `EXTENSION_PLUGIN_VALIDATION_FAILED`）；manifest 的
  `hooks` 要指到 `./hooks-qoder/hooks.json` 这个文件，给目录报 `Path must end with .json`。

> 如果你之前手工在 `~/.zcode/cli/config.json` 里配过
> `PermissionRequest` / `Stop` hook（指向 `~/.zcode/hooks/agentrmc-notify.js`），
> 装完插件请删掉那两条，避免通知重复。

## 文件

- `.zcode-plugin/plugin.json`、`.qoder-plugin/plugin.json`、`.codebuddy-plugin/`：各宿主自己的清单；
- `hooks/hooks.json`、`hooks-qoder/hooks.json`、`hooks-workbuddy/hooks.json`：各自的 hook 注册
  （WorkBuddy 根那份永远加载，manifest 再追加「等选方案」这份）；
- `hooks/agentrmc-notify.js`：共用的通知脚本（argv 可给宿主署名和要拉前台的进程映像名，
  一般不给，由自检决定）；
- `lib/host.js`：宿主自检（判据见上），hooks 与 MCP server 共用；
- `mcp/server.js`：stdio MCP server（pet_notify / pet_speak / pet_state）；
- `skills/agentrmc-pet/SKILL.md`：模型使用指南；
- `test/host.test.js`：`node test/host.test.js`，11 个用例覆盖四种宿主 + 两个 Qoder 变体的
  端到端判定（会把插件树复制到假装的 `~/.qoder` / `~/.qoder-cn` 里跑真脚本）。
