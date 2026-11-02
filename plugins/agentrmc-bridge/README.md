# AgentRMC Bridge（ZCode / Qoder 插件）

把 [AgentRMC](https://github.com/AgentRMC/Agentrmc) 桌宠接进你的 Agent：

- **自动提醒（hooks）**：Agent 需要确认工具调用（PermissionRequest）时桌宠气泡 +
  TTS 语音提醒（方案选择会念「等你选方案」），并把该 Agent 主窗口拉到前台；一轮
  回答结束（Stop）时桌宠念「X 已完成…」同样拉前台（任务一般很长，完成值得出声）。
  AgentRMC 没开就静默跳过（<100ms），不拖慢会话。
- **主动交互（MCP）**：三个工具——`pet_notify`（阶段进度气泡）、`pet_speak`
  （让桌宠 TTS 说一句话）、`pet_state`（查桌宠状态），配 `agentrmc-pet` skill
  教模型何时用、怎么不刷屏。
- **传输**：全部走 AgentRMC 本机回环口 `127.0.0.1:45915`（AgentBridge），
  无密钥、无外部依赖（hooks 与 MCP server 都是零依赖 node 脚本）。

## 宿主差异

|              | ZCode                              | Qoder                                          |
| ------------ | ---------------------------------- | ---------------------------------------------- |
| 清单         | `.zcode-plugin/plugin.json`        | `.qoder-plugin/plugin.json`                    |
| hooks 配置   | `hooks/hooks.json`（manifest 给目录）| `hooks-qoder/hooks.json`（manifest 要给到 .json 文件）|
| 拉前台       | 原生端缺省找 `ZCode.exe`           | hook 传 `raise: "Qoder CN.exe"`，按它找窗口    |
| 宿主署名     | 缺省 ZCode                         | argv + `AGENTRMC_NOTIFY_HOST=Qoder`（气泡署名）|

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
- **Qoder Desktop**：侧边栏 扩展 → 插件 → 添加插件 → 上传插件（ZIP）。Desktop 只有
  「官方市场」和「本地 ZIP」两种来源，不认「本地目录市场源」（那条路只有 CLI 的代码
  里在找 `marketplace.json`）。把 `plugins/agentrmc-bridge/` 打成 zip 上传即可，
  装完落在 `~/.qoder-cn/plugins/cache/local/agentrmc-bridge/<version>`。
- 打 Qoder 用的 zip 有两个坑：条目名必须用 `/` 分隔（PowerShell `Compress-Archive`
  写成 `\`，Qoder 解不出清单，会报 `EXTENSION_PLUGIN_VALIDATION_FAILED`）；manifest 的
  `hooks` 要指到 `./hooks-qoder/hooks.json` 这个文件，给目录报 `Path must end with .json`。

> 如果你之前手工在 `~/.zcode/cli/config.json` 里配过
> `PermissionRequest` / `Stop` hook（指向 `~/.zcode/hooks/agentrmc-notify.js`），
> 装完插件请删掉那两条，避免通知重复。

## 文件

- `.zcode-plugin/plugin.json`、`.qoder-plugin/plugin.json`：两个宿主各自的清单；
- `hooks/hooks.json`、`hooks-qoder/hooks.json`：各自的 hook 注册；
- `hooks/agentrmc-notify.js`：共用的通知脚本，`argv[3]` 给宿主署名、`argv[4]` 给
  要拉前台的进程映像名；不给就按环境变量认 ZCode / WorkBuddy；
- `mcp/server.js`：stdio MCP server（pet_notify / pet_speak / pet_state）；
- `skills/agentrmc-pet/SKILL.md`：模型使用指南。
