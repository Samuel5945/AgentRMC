# AgentRMC

把极米 / 快乐星球投影仪遥控器改造成电脑 Agent 的输入终端。

实测机型为极米「快乐星球 O3」13 键 BLE 遥控器（VID `0x000D` / PID `0x3838`）；
同款 HID 遥控器改 `config.json` 的 `device_mark` 即可复用。

按键走标准 HID（零驱动），语音可以本地 whisper 转写、也可以转给 Agent 自己的语音键，
按前台窗口自动切换键映射。

```
遥控器 → HID → Raw Input ─┬→ 按键 → 场景 Profile → 动作（模拟按键/滚动/长按手势/启动程序）
                          └→ 语音键 → ┬→ 本地 whisper → 指令表 / 粘贴给 Agent
                                      └→ 转给 Agent 自己的语音键（hold / toggle）
```

## 快速开始

```bash
git clone https://github.com/Samuel5945/AgentRMC.git && cd AgentRMC

tools\setup.bat            # 建 venv + 装依赖
tools\download_model.bat   # 下载 whisper small（约 480MB，默认走 127.0.0.1:7897 代理）

tools\run.bat              # 命令行版
# 或
.venv\Scripts\python.exe agentrc_gui.py   # 图形界面
```

打包 exe：

```bash
tools\build_exe.bat        # 产物 release\AgentRMC\AgentRMC.exe，约 348MB
```

## 项目结构

```
AgentRMC/
├── agentrc.py              核心：Raw Input 监听、语音状态机、按键路由、ASR、场景、指令
├── agentrc_gui.py          Qt 图形界面
├── config.json             语音键码、麦克风、ASR 参数
├── profiles.json           场景表（按前台窗口切换键映射，支持 extends / 长按 / 导航键表）
├── commands.json           语音指令表（说"打开浏览器"直接执行，仅 local 模式）
├── tests/
│   ├── test_keyrouter.py   无头测试：长按连滚 / 双动作 / 双击模式 / extends / 语音接法
│   └── test_gui_geometry.py 无头测试：跨屏/小屏窗口回夹
├── requirements.txt
├── assets/
│   ├── icon.ico            应用图标
│   └── remote/pairing.png  配对示意图（图文引导弹窗用，打包时一起拷进 release）
├── tools/
│   ├── setup.bat           建 venv + 装依赖
│   ├── download_model.bat  下载 whisper 模型（走代理，绕开 huggingface-hub）
│   ├── build_exe.bat       打包 exe
│   ├── run.bat             命令行启动
│   ├── learn_keys.bat      按键学习（换遥控器时重新校准）
│   └── probe_admin.bat     提权读原始 HID 报告（诊断用）
├── rc_probe/               探测脚本，保留作协议分析参考
│   ├── dump_gatt_winrt.py     枚举 GATT 服务表
│   ├── probe_notify.py        订阅私有通道
│   ├── probe_ctrl.py          试探控制通道命令
│   ├── hid_caps.py            读 HID 报告能力
│   ├── learn_keys.py          交互式按键学习
│   ├── analyze_raw.py         分析原始报告
│   ├── nolegacy_probe.py      NOLEGACY + SendInput 行为（单进程）
│   ├── nolegacy_probe2.py     NOLEGACY 的抑制范围（跨进程）
│   ├── uia_probe.py           读 Electron 窗口的 UIA 树（TTS 前置调研）
│   └── rawreport.py           提权读 256 字节透传包
├── legacy/                 早期 pywebview 方案（WebView2 不兼容，已弃用）
└── models/                 模型目录（git 忽略，需自行下载）
```

## 设备

| 项 | 值 |
|---|---|
| 蓝牙名 | `BLuetooth 4.0 RC` |
| MAC | `70:FD:88:CE:60:EC` |
| VID / PID | `0x000D` / `0x3838` |
| 厂商 | Realtek BT |
| 固件 | `B951C_HBG2_JH_V1.1.2` |
| HID Usage | `0x0001` / `0x0006`（Generic Desktop / Keyboard） |
| Appearance | `0x0180`（Generic Remote Control） |
| 来源 | 极米 / 快乐星球投影仪配套遥控器（实测极米「快乐星球 O3」，13 键） |

## 探测结论

### GATT 服务表

| 服务 UUID | 内容 | 结论 |
|---|---|---|
| `0x1812` | HID over GATT | 被 Windows HID 驱动独占，用户态读不到原始报告 |
| `0x180F` | Battery | 可读，正常上报电量 |
| `0x180A` | Device Info | 可读，拿到型号/固件 |
| `0xD1FF` | `A001`(write+notify) / `A002`(write) | 厂商私有。**可订阅，但按住语音键时无音频数据** |
| `0xD0FF` | `ffd1`~`ffe0` | 厂商私有，MAC/参数，无 notify 特征 |
| `0x6287` | `6387`(write) / `6487`(write+notify) | 厂商私有，无数据 |

订阅 `D1FF/A001` 后只收到一个 16 字节全零握手包，之后 4 分钟内零音频。**遥控器麦克风的音频没有走这条通道。**

### 按键：完全标准，零改造可用

遥控器的按键走标准 HID 键盘协议，Windows 原生识别，不需要任何驱动或破解：

| 物理键 | vkey | 扫描码 | Windows 键名 |
|---|---|---|---|
| 上 | 38 `0x26` | `0x48` | `Up` |
| 下 | 40 `0x28` | `0x50` | `Down` |
| 左 | 37 `0x25` | `0x4B` | `Left` |
| 右 | 39 `0x27` | `0x4D` | `Right` |
| OK | 13 `0x0D` | `0x1C` | `Enter` |
| 返回 | 27 `0x1B` | `0x01` | `Esc` |
| 菜单 | 93 `0x5D` | `0x5D` | `Apps` |
| 主页 | 36 `0x24` | `0x47` | `Home` |
| 语音键 | 116 `0x74` | `0x3F` | `F5`（**按住连发**） |
| 变焦 / 对焦 | 122 `0x7A` | `0x57` | `F11`（应用里就是全屏切换） |
| 音量 + / 音量 − / 电源 | — | — | **电脑上收不到任何报文**，见「已知限制 2」 |

### 语音键的识别特征

语音键不是"按一下发一个码"，而是**按住期间以约 31ms 间隔连续发送 keydown**，松手才发 keyup。这个特征正好可以用来区分遥控器和实体键盘：

```
keydown  keydown  keydown  ...  keydown   keyup
  0ms     31ms     62ms          2.4s
```

程序利用它做判定：第一次 keydown 先挂起 250ms，若期间收到第二次，判定为长按并开始语音；
否则说明只是轻点了一下，**什么都不做**——原生 F5 已经由 Windows 送给前台窗口了，
再补发一次只会让浏览器刷新两下。

> 这里很容易误判成"要帮实体键盘转发 F5"。不会：Raw Input 已经按设备路径过滤，
> 只收遥控器的报文，实体键盘的 F5 根本不进这个状态机。

## 改造架构

