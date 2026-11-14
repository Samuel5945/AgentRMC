# 待办、会话与派任务：方案与落地状态

> 状态快照 2026-11-06（对锚 `cd70ddd`）。本文只记录**已经定型**的东西：借鉴了什么、
> 定了什么、做到哪一步、还剩什么。代码位置按仓库相对路径 + 行号标注。

---

## 1. 一句话结论

桌宠是**注意力 Router**，不是第二个 Agent 运行时：宿主继续当 Worker，我们负责
「谁在干什么、卡在哪、是否需要人」，并把人送回正确的**会话**——不是送回某个 App。

两件事同时推进，互不替代：

- **待办与派发**（数据面）：会话身份 → 待办显示 → 定向派发。
- **界面渲染**（表现面）：Qt 只留系统集成，界面交给 WebView2 的 Chromium 渲染。

## 2. Magpie 研究结论（核验 `09812e66bc93`，仅静态源码）

| Magpie 有的 | 能不能借鉴 |
|---|---|
| `Session{Agent, ID, Cwd, Title, Start/Last, Resume, ParentSession…}` | **借鉴**：会话要有稳定身份；host 只当类型，不当实例 |
| 扫描各宿主 `projects/*.jsonl`，缓存 + 增量，按宿主／项目聚合 | **借鉴**：只读索引补标题与项目；不写宿主数据库 |
| WorkBuddy 解析器明确针对桌面版（JSONL + 元信息 + ai-title/custom-title） | **借鉴**：先在本机验证文件结构再接 |
| Qoder 解析器读的是 **CLI** 的 projects 与 `qodercli --resume` | **不能**当成 Qoder Desktop 支持，必须单列验证 |
| Resume 以「命令」为核心，直接开终端限 Mac App，新开不接任务 prompt | **不能**当派发闭环 |
| `Pending` = 索引未完成；网关 route 描述最近请求 | **不能**当待办队列／任务完成 |

结论：Magpie 是**会话目录 + 恢复入口**，不是待办调度器。AgentRMC 缺的是
Task/WorkItem 的**会话归属**与**可验证投递**，这两块得自己补。

## 3. 三批路线（数据面）

### 第 1 批：会话身份（已落地）

- 认领旧工单看**会话身份优先**：同 session 的卡 > 待绑定的派活回执 > 未关联的卡 >
  同宿主唯一一张已绑定的卡（`native/src/core/WorkItems.cpp:201` 起）。项目两边都知道
  又不一样就不算同一件事；不带项目的上报不再把卡片项目抹空。
- **一个会话只留一张活卡**：`closeOtherStates()`（`WorkItems.cpp:440`）在「合并进已有卡」
  与「新建卡」两处守住；状态一变，同会话旧账翻篇。
- 后续实机修正（`cd70ddd`）：WorkBuddy 的会话不再被误记到 Qoder CN 名下，判据顺序纠正，
  并对已错标的同 session 记录做归属自愈。
- 桩：`native/tests/workitem_test.cpp` 共 39 项断言（含串单、等待排序、陈旧判定、JSON 同序），
  两处不变式各打一次变异，分别只被自己那条用例抓死。

### 第 2 批：待办显示（已落地）

- 排序认**等待起点**（`attentionSince`）：`displayOrder()`（`WorkItems.cpp:398`）同一档内
  先开始等人的在前——一条新进度不再把等人的老账挤下去。
- 「等了多久」：`waitedSecs()`（`386`）→ 行尾 `· 等 N 分 / 小时 / 天`，时间列固定宽、右贴边。
- 「状态待确认」：`staleWorking()`（`393`）——挂着「进行中」但 30 分钟没有任何动静时，
  不说它在跑，也不说它做完。
- 行身份：项目目录名 → `会话 <短 id>` → `未关联会话`；同宿主同项目重名时才补会话短 id。
- `GET /work-items` 与面板同序（`toJson()` 走 `displayOrder()`，已结案排后面不丢）。
- 双击 = **先送到 Agent（没开就启动）再销账**；手动「标为已处理」只结账、不启动
  （`MainWindow.cpp:2686` `bringToAgent`；`3106` 附近的 `openWorkItem`）。

### 第 3 批：定向派发（已落地，Qoder 线等上游）

目标：从某条待办把指令派回**它那条会话**，并拿到「确实到了」的回执。

前置能力必须先逐宿主探明，**不能假设**：

| 能力 | WorkBuddy 系 | Qoder 系 | ZCode 系 | DSH |
|---|---|---|---|---|
| 拿到真实会话 id | 有（插件从宿主取） | 有（hook payload） | 有（hook payload / sqlite） | 有（agent.id，即盘上 `session-<uuid>` 目录名） |
| 打开指定会话 | **已接线**（UIA 通道，见下） | 桌面通道证伪（CLI 已否决） | 不可热打开（重启宿主后人工打开，见下） | 不需要（API 注入，见下） |
| 按 id 发送并回回执 | **已接线**（UIA 通道，见下） | 桌面通道证伪（CLI 已否决） | **已接线**（app-server 协议，见下） | **已接线**（插件进程内 followup，见下） |

#### WorkBuddy 系实测（2026-11-07，探针 `rc_probe/wb_open_session.py` / `wb_send_receipt.py`）

- 会话身份在盘上：`~/.workbuddy(-ai)/projects/<项目 slug>/<session id>.jsonl`；
  `ai-title` 行给标题（异步生成，首轮后约几秒），`message` 行即投递回执
  （含 `sessionId`）。UIA 树里**没有 session id**，会话行 Button 只带标题。
- 打开指定会话：侧栏会话行 Button 有 Invoke pattern，按标题点它，再读聊天区
  标题 Text 证明已切换（`暮霞…` / 探针会话往返均读回正确）。
- 发送前归属证明：聊天区标题 Text == 目标 `ai-title` 才允许发送；证明不了就不发
  （探针里证明失败的那几轮确实一条没发）。
