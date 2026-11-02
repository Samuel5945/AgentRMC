# AgentRMC · Agent 联动与语音交给 Agent PRD

- 文档版本：v1.2（P0 落地后的状态更新）
- 日期：2026-11-02
- 状态：P0 已提交；Native 0.3.4 Work Item + 注意力队列已出包装机；对外发布通道待执行
- 适用版本：AgentRMC Native `0.3.4`（Work Item + 注意力队列）/ `agentrmc-bridge` `0.3.4`（payload 带 session/project）
- 覆盖范围：版本演进与发布状态、hooks 提醒、桌宠气泡 / TTS、窗口回跳、语音分流给宿主 Agent、WorkBuddy 双宿主识别
- 不覆盖：遥控器按键映射、主题系统、ASR / TTS / LLM 模型本身（只在 §2.0 的时间线里点名，不展开需求）

---

## 0. 一句话

让 AgentRMC 成为桌面 Agent 的统一「提醒 + 语音入口」：Agent 等授权、等选方案、任务完成时桌宠负责喊人；用户对桌宠说的话，能落到正确的宿主窗口里。**同一台机器同时装了 WorkBuddy 和 WorkBuddy AI 时，必须认对、喊对、送回对的窗口。**

### 0.1 本轮进展（2026-11-02）

- [x] **FR-01**：`lib/host.js` 按插件安装路径区分 `.workbuddy`（→ `WorkBuddy.exe`）与 `.workbuddy-ai`（→ `WorkBuddyAI.exe`）；路径判不出才按产品名兜底 AI，并标记 `workbuddy-unknown-variant`。路径判据先匹配 `.workbuddy-ai`，避免子串误判。
- [x] **FR-02**：旧版与 AI 的 `raise` 目标互不串台；找不到目标窗口时静默，不拉起兄弟产品。原生端 `raise` 仍按进程映像名精确匹配。
- [x] **FR-03**：三个 `plugin.json` 与三个 `marketplace.json` 统一到 `0.3.3`；随包 zip 实测 0.3.3、含 `hooks-workbuddy/hooks.json`。两套 WorkBuddy 宿主缓存均已自动更新到 0.3.3。
- [x] **FR-07（版本元数据）**：`native/CMakeLists.txt` 成为版本唯一入口；`app.rc.in`、`version.nsi.in` 由 CMake 生成；旧的手写 `native/resources/app.rc` 已删除；`check-version.ps1` 在出包前校验 CMake / NSIS / exe FileVersion / ProductVersion 四处一致。实测 `0.3.0`。
- [x] 测试：`node test/host.test.js` **23/23 PASS**（含双 WorkBuddy 纯函数与真脚本 e2e）；native 8 个测试全绿。
- [x] 出包 / 装机：`dist\AgentRMC-Setup.exe`（20,009,113 B）+ `dist\agentrmc-bridge-0.3.3.zip`；静默安装到 `D:\apps\AgentRMC`，`/state` 验活，exe FileVersion / 注册表 DisplayVersion 均为 `0.3.0`。
- [ ] 提交 / tag / GitHub release / 更新 JSON / 网盘包体：待执行（对外通道，需单独确认）。

### 0.2 本轮 UX 重做（Native 0.3.2）

- [x] **桌宠右键菜单**：不再分组堆设置项，只留高频 5 项——开始/停止监听、
  说一句/结束说话、语音面板、完整界面、隐藏桌宠；识别方式、对话、主题、
  交给哪个 Agent、朗读最近全部移到语音面板 / 完整界面。
- [x] **语音面板承接设置**：新增「交给」下拉（`WorkBuddy` / `WorkBuddy AI` /
  `Qoder CN` 等产品名，钉住但未运行的显示「未运行」）和「朗读最近」按钮；
  识别方式、对话、语音反馈、模型设置继续由底部快捷行承担。
- [x] **目标菜单可读**：新增 `AgentWindow::displayName()`；桌宠气泡「已交给 X」
  也改用产品名，不再显示裸 exe。
- [x] **「没有遥控器」引导页重做**：从 3 行扩成「怎么触发 / 话交给谁识别 / 按需加一点」
  三段，覆盖全局热键、语音主界面、耳机、桌宠、转发/本地/云端、对话模式、TTS、
  Agent 提醒，并显示当前识别方式、提供「语音设置…」入口。
- [x] **使用引导页**：新增「没有遥控器也能用」「桌宠右键菜单（只留高频）」两节；
  「语音怎么用」改为有/无遥控器都适用，补充云端 ASR、对话模式与 Agent 分流。
- [x] **静默安装**：新增 `native/tools/install-silent.bat`，并在 `AGENTS.md` 记录
  Git Bash 直接传 `/S` 会被 MSYS 路径转换、必须走脚本或 `cmd //c` 的坑。

### 0.3 对话人设修正（Native 0.3.3）

- [x] **问题**：鲸鱼娘把 `ZCode` 当成未知词，回了一长串「ZCODE 是什么 / 你进行到哪一步 /
  下一步要什么」的反问，还带 markdown 加粗和 emoji；语音朗读体验很差。
- [x] **补本机语境**：所有人设（自定义 / 主题 / 默认）后追加固定事实——AgentRMC 是用户
  开发的 Windows 桌宠项目；ZCode、WorkBuddy、WorkBuddy AI、Qoder 都是 AI 编程助手 /
  本程序联动的宿主，不是编程语言；附带当前前台 Agent 名。
- [x] **约束回复**：最多两句话 / 50 字，不列清单、不连续反问；用户纠正听写或名词时
  直接一句确认并继续，最多问一个最短澄清问题。
- [x] **输出清理**：气泡 / TTS 前去掉 markdown、emoji、列表标记，超长按句号截到 120 字，
  避免「**ZCODE** 😊」被念出来。
### 0.4 WorkBuddy 收尾（2026-11-02）

- [x] **两套缓存已到 0.3.3**：`~/.workbuddy` 与 `~/.workbuddy-ai` 的 `installed_plugins.json`
  都指向 `0.3.3`；两套缓存内 `lib/host.js` 均含新路径判据，`hooks-workbuddy` 在位。
- [x] **DRY_RUN 双宿主矩阵**：`.workbuddy` → `WorkBuddy` + `WorkBuddy.exe`；
  `.workbuddy-ai` → `WorkBuddy AI` + `WorkBuddyAI.exe`；permission / question / stop
  三条事件全部正确。
- [x] **双开 raise 实测**：先让 WorkBuddy AI 在前台，发 `raise=WorkBuddy.exe` 后前台切到
  旧版 `WorkBuddy`；再发 `raise=WorkBuddyAI.exe` 后切回 `WorkBuddy AI`。
- [x] **stop 气泡点击实测**：stop 通知冒泡后点击气泡，前台从 `WorkBuddy AI` 切到
  `WorkBuddy`，没有拉起兄弟产品。
- [ ] 宿主完全退出后点击气泡（静默失败、不误拉）尚未单独验收。

### 0.5 Work Item + 注意力队列（Native 0.3.4 / Plugin 0.3.4）

- [x] **WorkItemStore**：JSON 持久化到用户数据目录；`session_id + kind + event_id`
  去重，无 event id 时 8 秒窗口兜底；`stop` 自动结束同 session 的 `needs-me`。
- [x] **通知落单**：`/notify` 接收 `session_id` / `project` / `event_id`；
  `permission/question` → `needs-me`，`stop` → `review`，`progress` → `working`。
- [x] **接口**：新增 `GET /work-items`（`attention_count / open_count / items`）与
  `POST /work-items/ack {"id":"..."}`；`/state` 保持兼容。
- [x] **主窗「待我处理」页签**：按等处理 → 待验收 → 进行中排序，双击回到 Agent，
  「标为已处理」归档，「清空已完成」清理；页签显示注意力求和。
- [x] **插件 0.3.4**：hook payload 透传 `session_id` / `project` / `event_id`；
  宿主测试 24/24。
- [x] **实测**：permission 建单、重复投递不重复、stop 转待验收并结束 needs-me、
  重启保留、UI 选中/ack/清空全部通过。

---

## 1. 背景与问题

### 1.1 用户问题

1. Agent 任务通常很长，用户会切走去干别的。等授权、等选方案、任务完成这三类事件需要主动提醒；只发系统通知容易淹没在通知中心里。
2. 提醒之后要能一键回到发通知的那个 Agent 窗口，而不是自己在一堆窗口里找。
3. 桌宠已经支持语音对话，但「让 Agent 去干活」和「跟桌宠闲聊」是两种意图：前者需要把话送进宿主 Agent 的输入框，后者才该走自带 LLM。
4. 同一家厂商可能同时装了两个版本 / 两个产品：`WorkBuddy` 和 `WorkBuddy AI`。两者进程名、配置目录、插件缓存都不同，但产品名和界面高度相似，用户自己都可能分不清。