```
┌──────────────────────────────────────────────────────────┐
│  BLE 遥控器  (HOGP)                                       │
└───────────────┬──────────────────────────────────────────┘
                │ 标准 HID 键盘报告
┌───────────────▼──────────────────────────────────────────┐
│  Windows HID 驱动                                         │
└───────────────┬──────────────────────────────────────────┘
                │ Raw Input（按设备路径过滤，不干扰真实键盘）
┌───────────────▼──────────────────────────────────────────┐
│  agentrc.py                                              │
│                                                          │
│  ┌─────────────┐   ┌──────────────┐   ┌───────────────┐ │
│  │ 按键 → 语义 │   │ 语音键长按   │   │ 动作执行      │ │
│  │ up/down/... │──▶│ 判定 + 录音  │──▶│ 模拟按键      │ │
│  └─────────────┘   └──────┬───────┘   │ 剪贴板 + 粘贴 │ │
│                           │            └───────────────┘ │
│                    ┌──────▼───────┐                      │
│                    │ faster-whisper│                     │
│                    │  本地转写     │                     │
│                    └──────────────┘                      │
└──────────────────────────────────────────────────────────┘
```

## 用法

### 首次准备

```bash
tools\setup.bat          # 建 venv + 装依赖
tools\download_model.bat # 下载 whisper small 模型（约 480MB，走 7897 代理）
tools\download_model.bat medium   # 可选：medium / large（=large-v3）
```

模型放在 `models/faster-whisper-<尺寸>/`，程序会优先加载本地目录，不需要联网。
图形界面的「本地模型」开关和尺寸下拉可以直接切换，详见下文。

遥控器第一次配对：把它靠近要连接的设备，**同时长按「返回」+「主页」约 3 秒**，指示灯快闪后
在系统蓝牙列表里点它。配对过一次之后不用再配，按一下任意键就会自动回连。
软件里点顶栏 **使用引导** 会弹出图文版步骤。

> HuggingFace 直连不通时，`huggingface-hub` 新版对 `HTTPS_PROXY` 支持有问题（会报 502）。
> 所以 `download_model.bat` 直接用 `curl -x` 走代理抓文件，绕开这个库。
> 代理地址在 bat 开头的 `set PROXY=` 改。

### 图形界面（推荐）

双击 **`release\AgentRMC\AgentRMC.exe`**。窗口两栏，左栏上下两块：

- **左上 · 遥控器** —— 按真实键位画的键位图：按下哪个键就实时高亮，进入模式时可用键描边，
  电源键与音量拨杆标灰（红外，电脑收不到）。下方是**连接状态灯**
  （绿=已连接 · HID 在位 / 橙=未连接，按一下任意键回连 / 灰=未发现）和 `蓝牙设置` 按钮
- **左下 · 语音** —— 细电平/录音脉冲、两行识别结果、`开始监听`（监听中变红）、
  右上角 `试麦`（录 4 秒并转写），以及一行 **`本地模型` 开关 + small/medium/large + 状态**。
  语音接法、指令条数、设备号收进「语音」标题的悬停提示，不占版面
- **右 · 三页签** —— `事件流`（可清空）、`按键映射`、`使用引导`

顶栏是一行状态文字：`● 监听中 · 76% · default`，右侧 `使用引导` 随时弹图文引导窗口。

两个按钮：`开始监听` / `试麦`。
`config.json` 里的 `ui.auto_listen` 为 `true` 时，程序启动后自动开始监听；
同时只允许一个实例（第二个会弹窗退出，避免每个键注入两次）。
`ui.start_minimized` 为 `true` 时窗口启动就藏在托盘里，不占屏幕也不抢 Qoder 的焦点
（有托盘就直接不显示，点托盘图标再叫出来）；
**第一次启动例外**——会正常开窗口并自动弹出图文引导，看过一次（切页签或关窗）才记住。

**托盘常驻**：右下角托盘图标（`assets/icon.ico`）就是后台监听的入口。

| 操作 | 行为 |
|---|---|
| 左键点托盘图标 | 唤回主界面并置前（窗口已经在前台时不会把它藏掉） |
| 右键托盘 → 显示主界面 / 隐藏到托盘 | 手动开关界面 |
| 右键托盘 → 开始监听 / 停止监听 | 不打开窗口也能停掉监听 |
| 右键托盘 → 退出 AgentRMC | 真退出：先 `ctl.stop()` 再结束进程 |
| 窗口右上角 `✕` | **只是收进托盘，监听继续**（第一次会弹一条气泡提示） |

`ui.close_to_tray` 设为 `false` 可以退回老行为（关窗即退出）；系统托盘不可用时也自动退回。
托盘提示文字实时显示状态：`AgentRMC · 监听中 · qoder`。

窗口跨屏/缩放：启用了 Qt 的 `PassThrough` DPI 策略（175%/225% 不取整）、用
`startSystemMove` 走系统原生拖动，切屏后把窗口回夹到当前屏幕的可用区域（小于最小尺寸
的屏幕会缩到能放下），并在退出时把几何存进 `QSettings("AgentRMC","AgentRMC")`，
下次启动恢复后同样回夹。

#### 按键映射页

右侧标签切到 **按键映射**，按物理键列出当前 Profile 的完整映射。顶部场景行用
**绿点 = 跟随前台命中 / 橙点 = 下拉框钉住**，后面是命中该场景的进程或标题；
下面是 `键位 / 单击 / 双击 / 长按` 四列，分成 方向键、功能键、语音与直通 三组。

**单击、双击、长按三个单元格都可以直接点**，弹出常用动作预设（导航键、滚动、浏览器、
编辑器、系统、鼠标、双击模式、按住重复/按住动作）。菜单最上面的 **`自定义组合键…`**
弹出一个录入框，**在键盘上按下想要的按键/组合（可含 Ctrl/Shift/Alt/Win）直接录入**，
不用碰 JSON；Esc 取消。选完立刻写回 `profiles.json` 并热重载，
同时保留上一版 `profiles.json.bak`。语音键和对焦键是固定行为
（语音接法 / F11 原生直通），单元格不可编辑。

| 元素 | 说明 |
|---|---|
| 场景行 | 绿色点 = 跟随前台自动命中；橙色点 = 固定预览。后面的匹配规则说明为什么命中（进程名 / 窗口标题，悬停看完整列表） |
| 下拉框 | 默认 `跟随前台`，切窗口时表格自动跟着变；也可以钉住某个 profile 预览 |
| `键位` | 遥控器物理键：`↑ ↓ ← → OK 返回 菜单 主页 语音 对焦` |
| `单击` | Profile 解析后的动作，下面一行是实际按键/效果，如 `滚轮 ↑ 300`、`Enter 直通`、`CTRL+T`；点格改动作 |
| `双击` | 默认是 Router 内置手势：返回=`音量/亮度模式`、菜单=`鼠标模式`、主页=`导航模式`。点格可以改成任意预设，或 `关闭双击`；改成其它动作后单击会延迟 0.3s 等第二击 |
| `长按` | 默认继承动作自带的 `hold_repeat` / `hold_action`；点格可以改成 `连续触发`、`连续加速`、`按住某动作` 或关闭。语音键固定 `按住说话` |
| 行联动 | 点选或悬停一行，左侧遥控器图上的对应键会亮起蓝环，眼不用在两个面板之间找 |
| 接管原生键 | 右下角开关：当前前台场景的映射键原生事件被吞掉，再按映射注入。实体键盘同名键也会按映射处理，双击键的单击延迟约 0.3s；默认关 |
| `高级配置` | 用默认编辑器打开 `profiles.json`（匹配规则、自定义动作等） |
| `重新加载` | 改完 json 不用重启程序，点一下热加载 |

