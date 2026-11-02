# AgentRMC × DeepSeek Super-agent 优化方案

- 日期：2026-11-02
- 状态：方案（未实施）
- 参考：
  - 文章《Exploring Super-agent Design: Towards a More Autonomous Agent System》
    <https://mp.weixin.qq.com/s/zoS7GlReWPhfCjENGxXCdg>
  - 仓库 `deepseek-ai/dsh-superagent-prototype`（MIT，研究原型，DeepSeek Harness bundle，GitHub + 可选飞书）
    <https://github.com/deepseek-ai/dsh-superagent-prototype>
- 结论先说：**这套东西的重点不是换模型，而是把 AgentRMC 从「通知壳 + 语音输入」升级成桌面上的「注意力 Router」。** 不建议在 AgentRMC 里再造一个 Agent 运行时；宿主 Agent 继续当 Worker，AgentRMC 负责 Trigger、Ticket、Attention、路由和回跳。

---

## 1. DeepSeek 这份东西到底是什么

### 1.1 文章：Super-agent 的四个特征

| 特征 | 含义 | 对 AgentRMC 的启发 |
|---|---|---|
| 连续 | 用户在 IM / GitHub 等熟悉界面里始终面对同一个 Agent，不用记 Session 上下文 | 桌宠应是跨 Agent / 跨会话的统一入口，而不是每次从零开始 |
| 高效 | 后台任务并行，前台随时能接受新指令 | 语音派活后不要阻塞桌宠；任务状态异步回传 |
| 主动 | 监听环境事件（Trigger），不需要人逐条下命令 | hooks / MCP / 语音 / 前台窗口都可以是 Trigger |
| 持久 | 始终在线，异常后能恢复 | 事件、工单、待办要落盘；桥接和宿主要有 Watchdog |

### 1.2 原型：一个 GitHub 仓库的主动式 Agent

- 形态：DeepSeek Harness 的 bundle，TypeScript，SQLite，`pnpm check`，MIT。
- 监听：GitHub（可选飞书）；被允许的人提出请求后，创建 **Ticket**，在独立 **Executor Session + 目录** 里执行，最后回到原对话回复（如 PR）。
- 有 Web panel，按「是否需要人」优先展示。
- 明确是研究原型，不是官方产品；文章里部分设计是未来方向，原型未全部实现。

### 1.3 原型里真正值得抄的领域模型

```text
Source → Trigger → Thread → Eval → Ticket → Assignment → Executor → Report
                      ↘ Subscription / Channel / Attention / Policy
```

| 概念 | 原型里的职责 | 关键规则 |
|---|---|---|
| Source | 事件流（GitHub/飞书） | 自己记住读取位置，重启不重放；过滤无关事件 |
| Trigger | 一条不可变事件 | 幂等；重复接受只产生一个 Ticket |
| Thread | 按时间排列的 Trigger | 同一 Thread 的 Eval 串行；可挂多个 Ticket |
| Eval | 判断是否接活 | 先确定性 Gate，再模型 Assessment |
| Ticket | 一个被授权的工作单元 | 自己定义状态，不跟随 Executor Session |
| Assignment | Ticket 绑定到一个 Executor 实例 | 可多次派工；最终报告才结束 Ticket |
| Report | Executor 的结构化汇报 | `progress / waiting / ready / blocked / failed / dropped` |
| Attention | 等人工决定的问题 | 一条具体问题，不藏在长报告里 |
| Policy | 版本化规则 | 谁可以发起、谁可以接、哪些 bot 可候选 |
| Outbox | 持久化出站消息 | 先存后发；按会话保序；失败退避重试 |

### 1.4 原型的执行与可靠性设计

- **Router / Worker 双层**：Router 只做人类交互、高层分派和全局记忆；Worker 在隔离上下文里执行具体任务，不直接回复人。
- **事件驱动 + 两级过滤**：Fast Path 用确定性代码过滤噪音，Slow Path 才调模型判断语义。
- **异步 ReAct / 全双工**：耗时的 `run()` 改成 `start()`，立即返回 Started；后台完成后作为新事件投递，前台不被阻塞。
- **计算 / 状态 / 环境分离**：Agent Loop 无状态，Session 状态入库，执行环境留在原地；设备休眠不等于任务丢失。
- **Watchdog**：外部定时 Control Loop，除了进程存活，还看 Worker 是否有实质进展、是否偏离目标。
- **Panel 三问**：需要我做什么？系统健康吗？某件事的完整历史是什么？