### 1.2 现状触发点

本机实测同时存在：

| 产品 | 版本 | 进程映像 | 安装目录 |
|---|---|---|---|
| WorkBuddy | 5.9.3 | `WorkBuddy.exe` | `D:\apps\tencentapp\WorkBuddy\` |
| WorkBuddy AI | 5.9.4 | `WorkBuddyAI.exe` | `D:\apps\AIAPPS\WorkBuddyAI\` |

两者都是腾讯同一产品线的 Electron 应用，独立安装、独立升级、独立配置家；检查时 WorkBuddy AI 正在运行，并承载本工作区的 Agent 会话。

---

## 2. 现状盘点

### 2.0 版本演进（0.1 → 0.3）

产品有三条独立的版本线，容易混：**Native 安装包**（`native/CMakeLists.txt` + `installer.nsi`）、**插件** `agentrmc-bridge`（三个宿主清单 + `marketplace.json`）、**Python 旧线**（整体留在 `legacy-python` 分支，仓库内不带版本号，与本 PRD 无关）。MCP server 自称的 `0.1.0`（`mcp/server.js:149`）是它自己的协议版本，不是产品版本。

| 版本 | 时间 | 内容 | 交付与发布状态 | 证据 |
|---|---|---|---|---|
| **0.1.0** | 2026-10-31 16:17 | 原生迁移线 M1–M5：无边框壳 + 配置/映射表 + Raw Input 输入链 + 原生键接管 + 三条语音路线（转发热键 / SenseVoice / Qwen3-ASR）；场景 Profile、耳机播放键开关、更新检查、四步引导、NSIS 安装包与网盘模型包 | GitHub release `v0.1.0`（唯一公开 release）+ 网盘安装包（约 14MB）与模型包（159MB） | `gh release view v0.1.0`；提交 `fd62245`→`fced4c7` |
| **0.2.0** | 2026-10-31 22:26 → 11-01 | 语音打开应用与自定义应用、无遥控器语音主界面 + 全局热键、语音桌宠、TTS 语音反馈 + 云端 ASR + LLM 对话闭环、主题系统（鲸鱼娘形象 / 背景 / 音色跟随）、ZCode 联动（AgentBridge `:45915` + `agentrmc-bridge` 插件 hooks + MCP 三工具） | **已完整对外发布**：官网 `agentrmc-update.json` 线上实测 `version: "0.2.0"`，网盘包体已滚动更新到 0.2.0（作者 11-02 确认；链接沿用原夸克 / 百度分享页，不换 URL）；GitHub **无 release、无 tag** | 用户确认 + `curl https://ligdesign.win/agentrmc-update.json`；提交 `941d4f4`→`ca329a2`；`README.md:400,425,478` |
| **插件 0.2.0 → 0.3.0 → 0.3.1 → 0.3.2** | 11-02 一天 | 0.2.0 适配 Qoder（独立清单 + hooks，`raise` 写死 `Qoder CN.exe`）→ 0.3.0 `lib/host.js` 自检取代写死 → `/notify` 加可选 `raise` 字段、拉前台改由原生端做 → 0.3.1 接入 WorkBuddy（`.codebuddy-plugin` 清单 + 市场源）→ 0.3.2 补 `hooks-workbuddy`、stop 念最终回复、文案去项目名 | 0.3.1 已提交（HEAD `137e3ca`）；**0.3.2 只在工作区**；本机两套 WorkBuddy 缓存分别是 0.3.2 / 0.3.1；`dist\agentrmc-bridge-0.3.1.zip` 是 09:48 的旧手动产物 | 三个清单 `version`（HEAD `0.3.1` / 工作区 `0.3.2`）；随包 zip 实测内含 0.3.2 |
| **0.3.0（当前）** | 11-02 10:52 出包 | `AgentWindow`（宿主窗口探测 / `alive` / `raise`，含双 WorkBuddy 白名单）、`VoicePet` 语音分流（干活→宿主，闲聊→LLM）、气泡可点回跳、stop 不抢前台、右键「交给哪个 Agent」 | **已出包 + 已静默装机 + 进程活着**（`/state` 实测 `pet_visible=1 chat_mode=1 tts_ready=1`）；**代码未提交，官网 JSON 与 GitHub 都还没有 0.3.0** | `git status`（`AgentWindow.*` untracked、17 个文件 modified）；exe 时间戳 11-02 10:52 |

一句话概括现状：**功能已经走到 Native 0.3.2 + 插件 0.3.3，版本元数据（CMake / NSIS / exe）已对齐；对外仍只说到 0.2.0，最新 0.3.2 待提交、发布待执行。** 发布通道仍是四个版本号（见 §2.6），任何「我装的是哪版」的排障都会先撞在这上面。

### 2.1 已交付能力（Native 0.3.3 + Plugin 0.3.3）

| 能力 | 现状 | 关键实现 | 状态 |
|---|---|---|---|
| 本机通知桥 | `127.0.0.1:45915` 提供 `POST /notify`、`POST /speak`、`GET /state`，纯回环、无鉴权 | `native/src/core/AgentBridge.*` | 已实现 |
| 等工具授权提醒 | hooks 发 `permission`：气泡 + TTS + 自动把宿主窗口拉回前台 | `plugins/agentrmc-bridge/hooks/agentrmc-notify.js`、`MainWindow.cpp` | 已实现 |
| 等选方案提醒 | Qoder / WorkBuddy 走 `PreToolUse` + `matcher:"AskUserQuestion"`，payload 复用 `permission` | `hooks-qoder/`、`hooks-workbuddy/` | 已实现（HEAD `137e3ca` 起；两套 WorkBuddy 缓存均已 0.3.3） |
| Work Item / 待我处理 | `/notify` 落成工单，`GET /work-items`、`POST /work-items/ack`，主窗按注意力排序 | `core/WorkItems.*`、`MainWindow::buildWorkPage` | 已实现（0.3.4） |
| 任务完成提醒 | 读 transcript 最后一条 assistant 文本，压成一句话念出；拿不到退回「已完成任务，等你回来验收」 | `hooks/agentrmc-notify.js` | 已实现 |
| 完成提醒不抢前台 | `stop` 只气泡 + 语音，不调 `SetForegroundWindow` | `MainWindow.cpp` | 已实现 |
| 气泡点击回跳 | 气泡带 `raise` 目标时可点；点了收起气泡并拉起对应进程窗口 | `VoicePet::bubbleOnly` / `PetBubble` / `AgentWindow::raise` | 已实现 |
| 宿主探测与前置 | 按进程映像名枚举可见顶层窗口；支持 `alive` / `foregroundExe` / `foregroundAgentExe` / `raise` | `native/src/core/AgentWindow.*` | 已实现 |
| 语音分流 | 对话模式下按本地关键词判断「干活 / 闲聊」：干活优先交给宿主 Agent，闲聊走自带 LLM | `VoicePet::handleUtterance` / `looksLikeTask` / `handOffToAgent` | 已实现 |
| 目标选择 | 显式指定 > 说话时前台 Agent > 最近发通知的宿主 > 按常见宿主探测；`VoicePet::onlineAgentExe` 四级顺序已按此实现 | `VoicePet::onlineAgentExe` | 已实现（但最后一档仍靠 `knownExes()` 顺序，偏向 AI；菜单项显示的是裸 exe 名） |
| 目标显式指定 | 桌宠右键「交给哪个 Agent」→「自动判断」+ 在跑宿主列表 + 「（没检测到在跑的 Agent）」禁用项，写回 `ui.agent_target` | `VoicePet.cpp:980-1005` | 已实现 |
| 粘贴失败兜底 | 注入失败时复制到剪贴板，气泡提示「已复制，手动发一下」 | `VoicePet::handOffToAgent` | 已实现 |
| 桌宠常驻可见 | 手动启动也会 `presentIfEnabled()`，不再只在托盘 | `MainWindow::showOnStartup` | 已实现 |
| WorkBuddy hooks | 根 `hooks/hooks.json`（永远加载）+ manifest 追加 `hooks-workbuddy/hooks.json` | `.codebuddy-plugin/plugin.json` | 已实现（0.3.2） |

### 2.2 双 WorkBuddy 专项事实