导航模式（qoder / agent）：默认双击主页进入，`↑↓` 翻页、`←→` 切换控件，单击主页退出；
也可以在上面的双击格里改成其它动作。

> 双击格的动作会延迟 0.3s 等第二击；不接管原生键时原生那一份仍立即触发（所以返回/菜单/主页单击不会丢）。
> 打开接管后原生被吞，单击才真正延迟。

窗口变矮时表格纵向滚动；窗口极窄时保持四列不挤压，横向滚动兜底。正常 1000×680 窗口没有滚动条。

源码方式启动：

```bash
.venv\Scripts\python.exe agentrc_gui.py
.venv\Scripts\python.exe agentrc_gui.py --tab keymap   # 启动直接停在按键映射页
.venv\Scripts\python.exe agentrc_gui.py --tab keymap --pin qoder   # 固定预览 qoder
```

截图/验收（不启动监听、不弹第二个实例）：

```bash
.venv\Scripts\python.exe agentrc_gui.py --shot out.png --tab keymap --pin qoder
```

### 命令行方式

```bash
# 列出可用麦克风
.venv\Scripts\python.exe agentrc.py --list-mic

# 调试：打印遥控器所有按键
.venv\Scripts\python.exe agentrc.py --list-keys

# 验证语音链路（录 5 秒并转写）
.venv\Scripts\python.exe agentrc.py --test-mic

# 正常运行
.venv\Scripts\python.exe agentrc.py
```

也可以双击 `tools\run.bat`。**不需要管理员权限。**

### 打包成 exe

```bash
.venv\Scripts\python.exe -m PyInstaller --noconfirm --windowed ^
  --name AgentRMC --onedir --icon=assets/icon.ico ^
  --collect-data faster_whisper ^
  --distpath release --workpath build_release ^
  --exclude-module tkinter ^
  --exclude-module PySide6.QtWebEngineCore ^
  --exclude-module PySide6.QtWebEngineWidgets ^
  --exclude-module PySide6.Qt3DCore ^
  --exclude-module PySide6.QtMultimedia ^
  agentrc_gui.py
```

产物 `release/AgentRMC/` 约 348MB（ctranslate2 58MB + Qt 运行时 + onnxruntime）。

打包后需要把 `models/` 和三个 json 配置放到 exe 同级目录（打包脚本不会自动带，因为模型 462MB）。
开发机上可以用目录联接省空间：

```powershell
New-Item -ItemType Junction -Path release\AgentRMC\models -Target D:\githubrepos\Agentrmc\models
```

程序启动时从 **exe 所在目录**读配置和模型（`_app_dir()` 里判断了 `sys.frozen`）。

按住语音键说话，松手结束：当前用 `hotkey` 模式，把语音交给 Qoder 自己的 Ctrl+Shift+V
（toggle：按下开始、松手停止；先把焦点点进 Qoder 输入框）。`local` 模式则自己录音转写
再粘贴，见下文。

### 实测性能

| 指标 | 值 |
|---|---|
| 模型加载 | 0.9s（本地） |
| 4.5s 音频转写 | 1.1s（约 4 倍实时） |
| 中文准确率 | 测试句 100% 正确 |
| 硬件 | CPU / int8 量化 |

### 无头测试

不碰真实键盘、不注入任何按键，验证长按连滚、点击/长按双动作、
`extends` 合并和 Qoder 开关式语音的时序：

```bash
.venv\Scripts\python.exe tests\test_keyrouter.py
```

## 语音指令

只在 `voice.mode: local` 下生效：识别出的文本会**先过一遍本地指令表**，命中就直接执行；
没命中才粘贴给 Agent。当前 `hotkey` 模式把语音直接交给 Qoder 自己的识别，不走这张表。

| 说 | 动作 |
|---|---|
| 打开浏览器 / 开浏览器 / 上网 | 启动 Edge |
| 打开计算器 / 记事本 / 资源管理器 | 启动对应程序 |
| 截图 / 截个图 | `Win+Shift+S` |
| 静音 | `VK_MUTE` |
| 音量加 / 大声点 | 音量 +3 格 |
| 音量减 / 小声点 | 音量 -3 格 |
| 播放 / 暂停 | 媒体播放键 |
| 锁屏 / 锁定电脑 | 锁定工作站 |
| 其他任何话 | 粘贴给 Agent |

规则在 `commands.json`，改完重启生效：

```jsonc
{
  "rules": [
    {
      "match": ["打开浏览器", "上网"],          // 命中任一关键词即触发
      "action": { "type": "run", "cmd": "start msedge" }
    },
    {
      "match": ["截图"],
      "action": { "type": "key", "vk": 83, "mods": ["win", "shift"] }
    }
  ]
}
```

`action.type` 支持：

| type | 字段 | 说明 |
|---|---|---|
| `run` | `cmd` | 执行命令/启动程序 |
| `key` | `vk`、`mods`、`repeat` | 模拟按键，`mods` 可为 win/ctrl/alt/shift |
| `paste` | `enter` | 粘贴文本，`enter: true` 则自动回车 |
| `enter` | — | 粘贴并回车 |

匹配时会先做归一化，去掉「帮我 / 请 / 一下 / 个 / 吧」这类口语填充词，所以「帮我截个图吧」也能命中「截图」。

## 场景 Profile

按键行为按**前台窗口**自动切换。程序每次按键时读一次前台进程名和窗口标题（300ms 缓存），命中哪个 profile 就用哪套映射。

内置 5 个：

| Profile | 匹配进程 | 上 / 下 | 左 / 右 | OK | 返回 | 主页 |
|---|---|---|---|---|---|---|
| `browser` | chrome / msedge / firefox / brave | 滚动 | 后退 / 前进 | Enter | Esc | 新建标签 |
| `editor` | code / cursor / windsurf / devenv | 上/下一个编辑器 | 上/下一个标签 | Enter | Esc | 命令面板 |
| `terminal` | windowsterminal / cmd / powershell / pwsh | 历史命令 | 按词跳转 | Enter | Esc | 清屏 |
| `qoder` | qoder / qoder cn | 滚轮（按住连滚） | 原生光标移动 | **原生 Enter 发送** | **原生 Esc 停止** | 点击滚到顶；双击切导航 |
| `agent` | workbuddyai / 标题含 workbuddy | 滚轮（按住连滚） | 原生光标移动 | **原生 Enter 发送** | **原生 Esc 停止** | 点击滚到顶；双击切导航 |
| `default` | 其他 | ↑ / ↓ | ← / → | Enter | Esc | 原生 Home |

