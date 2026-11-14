# 移动端适配：局域网 Web 优先（方案与分期）

> 状态：**M1 已实现**（2026-11-13，分支 `mobile-lan-web`，主力机安卓，用户拍板
> 跳过 M0 直接进 M1）。定位延续 `mission-control-plan.md` 的裁决：桌宠是
> **注意力 Router**，不是第二个 Agent 运行时——手机同理，是「看一眼谁在等我 →
> 结账 / 派一句话回去」的遥控器，不是第二个宿主。代码位置按仓库相对路径 +
> 行号标注。
>
> M1 落地件：`native/src/core/LanWebServer.{h,cpp}`（服务+配对+手机页）、
> `MainWindow` 集成（`lanShellState/lanState/setLanEnabled` + 系统页动作）、
> 外壳系统页「手机访问」卡片（二维码，`resources/qrcode.js` MIT）、Qt 回退页
> `showLanDialog()`、`native/tests/lanweb_test.cpp`。

---

## 1. 一句话结论

**先做局域网 Web，是对的，而且这个仓库的地基已经为它铺好了**：主窗外壳早已
是 HTML/CSS/JS（WebView2），数据契约 `webShellState()` 与动作契约
`handleWebAction()` 现成——手机页只是**同一份契约的第二个渲染器**，不另起
状态机。缺的只有三样：一个带鉴权的 LAN HTTP 口、一份竖屏页面、一个配对入口。

## 2. 为什么先做局域网 Web（路线对比）

| 路线 | 成本 | 结论 |
|---|---|---|
| **局域网 Web（本方案）** | 复用外壳 HTML + 既有 HTTP 栈 + 既有动作分发；新增一个 server 类 + 一份移动页 | **先做**。MVP 以天计，无商店/签名/双平台维护 |
| 原生 App（iOS/Android） | 两套工程、签名与商店审核、更新链路、与 native 的协议仍要重做一遍 | 不做 v1。Web 版验证完交互再评估是否值得包壳 |
| 公网中继 / 内网穿透 | 要服务器或第三方隧道、把控制面暴露公网、运维与审计成本 | 留到 M3 决策（推荐 Tailscale 类组网而非自建中继） |
| 直接暴露现有 45915 桥 | 桥是**无鉴权本机 IPC**（hooks/插件信任区），暴露即裸奔；给桥加鉴权会破坏所有已装插件 | **否决**。双端口双信任区，见 §4.1 |

产品面也成立：家里的核心场景是「人离开电脑，Agent 还在跑」。PC 端桌宠 TTS
已经负责「喊你」；手机负责「被喊到之后看一眼、结个账、派一句话」——这正好
是待办库 + 定向派发已经做完的事，四个宿主通道（WorkBuddy UIA / ZCode
app-server / DSH followup / OpenSquilla RPC）全部现成。

## 3. 现有地基（盘点）

- **HTTP 栈**：`native/src/core/AgentBridge.cpp` 手写 HTTP/1.1（QTcpServer，
  `Connection: close`，16KB 头 / 64KB 体上限，10s 半截请求超时）。
  **只监听 127.0.0.1:45915**（`AgentBridge.cpp:39`）。路由：
  - GET `/state` `/work-items` `/workers` `/health` `/dispatch/pending`
  - POST `/notify` `/speak` `/work-items/ack` `/work-items/update` `/command`
    `/dispatch/ack`
  - 无鉴权、无静态文件服务、无 CORS——它的信任模型就是「本机进程」。
- **整份状态契约**：`MainWindow::webShellState()`（`MainWindow.cpp:3003`）
  一次给出 `{work, workers, health, theme, events, system, keymap, apps,
  guide, header, left, dispatch}`。手机页要的是其中**子集**。
- **动作契约**：`MainWindow::handleWebAction()`（`MainWindow.cpp:3140`）
  收 `{cmd, id, …}` 共约 49 个命令，全部转给既有实现，做完统一
  `workWeb_->refresh()`（`MainWindow.cpp:3398`）——**手机动作走同一入口，
  桌面外壳自动跟着刷新**，双端一致性免费。
