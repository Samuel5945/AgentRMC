# AgentRMC Native（C++/Qt + whisper.cpp）

这是 Python 版之外的原生迁移线，目标是把 app 本体从 ~349MB 压到 **30–50MB**
（模型另算，GGML small ≈466MB），同时保留现有界面和功能。

当前分支：`feat/native-qt-whispercpp`

## 为什么重写

Python 版的体积大头不是模型，而是运行时：PySide6 94MB、ctranslate2 61MB、
av/FFmpeg 65MB、onnxruntime 38MB、numpy 27MB。原生线用：

- **Qt 6 Widgets（C++）**：保留现在的界面语言——同一套 QSS、圆角卡片、遥控器绘制、
  映射表和开关都能 1:1 迁移，不背 Python。
- **whisper.cpp（GGML）**：替代 faster-whisper/ctranslate2/onnxruntime/FFmpeg；
  模型换 GGML 格式，现有 CTranslate2 模型不能直接复用，需重新下载/转换。
- Win32 原生：Raw Input、低级钩子、keybd_event/SendInput、托盘、DPI、BLE WinRT。

## 工具链（本机已具备）

- Visual Studio 2026 Community：MSVC 14.51 + 自带 CMake
- Qt 6.8.3 MSVC 2022 x64：通过 aqtinstall 装在 `native/.qt/`（已 gitignore，不进仓库）

## 构建

```bat
native\tools\build.bat
```

产物：`native\build\Release\AgentRMCNative.exe`，脚本会执行 `windeployqt`
把 Qt 运行库放到同目录。

手动：

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  -S native -B native\build -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_PREFIX_PATH="%CD%\native\.qt\6.8.3\msvc2022_64"
...\cmake.exe --build native\build --config Release
```

## 里程碑

1. **壳（已完成）**：无边框窗口、主题/QSS、左侧遥控器卡片、三个 Tab、托盘、
   `--shot` 无头截图。
2. **配置与映射表（已完成）**：复用 `config.json` / `profiles.json` 同一套 schema；
   前台进程/标题匹配 + 跟随前台/固定场景；extends 合并；MapCell/MapRow 常驻边界、
   悬停、选中；单击/双击/长按预设菜单 + 自定义组合键；写回 `profiles.json` 并保留
   `.bak`；C++ 数据层自测覆盖写回/extends/去重。
3. **输入链（已完成）**：Raw Input + 设备过滤、KeyRouter（单击/双击/长按/连发加速）、
   动作注入（key/scroll/run/mouse/brightness）、媒体/导航/鼠标模式、单实例互斥、
   开始/停止监听、事件流；C++ 路由自测覆盖 tap/double/hold/modes。
4. **钩子（下一步）**：原生键接管开关、鼠标模式的低层钩子、耳机播放键；保持 Python 版
   踩过的链序/自喂 Router/物理键副作用。
5. **语音 / 模型（停点）**：这里会停一下再决定——继续用 QoderCN 自己的云端语音
   （hotkey 转发 `Ctrl+Shift+V`，不打包模型，app 维持 ~30MB），还是接 whisper.cpp
   本地转写。AgentRMC 本身不接 LLM；真正的 LLM 在 Qoder / WorkBuddy 服务端。
6. **收尾**：BLE 电量、跨屏 DPI、托盘菜单、windeployqt 打包、体积回归。

## 数据与兼容性

- 原生版读写 exe 同目录的 `config.json` / `profiles.json`；`build.bat` 首次构建时
  从仓库根目录播种，之后保留原生版自己的编辑（不会覆盖）。
- 和 Python 版保持同一格式，用户现有映射（左键双击=退格、长按=PageUp 等）直接可读。
- 映射页点格弹出预设菜单；单击支持「原生直通」，双击支持「关闭双击/双击模式」，
  长按支持「关闭/连续触发/连发加速/按住某动作」，三格都支持自定义组合键。
- 写回前自动把上一版保存为 `profiles.json.bak`。
- Python 版继续保留，作为行为参照和验收 oracle；两版可以并排跑，事件日志对比。
- 原生版和 Python 版共用单实例互斥量：Python 版在跑时，原生版的「开始监听」会提示
  `AgentRMC 已在运行`；先停 Python 版再测原生监听，避免每个键注入两次。
- M3 还没有原生键接管（M4）：此时映射仍是叠加，双击/直通的原生键还会到前台。
- 预览/截图：`AgentRMCNative.exe --pin qoder`，或
  `AgentRMCNative.exe --shot shot.png --pin qoder`。

## 体积预算

| 部分 | 目标 |
|---|---:|
| Qt Widgets 运行库（windeployqt） | 20–35MB |
| 程序 + 资源 | 3–8MB |
| whisper.cpp | ~5MB（不含模型） |
| 模型（GGML small） | ~466MB，可选 |

里程碑 3 后，`native/build/Release` 实测约 **30MB**（不含模型）。

## 自测

```bat
native\build\Release\AgentRMCStoreTest.exe
native\build\Release\AgentRMCRouterTest.exe
```

覆盖：预设写回、`.bak`、同键原生去重、extends 继承、custom 动作；
以及 KeyRouter 的滚动/tap/双击/长按/媒体/导航/鼠标模式。
