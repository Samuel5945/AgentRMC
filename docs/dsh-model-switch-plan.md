# 在 RMC 会话里切换 DSH 模型：现状与落地方案

> 状态：**已落地**（2026-11-21）。P0–P3 全部实现并装机验证。
> 实现细节与验收记录见文末「§8 落地记录」；前面几节保留研究结论原文，
> 便于对照「当初判断的」与「最后做的」。
> 结论先行：**RMC 现在能显示 DSH 会话、能派发、能停止/插话，但「切模型」这一格是空的**
> ——不是界面藏了，而是读、写、目录三条链一条都没接。写通道只能走
> DSH 进程内的 Host 插件（HTTP 直连 401），改日志/改缓存都是错路。

---

## 0. 一句话结论

| 能力 | ZCode | OpenSquilla | **DSH** |
|---|---|---|---|
| 聊天页胶囊显示「当前模型」 | ✅ 读 message.data.model | ✅ 读 sessions.model | ❌ 读不到（恒显示「选择模型」） |
| 胶囊能列出可切目录 | ✅ 个人供应商清单 | ✅ 桥推 osqModels | ❌ 恒「当前宿主不支持在 RMC 里切换模型」 |
| 能真的切 | ✅（派发 override，turn 后恢复） | ✅（网关 sessions.patch） | ❌ 没有通道 |
| 派发时带一次性 override | ✅ | ✅ | ❌ 传了也会被**静默丢弃** |

最后一行是现存缺口，不只是「没做」：页面若给 DSH 传 `model`，
`SessionDispatch::dispatch()` 只在 `osq` 分支保存 override，DSH 分支直接丢掉
（`native/src/core/SessionDispatch.cpp:352-354`）。所以「将来放开页面」也不会自动生效。

---

## 1. DSH 侧的协议事实（按运行版本 0.2.2-rc.5 核实）

### 1.1 唯一正确的切换入口：`AgentModel.select()` / `sessionController.selectModel()`

Host 侧链路（`@deepseek-ai/dsh-api-session-controller`）：

```
Remote  session.selectModel({sessionId, provider, model, reasoningEffort?})
  → SessionCommandController.selectModel()          src/commands.ts:166-195
      requireModel()            查目录：模型不在 listModels 里就拒（session/model-unavailable）
      llm.resolveCallConfig()   解析 maxTokens / effort 默认值
      agent.model.select(...)   追加一条 model/selection 事件
      agentDefaultModel.saveSelection()   ← 顺带把「默认模型」也改掉（见 1.4）
```

`agent.model.select()` 本体（`@deepseek-ai/dsh-agent/src/model-selection.ts`）只做一件事：
往会话日志追加 `model/selection`。投影 `modelSelection` 的 `pending` 因此变成新值，
`next = pending ?? lastUsed`（客户端目录 `directory.ts` 用的就是这个口径）。

**生效时机**：从**下一个 step** 开始；正在跑的 step 保留启动时的选择
（`dsh-api-session-controller` README:97、升级指南 `v0.2.1-alpha.4/session-model-selection`）。
所以 ack 成功 = 「下一步生效」，不是「这一轮已经换了」。

**切换会插一条上下文提示**：`installModelSelection()` 在 `agent/pre-step` 末尾追加一条
`source.kind = 'model-selection'` 的 user/message（正文 `[model changed: …]`）。
RMC 的 DSH 读取器只认 `source.kind == 'user'`（`SessionTranscript.cpp:1872-1878`），
这条会被滤掉——**不会在聊天页多出一个假气泡**，这点不用额外处理。

### 1.2 目录从哪来

`ctx.llm`（`@deepseek-ai/dsh-llm`）三个只读方法，等价于 Host 的 `session.modelCatalog`：

- `listProviders()` → `[{id, name}]`（已注册适配器，本机实测含
  `magpie` / `tr` / `jy` / `jy-gateway` / `deepseek-account` 等）
- `listModels(provider)` → `[{id, name, description?, badge?}]`
- `resolveModelInfo(provider, model)` → `{ context, reasoning: { efforts: [{id,name}], defaultEffort } }`

