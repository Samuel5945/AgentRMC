# 项目约定

## 交付方式：改动后必须打包到 dist

- native 端（Qt/C++）代码改完、测试通过后，**必须出包**：运行
  `native\tools\package.bat`（= build.bat 构建 + windeployqt + NSIS），
  产出 `dist\AgentRMC-Setup.exe`，把这份安装包作为交付物告诉用户。
- 打包成功后**直接静默装到本机**（用户约定）：用
  `native\tools\install-silent.bat`（内部就是 `dist\AgentRMC-Setup.exe /S`）。
  **不要在 Git Bash 里直接写 `./dist/AgentRMC-Setup.exe /S`**——MSYS 可能把
  `/S` 当路径参数转换，安装器会弹出向导让人手点「下一步」。命令行临时用时走
  `cmd //c "dist\AgentRMC-Setup.exe /S"`。安装器按注册表
  `HKCU\Software\AgentRMC\InstallDir` 原地升级（本机装在
  `D:\apps\AgentRMC`，不是默认的 %LOCALAPPDATA%）。注意 Git Bash 里
  `reg query ... /v X` 的 `/v` 也会被 MSYS 吞掉，要查就查整个键。装完手动启动
  安装目录里的 AgentRMCNative.exe——静默模式会跳过完成页的「立即启动」，
  应用不会自己起来。安装器自己会 taskkill 旧实例，不用先关应用；装完用
  `curl --noproxy '*' http://127.0.0.1:45915/state` 验证新实例活着。
- **启动应用绝对不要用 `cmd //c start ...`**：MSYS 会把 `start "" "D:\..."` 里的
  空标题和路径揉成 `\\`，Windows 当成 UNC 路径，直接弹「找不到网络路径」对话框
  （已经反复踩过）。要么让用户自己点开始菜单 / 安装目录里的 exe，要么在
  PowerShell 里 `Start-Process 'D:\apps\AgentRMC\AgentRMCNative.exe'`。
- curl 验证要带 `--noproxy '*'`：本机开着代理，不带的话 127.0.0.1 也会被拦成
  `upstream connect failed`，看起来像应用没起来。
- 可带参数 `lean` 跳过本地语音模型（同 build.bat）。
- 打包链路依赖：VS 自带 CMake、Qt 6.8.3（`native\.qt\`）、
  NSIS（`C:\Program Files (x86)\NSIS\makensis.exe`）。
- 临时验证用的 exe 不要留在 `native\build\Release`：installer.nsi 只按名字
  排除 `*Test.exe`，其它多余 exe 会被打进安装包。
- Python 旧版 GUI（agentrc_gui.py）如需打包用 `tools\build_exe.bat`，产物在
  `release\`（与 native 的 dist 交付无关）。

## Native 测试

- **跑 build.bat / package.bat 前先 `unset https_proxy http_proxy`**：Git Bash 里
  大小写两份代理变量同时存在时，MSBuild 会炸在环境字典上，报
  `MSB6001: “CL.exe”的命令行开关无效 ... 关键字:“HTTPS_PROXY”所添加的关键字:“https_proxy”`
  ——这不是代码错，别去改 CMake。
- 构建后在 `native\build\Release` 下跑相关测试（AgentRMCAppTest /
  AgentRMCStoreTest / AgentRMCThemeTest 等），改动涉及按键映射或应用启动时
  必须跑 AgentRMCAppTest。
- Git Bash 里没有 cmake；用 VS 自带的
  `"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`。

## 版本号：Native 与插件同号同步升

- **升版必须等用户明确说「升版」**（用户 2026-11-02 强调）：无论改了什么，都**不要**
  自己动 `plugins/agentrmc-bridge/VERSION`、`native/CMakeLists.txt` 的
  `project(... VERSION ...)`、`installer.nsi` 或各清单里的版本号。改完照常编译、跑测试、
  出包、静默装——**出包不要求升版**，同一个版本号原地重装即可（安装器按注册表原地升级）。
  版本号只在用户点头后改一次，改完再出一次包。
- **插件跟随 Native 同号**（用户 2026-11-02 决定，取代原来的「两条独立版本线」）：
  一次升版里 `native/CMakeLists.txt` 与 `plugins/agentrmc-bridge/VERSION` 写同一个号，
  对外也只报一个版本号。别再出现「Native 0.3.9 / 插件 0.3.8」这种说法。
- **不要每次内部改动都升版本**：功能改完先留在工作区，攒到一个可交付批次
  （要出包/要给别人装）再升。纯文档、注释、构建脚本调整不升。
- **升 minor 还是 patch**：新增用户可见能力升 minor（如 0.3 → 0.4）；修 bug 升 patch；
  内部重构/试验不升。插件跟着同号，不再单独判断「插件该不该升」——但插件**确实改了**
  才跟着升，纯 native 改动不必为了对齐而空升插件。
- 出包前跑 `native\tools\check-version.ps1`（CMake / NSIS / exe 一致）和
  `native\tools\sync-plugin-version.ps1 -Check`（五处清单 == VERSION）。
- 当前批次：Native `0.3.9` / 插件 `0.3.9`。
- **插件版本只有一个来源**：`plugins/agentrmc-bridge/VERSION`。构建时
  `native/tools/sync-plugin-version.ps1` 把它同步进 3 个 `plugin.json` 和
  2 个 `marketplace.json`；改插件版本只改 VERSION 文件，别逐个改清单。
- **别删旧版本号目录**：`~/.qoder(-cn)/plugins/cache/local/agentrmc-bridge/<ver>`
  里的旧那一层，宿主**已经开着的会话**还在按它的路径调 hook（会话启动时路径已烧进
  命令里）。删掉不会报错，只会让那个会话的提醒**静默消失**。Qoder 官方安装器自己会删，
  手工挪走前更要想清楚；App「系统」页的「等新版上报」就是给这种情况留的读数。
- **别删旧版本号目录**：`~/.qoder(-cn)/plugins/cache/local/agentrmc-bridge/<ver>`
  里的旧那一层，宿主**已经开着的会话**还在按它的路径调 hook（会话启动时路径已烧进
  命令里）。删掉不会报错，只会让那个会话的提醒**静默消失**。Qoder 官方安装器自己会删，
  手工挪走前更要想清楚；App「系统」页的「等新版上报」就是给这种情况留的读数。