| 维度 | WorkBuddy 5.9.3 | WorkBuddy AI 5.9.4 |
|---|---|---|
| 产品名 / 文件描述 | WorkBuddy | WorkBuddy AI |
| 进程映像 | `WorkBuddy.exe` | `WorkBuddyAI.exe` |
| 安装目录 | `D:\apps\tencentapp\WorkBuddy\` | `D:\apps\AIAPPS\WorkBuddyAI\` |
| 插件缓存根 | `C:\Users\PC\.workbuddy\plugins\cache\` | `C:\Users\PC\.workbuddy-ai\plugins\cache\` |
| 应用数据 | `C:\Users\PC\AppData\Roaming\WorkBuddy\` | `C:\Users\PC\AppData\Roaming\WorkBuddy AI\` |
| 本机当前插件版本 | `agentrmc-bridge` 0.3.3 | `agentrmc-bridge` 0.3.3 |
| 进程状态（检查时） | 运行中（7 个进程） | 运行中（6 个进程） |
| 主窗口状态（检查时） | 有可见主窗口（最小化） | 有可见主窗口（最小化） |
| 插件宿主判定 | 两者都会命中 `WORKBUDDY_APP_VERSION` / `CLIENT_INFO_PRODUCT_NAME=WorkBuddy` | 同左 |
| 当前插件 `raise` 兜底 | 0.3.3 起按安装路径判：`.workbuddy` → `WorkBuddy.exe` | `.workbuddy-ai` → `WorkBuddyAI.exe` |

### 2.3 当前链路

```
Agent 宿主
  └─ hook: PreToolUse / PermissionRequest / Stop
       └─ node agentrmc-notify.js
            ├─ lib/host.js 判宿主 + 判 raise 进程名
            └─ POST 127.0.0.1:45915/notify
                 └─ AgentRMC MainWindow
                      ├─ stop       → 气泡 + TTS（不抢前台，气泡可点）
                      ├─ permission → 气泡 + TTS + 自动 raise
                      └─ 其它       → 气泡 / 托盘

用户语音（对话模式 + 本地 ASR）
  └─ VoiceController::chatRequested
       └─ VoicePet::handleUtterance
            ├─ looksLikeTask? ── 否 → 自带 LLM 对话
            └─ 是 → onlineAgentExe()
                 ├─ 前台就是目标 Agent → 立即粘贴 + 回车
                 └─ 否则 → raise → 180ms → 粘贴 + 回车（失败则复制）
