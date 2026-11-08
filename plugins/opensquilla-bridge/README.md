# OpenSquilla 接入（外部桥）

OpenSquilla 和 Qoder / ZCode / WorkBuddy 不一样：**它没有插件目录，装不了
`agentrmc-bridge.zip`**。它的扩展单位是 Skill（`SKILL.md` + 脚本），扩展点是
网关的 WebSocket 事件流。

所以这里的接法是**外部桥**：一个零依赖的 node 脚本订阅 OpenSquilla 网关的会话
事件，在「一轮对话结束」「等授权」「等选方案」时，向 `127.0.0.1:45915` 发和插件
完全相同的那套 `/notify`。桌宠侧一行代码都不用改——它只是又来了一个宿主。

```
OpenSquilla 网关 :18791 (ws)
        │  connect → sessions.messages.subscribe（每个会话）
        │  session.event.done / approval_required / plan_presentation / turn_committed
        ▼
opensquilla-pet-bridge.mjs
        │  POST /notify {event:stop|permission|turn-start, title, message, raise, session_id, project}
        ▼
AgentBridge :45915  →  气泡 + TTS + 把 OpenSquilla 窗口拉回前台

反方向（定向派发，2026-11-08）：
AgentBridge :45915  GET /dispatch/pending?exe=OpenSquilla.exe（桥 1.5s 轮询取件）
        ▼
opensquilla-pet-bridge.mjs  →  sessions.resolve 守卫 → sessions.send 注入
        │  POST /dispatch/ack {dispatch_id, ok, error}
        ▼
原生端回执落账（ack ok = durable admission 成立）→ 工单转进行中
```

> **session_id 必须带**：原生端靠它把同一会话的旧「待验收」归档、把新事件并进
> 同一张工单。不带的话每个会话每跑完一轮就多一条重复待办（2026-11-03 踩过：
> 同一个 WebChat 会话攒了 3 条一模一样的「待验收」）。
>
> **project 同样要带**：桥缓存 `sessions.list` 的 `workspace/workspaceLabel`，
> 与 hooks 同口径报原样路径；外壳行身份取目录名（如 `Agentrmc`）。不带它
> 卡行只能兜底「会话 <短 id>」（2026-11-08 用户指出后补）。`turn_committed` 发的
> turn-start 则是「人已经回来开新轮」的静默归档信号，不出声、不建卡，也不吃
> 15s 去重——连开两轮时第二轮的归档必须照发，不然旧验收卡多挂一轮。
>
> **去重按「事件身份」不按会话**（2026-11-08 审查加固）：事件带 `approval_id` /
> `turn_id` / `stream_seq` 时，同会话的两次审批、两轮完成各提醒各的；`turn_committed`
> 会清掉该会话的上一轮账，`done → 新轮 → done` 的第二条完成不会被 15s 窗口吞掉。
> 身份还会作为 `event_id` 透传给原生端，让「同一会话第二道题」不被 180s 迟到去重压掉。
>
> **`sessions.list` 要翻页**：v4 契约是 `view:"session-list-v1"` + `cursor` →
> `nextCursor/hasMore`。只取首页会漏会话（订阅不到、project 也缓存不上），桥会
> 逐页取全（最多 20 页，防死循环）；老网关不认 view 时退回落单页数组。

> **离线补投**：AgentRMC 没开时通知不丢——落盘到与 hooks/DSH 同址同格式的
> `%APPDATA%\LIG\AgentRMCNative\pending-events.jsonl`（`AGENTRMC_SPOOL_DIR`
> 可改），原生端启动/每 5 秒补投，补投只落工单不出声、不抢前台。文件超 1MB
> 丢最旧留最新（三条插件线同一套守卫）。`--selftest` 是探针，失败不落盘。

> **假活自愈**：网关半开（睡眠/网络抖后 TCP 不断、`onclose` 不来）时进程看着
> 活却收不到事件。桥每轮 `sessions.list` 就是探针：连续 3 次失败强制换新连接；
> 握手限时（默认 20s）兜底连 101 都不回的 TCP 黑洞。心跳文件
> `%TEMP%\pet-bridge.heartbeat` 每 30s 写一拍，**网关断了也照写**——看门狗靠它
> 区分「活着但网关断了」（不杀）和「事件循环卡死」（超 3 分钟没更新，杀掉重启）。