注意 effort **值域是每个模型自定义的**：`tr` 的 `glm-5.3-flash` 有
`off/low/high/xhigh/max`，`magpie` 的 `trae-cn/*` 只有 `low/high/xhigh/max`。
RMC 现在那套固定五档 `['low','medium','high','xhigh','max']`
（`MainWindow.cpp:3799-3804`）对 DSH 是错的，必须按所选模型的 `efforts` 渲染。

### 1.3 为什么不能绕开插件

| 绕法 | 结论 | 证据 |
|---|---|---|
| native 直连 `POST http://127.0.0.1:2184/api/session/selectModel` | ❌ 401 | 实测；`dsh-client-connection/src/browser-auth.ts` 用进程内 launch token + `dsh-auth-*` cookie 鉴权，token 不落盘、不出进程 |
| 改会话日志 `session.v4.jsonl.zstd` | ❌ | 日志是权威且宿主独占写（多帧 zstd、writer 锁）；篡改会被 `formatVersion`/校验判废 |
| 改投影缓存 `~/.dsh/storages/session_projcache/…json` | ❌ | 只是折叠结果，`ver`/schema 不匹配即丢弃，且下一次 checkpoint 覆盖（`session-projection-cache` README:63-67） |
| 改 `cordis.patch.yml` 的 `agent-default-model` | ⚠️ 只改默认 | 只影响「没有会话级选择的会话」，活会话的 `model/selection` 不动，且要 HMR/重启 |

### 1.4 两个必须知道的副作用

1. **`selectModel` 会顺手改默认模型**：末尾 `void agentDefaultModel.saveSelection(selected)`，
   走 `configEditor` 原子写 profile patch 的 `agent-default-model` 条目
   （`dsh-agent-default-model/src/index.ts:75-95`、`dsh-config-editor` README:67）。
   用户可能不希望「在 RMC 里切一次，DSH 新会话的默认也变了」。
   → 建议插件**直接调 `agent.model.select()`**（会话级，不动默认），
   自己用 `llm.listModels()` 做可用性校验，效果等价于 `selectModel` 但无副作用。
2. **切到小窗口模型会被压缩门拦**：`dsh-compaction-basic` 在 `agent/request` 前置检查
   「切换目标的历史压力」，压缩失败抛 `MODEL_SWITCH_COMPACTION_FAILED`，并保留原模型
   （升级指南 `v0.2.1-alpha.4/model-switch-compaction`）。
   这个失败发生在**下一轮请求**，不在 select 那一刻——所以 ack 成功不代表下一轮能跑，
   失败会以「模型出错」那条提醒浮出来。

---

## 2. RMC 侧现状（三条链各自的断点）

### 2.1 读：DSH 会话模型读不出来

- `SessionTranscript::readDsh()`（`native/src/core/SessionTranscript.cpp:1765-1952`）
  解多帧 zstd、滤注入、配工具调用，**但从不给 `full.model` 赋值**。
  对比：jsonl 分支 `:662-678`、ZCode 分支 `:828-845` 都有。
- 所以 `lanSessionTranscript()` 出去的 `model` 字段对 DSH 恒为空
  （`MainWindow.cpp:4539`），页面 `mpCurModel()` 拿不到 → 胶囊显示「选择模型」
  （`WorkWebView.cpp:2838-2841`）。
- 投影缓存里其实**已经有**这个值：
  `~/.dsh/storages/session_projcache/sessions/<sid>.json` →
  `record.rows.modelSelection.val = {lastUsed:{provider,model,reasoningEffort}, pending}`
  （本机实测：`{"provider":"magpie","model":"trae-cn/deepseek-v4.1-flash-max","reasoningEffort":"max"}`）。
  而 `DshSessions::find()`（`native/src/core/DshSessions.cpp:47-91`）只读 title / cwd，
  没读这一行。

### 2.2 目录：`/models` 路由硬编码只认 OpenSquilla