```

### 2.4 现状缺口（按优先级）

> 以下是实施前快照。FR-01 / FR-03 / FR-07（版本元数据）已在 §0.1 落地；本表保留用于对照原始问题。

| 级别 | 缺口 | 影响 | 证据 |
|---|---|---|---|
| P0 | `lib/host.js` 无法区分 WorkBuddy / WorkBuddy AI，`raise` 一律给 `WorkBuddyAI.exe` | 从旧版 WorkBuddy 发的提醒会拉起错误的 app；旧版单独运行时则拉不到窗口 | `lib/host.js:108-114` |
| P0 | 双宿主插件版本漂移：旧版 0.3.2、AI 0.3.1 | AI 侧缺少 stop 念最终回复的新逻辑；两套副本版本号不一致，后续升级不可控 | 两个插件缓存目录实测：0.3.1 的 `hooks/agentrmc-notify.js` 无 `lastAssistantText` |
| P0 | `AgentWindow::knownExes()` 的兜底顺序把 `WorkBuddyAI.exe` 放第一位 | 两个都在跑且无前台/记忆时，默认选中 AI；用户若想交给旧版会送错 | `AgentWindow.cpp:57-64` |
| P0 | 0.3.0 / 0.3.2 的实现整体未提交：`AgentWindow.*`、本 PRD 是 untracked，17 个文件 modified | 一次误 reset 就丢整轮实现；也说不清「哪个版本对应哪笔代码」；对外与代码库脱节 | `git status`（11-02）、HEAD = `137e3ca` |
| P0 | 版本声明三处不一致：CMake / NSIS 是 `0.3.0`，`app.rc` 仍写 `0,2,0,0` | 装机 exe 的 FileVersion 实测 **0.2.0**，与程序内 `AGENTRMC_VERSION` 0.3.0 打架；对外说「已更新到 0.3」会被文件属性打脸 | 实测 `D:\apps\AgentRMC\AgentRMCNative.exe`；`app.rc` `FILEVERSION 0,2,0,0` |
| P0 | 对外发布只走到 0.2.0，0.3.x 三个通道都没登记：更新 JSON `0.2.0`、网盘包 `0.2.0`、GitHub `v0.1.0` | 用户看不到 0.3.x 的存在（语音分流 / 气泡回跳 / 双 WorkBuddy 白名单全在外面）；GitHub 还比自家更新源落后一个 minor | `curl agentrmc-update.json`、`gh release list`、作者确认网盘已到 0.2.0 |
| P1 | 插件副本版本三处各说各话：HEAD 清单 `0.3.1`、工作区清单 `0.3.2`、`dist\agentrmc-bridge-0.3.1.zip` 是 09:48 旧产物 | 手工从 `dist` 取包发用户 = 发旧包（缺 `hooks-workbuddy` 与 stop 念回复）；随包 zip 反倒是对的，命名却是 0.3.1 | 随包 zip 实测内含 0.3.2；`dist` 时间戳 |
| P1 | 现有测试把 bug 当期望：`host.test.js:68-76` 断言任意 WorkBuddy 的 `raise == "WorkBuddyAI.exe"` | 实现 FR-01 时不改断言测试就红；为了绿而留着 = 把「拉错 app」锁进回归基线 | `plugins/agentrmc-bridge/test/host.test.js` |
| P1 | 旧版 WorkBuddy 应用数据 / 插件缓存没有明确的产品级兼容承诺 | 升级、重装、清缓存后行为可能变化 | 现状未定义 |
| P1 | 语音启动内置表 `workbuddy` 指向旧版 AUMID `WorkBuddy.WorkBuddy`，没有 WorkBuddy AI 条目 | 「打开 WorkBuddy」开的是旧版；AI 要说全名，靠开始菜单 `.lnk` 兜名 | `AppLauncher.cpp:431-434` |
| P1 | `alive()` 要求「可见且有标题的窗口」 | 宿主收在托盘 / 后台运行时被判为不在线；语音不会分流过去，`raise` 也找不到窗口 | `AgentWindow.cpp:26-43,96-98` |
| P1 | 同一进程多窗口时只取第一个可见窗口 | 多开项目 / 多会话可能拉错窗口 | `findAgentWindow` |
| P2 | 任务 / 闲聊判定是固定正则 + 长度阈值，无用户覆盖 | 口述内容含「帮我 / 改一下」等词时可能被误送 Agent | `VoicePet::looksLikeTask` |
| P2 | 粘贴前固定等待 180ms | 慢机器上可能仍粘到旧窗口；快机器上白等 | `handOffToAgent` |
| P2 | 插件 hook 依赖 PATH 里的 `node` | 宿主自带 node 但 PATH 没有时，hooks 静默失效 | README 前提说明 |
| P2 | 双 WorkBuddy 没有自动化测试用例 | 回归只能靠手工 | `test/host.test.js` |

### 2.5 现场读数（2026-11-02 复核，实施前快照）

| 读数 | 值 | 说明 |
|---|---|---|
| `native/CMakeLists.txt` | `project(... VERSION 0.3.0)` | 工作区，未提交 |
| `native/tools/installer.nsi` | `APPVERSION "0.3.0"` | 工作区，未提交 |
| `native/resources/app.rc` | `FILEVERSION 0,2,0,0` / `PRODUCTVERSION 0,2,0,0` | 未随版本升级 |
| `D:\apps\AgentRMC\AgentRMCNative.exe` | FileVersion **0.2.0**，ProductVersion **0.2.0**，11-02 10:52 | 装机件 = 当前 0.3.0 构建，元数据滞后 |
| `dist\AgentRMC-Setup.exe` | 20.0MB，11-02 10:52 | 含 `plugins\agentrmc-bridge.zip`（内含 0.3.2） |
| 插件清单（三宿主 + 三 marketplace） | `0.3.2`（HEAD `0.3.1`） | 工作区 |
| 本机 WorkBuddy 缓存 | 旧版 `0.3.2` / AI `0.3.1` | 见 §2.2 |
| `GET /state` | `pet_visible=1 docked=0 listening=1 chat_mode=1 tts_ready=1` | 0.3.0 实例在跑 |
| GitHub releases | 只有 `v0.1.0`（2026-10-31） | 0.2.0 / 0.3.0 未发布，无 tag |
| 官网 `agentrmc-update.json`（线上） | `version: "0.2.0"`，`releaseDate: 2026-11-01` | 应用内「检查更新」的唯一依据；0.3.x 未登记 |
| 网盘包体（夸克 / 百度） | 0.2.0（作者 11-02 确认已滚动更新，分享链接不变） | 与更新 JSON 对齐；0.3.x 未上 |
| 随包插件 zip 内容 | 13 个条目，含 `hooks-workbuddy/hooks.json` 与三清单 0.3.2 | FR-03 的「随包必含」一项已满足 |
| `AgentWindow::knownExes()` | `WorkBuddyAI.exe` → `WorkBuddy.exe` → `CodeBuddy.exe` → `ZCode.exe` → `Qoder.exe` → `Qoder CN.exe` | AI 排第一，兜底偏向 AI |

### 2.6 发布通道矩阵（同一台机器上的四种「最新版」）

| 通道 | 谁读它 | 当前声称的版本 | 与真实代码的关系 |
|---|---|---|---|
| 官网 `agentrmc-update.json` | 应用内「检查更新」（`MainWindow.cpp:1538-1541`，可用 `update_url` 覆盖） | **0.2.0**（2026-11-01） | 落后一个 minor：0.3.0 的语音分流 / 气泡回跳 / 双 WorkBuddy 白名单都没进说明 |
| GitHub releases | 官网 / README 引的仓库页脚 | **v0.1.0** | 落后两个 minor，且无 tag |
| 网盘（夸克 / 百度） | 更新弹窗与官网下载按钮给的链接 | **0.2.0**（作者 11-02 确认包体已滚动更新） | 与更新 JSON 一致，是实际生效的分发渠道。注意分享页 URL 不随版本变化——「链接一样」不代表没换包，也只能从包体（体积 / 文件时间）核对，外部看不出来 |
| 本机 `D:\apps\AgentRMC` | 开发者自己 | 0.3.2 构建 + FileVersion 0.3.2 | 本机元数据已对齐；0.3.2 UX 提交待做 |

结论：**0.2.0 这一档对外是对齐的（更新 JSON + 网盘包体一致），不一致的是 0.3.x 完全没进对外通道，GitHub 还单独落后一个 minor；本机 exe 属性已在本轮对齐到 0.3.0，剩余差异在代码库 HEAD 与对外三通道。** FR-07 要收敛的是这条链——CMake → NSIS → exe 属性 → 更新 JSON → 网盘包 → GitHub release，六个点要么一次改齐，要么明确记录哪些通道不动（网盘不换 URL，只能靠换包体 + 体积对账）。

### 2.7 本轮落地后的读数（2026-11-02 11:35）

| 读数 | 值 | 说明 |
|---|---|---|
| `native/CMakeLists.txt` | `project(... VERSION 0.3.0)` | 版本唯一入口 |
| `native/resources/app.rc.in` | `@PROJECT_VERSION_*@` 模板 | 由 CMake 生成 `build/generated/app.rc`；旧 `app.rc` 已删除 |
| `native/tools/installer.nsi` | `!include build/generated/version.nsi` | 不再手写 `APPVERSION` |
| `native/build/generated/version.nsi` | `!define APPVERSION "0.3.0"` | 与 CMake 同步 |
| `native/build/Release/AgentRMCNative.exe` | FileVersion `0.3.0` / ProductVersion `0.3.0` | `check-version.ps1` 通过 |
| `D:\apps\AgentRMC\AgentRMCNative.exe` | FileVersion `0.3.0` / ProductVersion `0.3.0` | 已静默装机 |
| NSIS 注册表 `DisplayVersion` | `0.3.0` | 与 exe / CMake 一致 |
| 插件清单（三宿主 + 三 marketplace） | `0.3.3` | 工作区 |
| `native/build/Release/plugins/agentrmc-bridge.zip` | `0.3.3`，含 `hooks-workbuddy/` | 随包 zip |
| `dist\agentrmc-bridge-0.3.3.zip` | `0.3.3`，22,729 B | 旧 `agentrmc-bridge-0.3.1.zip` 已删除 |
| `dist\AgentRMC-Setup.exe` | 20,009,113 B，11-02 11:37 | 含 0.3.3 插件 |
| `GET /state` | `pet_visible=1 chat_mode=1 listening=1 tts_ready=1` | 装机后验活 |
| `node test/host.test.js` | 23/23 PASS | 含双 WorkBuddy 矩阵 |
| 两套 WorkBuddy 宿主缓存 | 均为 0.3.3 | 已自动更新；`installed_plugins.json` 均指向 0.3.3 |
| GitHub releases / 更新 JSON / 网盘 | 仍为 0.1.0 / 0.2.0 / 0.2.0 | 对外通道未动 |

### 2.8 UX 重做后的读数（2026-11-02 11:50，历史快照）

| 读数 | 值 | 说明 |
|---|---|---|
| `native/CMakeLists.txt` | `project(... VERSION 0.3.3)` | UX + 对话人设版本 |
| `native/build/Release/AgentRMCNative.exe` | FileVersion / ProductVersion `0.3.3` | `check-version.ps1` 通过 |
| `D:\apps\AgentRMC\AgentRMCNative.exe` | FileVersion / ProductVersion `0.3.3` | 静默装机器 |
| NSIS `DisplayVersion` | `0.3.3` | 与 exe / CMake 一致 |
| `dist\AgentRMC-Setup.exe` | 20,013,752 B，11-02 13:32 | 含 0.3.3 插件 |
| `dist\agentrmc-bridge-0.3.3.zip` | 0.3.3，22,729 B | 随包插件 |
| 桌宠右键一级 | 5 项：开始/停止监听、说一句、语音面板、完整界面、隐藏桌宠 | UI Automation 实测 |
| 语音面板 | 新增「交给」下拉与「朗读最近」按钮 | UI Automation 实测文本/按钮 |
| 对话人设 | 所有人设追加本机语境与术语表；回复清理 markdown/emoji 并限长 | 代码路径 `VoicePet::chatPersona` / `cleanChatReply` |
| `GET /state` | `pet_visible=1 chat_mode=1 listening=1 tts_ready=1` | 装机后验活 |
| 静默安装命令 | `native\tools\install-silent.bat` | Git Bash 不再直接传 `/S` |

### 2.9 Work Item 落地后的读数（2026-11-02 14:00）

| 读数 | 值 | 说明 |
|---|---|---|
| `native/CMakeLists.txt` | `project(... VERSION 0.3.4)` | Work Item 版本 |
| `D:\apps\AgentRMC\AgentRMCNative.exe` | FileVersion / ProductVersion `0.3.4` | 静默装机器 |
| NSIS `DisplayVersion` | `0.3.4` | 与 exe / CMake 一致 |
| `dist\AgentRMC-Setup.exe` | 20,041,532 B，11-02 14:00 | 含 0.3.4 插件 |
| `dist\agentrmc-bridge-0.3.4.zip` | 0.3.4，23,014 B | 随包插件 |
| Work Item 存储 | `%APPDATA%\LIG\AgentRMCNative\work-items.json` | 重启保留 |
| `GET /work-items` | `attention_count / open_count / items` | 实测 permission 建单、stop 转 review |
| `POST /work-items/ack` | 204 / 404 | UI 选中后 ack 实测 |
| 「待我处理」页签 | 现在是第一个页签，带注意力计数 | UI Automation 实测 |
| 插件宿主测试 | 24/24 PASS | 含 session/project/event_id 透传 |
| Native 测试 | 9 个测试全绿（新增 `AgentRMCWorkItemTest`） | 含 WorkItem 持久化/去重 |

---

## 3. 用户与场景

### 3.1 目标用户

| 用户 | 特征 | 核心诉求 |
|---|---|---|
| 单人开发者 / 重度 Agent 用户 | 同时开多个 Agent，任务动辄十几分钟 | 提醒必须喊对人；一键回到发通知的窗口 |
| 桌宠用户 | 喜欢语音交互，但不希望所有话都送进 Agent | 闲聊和干活自动分流，说错能改 |
| 多产品试用者 | 同一家产品装了稳定版 + 新版 | 两个版本互不串台，能显式指定 |

### 3.2 核心场景（JTBD）

1. **等授权**：Agent 要跑一条命令，用户正在浏览器里。桌宠出声「等你确认 Bash：…」，并自动把 Agent 窗口拉回前台。
2. **任务完成**：用户在看视频。桌宠念出 Agent 的最终结论，但不抢焦点；用户想回去时点一下气泡。
3. **语音派活**：用户对着桌宠说「帮我改一下这个报错，然后跑测试」。桌宠识别为任务，拉起当前 Agent，把原话粘进输入框并回车。
4. **语音闲聊**：用户说「今天好累」。桌宠不打扰 Agent，直接用自带 LLM 回一句。
5. **双版本共存**：用户从旧版 WorkBuddy 收到提醒，桌宠必须拉起旧的 `WorkBuddy.exe`；从 WorkBuddy AI 收到提醒，必须拉起 `WorkBuddyAI.exe`。
6. **显式指定**：两个 Agent 都开着，用户在右键菜单选「交给 WorkBuddy AI」，之后语音默认送它。

---

## 4. 产品目标与非目标

### 4.1 目标

1. 双 WorkBuddy 场景下，通知回跳与语音分流的**目标正确率 100%**（按进程名和窗口逐一验证）。
2. 提醒链路对会话**零阻塞**：AgentRMC 未启动 / 端口不可用时 hook 静默退出，不拖慢 Agent。
3. 语音派活在目标窗口已在前台时**一次成功**；不在前台时成功率达到可接受水平，失败必须保底复制。
4. 用户能看见并纠正分流结果：气泡明确显示交给了谁；右键可改默认目标。

### 4.2 非目标

- 不做跨机器 / 远程 Agent 通知。
- 不做 Agent 内部的会话管理、任务编排。
- 不替代宿主自己的通知中心；只做「桌宠这一层」的即时提醒。
- 不保证对未知宿主的自动识别；未知宿主按 ZCode 缺省行为处理。

### 4.3 成功指标

| 指标 | 目标 | 测量方式 |
|---|---|---|
| 双 WorkBuddy 目标正确率 | 100% | 自动化矩阵 + 手工双开验证 |
| 通知送达延迟（AgentRMC 已启动） | p95 < 150ms | hook 计时日志 |
| AgentRMC 未启动时 hook 退出 | < 100ms 且 exit 0 | 自动化 |
| 语音派活成功率（目标在前台） | ≥ 99% | 手工 / 埋点 |
| 语音派活成功率（目标不在前台） | ≥ 90% | 手工 / 埋点 |
| 误送 Agent 比例 | < 5% | 用户反馈 / 埋点 |
| 插件安装后无需手工改配置 | 是 | 双宿主安装验收 |

---

## 5. 需求

### 5.1 P0（必须：FR-01～FR-03 随插件 0.3.3；FR-07 随本轮提交与下一次出包）

#### FR-01 双 WorkBuddy 宿主识别

- 识别优先级：
  1. 调用方 argv 显式给出宿主名 + 进程名 → 照办（兼容老配置）。
  2. 插件安装路径（`scriptDir`）落在 `/.workbuddy-ai/` → `WorkBuddy AI` + `WorkBuddyAI.exe`。
  3. 路径落在 `/.workbuddy/` → `WorkBuddy` + `WorkBuddy.exe`。
  4. 都不匹配但环境变量表明是 WorkBuddy → 保持旧行为（`WorkBuddyAI.exe`），并在 `source` 中标记 `workbuddy-unknown-variant` 便于排查。
- 路径判断必须先匹配 `.workbuddy-ai`，再匹配 `.workbuddy`，避免子串误判。
- **禁止跨版本兜底**：识别为旧版时不得因为找不到旧版窗口就改拉 AI；找不到就静默，不拉起另一个 app。
- 气泡标题分别显示 `WorkBuddy` / `WorkBuddy AI`，不再混用。

验收标准：

- [x] `scriptDir` 为 `.workbuddy/plugins/cache/...` 时，DRY_RUN payload 的 `title=WorkBuddy`、`raise=WorkBuddy.exe`。
- [x] `scriptDir` 为 `.workbuddy-ai/plugins/cache/...` 时，payload 的 `title=WorkBuddy AI`、`raise=WorkBuddyAI.exe`。
- [x] 自定义配置家 + `WORKBUDDY_APP_VERSION` 时，不退化成 ZCode / Qoder。
- [x] 自动化测试覆盖上述矩阵（23/23 PASS）。

#### FR-02 通知回跳目标正确

- `permission` / `question`：自动 raise 发通知的宿主窗口。
- `stop`：不自动 raise；气泡可点，点击 raise 同一个宿主窗口。
- 宿主窗口收在托盘 / 最小化时：
  - 最小化 → `SW_RESTORE` 后再前置。
  - 完全隐藏（无可见顶层窗口）→ 本轮允许静默失败；不拉起同厂商另一个版本。
- `raise` 只按进程映像名精确匹配，不做模糊匹配。

验收标准：

- [x] 双开 WorkBuddy / WorkBuddy AI，分别触发 permission，各自拉起自己的窗口。
  （实测：`raise=WorkBuddy.exe` → 前台 WorkBuddy；`raise=WorkBuddyAI.exe` → 前台 WorkBuddy AI）
- [x] 从旧版触发 stop，气泡点击后拉起旧版，不拉起 AI。
  （实测：stop 气泡点击后前台从 WorkBuddy AI 切到 WorkBuddy）
- [ ] 宿主完全退出后点击气泡，无错误弹窗、无其它 app 被拉起。

#### FR-03 插件版本一致性与升级

- 两套 WorkBuddy 配置家各自安装、各自升级；安装包 / 市场描述中明确「两个 WorkBuddy 需要分别安装」。
- 0.3.3 发布后，旧版 `~/.workbuddy` 与 AI `~/.workbuddy-ai` 都应能通过各自插件市场升级到同一版本。
- 安装器随包的 `plugins/agentrmc-bridge.zip` 包含 `hooks-workbuddy/hooks.json`。
- 版本号规则：插件补丁 +1（0.3.3），Native 无代码改动则保持 0.3.0；若同时改 Native，再升 Native 0.3.1。

验收标准：

- [x] 两个缓存目录最终版本号一致。（均为 0.3.3）
- [x] `hooks-workbuddy/hooks.json` 在两个宿主都能被加载（或明确记录不支持）。
- [x] `node test/host.test.js` 全绿（23/23）。

#### FR-07 版本声明与发布一致性（新增 P0）

现状是同一台机器上四种「最新版」并存：功能 0.3.0（工作区 + 已装机）、exe 文件属性 0.2.0（`app.rc` 没跟上）、应用内更新说 0.2.0（官网 JSON）、GitHub 说 v0.1.0。要求单一事实源 + 一处改动全链路同步（通道明细见 §2.6）：

- 版本以 `native/CMakeLists.txt` 的 `project(... VERSION x.y.z)` 为唯一入口，`installer.nsi` 与 `app.rc` 由构建生成或按同一值改写，不允许手写第三处。
- 出包前跑一致性自检：`AgentRMCNative.exe` 的 FileVersion / ProductVersion、NSIS 显示版本、`AGENTRMC_VERSION` 三者必须相等。
- `dist` 只保留当前有效产物：插件 zip 文件名 = 清单版本（当前 `agentrmc-bridge-0.3.3.zip`），旧版本号文件删除或移入 `dist\archive\`。
- 一轮需求攒成一个提交并打 tag（`v0.3.0` 对应 Native、插件 tag 另计），GitHub release 至少把 0.2.0 与 0.3.0 补发出来；release 附件必须随创建命令一起上传（先建后补会 422，删了重建会烧掉 tag 名）。
- 提交范围里含 `AgentWindow.*` 与本 PRD——目前两者都是 untracked。
- 发布动作要覆盖整条链，不允许只改一处：GitHub release + tag、`agentrmc-update.json`（`version` / `releaseDate` / `notes` 中英双份）、网盘包体（换包后核对文件名与体积）、官网下载页。用户从「检查更新」点进去拿到的必须是同一版本。

验收标准：

- [x] 装机 exe 的 FileVersion 与程序内版本一致（实测读数写进发布记录：`0.3.0`）。
- [ ] `git log --oneline` 出现 0.3.0 / 插件 0.3.3 的实现提交，`git status` 无 untracked 源码。
- [ ] GitHub releases 有 v0.2.0、v0.3.0（或明确记录「0.2.0 跳过发布，直接以 0.3.0 为对外版本」的决策）。
- [x] `dist` 内不存在与当前清单版本不符的插件 zip（旧 0.3.1 zip 已删除，现有 0.3.3）。

### 5.2 P1（应该，随下一 Native 版本）

#### FR-04 目标选择与回退顺序（四级顺序已实现，剩命名与兜底）

`VoicePet::onlineAgentExe()` 已按固定顺序实现，不再靠候选顺序猜：

1. ✅ 用户在右键菜单显式指定的 `ui.agent_target`，且目标进程有可见窗口。
2. ✅ 说话瞬间前台窗口是已知宿主 → 用它。
3. ✅ 最近一次通知的 `raise`（`ui.agent_exe`），且仍在线。
4. ⚠️ 兜底才按 `knownExes()` 顺序探测——**两个 WorkBuddy 都在时仍是固定偏向 AI**（`AgentWindow.cpp:57-64` 把 `WorkBuddyAI.exe` 放第一位）。要求改为优先用户最近使用 / 最近通知的那个。

验收标准（只列未达标部分）：

- [ ] 兜底不再固定选中 AI：两个都在跑且无前台 / 无记忆时，按最近使用或明确提示用户选。
- [ ] 菜单中两个 WorkBuddy 都显示为独立选项，且是产品名（`WorkBuddy` / `WorkBuddy AI`）而非裸 `WorkBuddy.exe` / `WorkBuddyAI.exe`。

#### FR-05 语音启动别名

- 明确支持两种叫法：
  - `WorkBuddy AI` / `WorkBuddyAI` / `AI 工作伙伴` → `WorkBuddyAI.exe`
  - `WorkBuddy` / `工作伙伴` → 旧版 `WorkBuddy.exe`
- 仅安装其中一个时，未加限定的 `WorkBuddy` 打开已安装的那个。
- 「应用」页展示两个独立条目及安装状态。

验收标准：

- [ ] 双装时「打开 WorkBuddy」和「打开 WorkBuddy AI」分别打开对应版本。
- [ ] 单装时两个叫法都能打开已安装版本。

#### FR-06 宿主在线判定分层

- 区分「进程存活」与「可交互窗口」：
  - 语音分流、raise：必须有可交互窗口。
  - 状态展示、通知标题：进程存活即可。
- 托盘 / 隐藏窗口场景，`/state` 增加可选的 `agents` 字段（或等价能力），便于用户排查「为什么没送过去」。

验收标准：

- [ ] WorkBuddy AI 在托盘运行时，右键菜单仍显示为「运行中（窗口隐藏）」而非完全消失。
- [ ] 语音分流不会把内容送进隐藏窗口导致丢失。

### 5.3 P2（可选）

- 任务 / 闲聊判定可配置：右键菜单切换「总是交给 Agent / 自动判断 / 总是 LLM」。
- 任务判定支持一次「撤回并改为闲聊」的快捷操作。
- 多窗口选择：同一进程多个项目窗口时，按标题匹配当前项目 / 最近活跃窗口。
- 粘贴延迟自适应：检测前台窗口变化，而不是固定 180ms。
- hook 使用宿主自带 node（若可探测），减少 PATH 依赖。
- 通知去重与静默时段（会议 / 全屏）。

### 5.4 不做

- 不做 WorkBuddy / WorkBuddy AI 之间的自动迁移或配置同步。
- 不修改宿主安装目录或注入宿主进程。
- 不读取宿主云端数据；transcript 只读本地文件。

---

## 6. 交互规格

### 6.1 提醒气泡

| 事件 | 声音 | 气泡 | 自动前置 | 点击气泡 | 桌宠隐藏时 |
|---|---|---|---|---|---|
| `permission` | 是 | 7s | 是 | 前置 | TTS；无 TTS 时托盘 |
| `question`（映射为 permission） | 是 | 7s | 是 | 前置 | 同上 |
| `stop` | 是 | 7s | 否 | 前置 | 仅 TTS |
| `speak` | 是 | 默认时长 | 否 | 无 | 仅 TTS |
| 其它 `notify` | 否 | 6s | 否 | 有 raise 则可点 | 托盘 |

> **前台不静音**：前台状态只影响「是否自动前置」，不影响「是否出声」。即使宿主窗口
> 就在前台，用户也可能没看屏幕，`permission` / `question` / `stop` 的语音照常播放。

文案规范：

- 等授权：`等你确认 <工具名>：<最多 60 字摘要>`
- 等选方案：`等你选方案`
- 完成：`<Agent>：<最终回复第一句，最多 80 字>`；取不到则 `<Agent> 已完成任务，等你回来验收`
- 气泡标题使用宿主名，双 WorkBuddy 必须可区分。

### 6.2 语音分流

```
说话 → 本地转写
  ├─ 空文本 → 忽略
  ├─ 明显是任务（关键词或长度 ≥ 40）
  │    ├─ 有可用 Agent → 交给 Agent
  │    └─ 无 Agent 但有 LLM → LLM 兜底
  └─ 闲聊
       ├─ 有 LLM → LLM
       └─ 无 LLM 但有 Agent → 交给 Agent 兜底