- **派发**：`dispatch` 命令（`MainWindow.cpp:3355`）已带全套守卫（裁决 6：
  证明不了目标会话就不发、草稿保留、ZCode 通知态「重启 ZCode」语义）。
  手机派发**原样复用**，不新写一条派发路。
- **外壳 HTML**：`WorkWebView::htmlFor()`（`WorkWebView.cpp:537`）内嵌
  HTML/CSS/JS，主题 token 由 `webThemeJson()` 注入；美术主题背景走
  `https://agentrmc.theme/` 虚拟域名映射（WebView2 专属，LAN 侧要换成真实
  HTTP 路由，见 §4.3）。
- **测试基线**：`native/tests/bridge_test.cpp` / `workitem_test.cpp` /
  `dispatch_test.cpp` 等；新增 LAN 口照 `bridge_test` 的样子加一份。

## 4. 目标架构

### 4.1 双端口、双信任区（核心裁决）

- **45915 保持现状**：loopback-only、无鉴权，hooks / MCP / DSH 插件 /
  OpenSquilla 桥继续用，**零改动、零破坏**。
- 新增 `LanWebServer`（`native/src/core/LanWebServer.{h,cpp}`，与 AgentBridge
  同款手写 HTTP 风格，Qt 主线程），监听 **0.0.0.0:45916**（端口可配），
  **默认关闭**，系统页开关「手机访问」显式打开。
- 两个 server 不共享路由表：LAN 口只认 `/m/*` 前缀，其余一律 404。
  桥的 agent 侧路由（`/notify` `/command` `/dispatch/*` …）**永不**出现在
  LAN 口上。

### 4.2 配对与鉴权

- **配对码**：系统页「手机访问」卡片里按需生成（按钮「显示配对二维码」），
  **10 分钟有效**，窗口内可配对多台设备，过期自动作废。
  二维码在桌面外壳页内用一段内嵌 JS QR 编码画（系统页本来就是 HTML，
  不引 C++ 依赖），内容 `http://<LAN-IP>:45916/m/#pair=<码>`——
  放 fragment，不进服务端日志与 Referer。
  - **中文助记词（2026-11-14 用户拍板，取代 32 位 hex）**：12 个常用双字词
    （如「中国 国家 发展 工作…」），词表 2048 词（2^11）由
    `rc_probe/gen_pair_wordlist.py` 从 jieba 词典按词频取前 2048、滤纯虚词，
    入库 `native/resources/pair_words_zh.txt`（qrc）。12 词≈132-bit 熵≥128-bit。
    词表读不到回退 hex（配对绝不因词表坏掉）。服务端比对**去全部空白+小写**
    归一化（手机连着打「中国国家…」也行）；二维码 fragment 用
    `encodeURIComponent` 编成纯 ASCII（QR 不必处理 UTF-8），手机页
    `decodeURIComponent` 还原。`lanweb_test` 增 12 词/去空格归一化断言。
  - **推荐地址（2026-11-14）**：多网卡候选都列出，192.168（家用 WiFi）段标绿
    「推荐」，手机一眼知道用哪个；二维码仍取首个（192.168 优先）。
- **换设备令牌**：手机页 JS 拿 fragment 里的码 POST `/m/pair` → 服务端生成
  该设备的随机令牌，存**哈希**（`store_` 配置，含设备名/配对时间/最后可见），
  回 `Set-Cookie: armc_dev=<令牌>; HttpOnly; SameSite=Strict; Path=/m`，
  页面随即把 fragment 清掉。此后所有 `/m/*` 认 cookie 或
  `X-AgentRMC-Token` 头（常量时间比较）。
- **防爆破**：按 IP 计配对失败，5 次锁 15 分钟；令牌错误直接 401，不提示
  「差一点」。令牌与配对码不进日志、不进事件流。
- **设备管理**：系统页列出已配对设备（名字/配对时间/最后可见），可吊销
  （删哈希即失效）。v1 就做，因为这是暴露到 LAN 的控制面，吊销是底线。