`MainWindow.cpp:1440-1458`：`setModelsHandler` 第一句就把非 `OpenSquilla.exe` 的推送丢掉。
`dispatchStateJson()`（`:3753-3804`）也只有 `models`（ZCode）和 `osqModels`（OpenSquilla）两份账。

页面侧两道硬判断同样只认两家：
`mpSwitchable()`（`WorkWebView.cpp:2832-2836`，手机页同款 `LanWebServer.cpp:3242-3246`）。

### 2.3 写：没有切换通道，派发 override 还会被丢

- native 没有「切模型」的 cmd，`handleWebAction` 只有 `dispatchSession`（带一次性 model）
  和 `control`（stop/steer）。
- `SessionDispatch::dispatch()` 的插件分支里，`p.modelOverride` 只在 `osq` 时保存
  （`:352-354`），DSH 的 override 直接蒸发——**静默**，用户看到的是「发出去了但模型没变」。
- 好消息：可复制的模板是现成的。`control`（停止/插话）就是一条完整的
  「native 排队 → 插件 1.2s 轮询取件 → 执行 → ack → native 推结果」链路
  （`SessionDispatch.cpp:657-748` + `AgentBridge.cpp:317-337` +
  插件 `index.js:1468-1574`），模型切换与它同构。

---

## 3. 推荐方案：给插件通道加一条 `/model` 队列

### 3.1 数据流

```
聊天页胶囊（桌面 dchat / 手机 chat）
   └─ postObj {cmd:'switchModel', exe:'DeepSeek Harness.exe', session, provider, model, effort?}
        └─ native SessionDispatch::model(...)  排队（仿 control，超时 20s）
             └─ 插件 GET /model/pending?exe=DeepSeek Harness.exe   （1.5s 轮询）
                  ├─ agents.get(sid) 取活 Agent（不在活表 → resolveAgent 唤醒，同派发）
                  ├─ ctx.llm.listModels(provider) 校验模型在目录里
                  ├─ agent.model.select({provider, model, reasoningEffort?})
                  └─ POST /model/ack {model_id, ok, error?}
             └─ native 收 ack → pushSessionResult → 页面提示「已切到 X（下一步生效）」
```

目录反向推：插件每 30s（或 `llm/adapters-updated`、`llm/models-updated`、
`settings/document-updated`、`credentials/*` 变化时）`POST /models`
`{exe:'DeepSeek Harness.exe', providers:[{providerId,name,models:[{id,name,efforts,defaultEffort}]}]}`。

### 3.2 改动清单（按依赖顺序）

**① native：`/models` 按 exe 分账**（小改）
- `MainWindow::setModelsHandler` 接受 `DeepSeek Harness.exe`，存 `dshModels_` + `dshModelsAt_`；
  `dispatchStateJson()` 增 `dshModels`（复用 `osqModelsFresh()` 那套 120s 过期口径）。
- 页面 `mpListFor(prefix)` 增 DSH 分支；`mpSwitchable()` 放行 `DeepSeek Harness.exe`。
- 手机页 `LanWebServer.cpp` 同款两处。

**② native：DSH 会话模型读出来**（小改）
- `readDsh()` 里解析最后一条 `model/selection`（`{provider, model, reasoningEffort}`），
  赋 `full.model`（显示名：优先目录 `name`，退 `provider/model`）。
  **比投影缓存更实时**——投影有 write-behind 节流（`writeEveryEvents` /
  `writeIntervalMs`，加 `turn/end` 必写点），切换后可能滞后到下一轮结束。
- 顺手给 `DshSessions::Info` 加 `model` / `provider` / `effort`（读
  `rows.modelSelection.val.pending ?? lastUsed`），给列表页与卡片补身份，成本几乎为零。

**③ native：新队列 + 新 cmd**（中改）
- `SessionDispatch` 加 `Model` 结构（仿 `Control`）：`model(exe, sid, provider, model, effort, …)`、
  `takeModels(exe)`、`modelAck(id, ok, error)`、信号 `modelDone/modelFailed`，
  超时 20s（切换可能触发 resume，比 stop 慢）。
