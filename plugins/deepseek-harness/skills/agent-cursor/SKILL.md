---
name: agent-cursor
description: 用 AgentRMC 的「Agent 专用指针」把要操作的位置指给用户看、并真点击，全程不移动用户的系统鼠标。当需要指着屏幕上某处说明、演示 GUI 操作、点按钮/菜单/列表项，或用户抱怨「Agent 一动手我就用不了鼠标」时使用。触发词：虚拟指针、Agent 指针、指给我看、指一下、点这里、别抢我鼠标、agent cursor。
---

# Agent 专用虚拟指针

AgentRMC 常驻进程里有一根**属于 Agent 的指针**：彩色箭头 + 准心环 + 署名标签，
画在一块穿透置顶覆盖层上。它和你的鼠标是**两根互不影响的指针**——

- **Agent 指针不读你的光标**：你把鼠标甩到哪里，它都不跟着动。
- **Agent 动作不搬你的光标**：代码里没有 `SetCursorPos`、没有 `SendInput`、
  没有 `mouse_event`。点击走 UI Automation 语义调用，或向目标窗口投递消息。
- **两根可以同时用**：你拖动鼠标的同时，Agent 照样能指、能点。
- **每根指针有主**：`-Agent` 决定颜色和标签，多个 Agent 同时干活时你看得出是谁。

## 前置

AgentRMC 原生端（`AgentRMCNative.exe`）在跑，本机桥 `127.0.0.1:45915` 活着，
并且系统页 →「Agent 指针」是开着的（默认开）。

```powershell
curl.exe -s http://127.0.0.1:45915/cursor/state   # 探活；"enabled":false = 被用户关了
```

## 首选：工具调用

装了 AgentRMC 联动插件的宿主（Qoder / Qoder CN / ZCode / WorkBuddy / DSH）
直接有工具可调，不用自己拼 HTTP：

| 宿主 | 工具名 |
|---|---|
| Qoder / Qoder CN / ZCode / WorkBuddy | `mcp__agentrmc__cursor_point` |
| DeepSeek Harness | `cursor_point` |

参数：`x` / `y`（物理像素，必填）、`click`（默认 true）、`button`
（left/right/middle/double）、`hold_ms`（默认 8000，0 = 一直留着）。

## 其次：命令行 / HTTP

脚本：`scripts/cursor.ps1`（与本 SKILL.md 同目录；仓库里另有一份在
`native/tools/agent-cursor.ps1`）。

```powershell
pwsh -File scripts/cursor.ps1 move 1200 640 -Agent DSH      # 只指，不点
pwsh -File scripts/cursor.ps1 click 1200 640 -Agent DSH     # 移过去并左键点
pwsh -File scripts/cursor.ps1 click 1200 640 -Button right -Agent DSH
pwsh -File scripts/cursor.ps1 hide
pwsh -File scripts/cursor.ps1 state -Json
```

坐标是**物理像素**（与截图、`GetCursorPos` 同一套数）。多屏混 DPI 照这套数给，
换算由覆盖层逐屏做。

直接打 HTTP 也行（任何语言、任何 Agent）：

```
POST http://127.0.0.1:45915/cursor/click
{"x":1200,"y":640,"agent":"DSH","button":"left","backend":"auto","duration_ms":240,"hold_ms":8000}
```

## 点击是异步的——必须回读回执

接口**立刻**回 `{"ok":true,"clicked":true,"backend":"async"}`，真实结果要回
`/cursor/state` 的 `last_click` 查：

```json
{"ok":true,"backend":"uia","verified":true,"target":"Button:click me","error":""}
```

为什么异步：UIA 的 `Invoke` 会「调进目标进程」。如果目标正好在等我们的回包，
同步做就会双方互等、桥线程卡死到超时——点击其实生效了，调用方却只看到连接断了。
现在点击在独立线程里跑，回包不会被它拖住。

（用 `cursor_point` 工具时这一步插件已经替你做了，正文里直接给
`backend/verified/target`。）

| 字段 | 意思 |
|---|---|
| `backend` | `uia` = 语义调用成功（真点到控件）；`post` = 消息递给了目标窗口 |
| `verified` | 只有 `uia` 才 `true`。`post` 一律 `false`——递到 ≠ 生效 |
| `target` | 实际命中的窗口「类名:标题」，用来判断点对没有 |

**`post` 的 `verified:false` 是诚实不是失败**：消息递到了目标窗口，但「目标认不认」
在它手里。实测 Electron / CEF / Chromium 系（DSH、Qoder、Slack、VS Code）**能认**，
导航项、按钮都点得动；画布内自绘控件、DirectInput 游戏、管理员窗口则可能收下不响应。
这类目标优先用它们自己的通道（浏览器用 CDP、应用用自己的 API），别只凭
`clicked:true` 就往下跑。

### Chromium 系：默认走 post，别随手用 uia

`backend=auto` 碰到 `Chrome_*` 窗口会**主动避开 UIA、改走 post**。原因是一次实测：
用 UIA 的 `Invoke` 点 Chromium 应用（DSH）里的会话切换，**把它的渲染进程打崩了**
——应用当场弹「无法启动或已意外停止」，崩溃日志时间戳与点击同一秒。Chromium 的
UIA 树是惰性构建的，`Invoke` 走内部 a11y 通路，碰到正在重建的节点就带走渲染进程。

所以：`backend=auto`（默认）安全且够用；只有在目标程序**明确有可 Invoke 的控件、
且你愿意承担崩溃风险**时才显式传 `backend=uia`。

## 用户关了怎么办

系统页 →「Agent 指针」关掉后，`/cursor/*` 一律回 `409`，`/cursor/state` 里
`enabled:false`。这时候**不要重试**，如实告诉用户「指针被关了」，问要不要打开。

## 什么时候用

- 要跟用户说「我点的是这里」——比让用户找坐标快得多。
- 演示/教学：让用户看着 Agent 一步步操作，同时他的鼠标还能用。
- 需要在 Agent 干活期间保留鼠标控制权。

## 什么时候别用

- 目标程序有更好的接口：浏览器用 CDP、Office 用 COM、终端用命令。
- 需要拖拽、长按、多点触控：本指针只做移动和单击。

## 排查

| 现象 | 原因 |
|---|---|
| 退出码 1，连不上 | AgentRMC 没在跑，或 45915 被别的进程占了 |
| HTTP 409 / `enabled:false` | 用户在系统页把「Agent 指针」关了 |
| `last_click.ok=false` | 坐标上没有窗口，或目标窗不认合成消息（看 `error`） |
| 指针位置不对 | 坐标给成了逻辑坐标；本接口只认物理像素 |
| 点了没反应 | 看 `target` 是不是你以为的窗口——多半点到了被覆盖的窗口 |
