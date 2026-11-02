# AgentRMC Bridge —— DeepSeek Harness 版

把 [AgentRMC](https://github.com/AgentRMC/Agentrmc) 桌宠接进 DeepSeek Harness（下称 DSH）。

这是 `plugins/agentrmc-bridge/`（ZCode / WorkBuddy / Qoder 版）的 DSH 移植：**效果一样，
接法完全不同**。原版靠宿主自己的 `hooks.json`——宿主在固定事件上 fork 一个 node 脚本；
DSH 没有那种 hooks，它的扩展单位是 **Cordis 插件**，事件是进程内的 Event。所以这份
实现是一个 Host 插件：监听事件 + 注册原生工具，走的还是同一个本机回环口
`127.0.0.1:45915`，**原生端一行不用改**。

| 能力 | 原版（hooks + MCP） | 本版（Host 插件） |
| --- | --- | --- |
| 等工具授权 | `PermissionRequest` hook → `agentrmc-notify.js permission` | 会话事件 `approval/asked` |
| 等选方案 | `PreToolUse` + `matcher:"AskUserQuestion"` | 会话事件 `tool/call` 且 `name=ask_user_question` |
| 一轮结束 | `Stop` hook（读 transcript 捞最终回复） | 监听 `agent/turn-stopping`（从 `agent/assistant-stream` 攒最终回复） |
| 新对话开始 | `UserPromptSubmit` hook | 监听 `agent/inbox/claimed` |
| 四个工具 | stdio MCP server（每宿主一份子进程） | 直接注册成 Host 原生工具（无子进程） |
| 模型使用指南 | 随包 `skills/agentrmc-pet/SKILL.md` | 同样的正文，`apply` 里注册进 skills 服务 |

> **为什么不挂 `approval/request` 和 `user-questions/request` 这两个瀑布事件**：
> DSH Web UI 的应答者本身就是这两个瀑布的监听器，它一旦作答就不再调 `next()`，
> 排在它后面的监听器永远轮不到——实测桌面端点完选方案，桌宠一声没响、`/notify`
> 一条没到。改成挂**持久会话事件**（`session/event` 是 emit，所有监听器都会跑，
> 与顺序无关）：`approval/asked` 是授权服务问人之前落的审计事件，
> `tool/call`（`ask_user_question`）是提问被记录时落的。这与原版用 `PreToolUse`
> 旁听、而不是去当应答者的思路是同一个。

提醒文案、`event` 类型、payload 字段（`title` / `message` / `speech` / `raise` /
`session_id` / `project` / `event_id` / `plugin_version`）、离线补投文件
`%APPDATA%\LIG\AgentRMCNative\pending-events.jsonl`——全部与原生端 `AgentBridge.h`
的契约一致，和别的宿主混着跑也不冲突。

## 文件

- `package.json` —— bundle 清单（`dsh.bundle.patch` 指到下面的 patch）；
- `cordis.patch.yml` —— 往当前 profile 插一行 `agentrmc-bridge`；**改配置就改这里**；
- `index.js` —— Host 插件：事件监听、四个工具、skill 注册；
- `lib/bridge.js` —— 本机回环口客户端（零依赖，含离线落盘与日志节流）；
- `lib/text.js` —— 把最终回复压成「念得完的一句话」；
- `skills/agentrmc-pet/SKILL.md` —— 模型使用指南（正文会在 `apply` 时注册成 runtime skill）；
- `locale/en.json`、`locale/zh.json` —— 插件管理页显示的标题与说明；
- `icon.svg` —— 插件页图标；
- `test/plugin.test.js` —— `node test/plugin.test.js`，19 个用例，零依赖。

本包**不 import 任何 `@deepseek-ai` 包**：装进哪个 profile 都能加载，也不会被
DSH 版本升级带走。

## 安装

前提：AgentRMC 正在运行（`curl --noproxy '*' http://127.0.0.1:45915/state` 有 JSON 就是活着）。

**让 Agent 装（推荐）**：把这一句发给正在干活的 Agent——

> 用 plugin_manager 的 install_bundle，把 `D:\githubrepos\Agentrmc\plugins\deepseek-harness`
> 这个本地 bundle 目录装进当前 profile。

**命令行**：`plugin_manager` 是 Agent 工具，没有 shell 入口时手工改 profile 也可以：

```powershell
# 1) 让 profile 把这个目录当本地包（link 不复制，改代码即改生效内容）
cd "$env:DSH_PROFILE_DIR"
pnpm add "link:D:\githubrepos\Agentrmc\plugins\deepseek-harness"
# 2) 把它列进 profile 的 bundle 列表：package.json 的 dsh.profile.bundles 追加一项
#    "@local/agentrmc-bridge"
```

装完**重启 DSH**（Host 插件换了 JavaScript 代际，热更新不保证）。重启后：

- 插件页能看到「AgentRMC 桌宠联动」，图标与说明来自 `locale/` 与 `icon.svg`；
- 工具列表里多出 `pet_notify` / `pet_speak` / `pet_state` / `pet_work_update`；
- skill 目录里多出 `agentrmc-pet`。

停用：`plugin_manager` 的 `set_bundle` 关掉，或 `remove_bundle` 移除。

**AgentRMC 的「系统」页会替你盯版本**：它扫 `~/.dsh/profiles/*/package.json` 的
`link:` 依赖得到盘上版本，从本插件发的通知里拿 `plugin_version` 得到「进程里真在跑
的版本」，两个数一起和安装包内置版本对账。它**不会替你升级**（升 bundle 要 pnpm
+ 重启 Host）：落后时只在事件行里提示你重新装一次。
装法在 AgentRMC「系统」页的「接入 Agent」弹窗里（新手引导的「联动插件」同一个）：
点「复制安装话术」，粘给 DSH 里的 Agent 即可；那一句里写的路径就是
`安装目录\plugins\deepseek-harness`。

> 本插件随 AgentRMC 安装包一起发到 `安装目录\plugins\deepseek-harness\`（源码形态，
> DSH 不需要 zip），所以别人机器上直接指这个目录安装即可。
>
> ⚠ **别把 profile 长期 link 在安装目录里**：`install_bundle` 装的是 `link:`
> 依赖，profile 记住的是那个路径；安装目录会被卸载/覆盖安装动到。要长期用就把
> 这个目录复制到自己的固定位置再装，例如
> `Copy-Item -Recurse 安装目录\plugins\deepseek-harness "$env:USERPROFILE\.dsh\bundles\"`。

## 配置

改 `cordis.patch.yml` 里那一行的 `config:`。**整块替换**：写哪个字段就覆盖哪个，
没写的走 `index.js` 的 `DEFAULTS`。

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `port` | `45915` | AgentBridge 端口；也可用环境变量 `AGENTRMC_PORT` |
| `agentName` | `DeepSeek Harness` | 气泡署名 / 语音里念的宿主名 |
| `raiseExe` | `DeepSeek Harness.exe` | 拉前台的进程映像名，原生端按名字精确找窗口 |
| `requestTimeoutMs` | `1500` | 单次 HTTP 超时；超时就落盘等补投 |
| `spoolDir` | 空 → `%APPDATA%\LIG\AgentRMCNative` | 离线补投目录（与原版同一个文件） |
| `notifyApproval` | `true` | 等工具授权时出声 + 拉前台 |
| `notifyQuestion` | `true` | 等选方案时出声 + 拉前台 |
| `notifyTurnStart` | `true` | 新对话开始时告诉原生端归档旧「待验收」 |
| `notifyStop` | `true` | 一轮结束念最终回复 |
| `speakFinalReply` | `true` | 念正文；`false` 时只念「做完了」 |
| `registerTools` | `true` | 注册四个 `pet_*` 工具 |
| `registerSkill` | `true` | 注册 `agentrmc-pet` skill |
| `dryRun` | `false` | 只打印将要发送的 payload，不真发（也可用 `AGENTRMC_DRY_RUN=1`） |

本包**没有导出 `Config`**，所以插件管理页不会给这几个字段生成表单（配置状态显示为
`absent`），改配置只能编辑 `cordis.patch.yml`。这是为了不 import `@deepseek-ai/schemastery`
换取「换 DSH 版本也照样能加载」——字段本身在 `normalizeConfig()` 里有类型兜底。

## 四个工具

与原版 MCP server 完全同语义（详见 `skills/agentrmc-pet/SKILL.md`）：

- `pet_notify` —— 阶段进度气泡（不出声）；
- `pet_speak` —— 气泡 + TTS 念一句；
- `pet_state` —— 查桌宠状态；
- `pet_work_update` —— `progress` / `blocked` / `ready` 写进「待我处理」；后两个会冒泡 + 语音。

比原版多带一个 `agent_exe` 字段：原生端按进程映像名把工单归到 Worker 行，
`DeepSeek Harness` 不在它的已知宿主白名单里，靠产品名反查会挂空。

## 行为细节

- **不阻塞会话**：`approval/request` 与 `user-questions/request` 是 waterfall，
  监听器发完通知立刻 `next()`；`agent/turn-stopping` 是 serial，同样不等网络。
- **离线不丢**：连不上 / 5xx / 超时 → 追加进 `pending-events.jsonl`，原生端启动与
  每 5 秒补投（补投只落工单，不补喊）。
- **日志节流**：桌宠没开时每条事件都会失败，失败日志 60 秒只报一次；
  原版是一次性进程，看不见这个问题。
- **一轮结束念的是最后一条不带工具调用的回复**：只认 `assistant/message` 的
  committed 帧，中间阶段的输出不会被念出来；压成一句（去代码块、去 markdown、
  只留第一句、限长 80）。
- **同一轮只报一次 `turn-start`**：`agent/inbox/claimed` 对一轮里的多条消息都会
  触发，按 `turn` 去重。

## 已验证 / 未验证

已在真机跑通（DSH Desktop + AgentRMC 0.3.9，全部四类语音通道逐条触发过）：

- `install_bundle` 装进 `desktop` profile，`application: applied`；工具出现在工具的
  schema 列表里，`pet_state` 返回真实桌宠状态；`agentrmc-pet` skill 出现在会话
  skill 目录里；停用再启用会完整卸载/重装（skill 会跟着消失再回来）。
- **说话通道**：`pet_speak` → 气泡 + TTS 出声。
- **卡住通道**：`pet_work_update stage=blocked` → 气泡 + 语音；工单从 `working`
  合并成 `needs-me`（同一 id，不新长一张卡）。
- **完成通道**：`pet_work_update stage=ready` → 气泡 + 语音，工单转 `review`。
- **轮末自动提醒**：`agent/turn-stopping` → 桌宠念「DeepSeek Harness：<最终回复>」，
  并落一条 `stop` 工单；下一条用户消息触发的 `turn-start` 会把它自动归档。
- **选方案通道**：真实调用一次 `ask_user_question` → 桌宠念「DeepSeek Harness
  在等你选方案」并把 DSH 窗口拉回前台，工单落成 `question / needs-me`。
- 原生端 `/workers` 里出现 `DeepSeek Harness.exe`（`alive: true`、工单归到这一行），
  说明 `AgentWindow::knownExes()` 已经把它算进编制内。
- 19 个单元/集成用例全过（假 ctx 跑真插件；工具那条用真的假桥接验 body 字段）。

未验证 / 已知边界：

- **等授权通道**：本机这个会话的审批策略是 `never`（审批提示被禁用），没法真实触发
  `approval/asked`；代码路径与「选方案」同一条（都走 `session/event`），但要在
  `ask` 策略下的会话里再复核一次。
- Web 端（浏览器打开 `http://127.0.0.1:3136`）没有自己的窗口，`raise` 静默无效，
  气泡 / 语音照常。
- 插件页里的标题、说明、图标只验证了资源导出与安装无告警，**渲染外观未目视确认**。

## 测试

```powershell
node plugins\deepseek-harness\test\plugin.test.js
```

测试把 `AGENTRMC_PORT` 指到 `1`（必然连不上），于是每条通知都走「落盘等补投」这条
真实路径，断言读的就是真要发出去的内容——不 mock HTTP，也不会真的去喊你桌面上的桌宠。

## 版本

插件版本只有一个来源：`plugins/agentrmc-bridge/VERSION`。构建时
`native/tools/sync-plugin-version.ps1` 把它同步进 3 个 `plugin.json`、2 个
`marketplace.json` 和本包的 `package.json`；`check-version.ps1` 会核对全部六处。
**不要单独改本包的 `version`**。
