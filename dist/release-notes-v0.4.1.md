# AgentRMC v0.4.1 — 定向派发全链接线 · WebView2 外壳 · 语音系统控制 · 模型出错喊人

> **RMC = Remote Mission Control（远程任务中控台）**：手里一台遥控器，桌上一排 Agent——
> 谁在等你确认、谁做完等你验收、谁正在跑，中控台统一收进一处、统一喊人、统一回跳。

这一版把「说一句话，派给对的 Agent」从 WorkBuddy 一路接到 ZCode、DeepSeek Harness 和
OpenSquilla，四路定向派发全链活体验收；主窗右半区换成 WebView2 单管线渲染，Qt 混渲染
发虚问题消除；语音新增音量/亮度/媒体/电源系统控制；模型出错（StopFailure）也会喊人。
安装包约 **34.5 MB**（含经典 + 鲸鱼娘主题与联动插件；语音模型另包、软件内导入）；
联动插件与桌面端**同号 0.4.1**。

---

## 本次内容

### 定向派发（第三批全链接线）

- **ZCode 定向派发**：spawn 自带 app-server stdio 协议通道，create/send/回执全链；
  弹选项（AskUserQuestion）挂 PreToolUse 主路，permission 对称去重，不再漏喊。
- **DeepSeek Harness 定向派发**：SessionDispatch 插件通道 + AgentBridge 取件/ack 路由，
  插件生产化轮询注入，外壳卡片真实入口 E2E 全链活体验收。
- **OpenSquilla 定向派发**：网关 RPC 通道（桥取件 + resolve 守卫 + sessions.send + ack 回执），
  取件按 exe 过滤防互领；桥默认 --relay，卡面带回复正文。
- **WorkBuddy 定向派发**（0.4.0 已接线，本轮修复）：会话归属判据顺序纠正，
  同 session 归属自愈，不再记到 Qoder CN。
- **待办按会话立账**：同会话只留最新状态，双击先送到 Agent 再销账；
  启动宿主时洗掉继承的宿主会话环境，不再撞版。
- **ZCode 派发行不亮修复**：桥 stop hook 不带 raise，落卡前按显示名反查白名单 exe 补 agent_exe。
- **OpenSquilla 卡面归属与文案**：project 透传显示项目目录名、宿主名开头、
  泛标题 WebChat 回落、有回复时回复当主文案。

### WebView2 外壳

- **右半区整体换成 WebView2 单管线**：待办/事件流/系统/按键映射/应用/使用引导六页
  全部搬入，主题 token 上色、增量刷新不重载文档；顶栏左栏也搬入，整窗单管线消 Qt 混渲染发虚。
- **运行库缺失自动回退 Qt 页签**，不影响基本功能。
- 顶栏拖拽跟手、跟随系统换色提速、跨屏重喂边界三处实机修复。

### 语音系统控制

- **音量/亮度/媒体/电源**：语音可调节系统音量、屏幕亮度、媒体播放，
  关机/重启等危险动作要再说一句确认才执行。
- **TaskCue 路由**：「这句该谁接」交给 TaskCue，功能清单按需进上下文。
- **录音设备可选**：自动档躲开被占的麦克风；常开麦克风跟随设备变化，
  失效设备不再留在下拉里；设备下拉按 trim 比对，不再多出假的「现在没插着」。
- **TTS 超时**：合成请求加超时，挂死不再让桌宠永久失声。
- **撤掉每 2 秒设备轮询**，事件流不再刷屏。

### 提醒与桌宠

- **模型出错喊人**：订阅宿主的 StopFailure，模型报错时桌宠也会出声提醒。
- **提醒从四类变五类**：等授权、等选方案、等你回话、任务完成、模型出错，各家口径同步。
- **桌宠卡片状态行改单行省略**，长文案不再把下拉行挤掉半截。
- **音色文案改成跟随形象**，不再跟配色主题。

### 主题

- **主题包导入即启用**：导入后直接切到新主题，缓存作废、拖放校验；状态色令牌化。
- **主题包导入/缓存/拖放校验审查修复**。

### 控件与 UI

- **控件现代化**：自绘滑块、DSH 风格弹层与按钮配色。
- **浅底灰阶压深一档**，转写框按当前字体折行限高。

---

## English

### Directed dispatch (batch 3, full-chain wired)

- **ZCode**: spawn built-in app-server stdio protocol channel; create/send/ack full chain;
  AskUserQuestion hooked on PreToolUse main path, permission symmetric dedup — no missed alerts.
- **DeepSeek Harness**: SessionDispatch plugin channel + AgentBridge pickup/ack routing,
  production polling injection, shell card real-entry E2E full-chain live acceptance.
- **OpenSquilla**: gateway RPC channel (bridge pickup + resolve guard + sessions.send + ack),
  pickup filtered by exe to prevent cross-claim; bridge defaults to --relay, card carries reply body.
- **WorkBuddy** (wired in 0.4.0, fixed this round): session ownership predicate order corrected,
  same-session self-heal, no longer misattributed to Qoder CN.
- **Per-session work items**: only the latest state per session survives; double-click delivers
  to the agent before archiving; host launch strips inherited session env to avoid version collision.
- **ZCode dispatch row highlight fix**: bridge stop hook lacked raise; agent_exe back-filled
  from display-name whitelist lookup before card creation.
- **OpenSquilla card attribution**: project passthrough shows directory name, host name prefix,
  generic title falls back to WebChat, reply body becomes primary text when present.

### WebView2 shell

- **Right half fully migrated to WebView2 single pipeline**: all six tabs (Pending / Event log /
  System / Keymap / Apps / Guide) moved in, themed via CSS tokens, incremental refresh without
  document reload; top bar and left column also migrated — eliminates Qt mixed-render blur.
- **Automatic Qt-tab fallback** when the WebView2 runtime is missing.
- Three live-machine fixes: top-bar drag tracking, follow-system colour-switch speed,
  cross-screen re-feed boundary.

### Voice system control

- **Volume / brightness / media / power**: voice adjusts system volume, screen brightness and
  media playback; dangerous actions (shutdown/restart) require a second spoken confirmation.
- **TaskCue routing**: "who should take this sentence" delegated to TaskCue; capability list
  enters context on demand.
- **Recording device selectable**: auto mode avoids occupied mics; always-on mic follows device
  changes; stale devices no longer linger in the dropdown; trim comparison removes phantom
  "(not plugged in)" entries.
- **TTS timeout**: synthesis requests now time out so a hung call no longer silences the pet permanently.
- Removed the 2-second device poll that was flooding the event log.

### Alerts & pet

- **Model-error alerts**: subscribes to host StopFailure; the pet speaks when the model errors out.
- **Five alert categories** (was four): approval, option pick, waiting for reply, task done, model error.
- Pet card status line is single-line ellipsis; long text no longer clips the dropdown row.
- Voice timbre label follows the character, not the colour theme.

### Themes

- **Import = activate**: importing a theme package switches to it immediately; cache invalidated,
  drag-and-drop validated; status colours tokenised.

### Controls & UI

- Modernised controls: custom-drawn sliders, DSH-style popovers and button colours.
- Light-mode grey ramp darkened one step; transcript box wraps and caps height by current font.

---

安装包约 **34.5 MB**；联动插件与桌面端同号 **0.4.1**。
Installer ~**34.5 MB**; companion plugin matches Native at **0.4.1**.