- **CSRF**：POST 一律要求自定义头（`X-AgentRMC-Token` 或 `X-Requested-With`），
  不开 CORS——跨站表单带不上自定义头，预检也不放行。
- **防火墙**：首次开启监听时 Windows 会弹入站授权（或文档给 `netsh
  advfirewall` 命令让用户手动放行）；默认关闭意味着不装机即弹窗。

### 4.3 路由与动作白名单

| 路由 | 说明 |
|---|---|
| `GET /m/` | 移动页 HTML（内嵌 C++ 字符串，沿用 `htmlFor` 的模式，不新增松散文件与打包项） |
| `POST /m/pair` | 配对码换设备令牌（唯一免令牌路由，受防爆破约束） |
| `GET /m/state` | `webShellState()` 的**裁剪版**：`work / health / events / theme / dispatch / header(精简)`；不下发 keymap/apps/guide/system 重交互页 |
| `POST /m/action` | `{cmd,id,text}` → **白名单过滤后**转 `handleWebAction()` |
| `GET /m/theme/<rel>` | 主题素材（背景图）。复用 `webThemeJson()` 同款相对路径校验（`MainWindow.cpp:2960` 附近：必须在 themesRoot 内、不许 `../`），越界 404 |
| `GET /m/stream`（M2） | SSE 推送，替代轮询 |

**动作白名单（v1，已实现）**：`ack`（标为已处理）、`dispatch`（派回会话，带
text）、`open`（送到 Agent——在 PC 上拉起宿主，遥控器语义成立）、`raiseZcode` /
`restartZcode`（ZCode 通知态配套）、`readLast`（让 PC 桌宠念最近转写）。
**白名单制而非黑名单**：`sysUpgrade / appAdd / keyEdit / winClose / vSettings`
这类会在 PC 上弹 Qt 对话框或改本机配置的命令一律不进——手机上点了也没意义。
`tts` 原在候选里，实现时剔除：它会弹 TtsSettingsDialog（PC 对话框，手机够不着）。
`clear`（清已结案）v1 先不放，防误触，M2 再议。手机也不能操作 LAN 开关本身
（`lanToggle/lanPair/lanRevoke` 是桌面专用命令，不在白名单）。

### 4.4 手机页面（v1）

- 单列竖屏，三个底部页签：**待我处理 / 事件流 / 状态**。
  - 待我处理：与桌面**同序**（`displayOrder()`，等待起点优先），卡片 =
    宿主名 + 项目 + 正文 + 「等 N 分」+ 动作按钮（结账 / 派回会话 / 送到
    Agent）；派回会话展开文本框，被守卫拒绝时**草稿留在输入框**并显示原因
    （与桌面同口径）；`dispatch.supported` 之外的宿主隐藏派发行。
  - 事件流：只读时间线。
  - 状态：健康行、版本、桥接状态、已配对设备数；**不做**重配置。
- 主题：复用 `theme` 字段里的 token（classic 明暗 + 美术主题背景走
  `/m/theme/`），验收照 UI 约定跑 classic×light / classic×dark / 美术主题
  三档截图。
- 触控目标 ≥44px；`viewport` 禁缩放抖动；滚动只发生在列表内部，
  **隐藏滚动条**（CSS `scrollbar-width:none` + `::-webkit-scrollbar{display:none}`，
  对齐「所有地方都不要明显滚动条」的约定）。
- 轮询：页面可见时 2.5s 拉 `/m/state`，`document.hidden` 即停（省电），
  回前台立即拉一次。M2 换 SSE 后保留轮询当兜底。

### 4.5 推送与轮询（诚实的边界）

**LAN 的 `http://` 不是安全上下文**：Service Worker / Web Push /
`getUserMedia` 在手机浏览器上全部不可用。所以——

- v1：**没有后台推送**，页面开着才有实时性；「喊你」继续靠 PC 桌宠 TTS
  （在家场景够用）。页面内新待办用标题闪烁/角标提示。
