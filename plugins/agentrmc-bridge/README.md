# AgentRMC Bridge（ZCode / Qoder 插件）

把 [AgentRMC](https://github.com/AgentRMC/Agentrmc) 桌宠接进你的 Agent：

- **自动提醒（hooks）**：四类提醒都出声，等确认 / 等选方案 / 等回话时直接把该 Agent
  主窗口拉到前台——等工具授权（`PermissionRequest`，念「等你确认 Bash：命令…」）、
  等选方案（念「等你选方案」；Qoder 和 WorkBuddy 里走 `PreToolUse` +
  `matcher:"AskUserQuestion"`，见下）、**等你回话**（`Stop` 时回复以问句收尾 →
  念「X 在等你回话」，气泡带那句问话；正文提问没有选项卡片，宿主不为它发任何等待
  事件，只能从 transcript 判，见 `trailingQuestion`）、一轮回答结束（`Stop`，
  念「X：<最终回复>」，取不到 transcript 时退回「X 已完成任务，等你回来验收」）。
  完成提醒**不自动抢前台**，但气泡可点，点一下把 Agent 窗口叫回来。AgentRMC 没开就
  静默跳过（<100ms），不拖慢会话。
- **主动交互（MCP）**：四个工具——`pet_notify`（阶段进度气泡）、`pet_speak`
  （让桌宠 TTS 说一句话）、`pet_state`（查桌宠状态）、`pet_work_update`
  （主动汇报 `progress` / `blocked` / `ready`，把「待我处理」里的工单往前推；
  `blocked` / `ready` 是要人动手的，会冒泡 + 语音，`progress` 只落工单），
  配 `agentrmc-pet` skill 教模型何时用、怎么不刷屏。
- **传输**：全部走 AgentRMC 本机回环口 `127.0.0.1:45915`（AgentBridge），
  无密钥、无外部依赖（hooks 与 MCP server 都是零依赖 node 脚本）。
- **待我处理**：hook payload 有 `session_id` / `cwd` / `tool_use_id` 时随 `/notify`
  一起带给原生端（字段名 `session_id` / `project` / `event_id`），原生端落成
  Work Item；`GET /work-items` 查询，`POST /work-items/ack` 标记已处理，
  `POST /work-items/update` 由 `pet_work_update` 调用（`blocked` / `ready` 顺带走
  提醒通道：气泡 + 语音；`progress` 只落工单）。
  桌面「待我处理」页按等处理 / 待验收 / 进行中排序，重启不丢。`UserPromptSubmit`
  会在新对话开始时自动归档同 session 的旧「待验收」。
- **离线不丢**：AgentRMC 没开、连接失败或 5xx 时，hook 把 payload 追加到
  `%APPDATA%\LIG\AgentRMCNative\pending-events.jsonl`；原生端启动/每 5 秒补投，
  补投只落工单，不补喊。

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

### Qoder 本地插件怎么更新

Qoder 把从 zip 上传的插件记成 `agentrmc-bridge@local`（见
`~/.qoder/plugins/installed_plugins_v2.json`）。本地插件没有 marketplace 版本可对比，
插件页的「更新」按钮不会去拉新 zip——这是 Qoder 的行为，不是包没打。而且国际版和
Qoder CN 是两个独立配置家，装一家另一家不会自动有。

版本号只有一个来源：包根的 `VERSION`。构建时 `native/tools/sync-plugin-version.ps1`
把它同步进 3 个 `plugin.json` 和 2 个 `marketplace.json`，出包前
`check-version.ps1` 会核对这五处（Native 自己那条版本线独立，不要求同号）。

**App 里升级（推荐）**：AgentRMC「系统」页的「联动插件」一行会列出内置版本、各家已装
版本和「进程里真在跑的版本」，落后就点「升级联动插件」——它把 exe 旁的 zip 解进
`<配置家>/plugins/cache/local/agentrmc-bridge/<version>`，再把注册表那一条指过去
（改写前把原件备份到 `%APPDATA%\LIG\AgentRMCNative\plugin-backups\`）。只升级已装过的
宿主，没装过的不替它造条目。改完仍要重启对应的 app 才加载新代码。

**让 Agent 装**：「使用引导 → 接入 AI Agent」的「复制安装话术」已经把 zip 绝对路径和
内置版本写进去了，那句话要求 Agent 用宿主自己的安装器 `install_extension`
（`source=local`、`kind=plugin`、`filePath=<zip>`）来装，两个 app 各一次。

**手工**：插件页 → 找到 AgentRMC 桌宠联动 → 重新上传
`安装目录\plugins\agentrmc-bridge.zip`（或 `dist\agentrmc-bridge-qoder-<version>.zip`）
→ 重启该 app。

`loaded` 这个读数为什么必要：hook 的 payload 带 `plugin_version`（读自包里那份
`VERSION`），原生端按宿主记下最近一次上报值。注册表只能说盘上装成哪版——盘上 0.3.7、
进程里还跑 0.3.6 这种「装完没重启」的状态，只有这个数能区分开。

#### 升级完旧会话为什么一条提醒都没有

宿主在**会话开始时**把 `${QODER_PLUGIN_ROOT}` 解析成带版本号的缓存目录，并把它烧进
hook 命令行里。官方安装器装新版时会**删掉上一版目录**，于是那个还开着的会话每次触发
hook 都是 `node <已经不存在的文件>`——静默失败，不冒气泡也不报错，看起来像「桌宠不提醒了」。

- 判据：App「系统」页那一家挂着「等新版上报」，收到任意一条 hook 就自己清掉；
- 恢复：新开会话，或重启该宿主；
- 临时救旧会话：把新版包复制成它期望的那个旧版本号目录
  （`cp -r cache/local/agentrmc-bridge/0.3.7 .../0.3.6` 这类），路径一存在提醒就回来；
- AgentRMC 自己的「升级联动插件」**不删旧目录**，走这条路不会把活会话打断。

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
- `mcp/server.js`：stdio MCP server（pet_notify / pet_speak / pet_state / pet_work_update）；
- `skills/agentrmc-pet/SKILL.md`：模型使用指南；
- `test/host.test.js`：`node test/host.test.js`，11 个用例覆盖四种宿主 + 两个 Qoder 变体的
  端到端判定（会把插件树复制到假装的 `~/.qoder` / `~/.qoder-cn` 里跑真脚本）。
