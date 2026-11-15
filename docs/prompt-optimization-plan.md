# Agent 提示优化研究——ZCode / 各宿主在 AgentRMC 桌宠上的提示面

- 日期：2026-11-15
- 分支：`feat/betterchoice`
- 状态：第一轮研究完成（快赢落地见 §6）；第二轮完成 ZCode 远程授权真机探针——
  **决策式 PermissionRequest 三路全通**（§4），bridge 系远程授权从「待验证」变
  「待立项实施」
- 范围：所有「喂给 Agent 的提示」（决定 Agent 怎么用桌宠）+ 由提示契约派生的
  用户侧提醒文案（Agent 行为最终变成用户听到的声音）。不含桌宠 UI 本身
  （交互卡见 mission-control-plan.md 的 B1/B2/B3）。
- 结论先说：**提示面最大的问题不是缺提示，而是同一规则写了好几遍且已经开始
  互相打架**——SKILL.md 里一份 11-04 的「先喊一声」指引到现在会教 Agent 把同一
  件事念两遍；「等你选方案」气泡零信息量（题干就在 hook 手里却没用）；B1/B2/B3
  的可答卡片只覆盖 DSH，bridge 系（ZCode/Qoder/WorkBuddy/CodeBuddy）的提问在
  桌宠上没有卡。

---

## 1. 提示面盘点：Agent 在什么时刻看见什么

| # | 提示面 | 进上下文的时机 | 载体 | 覆盖宿主 |
|---|---|---|---|---|
| P1 | skill frontmatter description | **每个会话**都在（skill 列表常驻） | `skills/agentrmc-pet/SKILL.md` 头部 | bridge 四家 + DSH（`apply` 注册成 runtime skill） |
| P2 | SKILL.md 正文 | skill 被触发时才加载 | 同上 body | 同上 |
| P3 | 工具 description | MCP server 连上即常驻（tools/list） | bridge：`mcp/server.js` TOOLS；DSH：`index.js`（Host 原生工具，**另一份独立描述**） | 同上 |
| P4 | 工具返回文案 | 每次调用后 | `server.js` callTool 返回 text | 同上 |
| P5 | hook 自动提醒文案 | **不进 Agent 上下文**，直接到用户（气泡/TTS/卡片/工单） | `hooks/agentrmc-notify.js` | 各 hooks.json |

P1/P3 是**常驻成本**：只要插件装着，这些字就永远占着每个会话的上下文，
措辞要按「每个字都要常年付费」的标准抠。P2 只在被触发时付费，可以放宽讲细节。

P5 虽然不进 Agent 上下文，但它的文案质量由 **hook 手里的数据**决定——这是
「提示优化」最便宜的杠杆：hook 在 PreToolUse payload 里拿得到 AskUserQuestion
的完整 `tool_input`（题干、选项），却只发了一句泛化的「等你选方案」。

## 2. 各宿主「提醒事件」派发现状（2026-11-15 口径）

| 宿主 | 等授权 | 等选方案（准点性） | 任务完成 | 模型出错 |
|---|---|---|---|---|
| ZCode | PermissionRequest | PreToolUse(AskUserQuestion)→question，**弹窗那一刻**（75f1bad，2026-11-08 补的主路） | Stop | 无（运行时查不到 StopFailure） |
| Qoder / Qoder CN | PermissionRequest | PreToolUse→question，CN 实测弹窗那一刻（11-04）；国际版同引擎按同款对待 | Stop | StopFailure→error |
| WorkBuddy / AI | PermissionRequest（共享 hooks.json 永远加载） | Notification(permission_prompt) 准点 + PreToolUse 答完兜底 | Stop | 无（StopFailure 语义不同，故意不挂） |
| CodeBuddy | manifest 指 hooks-workbuddy；共享 hooks.json 是否同样加载**未验证** | 同 WorkBuddy | 同上 | 无 |
| DSH | 会话事件 `approval/asked`（插件在宿主进程内，天然准点） | `tool/call` name=ask_user_question | `agent/turn-stopping` | 无 |
| OpenSquilla | `session.event.approval_required` | `plan_presentation`→question | done 类 | 无 |