- 想要「手机锁屏也能收到通知」和「手机语音输入」，必须 HTTPS（M3 决策：
  本地证书分发 vs Tailscale 类组网自带 HTTPS）。**不要**在 v1 里用
  自签证书硬上——手机装证书的劝退成本比收益高。

## 5. 安全清单（LAN ≠ 可信）

1. 默认关闭；开启是系统页显式动作，状态持久化。
2. 除 `/m/pair` 外全路由要令牌；配对码短时效；令牌存哈希；常量时间比较。
3. 按 IP 防爆破；错误响应不泄露「令牌存在与否」。
4. 动作白名单；agent 侧 IPC 路由永不出现在 LAN 口。
5. 主题素材路径越界校验；请求体 64KB 上限、半截请求 10s 超时（沿用桥参数）。
6. 不开 CORS；POST 要自定义头；cookie `HttpOnly + SameSite=Strict`。
7. 令牌/配对码不落日志、不进事件流； pairing URL 用 fragment。
8. 45915 的 loopback 绑定**加测试锁死**，防未来重构顺手改成 Any。

## 6. 分期

### M0 · 探针（用户裁决跳过，2026-11-13 直接进 M1）
原计划 `rc_probe/lan_web_probe.py` 真机验证可达性；用户拍板主力机安卓、直接
M1。AP 隔离/防火墙排查并入 M1 真机验收（手机页配对失败文案已带这两条提示）。

### M1 · MVP（已实现，2026-11-13 真机验收通过）
- `LanWebServer` + 配对/令牌/设备吊销 + §4.3 路由 + 白名单动作。
- 系统页「手机访问」卡片：开关、LAN URL、二维码（qrcode-generator MIT，
  `resources/qrcode.js` 走 qrc）、设备列表；Qt 回退页 `showLanDialog()` 同款
  （纯文本配对码）。
- 移动页 v1（§4.4）。**用户真机反馈迭代（2026-11-13）**：「派回会话」不做
  独立展开按钮——点按钮后**原位变成输入行**（自动聚焦弹键盘、Enter/发送
  投递、✕ 收起），守卫拒绝时草稿留在输入框。
- 测试：`native/tests/lanweb_test.cpp` **62 项全绿**（401/防爆破/越界路径/
  白名单外命令拒绝/45915 仍 loopback/开关关闭即端口关闭/超大 body 排干回
  413）；`bridge_test` / `workitem_test` / `dispatch_test` 等全套绿。
- 真机链路探针 `rc_probe/lan_web_e2e.py` **19 项全绿**：UIA 走外壳系统页
  真实入口（开开关→出二维码→读码）→ HTTP 走 LAN 地址配对/鉴权/白名单/
  CSRF/越界/桥回环拒连 → UIA 按 Y 坐标只吊销探针设备 → 令牌失效。
  用户安卓真机扫码配对成功并实际使用（待办卡/结账/派发可见）。
- 交付照 AGENTS.md：`package.bat` 出包 → 静默装机 → 启动 →
  `curl --noproxy '*' http://127.0.0.1:45915/state` 验活。**不升版**（0.4.1），
  等用户明确说「升版」。

### M1.5 · 聊天页（2026-11-13 用户追加，电脑待办页 + 手机待办页）
- **交互口径（用户定）**：点待办进聊天页，**打开直接显示最新记录**（不做
  「滚到底」动画）；旧记录**往上翻时按页加载**，保持视觉锚点不跳。
- 读取器 `SessionTranscript`（只读、分页、进程内 mtime+size 缓存）：
  WorkBuddy/Qoder 系 jsonl（两记法、剥 system-reminder/user_query、保留换行、
  sidechain 过滤）、ZCode message+part 两表、OpenSquilla transcript_entries、
  **DSH session.v4.jsonl.zstd**（vendored libzstd 跨帧解压，见下）。单页 ≤100、
  单条 8000 字、SQLite 全量缓存 ≤500。
- 电脑外壳：待办卡单击 → `chatOpen` → 聊天视图（气泡：我右/Agent 左、时间戳、
  上下渐隐暗示可滚、新消息 pill 不打断读历史、底部 composer 派回会话）；
  上翻 → `chatOlder` → `pushChat` 单独推旧页；双击仍是回到 Agent。