- `AgentBridge` 加 `GET /model/pending?exe=` 与 `POST /model/ack`（照抄 control 那两段）。
- `MainWindow`：`cmd == "switchModel"` → 排队 + 提示；`modelDone/modelFailed` 接线推
  `pushSessionResult({control:'model'})`。
- **同时修 2.3 的静默丢弃**：`dispatch()` 的插件分支把 `modelOverride` 一并保存给 DSH
  （要么走「派发前先切」，要么派发件里带 model 由插件先 select 再 followup）。

**④ 插件：目录推送 + 取件执行**（中改，约 200 行）
- `pushModelCatalog()`：用 `ctx.llm.listProviders/listModels/resolveModelInfo` 手搓
  （不 import `@deepseek-ai` 包，与现有零依赖风格一致），`POST /models`。
- `pollModels()`：1.5s 轮询，`agents.get(sid)` → 不在活表则 `resolveAgent` 唤醒
  （复用 `wakeAgent`，`index.js:1290-1321`）→ 校验 → `agent.model.select()` → ack。
- 开关 `modelSwitch: true`（与 `dispatch` / `control` 平级），配置写
  `cordis.patch.yml`（本包没有导出 `Config`，这是既有约定）。

**⑤ 页面：胶囊提交 + effort 按目录渲染**（中改）
- 现在选中模型只改本地 `ctx.sel`，等派发时随 `model` 一起发（一次性 override）。
  改为两条路：**会话级切换**（立即提交 + 结果 toast）与**派发 override**（保留）。
  建议在面板底部加一行「设为这条会话的模型」，避免把两种语义混在一个点击上。
- `mpThoughts(prefix)` 改成按所选模型的 `efforts` 生成（DSH 目录带
  `efforts/defaultEffort`；ZCode / OpenSquilla 维持现状），`MP_LVL_LABELS` 补
  `off` → 「关」。

### 3.3 分期（每期都能独立验收）

| 期 | 内容 | 用户可见效果 | 风险 |
|---|---|---|---|
| P0 | ① + ②（只读） | 胶囊显示真实模型名；目录能看到但底部仍写「暂不支持切换」 | 极低 |
| P1 | ③ + ④ + ⑤（不含 effort） | 能切模型，提示「下一步生效」 | 中：唤醒冷会话拿 writer |
| P2 | effort 按目录渲染 | 思考强度与 DSH 自己一致 | 低 |
| P3 | 修派发 override 静默丢弃 | 「派一句话并指定模型」一次到位 | 低 |

---

## 4. 被否方案（省得后人再试一遍）

| 方案 | 否因 |
|---|---|
| native 直连 DSH 的 `/api/session/selectModel` | 401（进程内 token，见 1.3） |
| native 解析 DSH cookie/credentials 后直连 | 越界读凭据，且 cookie 与进程世代绑定，重启即失效 |
| 直接改 `cordis.patch.yml` 的 `agent-default-model` 当「切模型」 | 只改默认，不动活会话；还要 HMR |
| 复用 `dispatchSession` 的 `model` 字段当切换 | 语义是一次性 override；DSH 当前还会丢（2.3）。且会污染「派发」这条链的语义 |
| 把 `stop`/`steer` 的 control 队列扩一个 `action:'model'` | 超时语义不同（10s vs 20s+），混在一起会让「停止」被模型切换的慢回执拖住 |

---

## 5. 验收矩阵（落地时必须跑）

- **native**：`AgentRMCDispatchTest` 增 model 队列用例（入队 / 取件一次 / 跨 exe 取不到 /
  超时 / ack false 带原因）；`AgentRMCBridgeTest` 增 `/models` 接受 DSH 与
  `/model/pending`、`/model/ack` 路由；`AgentRMCTranscriptTest` 增
  `model/selection` 解析用例（fixture 要重生成：
  `rc_probe/gen_dsh_transcript_fixture.py`）；`AgentRMCDshSessionsTest` 增投影
  `modelSelection` 读取用例。
- **插件**：`node plugins\deepseek-harness\test\plugin.test.js` 增「目录推送 payload 形状」
  「取件 → select → ack ok」「模型不在目录 → ack false 带原因」「不在活表 → 唤醒路径」。