**关键结论：五类提醒（授权/选方案/回话/完成/出错）里，前四类在所有随包宿主
都已有「弹窗那一刻」的准点信号**。11-04 时代「有的宿主要等答完才派发、需要
Agent 提前喊一声」的局面已经不存在了——这直接判死了 SKILL.md 里的 pre-announce
指引（见 §3-1）。

## 3. 发现的问题（按严重度）

### 3-1【P1】SKILL.md「先喊一声」指引已过时，照做会双喊

`skills/agentrmc-pet/SKILL.md`（bridge 版）礼仪段最后的 2026-11-04 补记说：
「真要补的只剩 ZCode 这类没有 question 钩子的宿主」——但 ZCode 的 question 主路
是 **2026-11-08（75f1bad）** 落地的，晚于 SKILL.md 最后一次修改（58711c5，11-05）。
现在照这段指引，Agent 会在 AskUserQuestion 前先 `pet_work_update stage=blocked`
（出声「卡住了，等你」），弹窗那一刻 question hook 又喊「等你选方案」——同一件事
念两遍，正是去重逻辑天天在防的事。**该指引应整段删除**，改成正面规则：
随包宿主一律不要补喊。

### 3-2【P1】同一规则四份口径，已在漂移

「hook 自动提醒、别用工具重发」这条规则写了四遍：P1（frontmatter）、P2（body
礼仪段）、P3（`server.js` pet_notify 描述）、`server.js` 文件头注释。四份措辞
各不相同，且已经出现漂移实例：

- bridge SKILL.md vs DSH SKILL.md：同一套规则两份手写正文，DSH 版更干净
  （「不需要提前打招呼」一句话讲对），bridge 版背着一整段流水账。两份已经
  字面上分叉（「四类」vs「五类」时代各改各的）。
- `docs/voice-notify-formats.md` 自相矛盾：§二表格（L78）说 ZCode 有 question
  主路（11-08 更正过），§五第 5 条（L140）还说「ZCode 插件没有 question 钩子」。
  同一文档内两个时代的口径并存。
- DSH `index.js` 的工具描述与 bridge `server.js` 的工具描述是两份手写文案，
  措辞细节已经不一致（pet_notify 的防重发警告两边详略不同）。

维护规则应是：**常驻面（P1/P3）只讲一句结论，细节只在 P2 一处**；
两份 SKILL.md 共享同一骨架，宿主差异压到 2-3 行。

### 3-3【P2】「等你选方案」气泡零信息量，题干就在手边

permission 分支对普通授权会带细节（`等你确认 Bash：npm test`），question 分支
却只有一句泛化的「等你选方案」——用户听到声音、看到气泡，还是得切回宿主窗口
才知道要拍什么板。而 hook 的 `tool_input.questions[0].question` 就是题干，
提取成本一行。原生端按 `startsWith("等你选方案")` 认选方案语义
（`MainWindow.cpp` planChoice 判定 + 180s 迟到去重），**前缀不动、冒号接题干**
即可兼容。DSH 侧的交互卡（B2）已经有题干可展示，bridge 系补上题干后两边口径
才对齐。

### 3-4【P2】B1/B2/B3 可答卡片只覆盖 DSH；bridge 系展示卡是低垂果实

桌宠待答交互卡（34adca6）+ 手机/桌面聊天页内联应答（B1/B2）+ 内联独占
（questionInline/approvalInline，6a569a8 / 363448d）目前只接了 DSH——因为
DSH 插件跑在宿主进程内，能 hold 瀑布、能 resolve 提问。bridge 系是外挂 hook：

- **远程应答不可行**（结构边界，不是工作量问题）：AskUserQuestion 一旦发出，
  外部没有任何口子能往里注入答案。hook 是 fork 出来的一次性进程，答完也送不回去。
- **只读展示可行**：PreToolUse(question) 的 payload 就有题干 + 选项，POST
  `/requests`（契约见 `AgentBridge.h`，`{exe,session_id,project,requests:[{kind,id,toolName?,reason?,questions?}]}`）
  即可让桌宠弹卡、聊天页列卡。需要原生端给请求加「不可答」标记（bridge 系来的
  卡只展示 +「去宿主窗口」，不出允许/拒绝按钮）。