- 手机页：点卡片进聊天视图（隐藏页签），`GET /m/transcript?id=&offset=&limit=`
  分页拉取，2.5s 轮询新消息，composer 同款；卡片按钮阻止冒泡。
- 测试：`transcript_test`（两记法/分页游标/包装/SQLite/缓存失效/不支持宿主）
  + `lanweb_test` 增 transcript 路由断言（401/200/参数透传/404）。
- **真机反馈迭代（2026-11-13 晚，用户手机实测后）**：
  ① Agent 回复**去气泡改平铺全文**（长代码/长文不再被 82% 宽气泡挤），
  「我」的话保留右侧气泡；② 手机聊天输入行加 `env(safe-area-inset-bottom)`
  安全区 + 左右留白，发送按钮不再被手机圆角/手势条裁切；③ 手机待办卡
  **去掉单独的「派回会话」按钮**（第三按钮独占一行丑）——派发统一进聊天页
  输入框，卡片只留「回到 Agent / 标为已处理」两个均分按钮。
- **修掉的两个真 bug**：① 桌面首帧定位在元素解除隐藏前执行（scrollHeight=0
  落空）→ 先 render 再喂数据 + 双 rAF 兜底；② 内容没填满高窗口时无滚动事件、
  旧页永不加载 → `maybeAutoLoad` 自动续载直到填满；③ hasMore 改本地
  「已加载<总数」计算，轮询不再把加载完的列表标成还有旧页。
- **DSH 会话记录接入（2026-11-14，用户裁决 native 内置 libzstd，不走插件通道）**：
  原 M2 设想是插件通道，但那样要 DSH 开着、离线看不了，与其余四宿主「只读/缓存/
  离线可看」口径不一致。改为 **vendored libzstd**（`native/vendor/zstd`，BSD，
  源码入库；只编 common+decompress 解压子集，CMake 静态库 `zstd_decompress`，
  关压缩/字典/legacy/ASM）。`SessionTranscript::readDsh` 读
  `~/.dsh/sessions/<slug>/session-<uuid>/session.v4.jsonl.zstd`：
  `ZSTD_decompressStream` 循环喂入即跨帧（多帧拼接，等价探针 read_across_frames）；
  事件流里只取 `user/message`（`data.content[].text`）与 `assistant/message`
  （`data.message.content[].text`，`contentText` 天然滤掉 reasoning/tool-call，
  纯工具消息不出气泡）；标题取日志最后一条 `session/title.data.title`，无则退
  首条用户正文。解压上限 256MB 防病态文件。测试 fixture 由
  `rc_probe/gen_dsh_transcript_fixture.py` 预生成多帧 zstd 入库（native 无压缩
  子集），`transcript_test` 增 DSH 用例（多帧/reasoning 过滤/标题/分页/追加帧缓存
  失效/缺文件）。**离线可看**：DSH 关着也能翻历史。
- **待办卡会话标题补全（2026-11-14，自 bettermissioncontrol 并入）**：
  `DshSessions` 只读宿主投影缓存 `~/.dsh/storages/session_projcache/sessions/<id>.json`
  （plain JSON，AI 标题/首条正文/cwd，mtime+size 缓存）。`workItemsJson()` 给
  `/work-items`、桌面外壳、手机页三处统一补 `session_title`、空项目回填 cwd；
  行身份同口径升级（项目目录名 > 会话标题 > 短 id，同宿主同项目重名优先补标题
  截 12 字）。Qt 列表/桌面 Web/手机页三端一致。`dsh_sessions_test` 22 项断言。

### M2 · 体验
SSE 推送（轮询兜底）；设备管理细化；美术主题观感复检；`clear`、更多只读
控制；移动页与桌面外壳抽共享 CSS token（消两份拷贝）。

### M3 · 出户（需用户拍板，不默认做）
- 外网访问：推荐 Tailscale 类组网（零自建服务器、自带加密与设备准入），
  否决自建公网中继（运维 + 攻击面）。