两边都没有 → 气泡提示「右键 → 对话设置」
```

交给 Agent 的动作：

1. 目标窗口已在前台：立即粘贴 + 回车。
2. 否则：raise → 等待焦点切换 → 粘贴 + 回车。
3. 注入失败：复制原文到剪贴板，气泡「没贴上，已复制，手动发一下」。
4. 成功：气泡「已交给 <宿主名>」。

### 6.3 目标选择菜单

右键桌宠 →「交给哪个 Agent」：

- 「自动判断」（默认）
- 运行中的宿主列表（含两个 WorkBuddy，名称带版本区分）
- 没有检测到时显示「（没检测到在跑的 Agent）」并禁用

持久化键：

| 键 | 含义 | 写入方 |
|---|---|---|
| `ui.agent_target` | 用户显式指定的宿主进程名；空 = 自动 | 右键菜单 |
| `ui.agent_exe` | 最近一次发通知的宿主进程名 | `/notify` 处理 |

---

## 7. 宿主兼容矩阵

| 宿主 | 进程映像 | 配置家 / 插件缓存 | hooks 文件 | 等选方案事件 | raise 目标 |
|---|---|---|---|---|---|
| ZCode | `ZCode.exe` | `~/.zcode/cli/plugins/cache/` | `hooks/hooks.json` | `PermissionRequest`（tool_name 命中） | 空 → 原生缺省 `ZCode.exe` |
| Qoder | `Qoder.exe` | `~/.qoder/` | `hooks-qoder/hooks.json` | `PreToolUse` + `AskUserQuestion` | `Qoder.exe` |
| Qoder CN | `Qoder CN.exe` | `~/.qoder-cn/` | `hooks-qoder/hooks.json` | `PreToolUse` + `AskUserQuestion` | `Qoder CN.exe` |
| WorkBuddy | `WorkBuddy.exe` | `~/.workbuddy/` | 根 hooks + `hooks-workbuddy/` | `PreToolUse` + `AskUserQuestion` | `WorkBuddy.exe`（0.3.3 修正） |
| WorkBuddy AI | `WorkBuddyAI.exe` | `~/.workbuddy-ai/` | 根 hooks + `hooks-workbuddy/` | `PreToolUse` + `AskUserQuestion` | `WorkBuddyAI.exe` |

约束：

- 所有宿主共用同一个原生端和同一套 `/notify` 协议，不为单个宿主加特例字段。
- 未知宿主按 ZCode 缺省处理；不得退化成 Qoder 或 WorkBuddy。

---

## 8. 非功能需求

| 类别 | 要求 |
|---|---|
| 延迟 | hook 到气泡 p95 < 150ms；AgentRMC 未启动时 hook < 100ms 退出 |
| 阻塞 | hook 任何分支都不得阻塞 Agent 会话；2.5s 硬超时 |
| 可靠性 | 通知失败静默；语音注入失败必须复制保底 |
| 安全 | 仅监听 `127.0.0.1`；不引入外部依赖；不执行宿主传入的任意命令 |
| 隐私 | transcript 只读本地文件；不落盘、不上传；LLM 调用仅走用户配置的 endpoint |
| 兼容 | Windows 10/11 x64；Qt 6.8；Node 18+（hook） |
| 可观测 | `/state` 可见桌宠状态；hook 支持 `AGENTRMC_DRY_RUN=1` 输出判据 |
| 可维护 | 宿主判据集中在 `lib/host.js`；新增宿主只改一处 + 测试矩阵 |

---

## 9. 边界与异常

| 场景 | 期望行为 |
|---|---|
| 两个 WorkBuddy 都运行 | 前台 / 显式指定 / 最近通知优先；无信息时默认 AI，但气泡标题必须可区分 |
| 只有旧版 WorkBuddy | 通知与语音都指向旧版；不得尝试拉起 AI |
| 只有 WorkBuddy AI | 指向 AI |
| 宿主收在托盘 | 通知仍送达；raise 失败则静默；不拉起兄弟产品 |
| 同一 exe 多窗口 | 取第一个可见且有标题的窗口；P2 再做标题匹配 |
| 宿主以管理员权限运行 | 注入可能被 UIPI 拦截 → 复制 + 提示手动发 |
| AgentRMC 未启动 / 端口被占 | hook 静默退出 0；不影响 Agent |
| `node` 不在 PATH | hook 不执行；插件安装文档必须写明前提 |
| transcript 不存在 / 尾部损坏 | 退回通用完成文案 |
| 说话时前台不是 Agent | 按指定 / 最近通知 / 探测顺序选择；先 raise 再粘贴 |
| 说话内容像任务但用户想闲聊 | P2 提供模式切换 / 撤回；当前接受一定误判 |
| 双宿主插件版本不一致 | 各自市场升级；PRD 要求最终一致，不强制自动同步 |

---

## 10. 配置与数据

### 10.1 Native 配置

| 键 | 默认 | 说明 |
|---|---|---|
| `ui.pet_enabled` | `true` | 桌宠开关 |
| `ui.pet_chat` | `false` | 对话模式 |
| `ui.agent_target` | 空 | 显式指定的宿主进程名 |
| `ui.agent_exe` | 空 | 最近通知宿主进程名（自动写入） |
| `voice.llm_api.*` | 空 | 自带 LLM 配置 |

### 10.2 插件环境

| 变量 | 用途 |
|---|---|
| `WORKBUDDY_APP_VERSION` | 判定 WorkBuddy 家族 |
| `CLIENT_INFO_PRODUCT_NAME` | 判定 WorkBuddy 家族 |
| `AGENTRMC_NOTIFY_HOST` | 覆盖气泡标题 |
| `AGENTRMC_DRY_RUN=1` | 只输出 payload，不发送 |
| `QODER_*` | Qoder 变体判定（既有） |

### 10.3 宿主判据（0.3.3 目标实现）

```text
resolveHost(argv, scriptDir, env):
  1. argv 同时给宿主名 + 进程名 → 直接用
  2. scriptDir 路径：
       /.workbuddy-ai/ → WorkBuddy AI + WorkBuddyAI.exe
       /.workbuddy/    → WorkBuddy    + WorkBuddy.exe
       /.qoder-cn/     → Qoder CN
       /.qoder/        → Qoder
  3. 环境变量（Qoder 路径 / 产品 ID；WorkBuddy 产品名）
  4. 缺省 ZCode
  5. 标记 source，便于 DRY_RUN 与问题排查