---

## 2. AgentRMC 当前在什么位置

AgentRMC 已经有一个「Router 雏形」，但还没有领域模型和持久化。

| Super-agent 概念 | AgentRMC 现状 | 差距 |
|---|---|---|
| Source | `AgentBridge /notify`、MCP、语音 | 没有统一事件模型；不同来源各走各的 |
| Trigger | hook payload（permission/question/stop/progress） | 即发即忘；没有 id / 幂等 / 持久化 |
| Thread | 宿主 Agent 的 session/transcript | 没有接住 `session_id` 和项目上下文 |
| Eval | 本地正则（`looksLikeTask`）+ 按 kind 分支 | 没有策略层；所有事件按固定规则提醒 |
| Ticket | 无 | 没有「一件事」的状态和生命周期 |
| Assignment | `AgentWindow` 按进程/窗口查找 | 只绑到 exe，不绑到 session / 项目 |
| Report | stop/permission/question 文案 | 没有结构化状态（progress/blocked/ready…） |
| Attention | 气泡 + TTS + 托盘 | 没有「待我处理」队列和计数 |
| Outbox | 直接 HTTP POST | 失败即丢；AgentRMC 没开就收不到 |
| Watchdog | 无 | 桥接/宿主异常不可见 |
| Panel | 事件流（按时间） | 不是按「是否需要人」优先 |
| Policy | 无 | 没有 per-agent / quiet hours / 去重规则 |

一句话：**AgentRMC 已经有「人的入口」和「把话送给 Agent」的能力，缺的是把事件变成可追踪的工单，并按注意力优先级呈现。**

---

## 3. 最值得抄的五件事

### 3.1 注意力优先：只在人必须行动或结果有用时说话

> **设计约束（以产品方为准）**：语音是主提醒通道。`permission` / `question` / `stop`
> 三类语音提醒**不得因为宿主窗口在前台而静音**；前台状态只用于决定是否自动 raise
> 和气泡摆位。即使窗口在前台，用户也可能没看屏幕。

原型的核心原则是「Take as little human attention as possible」。落到 AgentRMC：

- `permission` / `question`：需要人立刻行动 → 气泡 + TTS + 自动 raise。
- `stop`：结果有用就说；**是否前台不决定是否朗读**——即使宿主窗口在前台，用户也可能没看屏幕。前台只影响要不要自动 raise / 气泡位置。
- `progress`：默认静默，只进事件流 / 面板；除非用户主动订阅。
- 同一 session 的重复事件去重；连续进度合并；支持安静时段。
- 面板第一屏先回答「有没有需要我现在处理的」，而不是按时间倒序堆日志。

这比再加功能更能提升体验。

### 3.2 工单化：把通知变成有状态的 Work Item

不要只发气泡。每个需要跟踪的事件落成一条 Work Item：

```json
{
  "id": "wi_...",
  "source": "workbuddy-hook",
  "agent_exe": "WorkBuddyAI.exe",
  "session_id": "7cd3...",
  "project": "D:/githubrepos/Agentrmc",
  "kind": "permission",
  "stage": "needs-me",
  "title": "等你确认 Bash",
  "message": "等你确认 Bash：npm test",
  "created_at": "...",
  "updated_at": "...",
  "attention_since": "...",
  "dedupe_key": "session:kind"
}
```

阶段建议对齐原型的 Ticket stages：

| 阶段 | 条件 | 桌宠行为 |
|---|---|---|
| `needs-me` | 等授权 / 等选方案 | 气泡 + TTS + 自动 raise |
| `review` | 任务完成 / 结果待验收 | 气泡可点；不自动抢前台 |
| `working` | 已派活 / 宿主在跑 | 静默；面板可见 |
| `blocked` / `failed` | 宿主报错或长时间无进展 | 一次提醒；面板置顶 |
| `done` | 用户验收 / 关闭 | 从注意力队列移除 |

