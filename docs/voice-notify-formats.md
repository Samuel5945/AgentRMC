# 各宿主语音提示文案对照（2026-11-04）

覆盖七路宿主：Qoder、Qoder CN、ZCode、WorkBuddy、WorkBuddy AI（以上共用
`plugins/agentrmc-bridge` 的 hook 脚本）、DeepSeek Harness（`plugins/deepseek-harness`
Cordis bundle）、OpenSquilla（`plugins/opensquilla-bridge` 外部 WebSocket 桥）。
另附 ZCode 手工接入旧脚本、MCP 手动工具与原生端兜底文案。

## 〇、共用管线

- 插件拼 `message`（气泡 / 工单 / 事件流永远用它）+ 可选 `speech`（语音专用短文案），
  POST `127.0.0.1:45915/notify`。payload 字段：
  `event / title / message / speech? / raise? / session_id? / project? / event_id? / plugin_version?`。
- 原生端念 `speech`，没带就退回念 `message`（`MainWindow::handleNotify`）。
- **念出口统一净化**（`VoicePet::speak` → `SpeechText::friendly`）：气泡保留原文，
  真正发给 TTS 的那一份先转成口语——路径/盘符转「D盘 foo」、URL 去掉、带标点的
  名字/装饰标点/表情去掉、连字符转空格。详见第七节。对话模式回复（`speakAs`）不走这道。
- 出声行为（`MainWindow::announceWork`）：
  - `stop`：念 + 气泡停 7 秒，**不抢前台**（气泡可点，点了再回去）。
  - `permission` / `question`：念 + 气泡 + 把宿主窗口拉回前台；桌宠收起且无 TTS 时退回托盘。
  - `speak`（MCP pet_speak）：只念，不抢前台。
  - `turn-start`：只归档同 session 旧验收，不出声、不建卡。
  - 离线补投（replay）：只落工单，不补喊、不抢前台。
- `{宿主}` 取值由 `plugins/agentrmc-bridge/lib/host.js` 按安装路径 / 环境变量判定：
  Qoder、Qoder CN、ZCode、WorkBuddy、WorkBuddy AI；OpenSquilla 只认显式声明
  （`AGENTRMC_NOTIFY_HOST` 或 `.opensquilla` 路径）。

## 一、自动提醒文案对照（按事件）

| 事件 | agentrmc-bridge 系（Qoder / Qoder CN / ZCode / WorkBuddy 两家） | DeepSeek Harness | OpenSquilla 桥 | ZCode 手工旧脚本 |
|---|---|---|---|---|
| **回合结束 stop** | 气泡+念：`{宿主}：{最终回复第一句≤80字}`；取不到回复 → `{宿主} 做完了，等你回来看`；**末句是问句则改走下面「等回话」** | 气泡+念：`DeepSeek Harness：{最终回复第一句≤80字}`；兜底同左；**末句问句同样改走「等回话」** | 气泡+念：`「{会话}」这轮跑完了，回来看看`（`--msg-stop` 可改，**不念回复正文**） | 气泡+念：`ZCode 已完成任务 {项目}，等你回来验收`（无项目时省略项目名） |
| **等回话（正文提问收尾）** | 气泡：`等你回话：{末句问话≤48字}`；念：`{宿主} 在等你回话`；按 `permission` 发出（出声 + 抢前台 + 待我处理卡）。正文提问没有选项卡片，宿主不为它派发任何等待事件，只能从回复收尾文字判 | 同款（判据在 `lib/text.js` 的 `trailingQuestion`，攒到的 assistant 正文直接判；气泡同文案，念 `DeepSeek Harness 在等你回话`） | 无（按模板念会话标题，判不了正文） | 无（不读 transcript） |
| **等授权 permission** | 气泡：`等你确认 {工具}：{路径/命令≤60字}`；念：`{宿主} 在等你点确认` | 气泡：`等你确认 {工具名}`（**不带路径细节**，无工具名时 `等你确认工具授权`）；念：`DeepSeek Harness 在等你点确认` | 气泡+念：`有个操作在等你确认`（`--msg-permission` 可改） | 气泡+念：`等你确认 {工具}：{细节}【{项目}】` |
| **等选方案** | 气泡：`等你选方案`；念：`{宿主} 在等你选方案` | 气泡：`等你选方案`；念：`DeepSeek Harness 在等你选方案` | 气泡+念：`「{会话}」方案出来了，等你选一个`（`--msg-plan` 可改） | 气泡+念：`等你选方案【{项目}】` |
| **新轮开始 turn-start** | 空文案，只归档旧验收，不出声 | 同左 | `新一轮对话开始`（同样只归档不出声） | 无此事件 |
| **其它事件** | `hook.message` 或 `有新消息` | 静默不发 | 静默不发 | `hook.message` 或 `有新消息` + `【{项目}】` |