- **真机**：DSH 里开着一条会话，RMC 聊天页切到另一个 provider 的模型 →
  胶囊变名 → 在 DSH 里发一句话 → DSH 日志出现新的 `request/header.config.provider` →
  下一轮 `assistant/message` 正常；再切回原模型验证可逆。
- **回归**：`node rc_probe\desktop_chat_attachments.mjs`（选模型按宿主分流的纯函数断言）
  与 `node rc_probe\mobile_page_probe.mjs`（手机页胶囊）。

---

## 6. 关键代码位置索引

| 位置 | 作用 |
|---|---|
| `native/src/core/SessionTranscript.cpp:1765-1952` | DSH 读取器（缺 model） |
| `native/src/core/DshSessions.cpp:47-91` | DSH 投影只读索引（缺 modelSelection） |
| `native/src/MainWindow.cpp:1437-1458` | `/models` 推送只收 OpenSquilla |
| `native/src/MainWindow.cpp:3753-3804` | `dispatchStateJson`：models / osqModels / thoughtLevels |
| `native/src/core/SessionDispatch.cpp:322-354` | 模型预检 + `if (osq)` 丢弃 DSH override |
| `native/src/core/SessionDispatch.cpp:657-748` | control 队列（模型队列的模板） |
| `native/src/core/AgentBridge.cpp:317-337, 457-465` | `/control/pending` 与 `POST /models` 路由 |
| `native/src/ui/WorkWebView.cpp:2825-2958` | 桌面聊天页胶囊状态机 |
| `native/src/core/LanWebServer.cpp:3207-3334` | 手机页同款状态机 |
| `plugins/deepseek-harness/index.js:1285-1412` | 派发取件 + `wakeAgent`（可复用） |
| `plugins/deepseek-harness/index.js:1468-1574` | control 取件（模型取件的模板） |
| DSH `dsh-api-session-controller/src/commands.ts:166-195` | `selectModel` 唯一官方入口 |
| DSH `dsh-agent/src/model-selection.ts` | `AgentModel.select` 与投影折叠 |
| DSH `dsh-compaction-basic/src/index.ts`（`MODEL_SWITCH_COMPACTION_FAILED`） | 切换的压缩门 |
| DSH `dsh-client-connection/src/browser-auth.ts` | 为什么 HTTP 直连 401 |

---

## 7. 待用户裁决的三点（已按建议执行）

1. **切模型要不要顺手改 DSH 默认模型？**
   建议不要（插件走 `agent.model.select`）。若要，就直接用 `sessionController.selectModel`，
   它会写 profile patch。
2. **冷会话（没在 DSH 窗口里打开）允不允许切？**
   允许＝复用派发的 `resolveAgent` 唤醒（会拿 writer）；不允许＝只在活表命中时切。
   建议 P1 只允许活表，P3 再放开。
3. **胶囊的点击语义**：一个点击＝「只切这条会话」还是「下次派发用这个模型」？
   建议拆成两个动作（会话级切换 + 派发 override），别复用同一个点击。

---

## 8. 落地记录（2026-11-21）

### 8.1 实际做法

三点都按建议执行：

- **不碰默认模型**：插件走 `agent.model.select()`，并在测试里断言
  `sessionController.selectModel` **一次都没被调用**（那条会写 profile patch）。
- **P1 只认活表**：`agents.get(sid)` 没命中就拒「没在 DSH 里打开」，**不唤醒**冷会话
  （唤醒要拿 writer，切个模型不值得）。
- **两个动作分开**：面板底部多一颗「设为这条会话的模型」（`MpCommit`），点了立刻
  `switchModel`；派发那颗按钮仍是一次性 override。

### 8.2 数据流（最终形态）