最小实现可以先用 JSONL 追加日志 + 内存索引，后续再换 SQLite（原生端目前没链 Qt6::Sql，避免为 MVP 加依赖）。

### 3.3 Router / Worker 正式化

- **Router = AgentRMC**：桌宠、语音、TTS、注意力队列、面板、路由；不执行代码。
- **Worker = 宿主 Agent**：WorkBuddy / WorkBuddy AI / ZCode / Qoder；负责真正的 ReAct Loop 和执行环境。
- Router 只保留「谁在干什么、卡在哪、是否需要人」的全局视角；Worker 的推理细节不进入桌宠上下文。
- 多 Worker 并发时，Router 负责去重、排序、回跳；不要让每个 Worker 各自弹一个气泡。

这正好解释为什么 AgentRMC 不应该自己再做一个 Agent Runtime：它已经是 Router，缺的是 Worker 注册表和 Work Item。

### 3.4 事件驱动 + 两级过滤

- **Fast Path（确定性）**：事件来源、kind、是否重复、安静时段、per-agent 白名单。前台状态只用于决定是否自动 raise / 气泡摆位，**不得用于静音语音提醒**。零延迟、零模型成本。
- **Slow Path（可选）**：只有模糊事件才调 LLM，例如「这条 stop 结果值不值得念」「这条 progress 是否意味着卡住」。默认关闭，避免延迟和费用。
- AgentRMC 现有的 `looksLikeTask` 已经是 Fast Path 的雏形；可以扩展到通知侧。

### 3.5 持久化投递 + Watchdog + 三问面板

- **Outbox**：`/notify` 先落盘再展示；AgentRMC 没开时事件不丢，下次启动补播（可配置只补「需要人」的）。
- **重试**：投递失败 2s 起退避，最多 1 分钟；连续失败三次才升级为系统问题。
- **Watchdog**：监控 45915 端口、宿主进程/窗口、最近事件心跳；面板显示「系统健康」；宿体重启后自动恢复。
- **Panel 三问**：需要我做什么 → 系统健康吗 → 某件事的完整历史。现有事件流可以作为「历史」页保留，但首屏换成注意力队列。

---

## 4. 不建议照搬的部分

| 不抄 | 原因 |
|---|---|
| GitHub / 飞书 Source、PR 合并、Review 流程 | AgentRMC 是桌面入口，不是代码托管 Bot；这些是 DSH 原型的部署内容 |
| 完整 Policy / Mandate / 跨 Source 身份映射 | 单用户桌面场景过重；先用简单 allowlist + 事件开关 |
| 云端无状态计算 + 会话状态入库 | AgentRMC 是本地常驻应用；本地 JSONL/SQLite 足够 |
| Watchdog 也做成一个 LLM Session | 桌宠只需要进程/端口/心跳级 Watchdog；模型级巡检成本高、收益低 |
| Worker 内部 Subagent / Agent Team | 宿主 Agent 已经负责；AgentRMC 不要重复造 |
| 为每个事件都调模型判断 | 延迟和成本不可接受；Fast Path 必须占绝大多数 |

---

## 5. 分阶段落地

### Phase 0：先止血（0.5 天）

- hook payload 补 `session_id`、`cwd`（有就带），`/notify` 用 `session_id + kind` 做去重键。
- 同一 session 的重复通知 5 秒内只处理一次。
- 前台状态只影响是否自动 raise；`permission` / `question` / `stop` 的语音提醒照常。
- `GET /state` 增加 `attention_count`。

验收：连续触发两次相同 permission，只出现一次气泡；前台在 Agent 时 stop 仍然朗读，只是不自动 raise。

### Phase 1：Work Item + 注意力队列（1–2 天）

- 新增本地 Work Item 存储（JSONL + 启动时重建索引）。
- `/notify` 落 Work Item；`GET /work-items`；`POST /work-items/{id}/ack`。
- 主窗新增「待我处理」区块/页签：按 `needs-me > review > blocked > working` 排序；每条显示 Agent、项目、阶段、最后一句、操作（raise / 验收 / 忽略）。
- 桌宠只对 `needs-me` 和 `review` 冒泡；`working` 静默。
- 重启 AgentRMC 后 Work Item 仍在。

