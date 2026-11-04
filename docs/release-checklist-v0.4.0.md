# AgentRMC v0.4.0 上线准备清单（2026-11-04）

本轮做到**本地准备为止**：版本号、构建、测试、发布物料、官网文案草稿全部就位；
**没有**出安装包、**没有**装机、**没有** push / tag / release、**没有**发布站点。

## 已完成并实测

| 项 | 读数 |
|---|---|
| 版本号 | `plugins/agentrmc-bridge/VERSION` = `native/CMakeLists.txt` = `0.4.0`；`sync-plugin-version.ps1` 同步 6 处清单（3×plugin.json + DSH package.json + 2×marketplace.json），`-Check` 报 `PLUGIN VERSION OK` |
| 版本一致性 | `check-version.ps1`：`cmake=0.4.0 nsi=0.4.0 exe.FileVersion=0.4.0 exe.ProductVersion=0.4.0` → `VERSION OK`（构建产物 `native/build/Release/AgentRMCNative.exe` 实测 FileVersion 0.4.0） |
| 构建 | `native/tools/build.bat`（**带 sherpa-onnx**，本机交付不带 lean）exit 0 |
| 原生测试 | ThemeTest（含新增 6 条配色回归）/ App / Store / WorkItem / HostNewTask / Plugin(37/37) / Router / Takeover / ChatText 全 PASS |
| 插件测试 | `agentrmc-bridge/test/host.test.js` 35/35；`deepseek-harness/test/plugin.test.js` 20/20（含新增「spool 上限守卫」用例） |
| 插件包 | `dist/agentrmc-bridge-0.4.0.zip`、`dist/agentrmc-bridge-qoder-0.4.0.zip`（各 33 KB，由 build.bat 产出） |
| 发布物料 | `dist/release-notes-v0.4.0.md`（中英双份） |
| 命名 | `Remote Mission Control（远程任务中控台）`已写进 README 开头与英文段、官网 hero lede（中英）+ meta description |
| 站点草稿 | `lig-site/agentrmc-update.json` → `0.4.0 / 2026-11-04`（notes/notesEn 各 10 条）；`agentrmc.html` 版本徽标兜底 0.3.9 → 0.4.0，`assets/i18n.js` 中英 lede 更新 |

## 未跑 / 待办（都需要你点头才能往下走）

1. **`AgentRMCBridgeTest` 没跑通，但不是回归**：它自己要在 `:45915` 起桥，而本机
   `D:\apps\AgentRMC\AgentRMCNative.exe`（PID 9352，09:53 启动的 **0.3.9**）占着这个端口，
   `bridge.start()` 直接失败。要么你同意我关一下 App 单跑这个测试，要么留到装机那轮补跑。
2. **UI 验收矩阵跑不了**：`rc_probe/ux_matrix.py` 打的是 `D:\apps\AgentRMC` 那个实例
   （会 `taskkill` + 覆写它的 `config.json`），而那个二进制是今早的 0.3.9 构建，
   **不含**本轮 `setScheme` 修复。所以配色偷换这处改用主题单测的 6 条回归用例做等价验收
   （已 PASS）。classic×light / classic×dark / 鲸鱼娘×嵌入+独立 的截图矩阵要等**出包+装机后**再跑。
3. **鲸鱼娘主背景体积（已拍：不压）**：`themes/xiaoai-whale/backgrounds/main.png`
   3.3 MB → **7.4 MB**，`preview.png` 0.56 MB → 0.61 MB；按原样进包，估算安装包
   38.7 MB → **42 MB 上下**（出包后实测为准）。同一天定的新规矩：安装包**只带**
   经典 + 鲸鱼娘两个主题，以后新主题一律出导入包，不再打进安装包
   （见 `AGENTS.md`「主题分发」）——所以官网下载区那句`安装包（约 20MB…）`这轮必须改口径。
4. **官网三处随版本变化的文案没改**（都属于素材而非文案，需要新素材才能改口径）：
   - 界面区 `截图都是真机当前版本 0.3.9` → 0.4.0 语音主界面重排后截图已不对版，需重截
     （语音主界面新布局 + 波形语音键 + 顶栏监听开关 + 派活模式下拉 + classic 浅色）。
   - 视频区 `两分钟看完 AgentRMC 0.3.9` / 封面 `agentrmc-video-cover-039.jpg` → 是否重录由你定。
   - 下载区 `安装包（约 20MB…）` → 实测体积出来后统一改。
5. **缓存号**：本轮只有 `agentrmc.html` 的 `assets/i18n.js?v=97` 升到 `?v=98`（它渲染了改过的
   文案）。其余 7 个源页面仍 `?v=97`，建议发布时一并升到 98 保持单一数字。
   `.stage/` 与 `dist/` 是发布产物，没动。
6. **站点改动未提交**：`agentrmc-update.json` / `agentrmc.html` / `assets/i18n.js` 三个文件
   现在是 lig-site 工作区的未提交改动，等你看过再决定提交与发布。

## 发布动作顺序（等你说「出包」/「发布」再走）

`package.bat` 出包 → 核对体积与 `check-version.ps1` → `install-silent.bat` 静默装 →
`curl --noproxy '*' http://127.0.0.1:45915/state` 验活 → 跑 `ux_matrix.py` + `ux_shots.py` 矩阵 →
补跑 `AgentRMCBridgeTest` → `feat/handoff-focus-and-classic-theme` 合 master（本地领先 21 提交）+ push →
`v0.4.0` tag + GitHub release（挂 `AgentRMC-Setup.exe` 与两个插件 zip）→
站点发布（更新 JSON + 页面 + 缓存号）→ 网盘换包（夸克 s/47bf9d92c5f5 / 百度 pwd 35c8，URL 不变，
这一步只能你手动传）。

注意：升版后**别删** `~/.qoder(-cn)/plugins/cache/local/agentrmc-bridge/0.3.9`，
宿主已开着的会话还按那条路径调 hook，删了提醒会静默消失。