- 发送与回执：composer 是唯一 Edit，剪贴板粘贴 + 读回证明内容 + Enter 发送；
  回执 = 目标 session 的 jsonl 新增含探针标记的 `message` 行。定向发送第二轮
  落在同一文件、其它会话零泄漏（`TARGETED_SEND OK / NO_LEAK True`，两变体各一遍）。
- 坑（派发实现必须处理）：
  1. 主窗会最小化或收进悬浮挂件：先按类名 `Chrome_WidgetWin_1` + 产品名找主窗并唤醒；
  2. 侧栏是虚拟化列表：滚出视口的行整棵子树不存在，先滚轮滚到顶；
  3. 分组（任务/空间）切走后会折叠：点组标题 Text 展开，组内行才出现；
  4. 行标题可能是截断首句或旧 ai-title：用前 12 字双向匹配，别全等；
  5. 剪贴板是共享资源：用前保存、用后还原。
- 探针留下三个无害会话（classic `566902ff…` / `abd603af…`，AI `42bca911…`，
  标题含「链路探针」），宿主只回了「收到」类短句；要不要删由用户定。
- **路由已接线（2026-11-07 晚）**：`core/UiDispatch` 把上述机制做成异步状态机
  （唤醒→等树→滚顶/分组双 toggle→点行→读回证明→粘贴读回→Enter），
  `SessionDispatch` 负责守卫（会话库自报身份、10s 活动闸、同会话在途、
  **UI 投递全局串行**）与回执轮询（jsonl 新增 user 行含派发原文）；回执落账
  把工单转进行中。活体 harness 对自建牺牲会话 `9b3f70f0…` 全链通过
  （`LIVE RECEIPT … WorkBuddy.exe`），桌面可见。外壳派发行按
  `dispatch.supported` 只对 WorkBuddy 两变体开放。
- 接线时实测补的坑：① Chromium 全量 a11y 树要在前台被查询后才建，named<15
  就等一拍重扫，别拿空树滚侧栏；② 分组开关是 toggle，不知展开/折叠就点一次找
  一遍再点回来找一遍；③ 聊天区顶部有时渲染首句气泡而非 ai-title，证明要两者
  都认；④ WorkBuddy 的 user 记录 role/content 在顶层、part 叫 input_text、正文
  外包 system-reminder 与 user_query，取标题要剥包装；⑤ 纯 Core 测试 harness
  默认 DPI unaware，GetWindowRect 与 UIA 坐标不一致，harness 起手要设 PMv2
  （主程序走 Qt Widgets 无此问题）；⑥ 聊天区标题实测在 rel y≈97，证明区域下界
  卡 y>100 会把标题切掉、只剩气泡（多轮会话停底部时顶部是最新一条），误报
  「读回不符」——区域放宽到 y>60；⑦ 拒绝/回执通知要绑定当次派发的卡 id，
  否则旧拒绝原因会挂在新选中的卡下面误导人。

#### Qoder 系实测（2026-11-07，探针 `rc_probe/qoder_send_receipt.py`）

- **桌面 UI 自动化不通**：Qoder CN 主窗 UIA 树只有标题栏三个按钮，renderer
  accessibility 默认不开（WorkBuddy 会开，Qoder 不会）；不重启加 flag 拿不到树，
  而重启会打断正在跑的会话，不做。
- **CLI 与桌面共用会话库**：`~/.qoder-cn/projects/<slug>/<id>.jsonl`；
  `qoderclicn --list-sessions` 能列出桌面会话（含当前会话），标题取首条用户消息。
  记录类型是 `user` / `assistant`（带 `sessionId`），**没有 ai-title 行**。
- 发送与回执（**重做轮，2026-11-07 晚**）：`--session-id <新uuid> --tools "" -p`
  发探针1，jsonl 落 `user` 行回执；`--resume <id> -p` 问「上一条探针标记」答对
  原标记 → 证明 resume 载入的就是该会话；回执2 落同一文件、其它会话零泄漏
  （`TARGETED_SEND OK / NO_LEAK True`）。
- **上一轮探针有事故，结论以重做轮为准**：`--tools ""` 只禁模型工具，插件 hook
  照跑——探针会话的 Stop 经 agentrmc-bridge 落进真实待办库（`06a433cc` 一条
  done 卡）。重做轮隔离四件套：`--settings '{"disableAllHooks":true}'` 关全部
  hook、`--strict-mcp-config` + 空 mcpServers 关 MCP、cwd 用临时目录（会话落在
  临时项目 slug）、跑前后对真实 work-items.json 做 sha1+mtime 断言
  （`REAL_STORE_UNTOUCHED True`）；结束自删本轮探针会话文件。
- 坑：
  1. 从 Qoder 桌面派生的 shell 里跑 CLI 会带 `QODER_AGENT_SDK_ENTRYPOINT` 等
     环境变量，CLI 报 `sdk_invalid_args` 拒绝启动——派发实现必须先清掉
     （native 侧 `d214d6f` 已在启动时洗掉继承的 QODER* 会话变量）；
  2. `--tools` 是**变长参数**：`--tools "" <prompt>` 会把 prompt 吞掉，CLI 静默
     rc=42；必须写成 `--tools "" -p <prompt>` 让选项终止变长收集；
  3. `--config-dir <临时配置家>` 会丢登录态（同样静默 rc=42），不能用来隔离；
     隔离靠 disableAllHooks + 空 MCP + 临时 cwd，不动配置家。
- **用户裁决（2026-11-07 晚）：CLI 不适合作派回路径**——送达与回执都成立，但
  Qoder CN 桌面端对 CLI 追加的轮次毫无反应（会话按工作区归堆、桌面不重扫），
  人回不去那条会话，闭环不成立。路由里 CLI 通道已休眠
  （`SessionDispatch::configHomeFor` 全宿主返回空，外壳派发行按
  `dispatch.supported` 隐藏），守卫/回执/草稿骨架保留待桌面通道。
