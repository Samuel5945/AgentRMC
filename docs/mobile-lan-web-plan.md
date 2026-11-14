# 移动端适配：局域网 Web 优先（方案与分期）

> 状态：**方案稿**（2026-11-13），未动代码。定位延续
> `mission-control-plan.md` 的裁决：桌宠是**注意力 Router**，不是第二个 Agent
> 运行时——手机同理，是「看一眼谁在等我 → 结账 / 派一句话回去」的遥控器，
> 不是第二个宿主。代码位置按仓库相对路径 + 行号标注。

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
  128-bit 随机，**10 分钟有效**，窗口内可配对多台设备，过期自动作废。
  二维码在桌面外壳页内用一段内嵌 JS QR 编码画（系统页本来就是 HTML，
  不引 C++ 依赖），内容 `http://<LAN-IP>:45916/m/#pair=<码>`——
  放 fragment，不进服务端日志与 Referer。
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

**动作白名单（v1）**：`ack`（标为已处理）、`dispatch`（派回会话，带 text）、
`open`（送到 Agent——在 PC 上拉起宿主，遥控器语义成立）、`raiseZcode` /
`restartZcode`（ZCode 通知态配套）、`tts`、`readLast`。
**白名单制而非黑名单**：`sysUpgrade / appAdd / keyEdit / winClose / vSettings`
这类会在 PC 上弹 Qt 对话框或改本机配置的命令一律不进——手机上点了也没意义。
`clear`（清已结案）v1 先不放，防误触，M2 再议。

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

### M0 · 探针（不改生产代码）
`rc_probe/lan_web_probe.py`：起一个带简易令牌的静态服务，把真实
`GET /state`（只读）拼成一份极简移动列表页；用户真机连家里 Wi-Fi 扫码打开，
验证可达性、轮询延迟、竖屏排版。**产出：真机截图 + 结论**，再进 M1。

### M1 · MVP（生产代码，本方案主体）
- `LanWebServer` + 配对/令牌/设备吊销 + §4.3 路由 + 白名单动作。
- 系统页「手机访问」卡片：开关、LAN URL、二维码、设备列表。
- 移动页 v1（§4.4）。
- 测试：新增 `native/tests/lanweb_test.cpp`（401/防爆破/越界路径/白名单外
  命令拒绝/45915 仍 loopback/开关关闭即端口关闭）；既有 `bridge_test` /
  `workitem_test` / `dispatch_test` 全绿。
- 交付照 AGENTS.md：`package.bat` 出包 → 静默装机 → 启动 →
  `curl --noproxy '*' http://127.0.0.1:45915/state` 验活。**不升版**，
  等用户明确说「升版」。

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

## 9. 待用户拍板

1. M1 范围确认：**结账 + 派发 + 送到 Agent** 三个动作够不够起步？（建议够）
2. 端口 45916、默认关闭——认可？
3. 主力手机是 Android 还是 iPhone？（影响 M0 探针与 M3 的 HTTPS 路线优先级）
4. M3 出户需求是否真实存在？（决定要不要为 HTTPS 提前留证书接口）