`agent` 用 `extends` 继承 `qoder` 的全部键位，只改了匹配进程、标题和语音接法（后者只在 `hotkey` 模式下用）。
Qoder 的语音快捷键 Ctrl+Shift+V 在 webview 里是 **keydown 开关**（正在录就停，否则开始）。
所以这里写 `"voice": {"trigger": "toggle"}`：遥控器按下时发一次组合键开始录音，松手再发一次
停止；不用再点第二下。

> Qoder 的 `workbench.action.toggleVoiceInput` 虽然是窗口级快捷键，但处理函数里有一句
> `composerFocused ? start : ignore`——焦点不在输入框时它**直接忽略**。所以目前仍然需要
> 先把焦点点进 Qoder 输入框。
>
> 曾试过 `voice.focus_click: [0.68, 120]`（按窗口相对位置自动点一下输入区，再发快捷键），
> 但窗口位置/缩放/DPI 一变就会点空，反而把焦点点丢，导致语音完全不出来；**默认已关闭**。
> 这个字段还留着，后续做一次性校准（把鼠标放到输入框，记录相对位置）再接上。

`Menu`（菜单键）在 agent/qoder 里是换行（`Shift+Enter`）；`F11`（变焦）保持原生直通。
`default` 的 Home 也改成了原生——之前它注入 Win 键，按一下就会弹开始菜单/任务栏。

### 加自己的场景

编辑 `profiles.json`，在 `profiles` 数组里加一项，**顺序即优先级**，`default` 放最后：

```jsonc
{
  "name": "music",
  "match": { "process": ["spotify.exe", "cloudmusic.exe"] },
  "keys": { "13": "play", "38": "next", "40": "prev" },
  "actions": {
    "play": { "type": "key", "vk": 179 },              // VK_MEDIA_PLAY_PAUSE
    "next": { "type": "key", "vk": 176 },              // VK_MEDIA_NEXT_TRACK
    "prev": { "type": "key", "vk": 177 }
  }
}
```

匹配字段：

- `match.process` —— 可执行文件名，不区分大小写
- `match.title` —— 窗口标题包含任一关键词
- 两个都为空 → 只能作为兜底（像 `default`）
- `extends` —— 继承另一个 profile 的 `keys` / `actions` / `double` / `hold` / `voice`，再覆盖；
  顺序仍然是优先级。`agent` 就是这样继承 `qoder` 的。

每个 profile 还可以带 `"voice": {"trigger": "hold" | "toggle"}` 覆盖全局语音接法。
`focus_click` 是可选实验项（相对坐标自动点输入区），默认不用，等做校准。

### 双击 / 长按表

映射页的单元格保存成 profile 顶层的两张表，也可以手写：

```jsonc
"double": {
  "13": "k13_d_mode_media",   // 双击动作名；"none" 关闭内置双击
  "27": "none"
},
"hold": {
  "38": "repeat_accel",       // none / repeat / repeat_accel / 动作名
  "13": "k13_h_mute"
}
```

- `double`：第二次按下时执行这个动作；单击会延迟 0.3s 等第二击。没有 `double` 表的键保持原行为；
  27/36/93（返回/主页/菜单）没有覆盖时仍回落到内置的 音量亮度 / 导航 / 鼠标 模式。
  `"none"` 会关掉该键的内置双击，单击不再延迟。
  **方向键上的双击（如左键=退格）要打开映射页右下角的「接管原生键」**，否则原生左移会和双击动作一起触发。
- `hold`：`none` 关闭长按；`repeat` 按住重复当前动作；`repeat_accel` 按住重复并越来越快；
  其它值当动作名，按住 0.6s 后执行。没有 `hold` 表的键继续看动作自带的
  `hold_repeat` / `hold_action`，老配置仍然兼容。
- 两张表都会跟着 `extends` 合并。

### 长按手势

动作里加这几个字段，就能用一个键做出两种操作：

| 字段 | 说明 |
|---|---|
| `hold_repeat` | 毫秒。按住时每隔这么久重复执行一次；`scroll` 就是"按住连滚" |
| `hold_accel` | `true` 时每重复一次间隔 ×0.86，最低 45ms（越按越快） |
| `hold_action` | 长按达到阈值后执行的动作 |
| `hold_ms` | 长按判定阈值，默认 450–600ms |
| `hold_defer` | `true` 时按下先不执行普通动作，松手才执行；这样"点击"不会先触发一次 |
| `repeat` | 一次执行里重复几次（`key` / `scroll` 都支持） |
| `label` | 映射页/事件流里显示的人话，比如 `"滚到顶"` |

事件流不再按键连发逐条刷屏：按键动作本身立即执行，但只会在**松手**时记录一条汇总，
例如 `长按 1.4s · 滚轮 ↓ 300 ×12`；轻点就是动作本身，语音则记一条 `语音 3.2s · 已停录`。

agent/qoder 的实际用法：

```jsonc
"scroll_down": { "type": "scroll", "delta": -300,
                 "hold_repeat": 100, "hold_accel": true },   // 按住 ↓ 连续滚
"chat_top": { "type": "scroll", "delta": 9000, "repeat": 3, "label": "滚到顶" }
```

### 双击模式（临时状态）

菜单键硬件只发一次按下+抬起、没有长按连发，所以三个模式统一用**双击**进入，
逻辑在 `KeyRouter` 里，不依赖具体 profile，任何前台界面都能用：

| 双击 | 进入模式 | 模式里的方向键 |
|---|---|---|
| 返回 | 音量 · 亮度 | `↑↓` 音量 ±，`←→` 主显示器亮度 ±（DDC/CI） |
| 菜单 | 鼠标 | 方向键移动指针（短按 8px、按住加速），`OK` = 左键单击 |
| 主页 | 导航 | `←→` `Shift+Tab`/`Tab` 选控件，`↑↓` `PageUp`/`PageDown` 翻页 |

- 不接管原生键时，这三个键的单击仍立即走原生；打开接管后，单击要等 0.3 秒的双击窗口再补发
- 进入/退出各一声蜂鸣，并在屏幕底部居中弹提示「xx模式 开/关」
- 退出：模式内单击 `返回` 或 `菜单`；12 秒不操作自动退出；停止监听也会清状态
- 鼠标模式期间方向键/Enter 由低层钩子接管：短按 8px、按住加速，`OK` = 单击；
  开关关着时只在这几秒影响物理键盘，开关开着时本来就在接管范围内；`返回`/`菜单` 不拦，用来退出

导航模式的按键可以在 profile 的 `nav.keys` 里改，`nav.exit_key` 指定单击退出的键。

### 动作类型

| type | 字段 | 说明 |
|---|---|---|
| `key` | `vk`、`mods`、`repeat` | 模拟按键，`mods` 可为 `win`/`ctrl`/`alt`/`shift` |
| `scroll` | `delta`、`repeat` | 滚轮，一格 120，正数向上；作用于鼠标所在窗口 |
| `run` | `cmd` | 执行命令或启动程序 |
| `none` | `note` | 接管关：不注入，原生直通；接管开：Router 补发源键（OK/返回/F11 用它避免双触发） |
| `brightness` | `delta` | 主显示器亮度 ±（DDC/CI，`dxva2`，不支持时提示失败） |
| `mouse` | `dx`、`dy` | 鼠标相对移动 |
| `click` | — | 鼠标左键单击 |
| `mode` / `nav_toggle` / `nav_exit` | `mode` | 进入音量·亮度 / 鼠标 / 导航模式，或退出导航 |

