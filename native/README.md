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

1. **壳（当前）**：无边框窗口、主题/QSS、左侧遥控器卡片、三个 Tab、托盘、
   `--shot` 无头截图。
2. **配置与映射表**：复用 `config.json` / `profiles.json` 同一套 schema；
   Profile 匹配、extends 合并、MapCell/MapRow 可编辑手势格。
3. **输入链**：Raw Input + 设备过滤、单实例、KeyRouter（单击/双击/长按）、
   动作注入（key/scroll/run/mouse/brightness）、媒体模式、导航模式。
4. **钩子**：原生键接管开关、鼠标模式、耳机播放键；保持 Python 版踩过的
   链序/自喂 Router/物理键副作用。
5. **语音**：hotkey 注入（剪贴板/组合键）；whisper.cpp 本地转写（模型不进仓）。
6. **收尾**：BLE 电量、跨屏 DPI、托盘菜单、windeployqt 打包、体积回归。

## 兼容性

- `config.json` / `profiles.json` 保持同一格式，用户现有映射（左键双击=退格、
  长按=PageUp 等）直接可读。
- Python 版继续保留，作为行为参照和验收 oracle；两版可以并排跑，事件日志对比。
- 原生版在里程碑 3 之前不接输入、不装钩子，不会干扰正在运行的 Python 版。

## 体积预算

| 部分 | 目标 |
|---|---:|
| Qt Widgets 运行库（windeployqt） | 20–35MB |
| 程序 + 资源 | 3–8MB |
| whisper.cpp | ~5MB（不含模型） |
| 模型（GGML small） | ~466MB，可选 |