- HTTPS 安全上下文：解锁 Web Push 后台通知与手机语音输入（MediaRecorder →
  既有 ASR 管线）。
- 原生 App / PWA 壳：只有当「商店存在感」成为需求时再评估。

## 7. 验收判据

- 真机配对后：待办与桌面**同卡同序**；手机结账 → 桌面卡同步结案；手机派发
  → 既有回执链路上卡（四通道各验一条），守卫拒绝时草稿保留、原因可见。
- 另一台机器访问 45915 → 连接拒绝；45916 无令牌 → 全路由 401；
  `/m/theme/../..` 越界 → 404；白名单外命令 → 403/400 且**不执行**。
- 开关关闭 → 45916 立即停止监听；重启后开关状态保持。
- 配对码过期后不可换令牌；吊销设备后其 cookie 立即失效。
- `lanweb_test` + 既有测试全绿；classic 明暗 / 美术主题移动页截图留证
  （`rc_probe/`）。
- 出包、静默装机、验活按 AGENTS.md 闭环；版本号不动。

## 8. 已知限制与坑（提前写死）

1. **HTTP 非安全上下文**：v1 无后台推送、无手机麦克风（§4.5），别在验收时
   当 bug 报。
2. **手机派发 = PC 派发**：ZCode 外部落库消息要重启宿主才可见（上游限制，
   见 mission-control-plan §3），手机上同样给「重启 ZCode」按钮，不承诺
   「派完手机上就能看到对话」。
3. **UIA 派发依赖前台焦点且全局串行**：手机连派两条会排队，不是丢——
   页面要显示「排队中」而不是假成功。
4. **多网卡机器**：LAN URL 要选对网卡（优先私有网段 IPv4），系统页把候选
   IP 都列出来让用户挑，别只猜一个。
5. **AP 隔离**：部分路由器默认开 AP 隔离，手机访问不到 PC——M0 探针第一件事
   就是排除它，失败时给出「检查路由器 AP 隔离 / 防火墙」提示文案。
6. **手写 HTTP 的边界**：沿用桥的「只认请求行前缀」解析即可，但 LAN 口面对
   的是浏览器（会发 favicon、预检、keep-alive 试探），未知路由一律干净 404 +
   `Connection: close`，别挂住。
7. **别把移动页做成第二套状态机**：所有动作走 `handleWebAction()`，所有数据
   来自 `webShellState()` 裁剪——这是外壳立项时的裁决，移动端同样适用。
8. **超大 body 先排干再回 413**（实测）：收缓冲里还压着没读的数据就关连接，
   Windows 发 RST 而不是 FIN，客户端连已收到的响应都丢。`drain_` 排干上限
   1MB，声称更大的直接断开。
9. **UIA 探针两个坑**（lan_web_e2e.py 实测）：① comtypes 的
   `GetCurrentPatternAs` 返回裸指针 int，要用 `GetCurrentPattern(10000)` +
   `QueryInterface(IUIAutomationInvokePattern)`；② Chromium 把行 div 拍扁，
   按祖先找「吊销」按钮会抓到别行（误吊销过用户设备）——按名字与按钮的
   **Y 坐标同行匹配**；吊销后令牌失效有一条异步链，验证要轮询别抢跑。
10. **DSH 沙箱按被启动程序位置限制子进程**：工作区内 exe 写用户 temp 被拒
    （测试 QTemporaryDir 全挂、NSIS 初始化即退）。对策已写进 AGENTS.md：
    测试 TEMP 指工作区内；安装器拷到工作区外再 /S。

## 9. 待用户拍板

1. ~~M1 范围确认~~ → 用户拍板直接 M1，已交付验收。
2. ~~端口 45916、默认关闭~~ → 已按此实现，用户真机在用。
3. ~~主力手机~~ → **安卓**（2026-11-13 用户确认，真机已配对使用）。
4. M3 出户需求是否真实存在？（决定要不要为 HTTPS 提前留证书接口）
5. 移动页后续体验项（M2）：SSE 推送、`clear`、共享 CSS token——按需排期。