## 配置 `config.json`

```jsonc
{
  "device_mark": "01000d_pid&3838",   // 换遥控器时改这里
  "voice_key": {
    "vkey": 116,                       // 语音键的 vkey
    "confirm_window_ms": 250,          // 判定连发的时间窗
    "min_repeats": 2,                  // 至少几次才算长按
    "silence_stop_ms": 700,            // 静音多久算说完（local 模式）
    "min_record_ms": 300               // 短于此视为误触（local 模式）
  },
  "media_voice": {                     // 耳机播放/暂停键 = 语音开关
    "enabled": true,
    "vkey": 179,                       // VK_MEDIA_PLAY_PAUSE
    "swallow_in": ["qoder", "agent"],  // 可省：写了就按名单，不写则凡是配了语音的场景都接管
    "silence_ms": 6000,                // 本地后端：耳机起的会话停嘴多久自动收尾
    "auto_stop_ms": 30000              // 转发热键：耳机起的会话多久释放组合键
  },
  "keys": { "38": "up", "40": "down" },        // vkey -> 语义名
  "actions": { "up": { "type": "key", "vk": 38 } },  // 语义名 -> 动作
  "voice": {
    "input_device": null,              // null=系统默认，或填设备序号
    "sample_rate": 16000,
    "asr_model": "small",              // small/medium/large，界面里的尺寸下拉会写这里
    "asr_language": "zh",
    "paste": true,
    "auto_enter": false,
    "mode": "hotkey",                  // local=自己录音转写（可选）；hotkey=转给 Agent
    "hotkey": {
      "vk": 86,                        // vk 86 = V，即 Ctrl+Shift+V（Qoder 的语音键）
      "mods": ["ctrl", "shift"],
      "trigger": "hold"                // hold=按住；toggle=按下开、松手再按一次关
    }
  },
  "ui": {
    "auto_listen": true,               // 启动即监听
    "start_minimized": true,           // 启动藏在托盘，不闪窗口
    "close_to_tray": true              // 关窗只隐藏继续监听；false = 关窗即退出
  }
}
```

profile 里的 `voice.trigger` 只在 `hotkey` 模式下生效（当前）；`qoder` 和 `agent`
（WorkBuddy）都是 `toggle`（Ctrl+Shift+V / Ctrl+D，每按一次切换开/关）。`local` 模式下这些都不参与。

### 语音键的两种用法

| mode | 行为 | 适用 |
|---|---|---|
| `local` | 遥控器语音键 → 电脑麦克风录音 → 本地 whisper 转写 → 粘贴 | 完全离线；识别效果视模型而定（实测本机不如 Qoder） |
| `hotkey`（当前） | 遥控器语音键 → 转给 Agent 自己的语音快捷键 | 省掉 462MB 模型，用 Qoder 自己的识别 |

`hotkey` 再分两种触发方式，写在 `voice.hotkey.trigger`（或 profile 的 `voice.trigger`）：

| trigger | 注入 | 适用 |
|---|---|---|
| `hold` | 按下发 keydown 不松手，松手才发 keyup | 目标程序是"按住说话" |
| `toggle` | 按下完整按一次组合键，松手再完整按一次 | Qoder 的语音是"按一下开始 / 再按一下停止" |

Qoder 的 `voiceInputComboShortcut` 只在 **keydown** 上做切换（webview 里 `handlePressed`
判断"正在录音就停，否则开始"），keyup 不生效。`qoder` profile 里写的是：

```jsonc
"voice": { "trigger": "toggle" }
```

程序会在按下时完整按一次 Ctrl+Shift+V（开始录音）、松手再完整按一次（停止并出字）；
短于 300ms 的按住会等满 300ms 再发停止，避免第二次按下被 Qoder 当成"还没开始"。
`hold` 也保留，适合目标程序是"按住说话"的场景。

> 两种方式都假设 Qoder 起始状态是 idle。如果它已经停在录音态，先手动点一下停止，
> 否则第一次按下会被当成"停止"。
>
> 副屏任务栏闪动：实测“测试麦克风”不按遥控器也会闪，所以是 **Windows 10 的麦克风使用
> 指示器**（`Shell_SecondaryTrayWnd`）在录音开始/结束时让任务栏露一下，不是 Qoder 浮层，
> 也不是 F5。任何用麦的程序都会触发；AgentRMC 关不掉。要消掉只能从 Windows 侧：
> 任务栏设置里关掉麦克风系统图标（如果有），或者关掉“自动隐藏任务栏”。

#### 耳机播放键也能开关语音

Qoder / WorkBuddy 场景在前台时，蓝牙耳机的**播放/暂停**手势会被接管为语音开关：
按一下开、再按一下关。耳机走 AVRCP，而 Windows 只在**电脑上没有活动播放会话**时
才把它注入成按键（带 `INJ` 标记、没有设备名、也没有 keyup），所以只能做 toggle，
走不了按住逻辑。

- 正在放歌时，播放/暂停被 Windows 直接投给 SMTC 媒体会话，键盘层收不到这一帧，
  接管不了——那种场合用遥控器 F5 开语音。
- 实测双击 = `0xB3`（播放/暂停）、三击 = `0xB0`（下一曲）。三击留给切歌，不抢。
- 生效范围：默认凡是配了语音的场景都接管；要收紧就在 `media_voice.swallow_in`
  写场景名名单。原生版语音设置里有「耳机播放键 = 语音开关」总开关，直接写回配置。
- 结束时机由程序兜底：本地后端读麦克风能量，停嘴 `silence_ms`（默认 6000ms）自动
  收尾；转发热键模式不录音、没有能量信号，`auto_stop_ms`（默认 30000ms）到点释放
  组合键，免得 Ctrl+Shift 一直按住。
- 换键改 `media_voice.vkey`（179 = 播放/暂停），整体关掉用 `media_voice.enabled`。
- 想复现这一帧不用戴耳机：`python rc_probe/sim_avrcp.py 2` 会注入同样的
  「带 INJ、只发 keydown」的 0xB3。

> 音量键不产生任何键码（AVRCP 绝对音量），接不了；耳机麦克风在系统里也没有录音端点。

#### 图形界面里的本地模型开关与尺寸

语音卡片中间那行 `本地模型 [开关] [small ▾] 状态` 就是 `voice.mode` / `voice.asr_model`：

- **关**（默认，hotkey）：启动和切换时都不加载 whisper，语音键转给 Qoder / WorkBuddy 自己识别；
  尺寸下拉只把选择写进配置，不占内存。
- **开**（local）：立即在后台加载所选尺寸，右侧状态显示 `加载中… / 就绪 / 失败`，
  事件流里能看到加载日志；关闭开关会释放模型（约 462MB）。
- 尺寸切换立刻生效：开着就重新加载，关着只记下选择。`large` 实际就是 `large-v3`。
- 两个选择都直接写回 `config.json`，下次启动照旧。`models/` 里没有对应目录时会先联网下载
  （走 `voice.proxy`）；网络不稳就用 `tools\download_model.bat medium` 先下好再切换。
