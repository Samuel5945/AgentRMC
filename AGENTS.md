# 项目约定

## 交付方式：改动后必须打包到 dist

- native 端（Qt/C++）代码改完、测试通过后，**必须出包**：运行
  `native\tools\package.bat`（= build.bat 构建 + windeployqt + NSIS），
  产出 `dist\AgentRMC-Setup.exe`，把这份安装包作为交付物告诉用户。
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