细节口径：

- **最终回复压缩**（bridge 系 `trimSentence` / DSH `lib/text.js` 同款）：去代码块、
  去 markdown 记号、压空白，只取第一句，超 80 字截到 78 字加 `…`。
  bridge 系从 transcript 取最后一条 assistant 文本；DSH 从 assistant-stream 攒正文，
  `speakFinalReply` 配置可关（默认开）。
- **问句收尾判定**（bridge 系 `hooks/agentrmc-notify.js` 与 DSH `lib/text.js` 各有一份
  同名 `trailingQuestion`，2026-11-04）：净化后的回复**最后一个**问号后面只剩装饰符号
  （空白/`*`/引号括号/破折号/省略号）才算待答；气泡取该问号所在的**最后一句**（不是
  第一句——问句前面往往已经汇报过一轮），边界认 `。；！!？?` 和「点后接空白」的英文
  句点（所以 `0.4.0` 不会被拦腰截断），≤48 字；语音仍是短的 `{宿主} 在等你回话`。
  问号后面还写了正文（「要不要我改？我先按 A 做了」）、URL 查询串、光一个问号——
  一律仍按 stop 念：判宽了会把客套问句当成待答，念错比不念更烦。
- **permission 细节**（bridge 系 / 旧脚本）：取 `file_path || path || command || url`，
  超 60 字截到 57 字加 `…`；工具名 / 细节为空时对应部分省略。
- **OpenSquilla 模板变量**：`{会话}` = 会话标题缓存，取不到用会话 key 末段（≤24 字），
  再兜底「这个会话」；`--relay` 时模板可用 `{text}`（回复前 80 字）；整条截 200 字。
  另有 selftest 文案：`联动桥自检：桌宠出声说明桥是通的`。

## 二、触发钩子对照（同一件事各家怎么报）

| 宿主 | 回合结束 | 等授权 | 等选方案 | 新轮开始 |
|---|---|---|---|---|
| Qoder / Qoder CN | Stop | PermissionRequest | PreToolUse matcher=AskUserQuestion → `question`（映射成 permission 发出） | UserPromptSubmit |
| WorkBuddy / WorkBuddy AI | Stop | PermissionRequest | Notification(permission_prompt) → `prompt`（弹窗那一刻，准）+ PreToolUse 兜底 | UserPromptSubmit |
| ZCode（插件） | Stop | PermissionRequest | 无独立钩子，AskUserQuestion 落 permission 分支 | UserPromptSubmit |
| DeepSeek Harness | agent/turn-stopping | session/event `approval/asked` | session/event `tool/call` name=ask_user_question → `question` | agent/inbox/claimed（同 turn 去重） |
| OpenSquilla | session.event.done / chat.done / task.succeeded | session.event.approval_required | session.event.plan_presentation（发 `question` 类） | session.event.turn_committed |

- DSH 挂 `session/event`（emit，所有监听器都跑）而不是 `approval/request` /
  `user-questions/request` 瀑布——Web UI 应答者作答后不再调 next()，排后面的监听器
  永远轮不到（实测桌面端点完选方案桌宠一声没响）。
- WorkBuddy 的 PreToolUse(question) 在**用户答完之后**才派发，只能当兜底；
  准时信号是 Notification(permission_prompt)。两条信号靠 asked-question.json 去重（见下）。
- OpenSquilla 默认不订阅 `cron:` 会话（`--include-cron` 打开）；pause-file 存在时
  免打扰（turn-start 归档信号照发）。