- `测试麦克风` 需要本地模型转写一次：开关关着时临时加载，测完自动释放。

`hotkey` 模式下，按键映射页第一行会显示实际接法，启动时**不会加载本地 whisper 模型**
（省 462MB 内存），默认就是这一种。切到 `local` 模式则会在启动时加载
`models/faster-whisper-<尺寸>`，按键映射页第一行显示「本地 whisper · 按住录音」。

切回 `hotkey` 时默认组合是 **Ctrl+Shift+V**（`vk 86` + ctrl/shift），也就是 Qoder 的语音键。
换成别的组合只改一行：

```jsonc
"hotkey": { "vk": 18, "mods": [], "trigger": "hold" }   // vk 18 = Alt
```

> 实测记录：先试的长按 Alt，注入的 Alt 事件**没能触发**目标的语音输入（`GetAsyncKeyState`
> 能查到 Alt 处于按下状态，说明注入本身没问题，是那个快捷键没被识别）；换 Ctrl+Shift+V 后
> 一次就通了。所以默认用 Ctrl+Shift+V。
>
> 注入方式从 `keybd_event` 换成了 **`SendInput` + 扫描码**（`MapVirtualKeyW` 取 scan code），
> Chromium/Electron 这类应用更认这个——`keybd_event` 只给 vk 时，Chromium 可能拿不到正确的
> `code`，表现就是"按了没反应"。

## 已知限制

### 1. 语音用的是电脑麦克风，不是遥控器的

这不是偷懒，是实测结论。遥控器麦克风在 Windows 上取不到，证据链：

| 尝试 | 结果 |
|---|---|
| 订阅 `D1FF/A001`（4 分钟） | 只有订阅瞬间一个 16 字节全零包，按住语音键期间零音频 |
| 往 `A002` 写 7 种候选命令（`04`/`01`/`00`/`08`/`0a`/16字节×2） | 全部写入成功，`A001` 零回应 |
| 订阅 `6287/6487` | 无数据 |
| `hidapi` 读 HID 报告 | `OSError: read error` |
| `CreateFileW` + `ReadFile`（普通权限） | `err=5 ACCESS_DENIED` |
| `CreateFileW` + `ReadFile`（**管理员**） | 仍然 `err=5` |
| 访问掩码遍历（`FILE_READ_DATA` / `GENERIC_READ` / `FILE_GENERIC_READ` / `READ_DATA\|SYNC` / `+OVERLAPPED`） | **全部 `err=5`**；只有 `READ_ATTRIBUTES`、`SYNCHRONIZE` 能过 |
| 零权限句柄发 `IOCTL_HID_GET_INPUT_REPORT` 等 5 个 IOCTL | 全部 `ERROR_INVALID_FUNCTION` |
| WinRT `HidDevice.FromIdAsync` | 需要 UWP capability，返回 `None` |

结论：`1812` 服务的 HID 接口被 hidbth 驱动独占，DACL 里没有给任何用户（含管理员）`FILE_READ_DATA` 权限，用户态拿不到 256 字节原始报告。而私有 GATT 通道（`D1FF`/`6287`）在 Windows 上既收不到音频、写命令也无回应。

**推测**：遥控器只在极米投影仪的认证环境下才推音频流；在 Windows 里按住语音键，它只发 F5 连发表示"我按了"，并不会真的开麦推流。

> 后续补充：翻 GitHub 同类项目后确认，通用电视盒子遥控器的语音走的是 BLE 的 ATVV 服务
> （`AB5E0001`），不经过 `1812` 的 HID 报告——所以上面这条"从 HID 报告里捞音频"的思路
> 对 ATVV 机型本身就不成立。完整协议见下面第 4 条。

#### 补测：用 Raw Input 绕过 DACL，把 HID 报告直接抓下来

上面那些 `CreateFileW` / `hidapi` 全部 `err=5`，但 **Raw Input 走的是内核投递 WM_INPUT
的路径，不需要 `FILE_READ_DATA`**，这条路之前没试过。补测结果（`rc_probe/rawsize.py`）：

| 项 | 结果 |
|---|---|
| HID 集合数（`hid.enumerate`） | **只有 1 个**：`usage_page=0x0001 / usage=0x0006`，路径结尾 `\KBD` |
| 厂商自定义集合 | **没有**，所以不存在能装音频的自定义报告 |
| 按住语音键 20.6 秒 | 收到 **188 个报告，全部恰好 16 字节**（标准 `RAWKEYBOARD`） |
| 报告内容 | **全是 `VKey=0x74`（F5）**：184 次 keydown + 4 次 keyup，即 4 次按下 + 连发 |
| 非 16 字节报告 | **0 个** |

也就是说：**按住语音键期间，遥控器在 HID 上只发标准键盘报告，一个额外字节都没有**。
配合下面"所有可订阅 GATT 通道零帧"，可以下死结论——**这台遥控器在 Windows 上不推任何
音频，它对电脑来说就是个键盘**。

### 2. 音量键 / 电源键在电脑上收不到：不是没映射，是没信号

遥控器是 13 键，但 `config.json` 里只登记了 8 个方向/功能键 + 语音键。音量 ±、电源
这几颗键**从来没有被捕获到过**，原因不是漏配，而是它们根本不发报文给电脑。

验证方法（`rc_probe/keyprobe.py`）：同时注册三路 Raw Input —— 键盘集合 `0x01/0x06`、
**Consumer Control `0x0C/0x01`（音量/静音/播放就属于这一类）**、系统控制 `0x01/0x80`，
并且把 HID 类型的原始报告也打出来。结果：

| 测试 | 结果 |
|---|---|
| 只注册键盘集合（`rawlisten` / `learn_keys` 的旧做法） | 按音量键无任何输出 |
| 三路全注册 + HID 原始报告，45 秒窗口 | **0 个报文** |
| 改成「随时按、实时打印」再跑 70 秒 | **0 个报文** |
| 按音量键时看 Windows 系统音量 | **完全没变** |

最后一条最关键：连系统音量都没动，说明报文**根本没进 Windows 的 HID 栈**，不是被我们的
过滤器漏掉。剩下的解释只有两种：

- 音量/电源是**红外**发的（电视/投影遥控器的常见做法，这样关机状态也能用），只对投影仪生效
- 或者走厂商私有通道，只认极米主机

**10 秒验证**：用手机相机对着遥控器前端，按音量键——手机摄像头能看见红外，如果看到
灯在闪，就是红外键，电脑端无解（除非外接红外接收器自己解码）。

替代方案：语音说「音量加 / 小声点」，`commands.json` 里已经有这两条规则，走语音链路
反而比按物理键更省事。

### 3. `F11` 是变焦 / 对焦键，且**不能映射，只能直通**

这颗键（`vkey 122` / `0x7A`）实测就是遥控器上的**变焦（对焦）键**，发标准 `F11`，
所以在应用里按下去就是全屏 / 还原。

> 如果 Qoder 正处全屏，按一下这颗键会退回窗口，Windows 任务栏就会露出来。
> 它和语音键（F5）是两颗不同的键，位置挨着，容易误按成"按语音键弹任务栏"。