```

---

## 11. 发布计划

### 已走到

| 版本 | 内容 | 交付物 / 状态 |
|---|---|---|
| 0.1.0（2026-10-31） | 原生迁移线 M1–M5 + 分发流程 | GitHub release `v0.1.0`（公开）+ 网盘安装包与模型包 |
| 0.2.0（2026-11-01） | 语音打开应用 / 语音主界面 / 桌宠 / TTS+LLM 对话闭环 / 主题系统 / ZCode 联动（AgentBridge + 插件） | **已对外发布**：官网 `agentrmc-update.json` 标 0.2.0 + 网盘包体已滚动更新到 0.2.0（作者确认）；GitHub 无 release、无 tag |
| Native 0.3.0（已提交 + 已装机） | 语音分流、可点气泡、`AgentWindow`（含双 WorkBuddy 白名单）、stop 不抢前台、右键目标菜单；版本元数据已收敛到 CMake（FileVersion / ProductVersion = 0.3.0） | 提交 `03151c5`；`dist\AgentRMC-Setup.exe` 已静默装机并验活；**未发布** |
| Native 0.3.2（UX 真正精简，已提交 `39670f7`） | 桌宠右键只留 5 项；识别/对话/主题/目标/朗读迁到语音面板与完整界面；语音面板新增「交给」「朗读最近」 | `dist\AgentRMC-Setup.exe`（20,010,218 B，11-02 11:58）；FileVersion / NSIS `0.3.2` |
| Native 0.3.3（对话人设修正，已出包 + 已装机） | 所有人设追加本机语境与 ZCode/WorkBuddy 术语表；回复禁止清单/连续反问；落气泡前清理 markdown/emoji 并限长 | `dist\AgentRMC-Setup.exe`（20,013,752 B，11-02 13:32）；FileVersion / NSIS `0.3.3`；本次提交 |
| Native 0.3.4（Work Item + 注意力队列，已出包 + 已装机） | `WorkItemStore` JSON 持久化、去重、stop 结束 needs-me；`GET /work-items` / `POST /work-items/ack`；主窗「待我处理」页签 | `dist\AgentRMC-Setup.exe`（20,041,532 B，11-02 14:00）；FileVersion / NSIS `0.3.4` |
| Plugin 0.3.3（工作区 + 已出包） | FR-01 双 WorkBuddy 识别、FR-03 版本统一；stop 最终回复、`hooks-workbuddy` 沿用 0.3.2 | `dist\agentrmc-bridge-0.3.3.zip` + 随包 zip；两套 WorkBuddy 宿主缓存均已 0.3.3 |
| Plugin 0.3.4（已出包） | hook payload 透传 `session_id` / `project` / `event_id`，给 Work Item 去重和分组 | `dist\agentrmc-bridge-0.3.4.zip`（23,014 B）；宿主缓存待自动更新 |

### 待排期

| 版本 | 内容 | 交付物 |
|---|---|---|
| 提交 / 发布 | Work Item 改动提交；tag、GitHub release、更新 JSON、网盘包体 | 对外动作需单独确认 |
| Plugin 0.3.4 宿主升级 | 旧版 `~/.workbuddy` 与 AI `~/.workbuddy-ai` 各自升级到 0.3.4 | 宿主插件市场 / 自动更新 |
| Native 后续 | FR-05 语音启动别名、FR-06 在线判定分层；FR-04 的兜底顺序部分 | 新安装包 + GitHub release |
| 补发 | 0.2.0 / 0.3.0 的 GitHub release（或记录「跳过 0.2.0，对外从 0.3.0 起算」） | release + 附件随创建命令上传 |

发布检查清单：

- [x] P0 已提交 `03151c5`；0.3.1 右键分组版已提交 `3bf3de4`；0.3.2 精简已提交 `39670f7`；0.3.3 对话人设随本次提交；tag 仍待打。
- [x] `app.rc` / CMake / NSIS 三处版本一致，装机 exe FileVersion 实测对得上（`check-version.ps1` 已接入 `package.bat`）。
- [x] `node test/host.test.js` 全绿，含双 WorkBuddy 矩阵（23/23）。
- [x] native 相关测试全绿（App / Store / Theme / Bridge / Router / Voice / Takeover / Audio）。
- [x] `native\tools\package.bat` 出包。
- [x] 静默安装 + 启动 + `curl /state` 验活。
- [x] 双开 WorkBuddy / WorkBuddy AI 手工验收：permission 双向前台、stop 气泡点击已实测；question 走 DRY_RUN payload 验证。
- [x] README / 插件 README / 市场描述同步双宿主说明。
- [x] `dist` 无陈旧版本号的插件 zip（当前仅 `agentrmc-bridge-0.3.4.zip`）。
- [ ] `agentrmc-update.json` 线上 `version` == 本次发布版本，`notes` 含 0.3.x 内容；用 ≤0.2.0 的实例点「检查更新」能收到提示，点进去的包 = 同版本。
- [ ] 网盘包体与发布版本对得上（体积 / 文件时间做账），或明确记录「本次不走网盘」。

---

## 12. 测试计划

### 12.1 自动化

| 层级 | 用例 |
|---|---|
| `lib/host.js` 纯函数 | `.workbuddy` → 旧版；`.workbuddy-ai` → AI；自定义路径 + env；argv 覆盖；未知宿主（已实现） |
| 既有断言改写 | `host.test.js:68-76` 已改为「`.workbuddy` → `WorkBuddy.exe` / `.workbuddy-ai` → `WorkBuddyAI.exe` / 判不出 → `workbuddy-unknown-variant`」 |
| hook DRY_RUN | 双 WorkBuddy 各自的 payload `title` / `raise`；stop transcript 读取；question 防刷屏；session/project/event_id 透传（已覆盖） |
| Work Item | `AgentRMCWorkItemTest`：建单、同 event id 去重、不同 tool id 新建、stop 结束 needs-me、ack、重载、清空、无 event id 8 秒去重 |
| Native | `knownExes()` 包含两个 WorkBuddy；`AgentWindow` 目标选择顺序（可注入窗口枚举）——待 P1 |
| 版本一致性 | `check-version.ps1`：CMake / NSIS / exe FileVersion / ProductVersion 四处相等（已实现并接入 package.bat） |
| 回归 | `host.test.js` 24/24 PASS；native 9 个测试全绿 |

### 12.2 手工矩阵

| 场景 | 步骤 | 期望 |
|---|---|---|
| 双开 permission | 两个 WorkBuddy 各触发一次授权 | 各拉各的窗口，标题正确 |
| 旧版 stop | 旧版跑完一轮 | 不抢前台；点气泡拉起旧版 |
| AI stop | AI 跑完一轮 | 不抢前台；点气泡拉起 AI |
| 双开语音 | 分别在前台时说任务 | 送给前台那个 |
| 显式指定 | 菜单选旧版后说任务 | 送旧版 |
| 托盘 | 宿主收托盘后触发提醒 | 不崩溃、不拉错 app |
| 未启动 | 关闭 AgentRMC 触发 hook | <100ms 退出，会话正常 |
| 注入失败 | 管理员权限宿主 | 复制 + 提示 |
| 老版本升级 | 从公开渠道的 v0.1.0 就地升级到 0.3.0（`Setup.exe /S`） | `config.json` / `profiles.json` 映射保留；桌宠、主题、语音路线不丢；`/state` 应答 |
| 版本读数对账 | 看程序内版本 + exe 属性 + 安装包显示版本 | 三处一致（本轮实测均为 0.3.0） |

---

## 13. 风险与开放问题

### 13.1 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| WorkBuddy 5.9.3 后续停止维护 | 兼容成本长期存在 | 明确「旧版尽力支持」；核心保证 AI 版 |
| 宿主升级改变配置家路径 | 自检失效 | 路径 + 环境变量双判据；未知变体标记 source |
| 托盘 / 隐藏窗口无 Win32 可见窗口 | raise 无法恢复 | P1 增加进程级探测与托盘激活策略；至少不拉错 |
| 用户分不清两个 WorkBuddy | 误送、误报 | 标题带产品名；菜单显示版本 / 安装路径 |
| 任务正则误判 | 闲聊被送进 Agent | P2 模式切换 / 撤回；文案可解释 |
| 插件版本漂移 | 功能不一致 | 发布检查清单 + 市场升级提示 |
| 0.3.x 全部留在工作区 | 一次误 reset / 换机就丢掉整轮实现；也无法说明「哪个版本对应哪笔代码」 | 本轮先补提交与 tag，再排 0.3.3（FR-07） |
| 对外版本口径不一 | 应用内更新与网盘说 0.2.0、GitHub 说 v0.1.0、程序内是 0.3.0、exe 属性是 0.2.0；用户报障时说不清自己跑的哪版 | 版本单一事实源 + 出包前自检 + 发布按 §2.6 通道矩阵逐条打勾 |
| 网盘 URL 不变 ⇒ 换包无痕 | 发新版时漏换包，就是「提示新版 / 下到旧包」，而且从链接上看不出来（0.2.0 这次换对了） | 发布记录里落文件名 + 体积 + 上传时间三件事，作为对账凭据 |
| 现有测试把已知 bug 写成期望 | FR-01 落地时要么测试红、要么靠改断言把 bug 洗白 | **已处理**：断言与实现同笔改为「旧版 WorkBuddy → `WorkBuddy.exe`」 |

### 13.2 开放问题（需要产品决策）

1. **默认宿主**：双装且无前台 / 无通知记忆时，默认 AI 还是旧版？当前代码默认 AI；是否改为「最近使用」？
2. **语音启动别名**：无修饰的「WorkBuddy」应该打开旧版（向后兼容）还是 AI（当前主力）？
3. **旧版支持级别**：WorkBuddy 5.9.3 是「正式支持」还是「尽力兼容」？
4. **托盘恢复**：宿主隐藏到托盘时，是否允许通过进程 / 托盘图标强制唤出？还是只提示用户手动打开？
5. **stop 回跳**：是否提供「完成提醒也自动拉前台」的可选项（默认关）？
6. **多窗口**：同一 exe 多项目窗口时，按标题匹配项目名是否足够？
7. **任务判定**：是否引入 LLM 二次判定（会增加延迟），还是保持本地规则 + 用户模式开关？
8. **插件 node 依赖**：是否改用宿主内置 node，还是继续要求 PATH？
9. **发布主渠道**：对外以官网 JSON + 网盘为准，还是补 GitHub release？两者都留的话，谁是「最新版」的唯一口径？
10. **0.2.0 要不要补 GitHub release**：GitHub 只有 v0.1.0。补发要重传附件（release 一旦创建即不可变，先建后补会 422，删了重建会永久烧掉 tag 名），还是可以接受「GitHub 从 0.3.0 起算」？
11. **网盘换包怎么验收**（0.2.0 已确认换过，链接不变这条已解决）：分享页 URL 不随版本变，外部只能从包体判断。0.3.x 发布时是否把「文件名 + 体积 + 上传时间」写进发布记录当对账凭据？还是接受「换没换只能靠自觉」？
12. **旧版用户升级路径**：0.1/0.2 用户升到 0.3 时，`config.json` / `profiles.json` 承诺全兼容，还是需要迁移提示？

---

## 附录 A：现场证据（2026-11-02）

### A.1 安装与注册表

```
HKCU Uninstall:
  WorkBuddy 5.9.3     DisplayIcon D:\apps\tencentapp\WorkBuddy\WorkBuddy.exe,0
  WorkBuddy AI 5.9.4  DisplayIcon D:\apps\AIAPPS\WorkBuddyAI\WorkBuddyAI.exe,0