### 3-5【P3】「等你回话」契约只教了一半

SKILL.md 教了「把问题写在最后」，没教**问号后面别再接任何正文**。
`trailingQuestion` 的判据是「最后一个问号后面只剩装饰符号」（TRAILING_NOISE），
Agent 写「需要我继续吗？——另外补充一下…」或问题后跟一句客套话，判定就失效，
桌宠把这轮念成「做完了」，球明明在人这边却喊不到人。补一句负面清单即可。

### 3-6【P3】CodeBuddy 的提醒覆盖存在未知数

`.codebuddy-plugin` 的 manifest 指 `hooks-workbuddy/hooks.json`，但 WorkBuddy
靠的是「根 `hooks/hooks.json` 永远加载 + manifest 再合并」这条宿主特性
（README L153）。CodeBuddy 是否也永远加载共享 hooks.json 没验证过——若不加载，
CodeBuddy 就没有 PermissionRequest / Stop / turn-start，只剩等选方案一条。
待实测，列在这里防止忘。

### 3-7【评估后否决】hook 向 Agent 上下文注入 additionalContext

UserPromptSubmit / PreToolUse 理论上可以往会话里注入提示（比如每轮提醒「桌宠
在线」）。**不推荐**：常驻 token 成本 × 每轮一次，而 Agent 需要的桌宠知识已经
由 P1/P3 常驻覆盖，注入增量信息约等于零。列在这里是为了把这条路记成
「考虑过、关掉」。

## 4. 边界研究：ZCode 真远程授权——**已实测可行**（2026-11-15 真机探针）

ZCode 的 PermissionRequest hook 是**决策式**的：hook 在授权判定时被调用，
返回决策 JSON 就替代宿主弹窗。探针（`rc_probe/zcode-perm-probe/`，读
`zcode.cjs` 源码定位契约后真机三连测）全部验证通过：

| 决策路 | hook 输出 | 真机结果 |
|---|---|---|
| **allow** | `{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}}` | 工具直接执行，**不弹窗** |
| **deny** | 同上，`decision:{behavior:"deny",message:"…"}` | 工具被拒，message 原文到达 Agent 工具层（两次复现） |
| **不输出 / exit 0** | 无 stdout | 回落宿主正常弹窗（探针静默直通即此路） |
| **超时** | hooks.json `timeout:15`，探针睡 25s | 运行时按无决策处理 → 回落弹窗（安全阀成立） |
| **exit 2** | （未触发，源码证实） | **硬 deny**——决策式 hook 绝不能 exit 2 |
| **modify** | `decision:{behavior:"allow",updatedInput:…}` | 可改写入参（未实测，源码 `j2o` 明确支持） |

- **payload 契约**（实测 capture）：`toolName / toolInput / toolCallId / requestId(perm_*) /
  riskLevel / reason / mode / permission_mode / sessionId / turnId / traceId /
  sideEffectScope / cwd / timestamp`（camelCase + snake_case 双写法都在）。
  riskLevel=high + reason="High risk tools require explicit approval" 是 build 模式
  下关沙箱 Bash 的实测读数。
- **yolo 模式不派 PermissionRequest**（全自动放行走不到授权判定）——远程授权只在
  会话用 build/普通模式时有意义，与 DSH approvalInline 的适用面一致。
- **allow 还能带 `permissionUpdates`（addRules）**：卡片上可以给「允许且记住」按钮，
  等价于人在弹窗里选「不再询问」。
- **stop 事件 payload 直带 `responseText` / `last_assistant_message`**：bridge 的
  stop 分支可以不再 tail 读 transcript（512KB 截断读法可退役），顺带拿到
  `toolCallCount`、`permission_mode` 等现成字段。