但**不能给它配映射**。实测：一次按键只产生 **1 个 keydown + 1 个 keyup**（`rc_probe/keyprobe.py`
抓的，没有连发），可是配上 `{"type":"key","vk":122}` 之后按一下会**切两次全屏、看起来像没反应**。

原因是我们的 Raw Input 注册只用了 `RIDEV_INPUTSINK`，**没有 `RIDEV_NOLEGACY`**：

```
遥控器按下 F11
   ├─→ Raw Input（我们的窗口）→ 执行映射 → SendInput 再发一次 F11   ← 第 1 次
   └─→ 传统键盘消息 → 前台窗口（应用）→ 切全屏                      ← 第 2 次
```

两次抵消。所以 `profiles.json` 里 122 的动作是 `{"type": "none"}`——**明确"不拦截"**，
让系统那一次生效。按键映射页会显示成「原生 F11 直通」。

> 接管开着时，原生那一份被钩子吞掉，Router 只补一次，F11 仍然只切一次。
> 不打开「接管原生键」时，这个坑对所有"想改行为"的键都成立：只要原生行为还在，映射就是叠加而不是替换。
> 详见下面第 5 条。

### 4. 若仍想用遥控器麦克风：得先有 ATVV，而这台没有

先把话说清楚：**遥控器麦克风的音频从来不走 HID 报告**。之前"音频可能在 `1812`
的 256 字节透传报告里"的推测是错的，翻 GitHub 上跑通过的项目就能确认——电视盒子
遥控器的语音走的是 BLE 上另一条独立通道 **ATVV**（Google Android TV Voice）。

| 项 | 值 |
|---|---|
| 服务 | `AB5E0001-5A21-4F05-BC7D-AF01F617B664` |
| TX | `AB5E0002` 主机写命令 |
| AUDIO | `AB5E0003` 遥控器 notify，音频帧 |
| CTL | `AB5E0004` 遥控器 notify，状态帧 |
| 编码 | IMA/DVI ADPCM，16 kHz 单声道，4:1 |

实测时序（三个项目一致）：

```
主机                                遥控器
 │── 订阅 0003 / 0004 ──────────────▶│
 │── TX GET_CAPS 0A 01 00 00 03 03 ─▶│
 │◀─ CTL 0x0B 能力帧（codec/帧长）───│
 │        （用户按下语音键）           │
 │◀─ CTL 0x08 请求开麦 ──────────────│
 │── TX MIC_OPEN 0C 00 ─────────────▶│
 │◀═ AUDIO 连续 ADPCM 帧 ═══════════│
 │◀─ CTL 0x0A 同步帧（重置解码器）───│
 │        （用户松开语音键）           │
 │◀─ CTL 0x00 流结束 ────────────────│
 │── TX MIC_CLOSE 0D ───────────────▶│
```

关键是**它是独立 GATT 服务，不碰 `1812`，所以不需要抢 HID 设备、不需要管理员、
也不受 hidbth 的 DACL 限制**。Windows 上直接用 WinRT 订阅就行，这也是
`flydog555/xiaomi-remote-voice-assistant` 的"电脑蓝牙直连"方案。

问题在于：这台极米/Realtek 遥控器**根本没有 `AB5E` 服务**。重新做了一次
UNCACHED 全量发现，8 个服务是：

```
1800 1801 180A 180F 1812 6287 D0FF D1FF
```

`D1FF/A001(notify)+A002(write)` 结构和 ATVV 同形，于是按 ATVV 时序完整试了一轮
（`rc_probe/atvv_probe.py`）：`GET_CAPS v1`（`0A 01 00 00 03 03`）、`GET_CAPS v0`
（`0A 00 04 00 03`）、`MIC_OPEN`（`0C 00`）、`MIC_CLOSE`（`0D`）全部写入成功，
`A001` 一个字节都没回——连 ATVV 的能力帧 `0x0B` 都没有。

结论：这台遥控器的语音**不是 ATVV，也不是能对任意主机开放的私有通道**。结合上面
Raw Input 的实测（按住 20.6 秒只有 188 个标准键盘报告、零非标准报告），从 Windows
这一侧能观察到的全部事实是：

| 观察 | 事实 |
|---|---|
| 按语音键时 HID 上有什么 | 只有 16 字节标准键盘报告，全是 F5 连发 |
| 按语音键时 GATT 上有什么 | 所有可订阅通道（`2a19` / `A001` / `6487`）**零帧** |
| 有没有厂商自定义 HID 集合 | 没有，只有键盘集合 |

所以对电脑来说它就是**纯键盘**。至于音频到底在哪，只剩两种可能：

- **A：录音在投影仪**——遥控器只是"按下/松开"的开关，投影仪用自己的麦克风录音并上传识别。
- **B：录音在遥控器**——但私有推流通道只对极米主机开放（需要主机侧认证/特定命令），
  换任何别的 host 都不开麦。

**30 秒就能定性**：找到遥控器上的麦克风孔（一般在顶部或正面），用胶带贴死，再对着
投影仪说「打开设置」。还识别 → A 成立；识别不了 → B 成立。

想复测（比如换一支小米 RC003-MS / 遥控器 2 Pro）：

```bash
tools\atvv_probe.bat                  # 30 秒窗口，按提示按住语音键
tools\rawtest.bat                     # HID 原始报告尺寸嗅探，带倒计时
python rc_probe\atvv_probe.py --seconds 40
```

脚本会先找 `AB5E`，找不到退化试 `D1FF`，把每一帧写进 `rc_probe/atvv_raw.jsonl`，
拿到音频就解成 `rc_probe/atvv_16k.wav`。换一支真 ATVV 遥控器，这套代码就能直接
把遥控器麦克风接进 whisper，电脑麦克风可以彻底不用。

### 5. 核心限制：默认映射是"叠加"，不是"替换"

遥控器的按键**本来就会作为普通键盘事件送到前台窗口**（Raw Input 只注册了
`RIDEV_INPUTSINK`，没有 `RIDEV_NOLEGACY`）。**默认（接管关）** 时：

| 你的映射 | 实际结果 |
|---|---|
| 映射成和原生一样（如 OK→Enter） | Enter 发两次 |
| 映射成别的键（如 主页→Ctrl+T） | 原生 Home + Ctrl+T 都触发 |
| 映射成 `none` | 只有原生那一次，正常 |

打开「接管原生键」后，原生那一份被钩子吞掉，上面这些叠加结果不再成立，见下一小节。

默认（接管关）时，agent/qoder 里 OK、返回、F11 全是 `none`：**原生 Enter / Esc / F11
本来就是对的，不再多注入一次**。上下滚轮和主页手势是"叠加"，原生方向键/Home 也会落到
聊天输入框里（最多移动一下光标，无害）。打开接管后，这些原生副作用也被吞掉，改由
Router 按映射处理。

#### NOLEGACY 实测：为什么不能做按设备的真替换（2026-10-27 补测）

之前写过"`RIDEV_NOLEGACY` 会把实体键盘一起废掉"，这次实测证明**那句话是错的**，
但结论仍然是——用户态做不了按设备的真替换：