```

### A.2 插件缓存

```
C:\Users\PC\.workbuddy\plugins\cache\dev-agentrmc-local\agentrmc-bridge\0.3.2
C:\Users\PC\.workbuddy-ai\plugins\cache\dev-agentrmc-local\agentrmc-bridge\0.3.1
```

### A.3 进程

```
WorkBuddyAI.exe  D:\apps\AIAPPS\WorkBuddyAI\WorkBuddyAI.exe
  ├─ 主进程
  ├─ gpu-process / utility / renderer / crashpad
  ├─ daemon-app-server-entry.js --stdio
  ├─ sidecar-entry.js
  └─ cli/bin/codebuddy --serve ... --settings ... --mcp-config ...
```

检查时主窗口句柄为 0（无可交互窗口），但进程与 Agent 会话均在运行。

### A.4 版本与发布读数（2026-11-02，实施前快照）

> 实施后的读数见 §2.7：exe FileVersion / ProductVersion、NSIS 显示版本均已为 `0.3.0`，插件清单为 `0.3.3`。以下保留实施前对照。

```
native/CMakeLists.txt        : project(AgentRMCNative VERSION 0.3.0)          # 工作区未提交
native/tools/installer.nsi   : 已改为 include build/generated/version.nsi    # 版本由 CMake 生成
native/resources/app.rc.in   : FILEVERSION/PRODUCTVERSION 取 @PROJECT_VERSION_*@
native/resources/app.rc      : 已删除（旧文件写死 0,2,0,0）

