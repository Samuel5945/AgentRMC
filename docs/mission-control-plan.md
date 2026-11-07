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

### 第 3 批：定向派发（**未开工**）

目标：从某条待办把指令派回**它那条会话**，并拿到「确实到了」的回执。

前置能力必须先逐宿主探明，**不能假设**：

| 能力 | WorkBuddy 系 | Qoder 系 | DSH |
|---|---|---|---|
| 拿到真实会话 id | 有（插件从宿主取） | 有（hook payload） | 有（agent.id） |
| 打开指定会话 | **已验证**（两变体，见下） | 待验证 | 待验证 |
| 按 id 发送并回回执 | **已验证**（两变体，见下） | 待验证 | 待验证 |

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

- 数据面：第 1、2 批已落地并装机验证；第 3 批定向派发的前置能力 **WorkBuddy 系
  已探明并实测**（打开指定会话 + 定向发送 + 回执），Qoder 系 / DSH 待探。
- 表现面：外壳＋六个页签已落，默认启用；桌宠／气泡保持 Qt。
- 版本号仍是 `0.4.1`（Native 与插件同号）：按约定等用户明确说「升版」才动。
- 剩余事项：
  1. 第 3 批：WorkBuddy 系已验证（见 §3 实测小节）；**Qoder 系 / DSH 仍待验证**，
     验完再决定路由与排队策略。
  2. 事件流／系统页的真实数据字段如有新增，记得同步给 `webShellState()`。
  3. 美术主题下的外壳观感复检（有探针图，但需人工确认一遍）。