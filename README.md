# AgentRMC

把极米/小米系 BLE 遥控器改造成电脑 Agent 的输入终端。

按键走标准 HID（零驱动），语音用本地 whisper 转写，按前台窗口自动切换键映射。

```
遥控器 → HID → Raw Input ─┬→ 按键 → 场景 Profile → 动作（模拟按键/滚动/启动程序）
                          └→ 语音键 → 录音 → whisper ─┬→ 本地指令表 → 直接执行
                                                       └→ 粘贴给 Agent
```

## 快速开始

```bash
git clone <repo> && cd AgentRMC

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
├── agentrc.py              核心：Raw Input 监听、语音状态机、ASR、场景、指令
├── agentrc_gui.py          Qt 图形界面
├── config.json             语音键码、麦克风、ASR 参数
├── commands.json           语音指令表（说"打开浏览器"直接执行）
├── profiles.json           场景表（按前台窗口切换键映射）
├── requirements.txt
├── assets/icon.ico         应用图标
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
| 来源 | 极米「快乐星球 O3」投影仪配套遥控器，13 键 |

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
| 待确认 | 122 `0x7A` | `0x57` | `F11` |

### 语音键的识别特征

语音键不是"按一下发一个码"，而是**按住期间以约 31ms 间隔连续发送 keydown**，松手才发 keyup。这个特征正好可以用来区分遥控器和实体键盘：

```
keydown  keydown  keydown  ...  keydown   keyup
  0ms     31ms     62ms          2.4s
```

程序利用它做判定：第一次 keydown 先挂起 250ms，若期间收到第二次，判定为遥控器长按并开始录音；否则判定为实体键盘单击，把这次按键补发出去。

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
```

模型放在 `models/faster-whisper-small/`，程序会优先加载本地目录，不需要联网。

> HuggingFace 直连不通时，`huggingface-hub` 新版对 `HTTPS_PROXY` 支持有问题（会报 502）。
> 所以 `download_model.bat` 直接用 `curl -x` 走代理抓文件，绕开这个库。
> 代理地址在 bat 开头的 `set PROXY=` 改。

### 图形界面（推荐）

双击 **`release\AgentRMC\AgentRMC.exe`**。

界面包含：

- **顶部状态** —— 运行状态、遥控器电量、当前命中的 Profile
- **语音面板** —— 实时波形（录音时变红）、识别结果、耗时
- **事件流** —— 按键 / 识别 / 指令 / 粘贴的实时记录，按类型着色
- **信息栏** —— 设备、语音键码、模型、指令条数

两个按钮：`开始监听` / `测试麦克风`（录 4 秒并转写，用来验证麦克风选对了没）。

源码方式启动：

```bash
.venv\Scripts\python.exe agentrc_gui.py
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

按住语音键说话，松手后约 1 秒，识别结果会自动粘贴到当前焦点窗口。

### 实测性能

| 指标 | 值 |
|---|---|
| 模型加载 | 0.9s（本地） |
| 4.5s 音频转写 | 1.1s（约 4 倍实时） |
| 中文准确率 | 测试句 100% 正确 |
| 硬件 | CPU / int8 量化 |

## 语音指令

识别出的文本会**先过一遍本地指令表**，命中就直接执行；没命中才粘贴给 Agent。这样常用操作不用绕一圈 Agent。

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
| `agent` | workbuddyai | 滚动对话 | — | **发送** | Esc | 跳到顶部 |
| `default` | 其他 | ↑ / ↓ | ← / → | Enter | Esc | Win |

`agent` 这个 profile 是给你和 WorkBuddy 对话用的：上下翻聊天记录、OK 直接发送、菜单键换行（`Shift+Enter`）。

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

### 动作类型

| type | 字段 | 说明 |
|---|---|---|
| `key` | `vk`、`mods`、`repeat` | 模拟按键，`mods` 可为 `win`/`ctrl`/`alt`/`shift` |
| `scroll` | `delta` | 滚轮，一格 120，正数向上 |
| `run` | `cmd` | 执行命令或启动程序 |

## 配置 `config.json`

```jsonc
{
  "device_mark": "01000d_pid&3838",   // 换遥控器时改这里
  "voice_key": {
    "vkey": 116,                       // 语音键的 vkey
    "confirm_window_ms": 250,          // 判定连发的时间窗
    "min_repeats": 2,                  // 至少几次才算长按
    "silence_stop_ms": 700,            // 静音多久算说完
    "min_record_ms": 300               // 短于此视为误触
  },
  "keys": { "38": "up", "40": "down" },        // vkey -> 语义名
  "actions": { "up": { "type": "key", "vk": 38 } },  // 语义名 -> 动作
  "voice": {
    "input_device": null,              // null=系统默认，或填设备序号
    "sample_rate": 16000,
    "asr_model": "small",              // tiny/base/small/medium
    "asr_language": "zh",
    "paste": true,
    "auto_enter": false
  }
}
```

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

> 后续补充：翻 GitHub 同类项目后确认，遥控器语音走的是 BLE 的 ATVV 服务（`AB5E0001`），
> 根本不经过 `1812` 的 HID 报告——所以上面这条"从 HID 报告里捞音频"的思路本身就不成立。
> 完整协议与实测见下面第 3 条。

### 2. `F11` 对应的物理键未确认

用 `run.bat --list-keys` 按一遍即可确定，不影响使用。

### 3. 若仍想用遥控器麦克风：得先有 ATVV，而这台没有

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

结论：这台遥控器的语音**不是 ATVV，也不是能对任意主机开放的私有通道**，麦克风
只在极米投影仪的认证环境里工作。想在 Windows 上用它，硬件层面就没门。

想复测（比如换一支小米 RC003-MS / 遥控器 2 Pro）：

```bash
tools\atvv_probe.bat                  # 30 秒窗口，按提示按住语音键
python rc_probe\atvv_probe.py --seconds 40
```

脚本会先找 `AB5E`，找不到退化试 `D1FF`，把每一帧写进 `rc_probe/atvv_raw.jsonl`，
拿到音频就解成 `rc_probe/atvv_16k.wav`。换一支真 ATVV 遥控器，这套代码就能直接
把遥控器麦克风接进 whisper，电脑麦克风可以彻底不用。

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
- [ ] 确认 `F11` 对应物理键
- [ ] TTS 播报 Agent 回复，形成语音闭环
- [ ] 若换 ATVV 遥控器（RC003-MS / 遥控器 2 Pro），把 `rc_probe/atvv_probe.py` 的解码链并进主程序