验收：两个宿主各跑一个任务，面板能区分；重启后列表不丢；点条目能拉起正确窗口。

### Phase 2：Worker 注册表 + 任务生命周期（2–3 天）

- 监控宿主进程/窗口/最近事件，维护 Worker 列表：`alive / foreground / last_seen / current_work_item`。
- 语音派活时创建 Work Item（`working`）；stop 时转 `review`；permission/question 转 `needs-me`。
- 支持显式选择 Worker（语音面板「交给」已有），并按 Worker 过滤注意力队列。
- 增加安静时段、per-event 开关、去重窗口设置。

验收：并发两个 Agent 时，通知和回跳不串台；注意力计数与列表长度一致。

### Phase 3：持久投递 + Watchdog + 面板重构（3–5 天）

- Outbox 落盘、重试、顺序；AgentRMC 未启动时事件暂存，启动后补播。
- `/health` 返回桥接、宿主、最近事件心跳；面板显示系统问题。
- 主窗首屏改成三问结构：待我处理 / 系统健康 / 历史；事件流降为历史页。
- 增加 `pet_work_update` MCP 工具，让宿主 Agent 主动汇报 `progress/blocked/ready`。

验收：杀掉 AgentRMC 再启动，未处理事件恢复；宿主崩溃后健康页可见；宿主恢复后自动继续。

### Phase 4：DeepSeek Harness 作为 Worker（可选）

- 不把 DSH 原型嵌进 AgentRMC；把它当又一个 Worker/Source 接入。
- 如果用户实际使用 DSH 的 GitHub/飞书 Super-agent，AgentRMC 只负责桌面提醒、语音派活和注意力面板。
- 通过 MCP / HTTP 适配，不复制其 Policy/Executor 实现。

---

## 6. 建议先做哪一刀

**Phase 0 + Phase 1：Work Item + 注意力队列。**

理由：

1. 直接实现文章最核心的「注意力优先」和「Ticket/Attention」，不是抄架构名词。
2. 复用现有 `AgentBridge /notify`、`AgentWindow`、桌宠气泡和主窗事件流，不需要新依赖。
3. 让当前「即发即忘」的通知变成可追踪、可恢复、可验收的工单，是后续 Worker 注册表、Watchdog、面板的地基。
4. 用户能立刻感知：少被打扰、知道哪件事在等自己、点一下回到正确窗口。

最小交付物：

- `WorkItem` 数据结构 + JSONL 存储 + 去重。
- `/work-items`、`/work-items/{id}/ack`。
- 主窗「待我处理」列表。
- 桌宠只对注意力项冒泡。
- 测试：重复事件、重启恢复、双宿主分流、前台只影响 raise 不静音。

---

## 7. 风险与边界

| 风险 | 缓解 |
|---|---|
| 变成第二个 Agent Runtime | 明确 Router 只做交互/路由；执行永远交给宿主 Worker |
| 通知仍然太多 | 默认安静；只对 `needs-me` / `review` 出声；进度只进面板 |
| session_id 缺失 | 用 `agent_exe + cwd + transcript_path` 指纹兜底 |
| JSONL 无限增长 | 按天切分 + 启动压缩；后续可换 SQLite |
| 模型慢路径增加延迟 | 默认关闭；只对模糊事件按需开启 |
| 宿主窗口隐藏 / 托盘 | `AgentWindow` 已知限制；Work Item 保留，不因 raise 失败丢事件 |

---

## 8. 一句话总结

DeepSeek 这份东西最值得 AgentRMC 抄的不是「多 Agent 编排」，而是三件事：

1. **把事件变成工单**（Ticket / Attention）；
2. **把注意力当稀缺资源**（Fast Path 过滤 + 安静优先）；
3. **把投递和恢复做可靠**（Outbox + Watchdog + 三问面板）。

AgentRMC 继续做桌面上的 Router，宿主 Agent 继续做 Worker；先把 Work Item + 注意力队列做出来，再谈并发和主动。