- **等回话不新增钩子**：还是 Stop 那一条，hook 内部按 transcript 收尾把事件改发成
  `permission`。原生端一个字没改，所以这条能力只随插件走，不用出安装包。

## 三、去重机制汇总

| 机制 | 位置 | 窗口 | 作用 |
|---|---|---|---|
| asked-question.json | bridge hook（spool 目录） | 30 分钟 | Notification 先喊过，PreToolUse 晚到的同一弹窗不再喊 |
| 「等你选方案」迟到去重 | 原生 announceWork | 180 秒 | `question` 类事件 **或** message 以「等你选方案」开头，且近期喊过 → 跳过 |
| 等回话不参与迟到去重 | 原生 announceWork（planChoice=false） | — | 「等你回话」发的是 permission 且不带「等你选方案」前缀，180 秒窗口吞不到它；一轮最多一条，本身不刷屏 |
| blocked 连报去重 | 原生 reportWorkItem | 120 秒 | 同会话连报 blocked 只喊一次 |
| ready 压 stop | 原生 announceWork | 90 秒 | pet_work_update(ready) 喊过后，紧接着的 hook stop 不念第二遍 |
| 同 turn 去重 | DSH inbox/claimed | 同一 turn | 一轮多条消息只报一次 turn-start |
| session+kind 去重 | OpenSquilla 桥 dedupe | 15 秒 | 同一会话同类事件不重复发 |

## 四、MCP 手动工具与原生端兜底（各宿主一致）

- `pet_notify`：Agent 自拟一句话，只冒气泡不出声（阶段进度用）。
- `pet_speak`：Agent 自拟一句话，冒气泡 + TTS，只念不抢前台。
- `pet_work_update`：文案由 Agent 自拟；省略时原生端默认（`MainWindow::reportWorkItem`）：
  - `progress` → `正在处理`（不出声）
  - `blocked` → `卡住了，等你`（走 question 通道，120 秒去重）
  - `ready` → `做完了，等你验收`（走 stop 通道，90 秒内压掉随后到的 hook stop）
- **原生端兜底**（插件 message 为空时，`AgentBridge.cpp` `defaultFor`）：
  stop = `任务完成，等你回来验收`；permission = `等你确认一个操作`；其它 = `有新消息`。

## 五、已知不一致（改动前先看这里）

1. **宿主名**：bridge 系 / DSH 的语音必带宿主名；OpenSquilla 念的文案里**没有**宿主名
   （只有 title 字段是 OpenSquilla）。
2. **OpenSquilla 方案文案去重失配 —— 已修（2026-11-04）**：plan_presentation 改发
   `question` 类事件；原生端迟到去重改为「`question` 类 **或** message 以『等你选方案』
   开头」（`MainWindow::handleNotify` 算 planChoice 传给 announceWork），自定义
   `--msg-plan` 文案也不再失配。blocked 仍走自己的 120s 节流（planChoice=false），
   不被 180s 窗口吞掉周期连报。
3. **speech 分离**：bridge 系 / DSH 把工具名、路径留在气泡，语音只念短文案；
   OpenSquilla 和旧脚本没有 speech 字段，气泡全文照念（旧脚本会把路径念出来）。
4. **stop 念什么**：bridge 系 / DSH 念最终回复第一句；OpenSquilla 念会话标题模板；
   旧脚本念固定句。
5. **选方案触发时机**：Qoder CN 的 PreToolUse(question) 在**弹窗那一刻**就派发
   （2026-11-04 实测：提问 12:07:06 弹出、hook 12:07:06 起跑，另有一条 12:07:37 的
   迟到信号带着**另一个 session_id**，于是同一件事立了两张「等你选方案」卡，
   asked-question.json 也因 session 对不上而没拦住——这条待查）；WorkBuddy 相反，
   PreToolUse 要等用户答完，靠 Notification 准时信号；ZCode 插件没有 question 钩子。
6. **等回话覆盖 bridge 系 / DSH**（2026-11-04 新增）：两家都从「Agent 这轮最后说了
   什么」判（bridge 读 transcript，DSH 用 assistant-stream 攒的正文）。OpenSquilla 桥
   按模板念会话标题、ZCode 手工旧脚本不读 transcript，手里没有可判的正文，要补得先
   接通素材。
