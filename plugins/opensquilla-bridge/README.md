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
        │  session.event.done / approval_required / plan_presentation
        ▼
opensquilla-pet-bridge.mjs
        │  POST /notify {event:stop|permission, title, message, raise}
        ▼
AgentBridge :45915  →  气泡 + TTS + 把 OpenSquilla 窗口拉回前台
```

## 文件

| 文件 | 作用 |
|---|---|
| `opensquilla-pet-bridge.mjs` | 常驻桥。订阅事件 → 发 `/notify`。零依赖（Node 22+ 内置 WebSocket/fetch） |
| `startup-bridge.vbs` | 开机自启，隐藏窗口 |
| `ensure-bridge.ps1` | 看门狗：桥不在就拉起来，并在 `%TEMP%\pet-bridge-watchdog.log` 留一行 `DOWN/UP`，用来查它是什么时候死的 |
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
$wd = "$env:USERPROFILE\.agents\skills\agentrmc-voice-reminder\scripts\ensure-bridge.ps1"
schtasks /Create /F /TN OpenSquillaPetBridgeWatchdog /SC MINUTE /MO 5 /TR `
  "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$wd`""
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
| `--msg-stop/--msg-permission/--msg-plan` | 文案模板，支持 `{session}` `{text}` |
| `--relay` | 把回复正文前 80 字也带进气泡（默认只报「这轮跑完了」） |
| `--log <path>` | 落日志；默认不写 |
| `--selftest` | 只发一条测试通知就退出 |

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

## 已知边界

- **事件覆盖**：`session.event.done`（回合结束）已实测触发；`approval_required`
  和 `plan_presentation` 的映射已写进桥，但还没在真实审批/Plan 流程里实跑过。
  真用到时看日志有没有 `NOTIFY permission`，没有就按事件名调。
- **短回合**：事件名在 `frame.event` 顶层，不在 `payload.event`；cron 这类短回合
  必须靠 `sessions.changed` 即时补订阅，靠轮询会漏。
- **端口**：45915 是单实例资源。被别的程序占了 OpenSquilla 这边发不出去，
  但不会影响 AgentRMC 主功能（`AgentBridge::start()` 失败会静默放弃监听）。