- **桌面可见通道两候选均已证伪（2026-11-07 深夜，Qoder CN 0.5.6 = 官方最新）**：
  ① deep link——`qoder-cn://` 协议已注册且主进程有 open-url →
  `deepLinkMainService`，但路由器只注册 3 个 handler（asar 逆向，与
  [官方文档](https://docs.qoder.com/user-guide/deeplink)一致）：
  discussion/issue/invite（云端协作对象，UUID+token 校验）、`mcp.add`、
  `extension.install`，**没有「按 session id 打开已有会话」的路由**；
  官方的 `/chat?text=…&isNewChat=false` 也只能预填「新建或当前」聊天且永不自动
  发送，CN 包里连这 3 个 chat 系 handler 都没有（CN 把 `aicoding.aicoding-deeplink`
  hostname 整体委托给 IDE 配套，重写为 `qoder-work-cn://`）；
  ② a11y 设置——`editor.accessibilitySupport` 在 CN 0.5.6 asar 里 **0 命中**，
  也没有 `setAccessibilitySupportEnabled`：Qoder 不是 VS Code workbench，
  没有这个设置可开。唯一能让 renderer 出树的是 `--force-renderer-accessibility`
  启动参数（Electron 核心开关），但须重启应用、打断在跑的会话，此前已裁决不做。
- 本地 IPC 亦非会话控制面：`~/.qoder-cn/mcp-router.json`（127.0.0.1:8628，
  `X-Api-Key` sha256 认证）是给外部 agent 代理 MCP server 的**反方向**通道；
  `remote-control-broker/` 是 Qoder 自家云端遥控的 local↔remote 会话绑定
  （`qs_…` 云会话 id），入口在他们的云，本地无调用面。
- UIA 复检（`rc_probe/qoder_recheck_uia.py`，只读）：CN 与国际版主窗都仍是
  7 元素荒树（6 Pane + 1 Window）。
- 国际版：本机有 app 无 CLI（`~/.qoder/bin` 只有工具二进制）；桌面 UIA 复检
  同样荒树，deep link 表未逆向（同族，预计一致）；定向派发结论与 CN 相同。
- 残留：上一轮事故留下会话 `06a433cc…` 与待办库里一条 done 探针卡，可删；
  重做轮自建的探针会话已自清，真实待办库指纹跑前后一致。
- **回复内容上卡（2026-11-07 晚）**：stop 落账时读该会话 jsonl 最后一条
  assistant 正文（`SessionDispatch::lastReply`，剥包装/压空白/截 60 字）替换
  通用收尾文案进待办卡，并在事件流记「会话回复：…」；「等你回话/选方案/确认/
  模型出错」这类要人动手的文案保留原语义。实测 WorkBuddy 卡从「做完了，等你
  回来看」变成回复正文「收到」。Qoder 系因通道休眠暂不生效（configHome 为空）。
- **现有派活老路端到端实测（2026-11-07 晚，`rc_probe/dispatch_e2e.py`）**：
  用户新建牺牲会话 `899a6b7c…` 并停在前台，走外壳左栏语音形态「打字框+发送」
  真实入口派发带标记探针 → 标记落在该会话 jsonl、宿主回「收到」、AgentRMC
  建 voice 工单并绑定该 session（stop 合并翻篇）、本会话零泄漏。即
  **「续当前会话」模式对 Qoder CN 可用**；定向任意会话（第 3 批路由）仍未接线，
  国际版未验。护栏：发送前断言前台为 Qoder 系，发送后断言目标回执+本会话无标记。

#### ZCode 系实测（2026-11-07 深夜，探针 `rc_probe/zcode_appserver_probe.py` / `zcode_dispatch_probe.py` / `zcode_uia_probe.py`）

- **桌面形态**：Electron（3.17.2），UI 与 agent 后端分离——桌面拉起
  `ZCode.exe resources/glm/zcode.cjs app-server --stdio`（`ELECTRON_RUN_AS_NODE=1`），
  之间走 **ndjson 双向协议**（`{"id":N,"method":…,"params":…}` ↔ `{"id":N,"result":…}`）。
- **UIA 与 deep link 同样不通**：主窗前台复扫后仍荒树（12 元素无 named，
  asar 里也没有 renderer-a11y 开关可开）；`zcode://` 只注册了
  `oauth/callback` 与 `workspace/open?path=`（开文件夹），无会话路由。
- **正解是 app-server 协议**（合法宿主 API，符合「宿主支持 API/插件命令」约束）：
  `session/list`（列会话，含 id/title/status/workspace/updatedAt）、
  `session/create`、`session/resume`、**`session/send`**（按 sessionId 投递，
  `{sessionId, content, …}`；忙时 -32010 拒绝）、`session/messages`（读收据）、
  `session/subscribe`/`session/events`、`session/stop` 等。
  注意两点：① server 会**反向请求客户端**，`session/create` 前会要
  `session/requestRuntimePreferences`，必须成功应答（zod `Uon` schema 只有
  `nativeSearchEnhancementsEnabled` 必填，其余有默认；回 error 或不回，create 直接失败）；
  ② 会话消息 shape：role 在 `msg.info.role`，正文在 `parts[]` 里 `type:"text"` 的
  `text` 字段（`step-start/reasoning/text/step-finish` 为一组）。
- **活体验收（炮灰会话 `sess_b7605697…`，workspace 即本仓库）**：
  `session/create` → `session/send`×3（全按 sessionId 命中）→ 助手真实回「畅通」
  （assistant 消息 parts 含 text，判据 role+text 无污染）→ `session/messages`
  读回。`session/resume` 跨实例可重开别人实例建的会话——**dispatch 到用户既有
  会话就是同一条路**。会话按 workspace 归堆，db 为共享 SQLite
  （`~/.zcode/cli/db/db.sqlite` + `rollout/model-io-<sid>.jsonl`）。
- **两条投递路径（2026-11-08 定案）**：
  ① **裸 `session/send`**：`{sessionId, content, modelSelection}`，消息直接落库、
  turn 照跑、hook 照常上卡——个人供应商模型会话全链通；**重开应用后对话区正常
  渲染**（与手发消息无差别）。
  ② **`v4/command` sendText**（stdio 同样服务 v4/*）：`{commandId, clientId,
  sessionId, type:"sendText", payload:{text, requestedDelivery:"startNow",
  modelSelection}, issuedAt, inputOrigin:"desktop"}` → ack `inputAccepted` →
  `session_input` 表落 `promoted` 行（与用户手发同一条准入路径）。**运行期对话区
  两条路径都不热刷新**（见下），落库后可见性无差别；v4 路径的额外价值是队列
  语义（孤儿补 drain）。
- **模型选择规则（用户裁决，2026-11-08）**：payload/setModel 一律带**该会话
  `session_entry` 表 `runtime/model_selection` 行的当前值**（= 用户在 UI 里选的），
  from==to 零净切换。禁止断言旧值或别的模型——modelSelection 是**会话级切换**，
  带错值会把用户定的模型顶掉（实测踩过，会话顶出「模型已切换」横幅）。
  无模型字段则 turn 死在 `model_creation`（新实例不绑模型：「Select a model
  before continuing」）。
- **账号计划模型边界（用户裁决：不破解）**：`account:zai-start-plan/*` 等账号
  供应商只活在桌面登录态里——外部实例注册表没有它（`provider/refreshRegistry`
  也补不进），raw/v4 两路都死在 `Model creation failed`；本地
  `credentials.json` 里的账号 API key 是加密存储（`enc:v1:`），**不从桌面进程外
  解密使用**。→ 账号模型会话暂不可外部派发，等上游开放；个人供应商会话可派发。
- **连接要活到落库+turn 结束**：消息落库是**异步**的——探针发完立刻 kill
  app-server，消息根本不进库（实测踩过：10:00/10:04 两条「已接受」的消息消失，
  假焦点/滚动实验全部作废）。真实派发器常驻连接 + 发后确认落库（查
  `message` 表 MAX(time_created)）再收尾。孤儿 input（promoted 后 turn 被杀）
  不丢——下次 resume 自动补 drain。
- **对话区可见性（2026-11-08 定案，全部对照实验过）**：外部落库的消息**在本进程内
  不再出现**——焦点切换、滚动、关标签重开、切项目、开新窗、最小化还原、外部实例
  resume/二次 resume **全部无效**（每项都对「已确认落库」的消息测过；曾误判
  「40-60 分钟自现」，实为重启后**首次打开会话时 runtime 首载读库**，会话运行时
  随进程驻留、关闭标签也不驱逐，之后同进程内永远陈旧）。→ 可见性 = **重启
  ZCode**；派发 UX 以**卡片为中心**：回执即时上卡，人回会话时值得重开一次宿主。
- **刷新机制代码级排查（2026-11-09，3.17.2 / app 0.21.1，asar 解包核对）**：
  上面的实验结论在代码里全部对得上，且**没有留任何外部热刷新口子**——
  ① 桌面实时总线 `TaskRealtimeBus`（out/main/index.js）只认本进程 utilityProcess
  fork 的 host（`registerHost` 收 MessageChannelMain port），`broadcastHub` 只向
  本进程 utility 子进程扇出，`sessionRoutes` 按 hostId 路由、host 退出即清；
  外部 app-server 进程不在册，事件进不去。② `session/lifecycle/changed`、
  `v4/sessionsIndex/reconcile`、`v4/conversation/resync` 都是**实例内**协议
  （各自 app-server 连接上的通知/请求），不跨进程广播。③ `ZCODE_OFFICIAL_ENDPOINT`
  命名管道是**云端 API 鉴权代理**（createOfficialCommandEndpoint：Bearer token →
  resolveHeaders → fetch zcode-server），不是桌面控制通道。④ `zcode://` deep-link
  只支持 workspace/open（外部路径还要弹确认框）、支付回调、share import、OAuth
  四种，**没有打开会话的路由**，second-instance 只做聚焦。⑤ 主进程无 db/rollout
  文件 watcher（仅 file-watcher 服务给编辑器用）。→ 「重启宿主」是当前版本唯一
  可见性路径，属上游架构使然；等上游提供跨实例通知或会话 deep-link 再改。
- **hook 隔离四件套（照搬 Qoder 重做轮教训）**：hook 事件走 agentrmc-bridge
  （UserPromptSubmit/PermissionRequest/Stop），桥的 `AGENTRMC_PORT` 与
  `AGENTRMC_SPOOL_DIR` 都是**环境变量**——给探针 app-server 喂
  `AGENTRMC_PORT=1`（不可路由）+ 临时 SPOOL_DIR，hook POST 失败落探针 spool。
  实证：探针 spool 收到 `turn-start`（带炮灰 sessionId），真实 store 与真实
  spool 全程无 MARKER。
- **已接线（2026-11-09）**：`SessionDispatch` 的 ZCode 路由按上面配方接完
  （spawn 常驻 app-server → 反向偏好应答 → resume → 裸 send 带 fresh
  modelSelection → 确认落库 → 轮 `session/events` 等 `turn.completed`）；仅对
  **个人供应商模型**的会话开放（账号模型会话按 `dispatch.supported` 关闭派发行）。
  接线后补的两处硬需求：① CLI 形态（ELECTRON_RUN_AS_NODE 跑 zcode.cjs）找内置
  供应商配置只认两个死路径，桌面端实际在 `resources/config/provider/`——spawn
  必须注入 `ZCODE_BUILTIN_PROVIDER_CONFIG_FILE`，否则启动即 exit=1；
  ② 对话区不热刷新，回执卡上带「重启 ZCode」按钮（`restartDesktop`），该卡
  通知态藏「回到 Agent」、Enter/双击只聚焦不销账（避免送进陈旧视图还顺手结账）。
- 与 Qoder 的本质差异：Qoder CLI 追加的轮次桌面**永不重扫**；ZCode 落库即持久，
  重启应用全量可见（会话运行时进程内驻留不重读）——闭环成立，UX 以卡片为中心。
- **终态与交互（2026-11-08 审查加固，mock 状态机回归锁定）**：整轮完成只认
  `session/events` 里的 `turn.completed`（`payload.response` 即最终正文）/
  `turn.failed`——首条 assistant 正文常是「我先检查一下」，拿它当完成会截断
  还在跑的轮次并 kill 后端。发消息前先记事件 seq 基线（`limit:1` 取最后一条），
  只认基线之后、且 `turnId` 与首条 `turn.started` 一致的事件。授权/提问请求
  （`interaction/requestPermission` / `interaction/requestUserInput`）**按协议明确
  拒绝**（`{decision:"deny"}` / `{action:"decline"}`）而不是回 -32000「不支持」，
  同时发一次 `interactionSkipped`：人知道「这一轮卡在等人、要回 ZCode 桌面处理」，
  模型也不会把「没人能答」误当成工具坏了。阶段时钟只在进入阶段时重置（轮询
  重置会让超时永不触发），轮询节拍单独计时。
- **卡片口径**：派发轮晚到的回复只补正文、不把 hook 已推进的「待验收/等你处理」
  降回进行中；回复缓存按会话在 turn-start / 新派发时清空，不拿上一轮正文冒充；
  未知 `dispatch_id` 的 ack 回 404（不能把无效回执当成功）。
- **「提前退出」取证与诊断加固（2026-11-09）**：用户实测定向派送报「ZCode
  app-server 提前退出」。从 `~/.zcode/cli/log/zcode-<date>.jsonl` 还原当天所有
  `zcode_protocol_agent` 实例生命周期，定位到两种死法：① 实例启动 +122ms 即
  `startup.failed: Protocol input closed`（stdin 管道在 resume 前就 EOF）；
  ② 实例 storage ready 正常、却永远收不到 resume，sessions=0 空转。对照实验：
  Python 控制台父进程跑完整 create→resume→send→落库**全通**；专编的 GUI 子系统
  （winexe、无控制台）C# 探针模拟 AgentRMC spawn 方式，stdin 保持打开 + session/list
  正常应答，**也通**——协议本身没坏，问题在 AgentRMC 进程 spawn 子进程时 stdin
  管道的具体环境（疑与它被谁拉起、继承的环境有关）。旧代码把真实死因吞成一句
  「提前退出」，无法定位。**修复**：`ZcodeDispatch` 捕获子进程 stderr 尾巴（封顶
  4KiB）+ exit code + 当前阶段，提前退出时拼进失败文案（如「…提前退出（resume
  阶段，exit=3）：Error: Protocol input closed」）；`start()` 补齐 `sent_/done_/
  buffer_/nextId_/awaitingId_` 重置防复用脏状态。另补协议缺口：
  `interaction/requestOfficialMcpAuthHeaders` 旧代码回 -32000，服务端降级
  `official_auth_unavailable` 且日志全是 auth_request_failed 噪音——现按 schema 回
  `{ok:false, reason:"official_auth_unavailable"}`（外部实例拿不到桌面登录态鉴权头，
  用户裁决不破解）。回归：dispatch_test 新增 early-exit / official-auth 两个 mock
  场景（已用 Electron-as-node 实跑验证）。**注意**：诊断加固让下次失败能直接看到
  死因，但 stdin 管道为何 EOF 的根因仍待在有工具链的机器上 build 后复测确认。

#### DSH 系实测（2026-11-07 深夜，插件临时探针 + `rc_probe/dsh_zstd_scan.py`）

- 通道既不走 UI 也不走 CLI：DSH 的扩展单位是 Cordis Host 插件，而 **agents
  服务就在插件 ctx 上**（`session-controller` 同款 `inject: ['agents',…]`；
  desktop-host 退出检查也用 `agents.list()`，`apps/desktop-host/src/quit-inspection.ts:31`）。
- 官方注入面：`Agent` 运行时面有 **`followup(message)`**（「排队一轮普通
  follow-up 并唤醒 driver」，`packages/core/agent/src/runtime-types.ts`）；ACP 桥
  测试即同款原型 `agent.followup(createUserMessage({content:[{type:'text',text}],
  source:{kind:'user'}}))`。Web UI 的正路 `SessionCommands.prompt()`
  （`packages/api/session-controller/src/commands.ts:317`）就是 resolveAgent →
  admission → followup，且 `resolveAgent` 对**离线会话会从盘上 resume**
  （`agent.ts:151`）——接线时照它走，未打开的会话也能派。
- 网关通道否决（native 侧）：桌面本地口 127.0.0.1:10624 要 HMAC cookie +
  credentials 存储（`packages/client/connection/src/browser-auth.ts`），外加整套
  typert Remote RPC——native C++ 复刻太重；插件进程内注入用官方服务面，零协议成本。
- 探针过程（注入代码为临时改动，验证后已撤）：给 `plugins/deepseek-harness/index.js`
  加文件触发的 `list`/`inject` 探针（读
  `%APPDATA%\LIG\AgentRMCNative\dsh-probe.json`，写 `dsh-probe-result.json`），重启
  DSH 加载；用户自建牺牲会话 `9c08421f…`（default-workspace，空会话）；
  `agents.get(<id>)` → 手搓 UserMessage（`{id,role:'user',content:[{type:'text'}],
  source:{kind:'user'}}`，与 ACP 同形，不必 import @deepseek-ai 包）→ `followup()`。
- **结果全绿**：注入被官方接受（durable 记 `agent/inbox/spliced` + `user/message`，
  source 落 `kind:"user"`）；轮次跑通（egg-alpha，重试 5 轮 ~57s）；**回执免费**——
  turn-start / turn-stopping 通知本来就带 session_id，native 落卡绑定正确会话，
  `stop` 卡 message 即回复正文「收到 AgentRMC-P1-9K3T」；zstd 全库扫描 22 个
  会话文件，标记只在目标会话，其它会话零变化零泄漏（改前改后全库文件 diff 亦仅
  目标一个）。
- 坑（接线时必须处理）：
  1. 会话库 `session.v4.jsonl.zstd` 是**多帧拼接**：zstandard `decompress()` 只解
     第一帧，追加轮次全在后续帧——必须 `stream_reader(read_across_frames=True)`
     跨帧流式读，否则全库扫描零命中假阴性。native 侧回执走插件通知即可，不必读盘。
  2. turn-start 归档会 touch 旧卡（updated_at 刷新、created_at 保留），对账别把
     它当新卡；轮次本身仍由 LLM 节奏决定（首轮实测 ~57s，二段 ~2.5s），回执
     （= ack）不等它，但派发后「卡转 review 带回复」要等轮次跑完。
  3. `agent.id` 带 `session-` 前缀（WorkBuddy 是裸 uuid），正好是盘上目录名，
     守卫判别可直接对目录。
  4. 插件里加定时器要 `unref()`，否则测试进程跑完不退出。
- 待清理（用户裁决）：牺牲会话 `9c08421f…`（含两条「收到」轮次）与其 `review`
  待验收卡；在跑的 DSH 进程还持着撤码前的探针代码（文件触发、无文件即惰性），
  下次重启自然回到干净代码。
- **已接线（2026-11-07 深夜）**：`SessionDispatch` 加插件通道（`Channel::Plugin`），
  `AgentBridge` 开 `GET /dispatch/pending?exe=…`（取件）与 `POST /dispatch/ack`
  （回执）两条路由；插件侧生产化（`dispatch: true` 默认开，1.5s 轮询取件 →
  `agents.get()` 守卫 → 手搓 UserMessage → `followup()` → ack）。**回执 = ack ok**：
  followup 同步落 durable inbox，与 WorkBuddy 的 jsonl user 行同级证明，不引入新
  通知事件类型；ack ok:false 带插件侧拒绝原因走既有 `failed` 通道上卡。守卫分工：
  native 只做 id 格式哨兵（必须 `session-` 前缀）+ 同会话在途 + 60s 取件超时
  （DSH 没开 / 插件过旧），归属证明在插件侧（活表 `agents.get` 命中才注入）；
  不做 10s 活动闸（进程内 followup 排队安全，轮次中注入＝下一轮消费）。派发行按
  `dispatch.supported` 对 DSH 开放（`MainWindow::supportedDispatchExes` 加
  `supportsPluginChannel`）。
- **端到端活体验收（`rc_probe/dsh_dispatch_e2e.py`）**：外壳卡片派发行真实入口
  （UIA 选中探针卡 → dpText 粘贴带标记指令 → 「派回会话」）→ 卡转 working 且
  message 即派发原文（回执落账）→ 轮次真跑 → 卡转 review 带回复「收到」；
  zstd 全库扫描标记只在牺牲会话，零泄漏。接线坑：① WebView2 的 a11y 树挂在
  `Chrome_WidgetWin_1` 子窗上，从顶层窗走树是空的（桌宠气泡窗反而有名字）——
  要先 EnumChildWindows 找 Chrome 子窗再 `ElementFromHandle`；② 假桥接测试响应
  必须带 `Connection: close`（原生端本来就回这个头），否则 node 全局 agent 池化
  连接把测试的关-开序列拖死；③ 插件假 ctx 的 `apply` 要传 `.ctx`（makeCtx 返回
  包装对象），传错了守卫静默跳过、轮询器根本不注册。
- 离线 resume 未接：`resolveAgent` 对离线会话会从盘上 resume，但 composition/
  presets 那套在插件里复制不划算——V1 只派活会话，不在活表就拒绝（裁决 6）。
- **会话记录补待办身份（2026-11-14）**：会话正文虽是 zstd 多帧 native 不读，
  但宿主同时把每个会话的投影写在
  `~/.dsh/storages/session_projcache/sessions/<session-id>.json`——**plain JSON**，
  含 AI 标题（`rows.title.val`）、首条用户正文（`rows.titleInput.val.first.text`，
  标题异步生成前的回落）、工作目录（`rows.workingDirectory.val` / `identity.cwd`）。
  `core/DshSessions` 只读这份投影（按 mtime+size 缓存，主线程调用不加锁；
  id 哨兵与派发同口径，只认 `session-` 前缀、限字母数字连字符，杜绝穿越目录），
  待办侧三处消费：① `/work-items` 与外壳共用的 `workItemsJson()` 补
  `session_title`、空项目回填 cwd（只补显示不落盘，标题是异步生成的，落盘会钉死
  旧值）；② Qt 列表与 Web 外壳的行身份同口径升级：项目目录名 > 会话标题 >
  会话 <短 id>，同宿主同项目重名时优先补标题（截 12 字）而不是短 id——Web 外壳
  此前根本没有重名消歧，两条同项目会话只剩正文可分；③ Qt 行 tooltip 补标题行。
  桩：`dsh_sessions_test.cpp`（临时家目录假投影，22 项断言：标题回落/截断、
  cwd 优先级、坏 JSON、id 哨兵、缓存失效与文件删除）。派发守卫不受影响——
  归属证明仍在插件侧活表，投影只供显示。

#### OpenSquilla 系实测与接线（2026-11-08，探针 `rc_probe/osquilla_dispatch_probe.mjs` / `osquilla_dispatch_e2e.py`）

- **通道是网关 WS RPC**（合法宿主 API，与 ZCode app-server 同类）：桥本来就有
  operator.admin 握手（`connect.challenge → connect → hello-ok`），只用了
  `sessions.list` / `sessions.messages.subscribe` 做反向提醒；正向探明
  `sessions.create {agentId,kind,displayName}` → `{key}`、`sessions.send
  {key,message,client_message_id,_source}` → 受理返回 `turn_id`、
  `sessions.resolve {key}` 定向归属证明。会话 key 形如
  `agent:main:webchat:<id>` / `agent:main:cli:<id>`。
- **活体探针全绿**：牺牲会话 `agent:main:cli:344fb256`，send 受理 → turn 真跑
  （回「收到」）→ 读回标记命中 → 全库零泄漏（`TARGETED_SEND OK / NO_LEAK true`）。
- **坑①：state home 跟配置走**。桌面网关（:18791）的会话库是
  `%APPDATA%\@opensquilla\desktop-electron\opensquilla\state\sessions.db`
  （**SQLite**，`transcript_entries` / `turn_ingress_receipts`）；`~/.opensquilla`
  是另一个 gateway 实例的家，去那里读回/扫零泄漏会假阴性（首轮探针踩过）。
  读回与零泄漏断言一律走只读 SQLite 或 RPC，不猜目录。
- **坑②：`sessions.list` 带 cursor 分页**，不适合做归属守卫；用
  `sessions.resolve {key}`（命中返回记录、未知报错）。
- **接线（复用 DSH 插件通道模式）**：`SessionDispatch::supportsPluginChannel`
  对 `OpenSquilla.exe` 开放；native 只做 `agent:` 前缀哨兵 + 同会话在途闸；
  **`takePluginDispatches` 补按 exe 过滤**（原先不滤——DSH 与 OpenSquilla 同在
  Plugin 通道会互领件，测试有双宿主混合取件断言）。桥侧 1.5s 取件 →
  resolve 守卫 → send → `POST /dispatch/ack`；ack ok = durable admission 成立
  （与 DSH followup 同级回执）；`--no-dispatch` 可关。桥测试 14/14、native
  dispatch 测试全绿。
- **端到端活体验收**（`osquilla_dispatch_e2e.py`，外壳真实入口）：UIA 选中探针卡
  → dpText 粘贴带标记指令 → 「派回会话」→ 卡转 working 且 message 即派发原文
  （ack 回执落账）→ 轮次真跑 → 卡转 review；SQLite 全库扫描标记只在牺牲会话，
  零泄漏。**E2E PASS**。
- **坑③：工具 call 派生的进程活不过该 call**（桥 README 早有记录，这次应用也
  踩到）：手动 `Start-Process` 拉起的应用/桥在 call 结束后被连进程树端掉。
  桥靠**计划任务看门狗**自愈拉活；应用验收时用一次性计划任务拉起（验完删任务）。
- **坑④：`ensure-bridge.ps1` 必须带 UTF-8 BOM**：PS 5.1 无 BOM 按 GBK 读，中文
  注释乱码炸语法（仓库原件漏了 BOM，已补；voice-reminder.ps1 本来就有）。
- **项目归属（2026-11-08 用户指出后补）**：桥缓存 `sessions.list` 的
  `workspace/workspaceLabel`，`/notify` 与 hooks 同口径带 `project`（原样路径），
  外壳行身份取目录名（`Agentrmc`），修掉「会话 <短 id>」兜底。native 本就认
  `project` 字段，零改动；桥测试有透传/未知会话不带两断言。
- **文案口径（2026-11-08 用户指出开头不该是 WebChat）**：默认模板宿主名开头
  （「OpenSquilla：…」，对齐 hooks 系「宿主名：正文」）；`{session}` 取会话标题，
  泛标题 WebChat 不念、回落项目目录名。卡面加粗抬头本就是宿主名，消息开头此前
  误用会话标题才冒出 WebChat。**stop 有回复时回复当主文案**（用户再指出主文案
  不该是收尾句「这轮跑完了」）：「OpenSquilla：收到」，没回复才念收尾句；
  自定义 `--msg-stop` 不干预。
- 已知边界：OpenSquilla 卡面回复文案走桥模板（configHome 为空，`lastReply`
  不读它的 SQLite）；**看门狗与开机自启默认带 `--relay`**（2026-11-08 用户定：
  卡面带回复正文），模板没写 `{text}` 时自动附「回复：…」前 80 字。牺牲会话
  `agent:main:cli:344fb256` 经用户裁决于 2026-11-08 删除（`sessions.delete`，
  sessions/transcript/ingress 级联零残留）。

已定的设计约束：

- 宿主若支持 API／插件命令：新建或指定会话，发送带 `dispatchId`，回执关联到工单。
- 只有 UI 自动化的宿主：必须能证明「当前输入框属于目标会话」才发送；证明不了 →
  **不发送**，保留草稿，绝不降级发进另一条会话。
- 依赖前台焦点的投递**全局串行**，避免两次派发互抢输入框。
- 普通桥接 MCP **不带** `session_id`（宿主没给可信来源）：无归属的上报落成「未关联会话」，
  绝不按「最近一条」强绑。

## 4. 界面渲染（表现面）

### 4.1 为什么离开 Qt 画字

用户判定 Qt Widgets 在 200% 缩放下字形发虚；同机器同字号，浏览器明显更清楚。实测：

- 深色文字边缘 **98.8%** 有 R/B 失衡 → Qt 确实在用 ClearType 次像素；ramp 中位 0、p90 1
  → 没有被拉伸，几何上不糊，糊在字形合成。
- `QFont::NoSubpixelAntialias` 在 Widgets 路径上 **0.0% 像素差**（无效）；`PreferFullHinting` 仅 1.9%。
- Chromium 也是次像素（70–81%），但**墨芯更淡却更清楚** → 差别在 hinting／定位／gamma。
- 结论：Qt 里拿不到那个效果；WinUI3 不保证（XAML 默认也走 ClearType）；
  Chromium 系（WebView2／Tauri）是复现同款渲染的最短路径。

### 4.2 架构（已落地，默认启用）

- Qt 继续管：窗口、托盘、全局热键、TTS、ASR、UIA、桥接、`WorkItemStore`。
- **整窗**（顶栏 + 左栏 + 页签 + 页面）由 `WorkWebView` 画：`MainWindow.cpp:710` 起把
  `tabs_` / `headerBar_` / `leftBody_` 三个 Qt chrome 藏起来，`workWeb_` 直接进外层布局。
  Qt 控件仍然活着当状态机（语音、遥控器、映射的状态都从它们读），只是不出画面——
  **混渲染会让 Qt 的 backing store 发虚**，所以只留 WebView2 一条管线。
- 桌宠与气泡**保持 Qt**（用户裁决，未迁移）。
- 数据：`webShellState()`（`MainWindow.cpp:2774`）一次推 `{work, workers, health, …}`；
  动作：`handleWebAction()`（`2904`）收 `{cmd:"open|ack|clear", …}` 转给既有实现，
  页面里不另存状态机。
- 默认走外壳；`AGENTRMC_WORKWEB=0` 回退老页签（`MainWindow.cpp:667` 起）。
- 运行库缺失自动退回 Qt 页签：`WorkWebView::runtimeAvailable()`（`WorkWebView.cpp:215`）。
- 打包：`native/tools/build.bat` 把 `WebView2Loader.dll` 复制到 exe 旁边；找不到则外壳退回 Qt。

### 4.3 外壳内容

页签：待我处理／事件流／系统／按键映射／应用／使用引导，全部由 `htmlFor()`
（`WorkWebView.cpp:537`）里的 HTML/CSS/JS 渲染；顶栏与左栏同理（`renderTop()` / `renderLeft()`）。
增量刷新走 `ExecuteScript`，不整页重载。

### 4.4 测量与探针

`rc_probe/` 下已有 Web 外壳真窗截图探针（`default_shell_*`、`web_classic_*`、
`_blur_web_on.png` 等），覆盖 classic 明暗与不同配色；另有 MyGo 桌宠生死线探针，
结果记在 `bcb1ac0` 与 `8399265`（透明误判修正）。

## 5. 已定的裁决（避免反复讨论）

1. **不许模型自报 `session_id`**：宁可留「未关联会话」，也不接受猜出来的 id。
2. **turn-start 不建卡**：新一轮开始只归档旧验收，不为每轮对话长一条待办。
3. **stop ≠ 任务完成**：只表示「这一轮说完了，等你回来看看」。
4. **双击 = 先送到 Agent（没开就启动）才销账**；手动结账不启动。
5. **同会话只留一张活卡**：状态变化时旧账翻篇，列表里不并排挂三条同类。
6. **只在确认可用时派发**：证明不了目标会话就不发，保留草稿。
7. **桌宠与气泡不迁 WebView2**：它们是透明贴边小窗，且只有一两行短文案。

## 6. 验收判据

- 同宿主两个会话并行：A 的进度不得写进 B 的卡（`workitem_test.cpp` 有对应断言）。
- 同会话状态变更后列表只剩最新一条；等待排序按 `attention_since` 单调。
- 双击：宿主没开 → 启动后再销账；启动失败 → **不销账**且事件流写明原因。
- WebView2 不可用 → 退回 Qt 页（已验：无 SDK 时 0 个 `Chrome_WidgetWin` 子窗、进程存活）。
- 真实待办文件不得被测量脚本写坏（跑前后对 size + mtime）。
- 外壳版在 classic 明暗／美术主题下都要有截图留证（`rc_probe/`）。

## 7. 已知坑（WebView2 嵌 Qt）

1. 回调参数**不归我们**：`Invoke` 里多 Release 会崩 `EmbeddedBrowserWebView.dll`，
   少 `AddRef` 会崩自己（`0xC0000005`，落在本模块偏移）。
2. loader 路径少一层：NuGet 包里是 `runtimes/win-x64/native/WebView2Loader.dll`。
3. `put_Bounds` 要在布局之后：控件还没尺寸时喂 0×0，页面永远画不出来；`showEvent` 不可靠，
   用 `QTimer::singleShot(0)` + `syncBounds()`（`WorkWebView.cpp:306`）。
4. 消息回调在 `ICoreWebView2_2`（`add_WebMessageReceived`），不在 v1 接口上。
5. GUI 子系统下 `qWarning` 打不进管道 → 用临时 trace 文件定位，定稿前删除。
6. 增量刷新要用 `ExecuteScript` 推数据；整页 `NavigateToString` 会丢选中、丢滚动位置。

## 8. 现状与剩余

- 数据面：**三批全部落地**。第 1、2 批已装机验证；第 3 批定向派发四路接线并
  活体验收——WorkBuddy 系 UIA、ZCode app-server 协议、DSH 插件进程内 followup、
  OpenSquilla 网关 RPC（各路线细节见第 3 批各节）。Qoder 线等上游。
- 表现面：外壳＋六个页签已落，默认启用；桌宠／气泡保持 Qt。
- 版本号仍是 `0.4.1`（Native 与插件同号）：按约定等用户明确说「升版」才动。
- 剩余事项（均非主任务阻塞项）：
  1. **Qoder 线等上游**：Qoder CN 的 CLI 通道实测后被用户否决（桌面不可见），
     路由休眠；桌面可见通道两候选（deep link、a11y 设置）2026-11-07 深夜均证伪
     ——官方 deeplink 支持按 id 打开会话、renderer a11y 开关或本地会话 API
     任一出现再接线；「续当前会话」模式保持可用。
  2. **ZCode 对话区可见性受上游限制**：外部落库的消息要重启宿主才可见（代码级
     排查见第 3 批 ZCode 节，四条候选通道全排除）。已用卡片为中心 UX + 回执卡
     「重启 ZCode」按钮兜底；等上游出现跨实例通知或会话 deep-link 才能改成
     「聚焦即刷新」。
  3. 事件流／系统页的真实数据字段如有新增，记得同步给 `webShellState()`。
  4. 美术主题下的外壳观感复检（有探针图，但需人工确认一遍）。