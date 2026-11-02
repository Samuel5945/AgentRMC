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
  `curl http://127.0.0.1:45915/state` 验证新实例活着。
- 可带参数 `lean` 跳过本地语音模型（同 build.bat）。
- 打包链路依赖：VS 自带 CMake、Qt 6.8.3（`native\.qt\`）、
  NSIS（`C:\Program Files (x86)\NSIS\makensis.exe`）。
- 临时验证用的 exe 不要留在 `native\build\Release`：installer.nsi 只按名字
  排除 `*Test.exe`，其它多余 exe 会被打进安装包。
- Python 旧版 GUI（agentrc_gui.py）如需打包用 `tools\build_exe.bat`，产物在
  `release\`（与 native 的 dist 交付无关）。

## Native 测试

- 构建后在 `native\build\Release` 下跑相关测试（AgentRMCAppTest /
  AgentRMCStoreTest / AgentRMCThemeTest 等），改动涉及按键映射或应用启动时
  必须跑 AgentRMCAppTest。
- Git Bash 里没有 cmake；用 VS 自带的
  `"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`。

## 版本号：两条独立版本线

- **Native 和插件分开编号**：桌面端 `native/` 与 `agentrmc-bridge` 各自演进；
  `Native 0.3.9` 不代表插件也是 0.3.9。对外说明时必须分别写。
- **不要每次内部改动都升版本**：功能改完先留在工作区，攒到一个可交付批次
  （要出包/要给别人装）再升。纯文档、注释、构建脚本调整不升。
- **Native**：新增用户可见能力升 minor（如 0.3 → 0.4）；修 bug 升 patch；
  内部重构/试验不升。
- **插件**：只有 hook 事件、payload、宿主兼容性变化才升 patch；文档/测试调整不升。
- 出包前仍跑 `native\tools\check-version.ps1`，保证 CMake / NSIS / exe 一致；
  这只是“同一版本号内部一致”，不是要求 Native 与插件同号。
- 当前批次冻结：Native `0.3.9`、插件 `0.3.6`；下一次对外发布再统一决定
  是升 Native minor 还是只发 patch，不在每次改动后递增。