| 探测 | 做法 | 结果 |
|---|---|---|
| `rc_probe/nolegacy_probe.py` | 本进程注册 NOLEGACY，然后 `SendInput` F13 给自己的窗口 | 只收到 `WM_INPUT`，**没有 `WM_KEYDOWN`** |
| `rc_probe/nolegacy_probe2.py` | A 进程注册 NOLEGACY；B 进程建窗口并抢焦点；A 注入一次、B 自己注入一次 | B 收到 **down/up/down/up 四组**，全部正常 |

结论：

- `RIDEV_NOLEGACY` 只抑制**注册它的进程**的传统消息，不是全系统；
- 因此不开接管时，前台程序照样收到遥控器的原生按键，开关它没有任何拦截效果；
- 同时别的进程用 `SendInput` 注入的键也完全不受影响，所以"重注入"不是问题所在。

想在用户态按设备真正"吞掉"遥控器的某个键，只剩内核过滤驱动（比如 Interception，
要管理员、要签名驱动、按设备句柄过滤）或硬件桥接。这个项目默认不走这两条路，把
映射设计成"原生行为可接受"的叠加；需要真替换时用下一小节的用户态接管开关。

#### 可选：「接管原生键」（所有场景，2026-10-30）

映射页右下角新增开关（`config.json → ui.native_takeover`，默认关）。打开后，用低层钩子
（`WH_KEYBOARD_LL`）把**当前前台场景**的所有映射键原生事件吞掉，并把同一个 keydown/keyup
直接喂给 Router，再按单击/双击/长按注入。**物理键被吞后 Raw Input 不保证还会到**，
所以不能只依赖它；万一 Raw Input 也到了，会用 50ms 去重避免执行两次。

- 双击方向键现在可以真替换：左键双击=退格，不会再先原生左移两次。
- 代价：用户态只能按 VK 处理，**实体键盘的同名键（方向键/Enter/Esc/F11 等）也会按当前场景的映射执行**，
  不再走原生；双击键的单击要等 0.3s 的双击窗口。
- 所有场景生效：切场景就换成该场景的键集合；关掉开关立即放行。语音键 F5 不在接管集合里。
- 这不是按设备过滤；要"只吞遥控器、不伤键盘"仍然只能上内核过滤驱动或硬件桥接。

还有两个绕不开的副作用：

- **滚轮作用于鼠标所在窗口**，不是前台窗口。用遥控器时把鼠标停在聊天区。
- **语音键的原生 F5 仍会送到前台**：浏览器里可能刷新，IDE 里可能触发 F5 绑定。
  用语音时建议焦点停在 Agent/Qoder 的聊天输入框。

四种选择，按侵入性排序：

1. **只映射"原生行为无害"的键**（默认，最轻）：OK/返回/F11 用 `none`；需要额外动作时
   用长按或原生重复无害的键（如主页）。
2. **打开「接管原生键」开关**（可选，所有场景）：吞掉映射键原生事件再注入，方向键双击也能真替换；
   代价是实体键盘同名键也按当前场景映射处理，双击键的单击延迟 0.3s。
3. **内核过滤驱动**（Interception 等）：按设备吞键、再注入，代价是管理员权限 + 驱动签名，
   以及提权窗口的 UIPI 问题。
4. **硬件桥接**：ESP32-C6 / CH585D 从 BLE 空口直接抓，根本不进 Windows 键盘栈
   （参考 `flydog555/xiaomi-remote-voice-assistant` 的做法）。

## 参考项目

写这部分时参考的都是实测跑通过的开源实现，不是纸上协议：

| 项目 | 平台 | 价值 |
|---|---|---|
| [flydog555/xiaomi-remote-voice-assistant](https://github.com/flydog555/xiaomi-remote-voice-assistant) | Windows | WinRT 直连 ATVV，`core/ble_service.py` 是最完整的 Windows 参考；还给了 ESP32-C6 / CH585D 硬件桥接，用来绕开 Windows 内核吞掉的返回键与音量键 |
| [godarrenw/mi_remote_control](https://github.com/godarrenw/mi_remote_control) | macOS | `DESIGN.md` 把两条通道（HID 按键 / ATVV 语音）讲得最清楚，附完整时序图 |
| [dailiuyi/xiaomi-remote-voice-linux](https://github.com/dailiuyi/xiaomi-remote-voice-linux) | Linux | BlueZ + PipeWire 建专属音源，适合当"音频链路怎么接进系统"的范例 |
| [HD838A/remote-mic-app](https://github.com/HD838A/remote-mic-app) | iOS/macOS | `TECHNICAL.md` + 兼容性 bug 记录，列了 RC001/RC003 的服务清单，能用来对比"哪些遥控器真有 AB5E" |

顺带一个可以复用的结论：这些项目都验证了**按键走 HID、语音走 ATVV，两条通道互不
干扰**。所以我们现在的做法（Raw Input 收键 + 电脑麦克风收音）在架构上是对的，
只是把 ATVV 那一半换成了系统麦克风。


## 待办

- [x] 确认按键映射（8 个键已确认，`F5`=语音键长按连发）
- [x] 验证语音链路（本地 whisper，4.5s 音频 1.1s 转写）
- [x] 验证遥控器麦克风是否可用（结论：Windows 上不可用，这台没有 ATVV 服务）
- [x] 多场景 Profile（按前台窗口切换键映射）
- [x] Qt 图形界面（波形 / 识别结果 / 事件流 / 电量）
- [x] 图形界面加「按键映射」页（跟随前台自动切换，可打开/热加载 profiles.json）
- [x] 查清音量键为何不能映射（三路 Raw Input 零报文 + 系统音量无变化 → 报文没进 Windows）
- [ ] 确认 `F11` 对应物理键
- [ ] 用手机摄像头确认音量键是不是红外（是则电脑端彻底无解）
- [x] 语音键 hotkey 模式：转给 Qoder 的 Ctrl+Shift+V（按下开、松手关）
- [x] 多 Agent profile：Qoder / Qoder CN 继承 agent 键位，语音用 toggle
- [x] 长按连滚 + 主页"点击到顶 / 长按到底"双动作
- [ ] Qoder 语音前自动聚焦输入框（盲点不可靠，待做一次性校准）
- [x] default 的 Home 不再注入 Win（按一下弹开始菜单/任务栏的坑）
- [x] 跨屏移动/缩放：PassThrough DPI + 原生拖动 + 切屏回夹 + 几何记忆
- [x] 启动即监听 + 单实例保护 + 托盘常驻（关窗只隐藏、启动不闪窗）
- [x] 实测 NOLEGACY 的真正语义（按应用生效，用户态无法拦截原生键）
- [x] 耳机播放键接管语音（前台场景吞键；AVRCP 无 keyup 只能 toggle；有活动播放会话时系统不注入按键，抢不到）
- [x] 图形界面切换本地模型开关与 small/medium/large 尺寸（写回 config.json，热加载/释放）
- [ ] TTS 播报 Agent 回复，形成语音闭环
- [ ] 若换 ATVV 遥控器（RC003-MS / 遥控器 2 Pro），把 `rc_probe/atvv_probe.py` 的解码链并进主程序