```
聊天页胶囊 → 选中模型 → 「设为这条会话的模型」
  └─ {cmd:'switchModel', exe, session, providerId, modelId, level?}
       └─ SessionDispatch::switchModel()   第三条独立队列（20s 超时）
            └─ 插件 GET /model/pending?exe=…（1.5s 轮询）
                 ├─ agents.get(sid)（只认活表）
                 ├─ ctx.llm.listModels 校验目录
                 ├─ agent.model.select({provider, model, reasoningEffort})
                 └─ POST /model/ack
            └─ native 收 ack → pushSessionResult({control:'model'}) → 页面 toast
目录反向：插件 POST /models {exe:'DeepSeek Harness.exe', providers:[…details]}
```

### 8.3 踩到的两个坑（都不是「新功能没做对」，是旧账被照亮）

**坑一：原生端沙箱写不进工作区（构建直接失败）。**
CMake 报 `Could not open file for write … Permission denied`。根因不是 ACL 缺项——
`D:\githubrepos\Agentrmc` 上挂着一条 **Low 完整性标签**（`icacls` 的
`Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)`），DSH 的沙箱进程继承它，写同级
或更高完整性对象一律被拒。用 `diagnose-windows-sandbox-acl` 修完 ACL 仍然写不进，
因为问题在完整性级别而不是 DACL。**对策：构建/出包/装机这类要写工作区的命令必须在
完全权限下跑**（本会话后来切到 danger-full-access 就正常了）。这条值得记进 AGENTS.md。

**坑二：OpenSquilla 的模型菜单两分钟后永久消失（用户当场报的）。**
现象：DSH 能切了，OpenSquilla 又不行了。查下来是两条旧逻辑咬在一起：

- native 的 `osqModelsFresh()` 按「最后一次推送时刻」算 **120s TTL**；
- 桥的 `pushModels()` 目录没变就 `return`（sig 比对，省流量）。

于是原生端一重启，目录只活两分钟；之后 `osqModelsFresh()` 恒返回空，页面就写
「当前宿主不支持在 RMC 里切换模型」。这不是本次改坏的——`2c924fd` 起就埋着，
只是以前没人重启原生端（这次为了装机重启，正好踩中）。

修法（两侧都改，任一侧单独修都能救急）：

1. **native**：目录的「新鲜」判据改成**宿主还活着**，不是「目录变过」。
   新增 `MainWindow::touchModelCatalog(exe)`，在 `/dispatch/pending`、
   `/control/pending`、`/model/pending` 三个 1.2~1.5s 的取件口里续命——
   那才是真心跳；真掉线了两分钟内自然过期。
2. **桥**：目录内容没变也按 `modelsRepushMs`（默认 45s）重推一次；且用**独立
   定时器**（不与 `refreshMs` 绑：refreshMs 可被调到几分钟，绑一起菜单又会消失）。
   加 `modelsBusy` 重入锁（两个定时器都调它，实测 192ms 内连推两次）。

### 8.4 验证

| 层 | 结果 |
|---|---|
| native 全量测试 | 全绿（Dispatch/Bridge/Transcript/DshSessions/LanWeb/… 共 21 个 exe；AsrTest 需 wav+模型参数，按旧例跳过） |
| DSH 插件测试 | 62/62（新增 6 条换模型用例） |
| OpenSquilla 桥测试 | 28/28（新增 1 条定期重推回归） |
| 桌面页探针 | 全绿（新增 8 条 DSH 路径：读 dshModels / 档位按 details / 提交键只在选中后出现 / ZCode 不出提交键） |
| 真机 | 原生端重启 → 两宿主目录都持续重推（DSH 60s、OpenSquilla 45s），过 120s TTL 仍在；`/model/pending` 200、未知 ack 404、`/models` 204 |
| 真实会话冒烟 | `AgentRMCTranscriptTest --live <sid>` → `model=magpie/trae-cn/deepseek-v4.1-flash-max`（模型读数通了） |

**桥的部署副本要手动同步**：运行时跑的是
`C:\Users\PC\.agents\skills\agentrmc-voice-reminder\scripts\opensquilla-pet-bridge.mjs`，
改仓库那份（`plugins/opensquilla-bridge/`）之后要 `Copy-Item` 过去并重启桥
（`ensure-bridge-hidden.vbs`），否则改了等于没改——这次就是先漏了这步，
导致桥还在跑旧代码、菜单照旧消失。

