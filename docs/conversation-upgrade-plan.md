# 对话能力升级：借鉴 Luma 的方案

> 状态：**C 手机传文件已实现；B1 待答请求只读展示已实现**（2026-11-14 真机验收）；
> B2/B3 内联应答与 A OpenCode/Codex 接入仍为设计。触发：用户给了
> [Luma](https://github.com/modao7067/luma)（安卓上跑 Codex/Claude Code/OpenCode/DSH
> 等 CLI 的对话前端），要借鉴它的 OpenCode/Codex 接法、授权/选择项显示、文件传输，
> 升级 AgentRMC 的对话能力。Luma 源码已浅克隆到 `out/luma-src`（GPL-3.0，
> **只借鉴思路与协议事实，不抄代码**）。本文落到协议细节与 RMC 的具体扩展位。

---

## 0. Luma 与 AgentRMC 的根本差异（先摆正）

- **Luma：手机即算力**。agent CLI 跑在手机里的 Debian（PRoot/root chroot），Luma 是
  CLI 前面的对话界面。文件是「手机本地拷进 agent 环境」。
- **AgentRMC：手机即遥控**。agent 跑在 PC，手机经局域网（`LanWebServer`
  0.0.0.0:45916，配对令牌）看待办/结账/派一句话。文件是「手机→PC 跨设备上传」。
- 重合点：都解决「agent 停下来等人时喊你 + 把授权/提问变成可点选的交互」。Luma 支持
  的 DSH/Qoder/Codex/OpenCode 与 RMC 宿主高度重合，协议事实可直接复用。

## 1. 研究结论（Luma 怎么做的，协议事实）

### 1.1 OpenCode —— 干净的官方 HTTP REST + SSE（最好接）

Luma `OpenCodeClient.kt` / `OpenCodeSession.kt`：OpenCode 起一个 HTTP server
（`opencode-launch.py {port} {password}`，OkHttp 调用），关键 endpoint：

| 用途 | endpoint |
|---|---|
| 探活 | `GET /api/info` |
| 建会话 | `POST /api/session`（id/title/…） |
| **发消息** | `POST /api/session/{id}/prompt`（body 带 parts/files） |
| **读历史** | `GET /api/session/{id}/message?limit=100&order=asc&cursor=…`（**游标分页**） |
| 单条消息 | `GET /api/session/{id}/message/{msgId}` |
| **授权应答** | `POST /api/session/{id}/permission/{nativeId}/reply`（DELETE=跳过） |
| **提问应答** | `POST /api/session/{id}/form/{nativeId}/reply` |
| 打断 | `POST /api/session/{id}/interrupt` |
| 切模型 | `POST /api/session/{id}/model` |
| 事件流 | `GET /api/event`（SSE） |
| 活跃/子会话 | `GET /api/session/active`、`/api/session?parentID=…` |

文件：`OpenCodeProjection.promptFiles(text, images, cwd)` 把图片/文件作为 prompt 的
file part 引用（按路径）。**结论：OpenCode 的派发/记录/授权/提问全有官方 HTTP 接口，
比 ZCode app-server 还直白，是 RMC 下一个最该接的宿主。**

### 1.2 Codex —— app-server JSON-RPC（与 ZCode 同源）

Luma `CodexRpc.kt`/`CodexTransport.kt`/`ExternalCodexPolicy.kt`：Codex 跑 app-server，
JSON-RPC over stdio 或 **unix socket**（`--remote 'unix://…'`、`app-server proxy --sock`）。
方法如 `thread/loaded/list`（游标分页）、`config/read`、`skills/extraRoots/set`，
resume 带 `threadId`/`excludeTurns`。
- **提问**（`CodexAsyncQuestions.kt`）：Codex 发 `agentMessage`（`delivery:"async"`）带
  `questions[]`（title+options）；答复包成
  `<send_user_message_question_reply>[{questionItemId,question,answer}]</send_user_message_question_reply>`
  作为**普通用户消息**发回（不是独立 API）。pending = 全部问题 − 已答（按 questionId）。
- 附件（`CodexProjection.kt`）：input item `localImage`(path)、`mention`(file path+name)、
  `skill`(path+name)——文件按路径引用。

### 1.3 授权/选择项的统一数据模型（`CockpitContract.kt`）

```
UserRequest(id, title, body,
            choices: [RequestChoice],      // 授权：允许/拒绝/总是允许
            questions: [RequestQuestion],  // 提问：单选/多选/自由输入
            answering, submitChoice, skipChoice)
RequestChoice(id, label, description, sandbox, approval)
RequestQuestion(id, header, prompt, options[], multiple, secret,
                allowOther, required, defaultSelection[], optionValues[])
```
- 会话视图 `SessionView.requests: [UserRequest]` —— **待答请求内联在对话里**，不是单独弹窗。
- 统一应答动作 `answer(requestId, choice, answers: Map<questionId, List<answer>>)`。
- 状态机：`answering`（已发待确认）防止重复发送；pending 跟踪已答/在途。
- 提问支持单选/多选/自由文本（allowOther）/secret（密码式输入）。

### 1.4 文件：拷进 attachments 目录 + 按路径引用（`AgentRuntime.kt`）

- 入口：相册/文件选择（`OpenDocument`/`PickVisualMedia`）、**剪贴板粘贴图片**
  （`ComposerSelection.clipboardHasImage`）、拍照（`PhotoCaptureProvider` FileProvider）。
- 落盘：`contentResolver` 读流 → 拷到 `privateDirectory(env)/attachments/`（chmod 0700）。
- agent 访问：给 agent 的路径是 `hostPath(file)`（root）或
  `guestPath(env, "attachments/<name>")`（PRoot 客户机路径）——**文件拷进 agent 文件系统
  可达的目录，消息里按路径引用**，agent 自己读。
- composer：`Attachment(id, uri, name, busy)` 缩略图、可删除；`referenceFile(path)` 把
  文件浏览器里的文件引用进输入框；`FileBrowser(path, entries)` 浏览/预览/下载。

## 2. AgentRMC 现状基线（扩展位）

- 聊天页：`SessionTranscript`（只读分页，已支持 WorkBuddy/Qoder/ZCode/OpenSquilla/**DSH**）；
  桌面外壳 `chatOpen/chatOlder/pushChat`，手机 `GET /m/transcript`。
- composer：纯文本，`sendChat()` → `dispatch` 动作 → `SessionDispatch` 派回会话。
- 动作白名单（`LanWebServer::isAllowedAction`）：ack/dispatch/open/raiseZcode/restartZcode/readLast。
- 授权/提问：目前只走「待我处理」提醒 + 桌宠语音，**聊天页里没有内联可点选交互**。
- 派发通道：DSH 插件 followup / ZCode app-server / WorkBuddy UIA / OpenSquilla 网关 RPC。

## 3. 升级方案（三块，各自独立可分期）

### A. 接入 OpenCode（优先）/ Codex 的派发 + 记录

- **OpenCode**：新增 `OpencodeDispatch`（HTTP 客户端，照 `ZcodeDispatch` 的守卫/回执模式）：
  派发 = `POST /api/session/{id}/prompt`；记录 = `SessionTranscript::readOpencode`
  （`GET …/message?cursor=`，游标分页转成 RMC 的 offset-from-end 口径）；
  归属守卫 = `GET /api/session/{id}` 命中。server 地址/密码从 OpenCode 配置探明
  （Luma 用 `opencode-launch.py` 起；RMC 场景 OpenCode 已在 PC 跑，需探明其 server 口）。
- **Codex**：app-server JSON-RPC（复用 ZCode 的 ndjson 经验），resume(threadId) + 发消息；
  记录读 Codex 会话库。提问走 §1.2 的特殊回复消息。
- 价值：宿主从 4 个扩到 6 个，OpenCode/Codex 用户也能「派一句话 + 看记录」。

### B. 聊天页内联授权/选择项（借鉴 §1.3 模型）

- **B1 已实现（2026-11-14 真机验收通过）**：聊天页只读展示待答请求。
  - 插件 `pendingRequests`（按会话攒）：`approval/asked`→`approval/decided`、
    `ask_user_question` `tool/call` 解析 questions→`tool/result`/reply 清账；
    变化经 `POST /requests {exe,session_id,project,requests}` 推原生。
  - `AgentBridge::setRequestsHandler` 收账；`MainWindow::pendingRequests_`（内存账）；
    `chatStateFor()` 带 `requests` 字段（桌面外壳 + 手机 `/m/transcript` 共用）。
  - 聊天页（桌面+手机）在消息后渲染 `.reqcard`：🔐 授权（工具名+原因）、
    ❓ 提问（问题+选项列表），提示「暂在 DSH 界面应答」。真机截图确认卡片显示。
  - 桥测试增 /requests 204+payload 断言；插件测试 28/28、lanweb 81 全绿。
- `SessionTranscript::Result` 增 `requests: [UserRequest]`（choices/questions），从宿主
  当前待答请求填（OpenCode 的 permission/form、DSH 的 approval/asked + ask_user_question）。
- 聊天页渲染交互卡片：授权显示命令/路径 + 允许/拒绝/总是允许按钮；提问渲染选项
  （单选/多选/自由输入），点选即答。
- 新增动作 `answer`（白名单加）：`{cmd:'answer', id, requestId, choice, answers}` →
  路由到宿主应答接口（OpenCode `permission/form reply`；DSH 经插件）。
- 复用 RMC 已有的「待我处理」注意力队列做通知，聊天页做**就地应答**——提醒喊人、
  卡片点选，两条腿。`answering` 态防重复发送（照 Luma）。
- 价值：手机上不用切回 PC 就能批授权/答提问，遥控闭环真正成立。

#### B 的 DSH 应答 API（2026-11-14 解包核实，out/dsh-x/{uq-host,approval-host}.js）

- **提问** `ctx.userQuestions`（`dsh-user-questions`）：
  - `answer(agent, callId, {answers:[{id,selected,custom?}]})` 是公开 Remote 方法，
    steer 一条 `source.kind="user-question-reply"` 用户消息进 agent。**但只认
    `continued`（前台超时转待答）的提问**；前台提问走 `user-questions/request` waterfall。
  - 待答提问在投影 `userQuestionProjectionHandle.stateOf(session).questions.active`
    （state=open/continued）。
- **授权** `ctx.approval`（`dsh-user-approval`）：`request(req)` 落 `approval/asked`
  （id/toolName/callId/reason）→ `decide()` 跑 `approval/request` waterfall，应答者返回
  `allowed-once / rejected / cancelled / unavailable`（仅 allowed-once 是放行）。
- **核心难点（风险）**：前台提问与授权都走 **waterfall**，而 **DSH Web UI 本身就是
  瀑布上的应答者**（一作答就不调 next()，排它后面的监听器永远轮不到——插件现有注释
  已记载）。插件要内联应答须在瀑布注册应答者，并与 Web UI **竞争/共存**：
  - 插件应答者排在 Web UI 前并 hold 请求 → RMC 独占应答，DSH GUI 不再弹（需取舍）；
  - 或两边竞速（谁先答谁生效，另一边作废）——实现复杂，碰不好会**卡住 agent 授权**。
- **分期落地（2026-11-14 全部完成）**：
  1. **B1 只读展示** ✅：聊天页渲染待答请求（授权命令/提问选项），真机截图确认。
  2. **B2 提问内联答** ✅（真机闭环验证）：聊天页可点选项卡（`request.answerable`＝
     插件 15s 心跳在线 + kind=question）→ `/m/action cmd=answer`（白名单）→
     `handleWebAction` 落 `pendingAnswers_` 收件箱 → 插件 1.2s 取件
     `/request-answer/pending` → `answerQuestion` 两路：
     - **timed**（profile 配 mode:timed，有前台 wait）→ `wait.completion.resolve(batch)`
       即时回给等待中的轮次；
     - **legacy**（桌面默认，无 wait）→ 直接 `agent.steer` 一条
       `source.kind="user-question-reply"` 消息（与官方 `answer()` 同形同落点，
       绕过 continued 状态门），Web UI 经投影收卡、agent 认领 reply 当答复。
       inbox `nextTurn/nextStep` 查重防重复提交（官方 REPLY_QUEUED 同款判据）。
     真机验证：手机提交「手机上提交一次」→ 本会话直接收到该答案（callId 匹配）。
     插件测试 31/31（resolve/steer/查重三用例）、桥 /request-answer 路由测试全绿。
  3. **B3 授权内联答** ✅（opt-in 独占，2026-11-14 晚实现+单测）：进程内 Cordis
     `ctx.approval` 只有 `request()`、无按 id 应答口，Web UI 自己是 approval/request
     瀑布应答者。故做成 **opt-in 独占**：配置 `approvalInline:true` 时插件注册为
     独占应答者 hold 住请求，等 RMC 聊天页允许/拒绝（pull 模型成立——我们是应答者），
     桌面 Web UI 授权卡不再弹；**默认 false 保持安全只读**。聊天页授权卡 answerable
     时渲染 允许/拒绝 两键 → `cmd=answer{outcome:'allowed-once'|'rejected'}`。
     单测：B3 独占 resolve / 默认关不注册 两例绿（插件 33/33）。
     另证实 DSH Web API 层有干净 `respondApproval`/`answerQuestion`（dsh-Go
     dsh_client），若将来要「不独占、与 Web UI 共存」可改走该远程面。
  4. **OpenSquilla 内联应答** ✅（2026-11-14 晚，子代理实现+端到端自检）：外部桥
     `opensquilla-pet-bridge.mjs` 增「推待答+取应答」：approval_required/plan_presentation/
     交互式 user_input → POST /requests（answerable:true）；poll /request-answer →
     网关干净 RPC `exec/plugin.approval.resolve {id,approved,choice}` /
     `chat.clarify_submit {sessionKey,requestId,fields}` → ack。无 waterfall 竞争。
     `--no-answer` 可关。test-inline-answer.mjs 全绿（事件→/requests、答案→RPC、
     ack/销账/清账、--no-answer 零流量）。
  - OpenCode 的 permission/form reply 是干净 HTTP，无 waterfall 竞争，接 A 后 B 最易。

#### 横向对照（2026-11-14 晚：开源 dsh-Go × 本地 OpenSquilla 蛋仔 × AgentRMC）

- **dsh-Go**（github.com/ccch1mneyyy/dsh-Go，Flutter/Dart，AGPL-3.0，sdk/examples MIT）：
  手机本机跑 DSH（PRoot Ubuntu+Node，免 Root）或 SSH / DSHarness Remote 遥控电脑；
  `dsh_client` = DSH Web API Dart 客户端（respondApproval/answerQuestion/prompt/steer）；
  自带桌宠 Deepy/鲸娘 + 「App 外显示」桌面_overlay_。**只借协议事实/设计，不抄代码**
  （AGPL + 作者要求改造前联系 + 品牌素材不在授权内）。
- **OpenSquilla 蛋仔**（本地 desktop/pet，Electron 零依赖）：双角色——网关 WS 客户端
  （`exec.approval.resolve`/`plugin.approval.resolve` {id,approved,choice}、
  `chat.clarify_submit` {sessionKey,requestId,fields}、`sessions.send`）+ companion HTTP
  **45915 复用 AgentRMC 桥契约**（/state /speak /notify /dispatch/pending /dispatch/ack）。
  → OpenSquilla 侧内联审批/澄清是干净 RPC，无 waterfall；且与 AgentRMC 同端口契约可互通。
- **AgentRMC（本仓）**：B1 只读 + B2 提问内联答（steer/resolve）已真机闭环；B3 待走
  DSH Web API 远程面。三者命题同构：手机/桌面遥控 + 内联应答 + 桌宠提醒。

### C. 手机→PC 会话传文件（借鉴 §1.4，跨设备版）

- 手机聊天页 composer 加 📎（`<input type=file>`，图片/文件）+ 粘贴图片。
- 新增路由 `POST /m/upload`（令牌鉴权，multipart 或 raw + 文件名头，限大小如 25MB）：
  PC 落盘到 **`<会话工作目录>/.agentrmc/inbox/<session>/<时间戳>-<name>`**
  （或统一 `%APPDATA%/LIG/AgentRMCNative/uploads/<session>/`，二选一，见下）。
- 落盘后返回 PC 绝对路径；`sendChat` 把路径并进派发文本（如「请查看文件 <path>」），
  agent 按路径读（与 Luma「拷进可达目录 + 路径引用」同构，只是跨设备）。
- **落盘位置取舍**：① 会话工作目录下 `.agentrmc/inbox/`——agent 天然可达、随项目走，
  但会进 git 工作区（需 .gitignore 提示）；② 统一 uploads 目录——干净不污染项目，
  但 agent 需绝对路径访问（多数 agent 可读任意路径）。**建议①**（agent 可达性最稳）。
- 安全：上传要令牌、限大小/类型、文件名 sanitize（防穿越）、落盘目录禁执行。
- 价值：手机拍图/选文件直接喂给 PC 上的 agent，对话能力从「纯文本」升到「带附件」。
- **已实现（2026-11-14）**：
  - `LanWebServer`：`POST /m/upload?id=<待办id>`，body=文件原始字节，文件名走
    `X-AgentRMC-Filename` 头（percent 编码兼容中文）；CSRF + 设备令牌鉴权；
    上传 body 限放宽到 **25MB**（`kMaxUploadBytes`，其余路由仍 64KB）。
  - `MainWindow::lanUpload`：按待办 id 解析会话 → 文件名 sanitize（取 basename、
    去控制字符、限长保扩展名，杜绝 `../`）→ 落盘
    `<project>/.agentrmc/inbox/<session>/<时间戳>-<name>`（无项目退
    `%APPDATA%/LIG/AgentRMCNative/uploads/<session>/`）→ 返回 PC 绝对路径 + 事件流记一笔。
  - 手机聊天页 composer 加 📎（`<input type=file>`）：选文件 → 上传 → 路径自动填入
    输入框（「请查看文件：<path>」），用户可补话再发送，agent 按路径读。
  - **落盘位置（用户 2026-11-14 裁决：保持项目相对）**：`<agent工作目录>/.agentrmc/inbox/`，
    跟的是**会话的项目目录**而非 AgentRMC 安装目录（agent 天然可达）。落盘时自动写
    `.agentrmc/.gitignore`（`*`）防上传文件误提交进项目仓库。
  - **去冗余回执通知（用户 2026-11-14）**：聊天页已能直接看记录，派发成功不再弹
    「会话 X 回执已落账」；ZCode 桌面端不热刷新，保留重启提示；失败通知保留。
  - `lanweb_test` 增 10 项（401/403/200/404/中文文件名解码/100KB 超 64KB 限）。

## 4. 分期建议

1. **C 传文件**（最独立、用户最想要、改动集中在 LanWebServer + composer）→ 先做。
2. **B 内联授权/选择项**（遥控闭环的关键，依赖各宿主应答接口，OpenCode/DSH 先行）。
3. **A OpenCode/Codex 接入**（扩宿主，OpenCode 先于 Codex；需探明 PC 上 server 口）。

> 版权：Luma 是 GPL-3.0。本方案只复用**协议事实**（endpoint/方法/消息格式，属接口
> 事实不受版权保护）与**通用设计思路**，不复制其源码表达。RMC 保持自有许可。