7. **blocked 的工单会被随后的 permission 提醒合并改写**：`WorkItemStore::upsert` 在
   1800 秒窗口内把同 Agent 的活跃卡更新成新事件（实测 12:06:52 报的 blocked 自定义
   文案，被 12:07:06 的「等你选方案」覆盖）。一张卡只表达最新那件要人动手的事，
   所以 `pet_work_update` 报完之后别指望原文还在。
8. **残留缺口：先报 ready 再用问句收尾会喊两遍**。`ready` 的 90 秒「压 stop」只挡
   stop 事件，等回话发的是 permission，`announceWork` 里那条 `readyAnnounced_.take()`
   根本走不到。后果是念完「做完了，等你验收」再念一句「在等你回话」，并且多抢一次
   前台。hook 与 MCP 是两个进程，要彻底修得让它们共享「这轮已经喊过」的记账；
   当前先在 skill 里约束模型：**用问句收尾就别补 blocked / ready**。

## 六、文案出处

| 内容 | 文件 |
|---|---|
| bridge 系全部 hook 文案 | `plugins/agentrmc-bridge/hooks/agentrmc-notify.js` |
| 宿主名 / raise 判定 | `plugins/agentrmc-bridge/lib/host.js` |
| 各家 hook 注册 | `plugins/agentrmc-bridge/hooks/hooks.json`（ZCode 通用）、`hooks-qoder/hooks.json`、`hooks-workbuddy/hooks.json` |
| DSH 文案与事件监听 | `plugins/deepseek-harness/index.js`、`lib/text.js`（文案断言见 `test/plugin.test.js`） |
| OpenSquilla 模板与事件映射 | `plugins/opensquilla-bridge/opensquilla-pet-bridge.mjs`（CFG / eventKind / renderTemplate） |
| ZCode 手工旧脚本 | `native/tools/zcode-hook/agentrmc-notify.js`（README「手工接入」用） |
| 原生端兜底 / 出声 / 去重 | `native/src/core/AgentBridge.cpp`（defaultFor）、`native/src/MainWindow.cpp`（handleNotify / announceWork / reportWorkItem） |
| MCP 工具定义 | `plugins/agentrmc-bridge/mcp/server.js`、`plugins/deepseek-harness/index.js`（工具段） |
| 念出口净化 | `native/src/core/SpeechText.cpp`（friendly）、`native/src/ui/VoicePet.cpp`（speak 接线）；测试 `native/tests/speech_text_test.cpp` |

## 七、念出口统一净化（2026-11-04）

气泡和语音以前用同一段文本，路径/带标点的名字会被 TTS 照念（「D 冒号 反斜杠…」）。
现在 `VoicePet::speak` 在发给 TTS 前过一道 `SpeechText::friendly`，**气泡仍显示原文**
（路径/标点看得见），只有念的那一份转成口语。覆盖所有宿主提醒 + MCP pet_speak +
内部反馈（`已切换到 {backend}`、`语音反馈失败：{error}` 等）；对话模式回复走 `speakAs`，
不经这道（那是用户主动对话，保留完整内容）。

转换规则（`SpeechText::friendly`，收尾复用 `ChatText::cleanReply` 去 markdown/表情/限长）：

| 输入 | 念成 |
|---|---|
| `D:\repo\foo.cpp` / `D:/repo/foo.cpp` | `D盘 foo` |
| 裸盘符 `C:` | `C盘` |
| 相对/POSIX 路径 `native/src/foo.cpp`、`/home/u/app.js` | 最后一段去扩展名 `foo` / `app` |
| URL `https://…` | 去掉 |
| 文件扩展名 `foo.cpp` | `foo` |
| 连字符标识符 `handoff-focus` | `handoff focus` |
| 装饰标点 `「修复 bug（紧急）」` | `修复 bug 紧急`（保留括号正文） |
| 表情 / markdown `**改好了** 😊` | `改好了` |
| 净化后无实义内容 | 返回空 → 跳过 TTS（气泡照常出） |

保留 `。！？，、；：` 做停顿；版本号 `0.3.9`、纯英文词（sensevoice）原样念。
断言见 `native/tests/speech_text_test.cpp`（`AgentRMCSpeechTest`，17 条）。