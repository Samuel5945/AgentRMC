---
name: agentrmc-opensquilla-bridge
description: AgentRMC 桌宠联动桥——把 OpenSquilla 的会话事件（一轮跑完 / 等授权 / 等选方案）推给 AgentRMC 桌宠出气泡和语音，并取回桌宠派回会话的定向指令。Use when the user asks to install / verify / troubleshoot the AgentRMC pet bridge for OpenSquilla, wants session-finish or approval notifications as pet bubbles/TTS, or reports the bridge is down / silent / noisy.
---

# agentrmc-opensquilla-bridge：OpenSquilla → AgentRMC 桌宠桥

本目录是一个**常驻桥脚本**（`opensquilla-pet-bridge.mjs`，零依赖）+ 它的自启/看门狗
脚本。桥已作为技能装进本目录，开机自启 + 计划任务看门狗会自动把它拉起来——
日常不需要手动跑，这个文件主要用于排障和手动验证。

## 它做什么

```
OpenSquilla 网关 :18791 (ws)
        │  session.event.done / approval_required / plan_presentation / turn_committed
        ▼
opensquilla-pet-bridge.mjs
        │  POST 127.0.0.1:45915 /notify（与 ZCode/Qoder/WorkBuddy 的 hooks 完全同款）
        ▼
AgentRMC 桌宠：气泡 + TTS + 把 OpenSquilla 窗口拉回前台
        │  GET /dispatch/pending（1.5s 轮询，反向定向派发）
        ▼  sessions.resolve 守卫 → sessions.send 注入 → POST /dispatch/ack 回执
```

## 本机要点（2026-11-15 适配，没有 node.exe）

- **JS 引擎是 ZCode 的 Electron 当 node 用**：`ELECTRON_RUN_AS_NODE=1` +
  `%LOCALAPPDATA%\Programs\ZCode\ZCode.exe`（Node 24.21，自带 WebSocket）。
  任何手动跑脚本的命令都要带这两个，别写 `node`。
- 自启：`shell:startup\OpenSquillaPetBridge.vbs`；看门狗：计划任务
  `OpenSquillaPetBridgeWatchdog`（每 5 分钟，桥不在拉起、心跳超 3 分钟杀掉重启）。
- 目录里的 `startup-bridge.vbs` / `ensure-bridge.ps1` 是本机适配版
  （仓库原版写死 `C:\Users\PC` 且假定 node.exe），升级桥重新拷贝时别覆盖它们。
- 目录里的 `.vbs`/`.ps1` 必须保持纯 ASCII：wscript/PowerShell 按 ANSI(GBK)
  解析，UTF-8 中文注释会吃换行吃掉代码行。

## 常用排障命令

```powershell
# 1) 桥在不在（应当有至少一个 ZCode.exe 进程命令行里带本目录的 .mjs）
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*opensquilla-pet-bridge.mjs*' }

# 2) 手动验一次通知链路（桌宠要开着；会真的冒一条气泡）
ELECTRON_RUN_AS_NODE=1 "$env:LOCALAPPDATA\Programs\ZCode\ZCode.exe" `
  "$env:USERPROFILE\.agents\skills\agentrmc-opensquilla-bridge\opensquilla-pet-bridge.mjs" --selftest

# 3) 手动补位一次（等同看门狗干的事）
powershell -NoProfile -ExecutionPolicy Bypass -File `
  "$env:USERPROFILE\.agents\skills\agentrmc-opensquilla-bridge\ensure-bridge.ps1"

# 4) 日志与心跳
Get-Content "$env:TEMP\pet-bridge.log" -Tail 30
Get-Content "$env:TEMP\pet-bridge-watchdog.log" -Tail 10
```

- **免打扰**：`New-Item "$env:TEMP\opensquilla-pet-bridge.pause"`，恢复删掉它。
  免打扰只静音提醒，不影响攒账/内联应答。
- **重要**：别在 agent 的 tool call 里直接起桥当常驻进程——这台机器上工具调用
  派生的进程活不过那次调用。常驻必须靠计划任务/shell:startup 拉起，拉起后在
  下一个 tool call 里复查进程还在不在。
- 纯逻辑自检（不连网关不发通知）：`--selftest-mapping`。

## 参数速查

`--raise <exe>`（默认 OpenSquilla.exe）/ `--no-raise` / `--include-cron` /
`--relay`（结束提醒附回复开头，自启已带）/ `--log <path>` / `--selftest` /
`--no-dispatch`。完整参数表见 README.md。

更多背景（事件覆盖、幂等口径、已知边界）看本目录 README.md 和仓库
`plugins/opensquilla-bridge/README.md`。