## 文件

| 文件 | 作用 |
|---|---|
| `opensquilla-pet-bridge.mjs` | 常驻桥。订阅事件 → 发 `/notify`。零依赖（Node 22+ 内置 WebSocket/fetch） |
| `startup-bridge.vbs` | 开机自启，隐藏窗口 |
| `ensure-bridge.ps1` | 看门狗：桥不在就拉起来；进程在但心跳超 3 分钟没更新判卡死，杀掉重启。日志在 `%TEMP%\pet-bridge-watchdog.log`（`DOWN/UP/WEDGED`）。路径按 `$PSScriptRoot` 取，整目录拷走就能用 |
| `ensure-bridge-hidden.vbs` | 看门狗的零闪窗壳：计划任务跑它（wscript 是 GUI 子系统，不建控制台），它再隐藏执行同目录的 `ensure-bridge.ps1`，退出码透传给任务 |
| `voice-reminder.ps1` | 立即说一句（不走事件），`/state` 验活 → `/speak` 或 `/notify` |

## 安装

```powershell
# 1) 桥本体放进 OpenSquilla 的个人技能层（网关会把它当技能收进来）
$dst = "$env:USERPROFILE\.agents\skills\agentrmc-opensquilla-bridge"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item plugins\opensquilla-bridge\* $dst -Force

# 2) 先手动验一次（桌宠要开着）
node "$dst\opensquilla-pet-bridge.mjs" --selftest          # 期望：SELFTEST OK

# 3) 挂进启动项
Copy-Item plugins\opensquilla-bridge\startup-bridge.vbs `
  "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\OpenSquillaPetBridge.vbs"

# 4) 每 5 分钟补位（强烈建议装；开机自启只救登录，救不了半路被杀）
#    必须走 wscript 隐藏壳：任务直接跑 powershell 的话，-WindowStyle Hidden 是
#    「先建控制台再隐藏」，桌面上每 5 分钟闪一下黑窗口（2026-11-03 已修）。
$wd = "$env:USERPROFILE\.agents\skills\agentrmc-voice-reminder\scripts\ensure-bridge-hidden.vbs"
schtasks /Create /F /TN OpenSquillaPetBridgeWatchdog /SC MINUTE /MO 5 /TR "wscript.exe //B `"$wd`""
```

> **别在 agent 的 tool call 里直接起桥。** 这台机器上从工具调用派生的进程活不过那次
> 调用（实测 15–30 秒内消失，日志里没有任何退出/断线记录，像被连进程树一起端掉）。
> 所以「我刚刚启动了桥」≠「桥在跑」——必须由 **计划任务** 或 **shell:startup** 启动，
> 并在下一个 tool call 里 `Get-CimInstance Win32_Process` 复查它还活着。
> 2026-11-02 那次「语音提示完全没触发」就是这么来的：桥早死了，而用来补位的
> `bridge-hidden.vbs` 写的是 JScript 语法（`var` / `new ActiveXObject`），`.vbs`
> 扩展名让 wscript 按 VBScript 解析，第 3 行直接 800A0401 弹窗，补位从来没成功过。

桥默认 `--raise OpenSquilla.exe`（谁发的就拉谁）。要改成拉桌宠自己：
`--raise AgentRMCNative.exe`；只想出声不抢焦点：`--no-raise`。

## 常用参数

| 参数 | 说明 |
|---|---|
| `--raise <exe>` | 点提醒时拉哪个窗口的进程映像名，默认 `OpenSquilla.exe` |
| `--no-raise` | 只出声/冒泡，不抢前台 |
| `--include-cron` | 也提醒 cron 会话（默认关，避免自激刷屏） |
| `--msg-stop/--msg-permission/--msg-plan` | 文案模板，支持 `{session}` `{text}`。默认宿主名开头（「OpenSquilla：…」，与 hooks 系同口径）；`{session}` 取会话标题，泛标题 WebChat 回落项目目录名 |
| `--relay` | 卡面/气泡带回复正文（前 80 字）。stop 有回复时默认主文案就是回复（「OpenSquilla：收到」，hooks 同口径），没回复才念收尾句；自定义 `--msg-stop` 不干预（没写 `{text}` 时回复附尾）。看门狗与开机自启默认带它（2026-11-08 用户定） |
| `--log <path>` | 落日志；默认不写 |
| `--heartbeat-file <path>` | 心跳文件位置，默认 `%TEMP%\pet-bridge.heartbeat`（看门狗读同一个） |
| `--refresh <ms>` / `--call-timeout <ms>` / `--hello-timeout <ms>` | 探针周期 / 单次调用超时 / 握手限时，默认 30000 / 15000 / 20000 |
| `--selftest` | 只发一条测试通知就退出 |
| `--no-dispatch` | 关掉定向派发取件（只保留事件提醒） |