D:\apps\AgentRMC\AgentRMCNative.exe        FileVersion 0.2.0  11/02 10:52
native\build\Release\AgentRMCNative.exe    FileVersion 0.2.0  11/02 10:52
dist\AgentRMC-Setup.exe                    20,017,825 B       11/02 10:52
dist\agentrmc-bridge-0.3.1.zip             18,953 B           11/02 09:48      # 陈旧
plugins\agentrmc-bridge.zip（随包）        内含三清单 version 0.3.2            # 由 build.bat 现从工作区打

HEAD 清单 version = 0.3.1，工作区 = 0.3.2（三个 plugin.json + 三个 marketplace.json）
git log -1 = 137e3ca（插件 0.3.1 时代）；AgentWindow.cpp/.h 与 docs/PRD-agent-integration.md = untracked

gh release list --repo Samuel5945/AgentRMC  →  AgentRMC v0.1.0   Latest   2026-10-31   （仅此一条）
curl --noproxy 127.0.0.1 http://127.0.0.1:45915/state
  → {"chat_mode":true,"docked":false,"listening":true,"pet_enabled":true,
     "pet_visible":true,"recording":false,"transcribing":false,"tts_ready":true}
```

### A.5 代码位置（版本与判据精确行号）

| 内容 | 文件:行 |
|---|---|
| 版本宏注入 | `native/CMakeLists.txt:3,98` → `native/src/main.cpp:30` |
| PE 版本资源 | `native/resources/app.rc.in`（`@PROJECT_VERSION_*@`，CMake 生成 `build/generated/app.rc`） |
| 兜底宿主顺序 | `native/src/core/AgentWindow.cpp:55-64`（`knownExes()`） |
| 四级目标选择 | `native/src/ui/VoicePet.cpp:592-615`（`onlineAgentExe`） |
| 任务判定正则 | `native/src/ui/VoicePet.cpp:579-590`（关键词 + 长度 ≥ 40） |
| 右键目标菜单 | `native/src/ui/VoicePet.cpp:980-1005` |
| WorkBuddy 启动别名 | `native/src/core/AppLauncher.cpp:431-434`（→ `WorkBuddy.WorkBuddy`） |
| 把 bug 当期望的断言 | `plugins/agentrmc-bridge/test/host.test.js:68-76` |
| 随包插件 zip 生成 | `native/tools/build.bat:45-56` |

### A.6 代码位置（联动链路）

| 内容 | 文件 |
|---|---|
| 宿主判定 | `plugins/agentrmc-bridge/lib/host.js` |
| 通知脚本 | `plugins/agentrmc-bridge/hooks/agentrmc-notify.js` |
| WorkBuddy hooks | `plugins/agentrmc-bridge/hooks-workbuddy/hooks.json` |
| 宿主窗口探测 | `native/src/core/AgentWindow.cpp` |
| 通知处理 | `native/src/MainWindow.cpp`（`AgentBridge::notify` 连接） |
| 气泡与语音分流 | `native/src/ui/VoicePet.cpp` |
| 本机通知桥 | `native/src/core/AgentBridge.cpp` |
| 语音启动应用 | `native/src/core/AppLauncher.cpp` |
| 场景匹配 | `profiles.json`（`agent` profile） |

---

## 附录 B：术语

| 术语 | 含义 |
|---|---|
| 宿主 | 发通知 / 接收语音的 Agent 桌面应用，如 WorkBuddy AI、Qoder、ZCode |
| raise | 把宿主主窗口还原并前置的动作 |
| 派活 | 把用户口述的任务文本粘贴进宿主输入框并回车 |
| 双 WorkBuddy | 同一台机器同时安装 `WorkBuddy` 与 `WorkBuddy AI` |
| transcript | 宿主保存的会话记录 JSONL，stop 时用于提取最终回复 |