**落地蓝图（bridge 系真远程授权，待立项实施）**：
1. PermissionRequest hook 从 fire-and-forget 改为「出声提醒 + 推可答卡到
   `/requests` + 轮询 `/request-answer/pending`（限时）」一体：现在的 notify
   hook 与决策 hook 是两个并发条目（探针实测同一 PR 两条都跑），合并成一个
   hook 免得「桌宠喊了、卡片又答了」两条路打架；等答案期间桌宠已有「等你确认」
   语音 + 卡片，答案从卡片来 → allow/deny 直接返回，超时/桌宠没开 → 静默退出
   0（绝不 exit 2），宿主正常弹窗。
2. timeout 单独放大（秒单位，实测 15s 生效；轮询窗建议 60–120s）。
3. 会话启动快照 hooks 配置：插件更新后要**新会话**才挂上新 hook（tee 实测：
   脚本内容实时生效、hooks.json 条目要新会话）——发版说明要写清。
4. 探针副产品发现：本机 ZCode 插件缓存停在 11-05 内容（版本号同为 0.4.1 但
   缺 11-08 的 question 主路 + 内容去重加固）——**ZCode 里 AskUserQuestion 弹窗
   提醒至今没生效**，需要在 ZCode 里重装/更新一次插件才能把 11-08 修复和
   本次题干改动带进活跃会话。

## 5. 提示文案的目标结构（改完后应长什么样）

```
frontmatter description（常驻，~150 字）
  ├─ 触发词（桌宠/鲸鱼娘/汇报进度/桌宠在不在）
  └─ 一句防重发警告（五类提醒 hook 自动完成）——细节不留在这层

SKILL.md 正文（触发后加载，唯一细节源）
  ├─ 四个工具怎么选（notify=过程、speak=收尾、work_update=账、state=先看一眼）
  ├─ 提醒分工：hook 自动五类 vs 主动汇报三场景（blocked 只给「回合中卡住」）
  ├─ 各随包宿主一律不要 pre-announce（准点信号已全覆盖）
  └─ 等你回话契约：问题放最后一句 + 问号后不接正文

工具 description（server.js / index.js 同步，各 ~80 字）
  └─ 一句职责 + 一句最易犯的错（别刷屏 / 别拿 blocked 当进度条 / 问句收尾别补 blocked）
```

## 6. 本轮落地（快赢，全部插件侧，无 native 改动）

1. **重写 bridge SKILL.md**：删过时 pre-announce 流水账，礼仪段改成「提醒分工 +
   正面规则」；补「等你回话」负面清单（3-5）。
2. **DSH SKILL.md 同步**：共享同一骨架，宿主差异只留工具前缀与「模型出错」两处。
3. **`server.js` / DSH `index.js` 工具描述对齐**：pet_work_update 补「问句收尾别发
   blocked」，两份措辞取齐。
4. **hook question/permission(AskUserQuestion) 带题干**（3-3）：前缀
   `等你选方案：` + 题干 ≤40 字，speech 不变；DRY_RUN 测试补断言。
5. **`docs/voice-notify-formats.md` 修矛盾**（3-2 的文档实例）：L140 更正 ZCode
   question 主路；§一等选方案行补题干格式。

不升版（用户说了才算）；装机时随下一次出包自然带上。

## 7. 后续候选（按价值排序，未排期）

1. **bridge 系真远程授权（ZCode 先行）**（§4 蓝图）：决策式 PermissionRequest
   已实测三路全通，按蓝图合并 notify+决策 hook、推可答卡、限时轮询。ZCode
   跑通后再看 Qoder 家有没有同款事件语义（源码还没查）。
2. **CodeBuddy 覆盖实测**（3-6）：一条 DRY_RUN + 真机弹窗即可定论。
3. **stop 分支改读 `responseText`**（§4 副产品）：ZCode 的 stop payload 直带最终
   回复全文，tail 读 transcript 的兜底退为「字段缺失时才用」。
4. **speech 带题干**：「ZCode 在等你选方案」→「ZCode 在等你选方案：是否出包」。
   TTS 念题干大概率可用，需真机听感定夺（选项文本要净化，复用
   `SpeechText::friendly` 口径）。
5. **工具返回文案引导**：pet_work_update 返回体带一句「blocked 已出声，无需再
   pet_speak」之类的回执提示（P4 面，收益小，攒着顺手做）。