免打扰：`New-Item "$env:TEMP\opensquilla-pet-bridge.pause"`，恢复就删掉它。

## 为什么不用插件那套

试过把 OpenSquilla 塞进 `PluginUpgrade.cpp` 的 `kFamily` 表，不行：那张表每一项
都指向一个**插件注册表 JSON**（`installed_plugins_v2.json` 之类）。OpenSquilla
没有这种东西——它的技能只有 `SKILL.md` 和 `skills-lock.json`，硬套只会让「系统」
页每 5 秒报一次「读不出注册表」。

所以原生端对它的处理是分两条：

- `AgentWindow::knownExes()` 认 `OpenSquilla.exe` —— 这样它的待办才有人认领
  （否则 `/work-items` 里有、`/workers` 里没有，`open_count` 对不上）；
- `pluginHealth` 里单独一段 `bridges`，只报「装没装 / 进程在不在」，不参与版本对账。

## 定向派发（2026-11-08 接线）

外壳待办卡的「派回会话」对 OpenSquilla 开放：原生端把指令排进队列，桥每 1.5s
取件，先 `sessions.resolve {key}` 证明会话在活表（证明不了就不发，拒绝原因走
ack ok:false），再 `sessions.send {key, message, client_message_id, _source}`
注入。网关受理是 durable admission（`turn_ingress_receipts` 落账、返回
`turn_id`），ack ok 即「确实到了那条会话」的回执，与 DSH followup 同级。
会话 key 形如 `agent:main:webchat:<id>`；native 侧只做 `agent:` 前缀哨兵 +
同会话在途闸，取件按 exe 过滤（DSH 插件领不走 OpenSquilla 的件）。

**幂等与口径（2026-11-08 审查加固）**：`client_message_id` 由 `dispatch_id` 派生
（`agentrmc-<id>`），同一件重取/重试网关能认重，不再每条随机 UUID。`sessions.send`
超时或被强制重连时**不等于被拒**——可能网关已受理只是回复丢了，ack 写「送达未知：
…（可能已受理，先去会话里确认再重派）」；只有网关明确回错误才是「被拒」。
`--send-timeout`（默认 30s）单独控制受理等待。

探针：`rc_probe/osquilla_dispatch_probe.mjs`（`--list` 只读 / `--send` 牺牲会话
全链）。实测坑：**桌面网关的 state home 跟配置走**（`%APPDATA%\@opensquilla\
desktop-electron\opensquilla\state`，会话库是 SQLite `sessions.db`）；
`~/.opensquilla` 是另一个实例的家，去那里读回会假阴性。

## 已知边界

- **事件覆盖**：`session.event.done`（回合结束）已实测触发；`approval_required`
  和 `plan_presentation` 的映射已写进桥，但还没在真实审批/Plan 流程里实跑过。
  真用到时看日志有没有 `NOTIFY permission`，没有就按事件名调。
  `turn_committed` → turn-start 是后补的：就算这个事件哪天改名，只影响「新轮
  开始即时归档」，不影响主链路——stop 自带同会话旧验收归档，列表仍只留最新一张。
- **短回合**：事件名在 `frame.event` 顶层，不在 `payload.event`；cron 这类短回合
  必须靠 `sessions.changed` 即时补订阅，靠轮询会漏。
- **端口**：45915 是单实例资源。被别的程序占了 OpenSquilla 这边发不出去，
  但不会影响 AgentRMC 主功能（`AgentBridge::start()` 失败会静默放弃监听）。
