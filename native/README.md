# AgentRMC Native（C++/Qt + sherpa-onnx）

这是 Python 版之外的原生迁移线。目标是把 app 本体从 ~349MB 压到 **~30MB**
（不装本地后端；带 sherpa 运行库约 46MB），本地模型另算、可选，同时保留现有界面和功能。

当前分支：`feat/native-qt-whispercpp`

## 为什么重写

Python 版的体积大头不是模型，而是运行时：PySide6 94MB、ctranslate2 61MB、
av/FFmpeg 65MB、onnxruntime 38MB、numpy 27MB。原生线用：

- **Qt 6 Widgets（C++）**：保留现在的界面语言——同一套 QSS、圆角卡片、遥控器绘制、
  映射表和开关都能 1:1 迁移，不背 Python。
- **sherpa-onnx C API**：本地语音走 SenseVoiceSmall / Qwen3-ASR，只用
  onnxruntime，不用 PyTorch / ctranslate2 / FFmpeg；不装模型时语音键仍默认转发
  Qoder 的 `Ctrl+Shift+V`。
- Win32 原生：Raw Input、低级钩子、SendInput、WinMM 录音、托盘、DPI、BLE WinRT。

## 工具链（本机已具备）

- Visual Studio 2026 Community：MSVC 14.51 + 自带 CMake
- Qt 6.8.3 MSVC 2022 x64：通过 aqtinstall 装在 `native/.qt/`（已 gitignore，不进仓库）
- 本地语音运行时：sherpa-onnx v2.0.1 Windows x64 预编译库放在
  `native/third_party/sherpa-onnx/`（已 gitignore）；模型放在 `native/models/`。

## 构建

```bat
rem 可选：本地语音运行时 + SenseVoice 模型（不装也能编译/使用 hotkey）
native\tools\download_voice_models.bat runtime
native\tools\download_voice_models.bat sensevoice

native\tools\build.bat
```

产物：`native\build\Release\AgentRMCNative.exe`，脚本会执行 `windeployqt`
把 Qt 运行库放到同目录；CMake 检测到 sherpa-onnx 时会把 DLL 一起复制过去。
没有 sherpa 运行库时仍可构建，语音页会提示“当前构建未包含运行库”。

只打 hotkey 包（不带本地转写运行库，约 26–30MB）：

```bat
cmake -S native -B native\build -DAGENTRMC_ENABLE_SHERPA=OFF ...
```

`build.bat` 会删掉 Widgets 用不到的 `dxcompiler/dxil/d3dcompiler/vc_redist`，
带 sherpa 的 Release 目录实测约 46MB。

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
4. **钩子（已完成）**：映射页「接管原生键」开关（全场景）、WH_KEYBOARD_LL 吞键并
   直接喂 Router、直通动作补发源键/连发补发、鼠标模式低层钩子（短按 8px、按住加速）、
   钩子链序无关；接管自测覆盖 pass/double/repeat/模式标记/钩子判定。
   耳机播放键属于语音，和 M5 一起接。
5. **语音（已完成）**：三条路线共用一套语音键状态机——
   - 默认 **Qoder 热键**：0 模型，转发 `Ctrl+Shift+V`（Qoder hold / WorkBuddy
     toggle）；不编译 sherpa 时 app 约 30MB，带运行库约 46MB；
   - **本地 SenseVoice**：sherpa-onnx + SenseVoiceSmall int8，下载 158MB、
     解压约 230MB，中文/粤语/英日韩，自带标点 ITN；16kHz 单声道 PCM 直接送识别，
     3 秒测试音频热态约 150ms（首帧含模型加载约 1.8s）；
   - **本地 Qwen3-ASR**：同一套 sherpa 运行时的 `qwen3_asr` 分支，0.6B int8
     下载 838MB、解压约 1GB，代码路径已接，按需下载。
   录音用 WinMM（Qt 6.8 没装 Multimedia），自动降级 44.1/48kHz 再重采样到 16k。
   部分设备打开后约 0.85s 才送第一块数据，所以监听开始时预热麦克风并保留 400ms
   预录，按住即说不会丢开头。支持按住说话、静音兜底、3 秒自测、转写后粘贴/复制。
   耳机播放键在 Qoder / WorkBuddy 前台时接管为开关，按一下开始、再按一下结束。
   AgentRMC 本身不接 LLM；真正的 LLM 在 Qoder / WorkBuddy 服务端。
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
- 映射页右下角「接管原生键」默认关；打开后当前前台场景的映射键原生事件由低层钩子
  吞掉再喂 Router。用户态只能按 VK 吞，实体键盘同名键也会按映射处理；双击键的单击
  延迟约 0.3 秒。关掉开关立即恢复原生。
- M4 的鼠标模式：方向键/Enter 由低层钩子接管，短按 8px、按住逐级加速；退出仍用
  返回/菜单。
- 语音设置写在 `config.json` 的 `voice` 下：
  `backend`（`hotkey` / `sensevoice` / `qwen3`）、`sensevoice_dir`、`qwen3_dir`、
  `paste`、`auto_enter`。没有 `backend` 时兼容 Python 版的 `mode: local`。
- 模型目录优先用配置里的路径；留空时依次找 exe 同目录 `models/<名称>`，开发时再退到
  `native/models/`。设置页把默认路径显示为占位符，不会写死机器路径。
- 本地后端在「开始监听」时预热麦克风，停止监听或切回 hotkey 时关闭；Windows 的
  麦克风指示器会在这段时间常亮，这是预期行为。
- 耳机播放键（0xB3）只在当前场景命中 `media_voice.swallow_in`（默认 qoder / agent）
  时被低层钩子吞掉并转成语音开关，其他场景保持系统媒体键。
- 预览/截图：`AgentRMCNative.exe --pin qoder`，
  `AgentRMCNative.exe --shot shot.png --pin qoder`，或
  `AgentRMCNative.exe --shot-settings dialog.png`。

## 体积

| 部分 | 实测 |
|---|---:|
| Qt Widgets + 程序 + 资源（windeployqt） | ~26MB |
| sherpa-onnx + onnxruntime DLL（可选） | ~20MB |
| SenseVoiceSmall int8 | ~230MB（下载包 158MB） |
| Qwen3-ASR 0.6B int8（可选） | ~1GB（下载包 838MB） |

不编译 sherpa 的 hotkey-only 包约 **26–30MB**；带本地 SenseVoice 的包约 **280MB**。
Qwen3-ASR 的模型路径没有随仓库下载，按需运行下载脚本。

## 自测

```bat
native\build\Release\AgentRMCStoreTest.exe
native\build\Release\AgentRMCRouterTest.exe
native\build\Release\AgentRMCTakeoverTest.exe
native\build\Release\AgentRMCVoiceTest.exe
native\build\Release\AgentRMCAudioTest.exe 2 rc_probe\mic_test.wav
native\build\Release\AgentRMCAsrTest.exe native\models\sensevoice\test_wavs\zh.wav native\models\sensevoice sensevoice
```

覆盖：预设写回、`.bak`、同键原生去重、extends 继承、custom 动作；KeyRouter 的
滚动/tap/双击/长按/媒体/导航/鼠标模式；接管下的直通补发、连发补发、模式标记和
钩子吞键判定；语音后端解析、WAV 头、SenseVoice/Qwen3 模型发现；真实麦克风采集
（含 1 秒预热、400ms 预录、松手等最后一块）；以及 SenseVoice 端到端转写
（第二次为热态延迟）。`AgentRMCAsrTest` 只在启用 sherpa 时生成。
