# mygo-probe — MyGo 桌宠生死线探针

用最小代码验证 [MyGo](https://github.com/egoist/mygo)（纯 Go 桌面框架，无 cgo）
能不能承载 AgentRMC 桌宠形态。结论（2026-11-07，Win10 19045，4K 200% DPI）：
**鼠标穿透/拖动/无边框/置顶都通过，但逐像素透明 FAIL——原生 UI 模式白底。**

## 生死线验证结果

| 验证项 | 方法 | 结果 |
| --- | --- | --- |
| 逐像素透明 | **红板判定**：探针身后垫纯红窗体，开窗前后窗角三段采样 | ❌ **白底实锤**。pre=(255,0,0) 红 → mid=(255,255,255) 白 → post=(255,0,0) 红。`Transparent: true` 在 Windows 原生 UI 模式不生效（DirectComposition/WS_EX_NOREDIRECTIONBITMAP 路径在，但交换链没透） |
| ~~早期误判~~ | 最初用「开窗 vs 关窗」比对，身后恰好是白色窗口，白vs白相等被误判成透明 | 教训：透明判定必须垫已知颜色背景板 |
| 置顶 | WS_EX_TOPMOST | ✅ |
| 不进任务栏 | SkipTaskbar 选项（未逐项目检任务栏） | ⚠️ 设置生效，未人眼复核 |
| 透明底 PNG 立绘 | ui.Image 渲染鲸鱼娘 idle 帧 512→200 | ✅ |
| rgba 半透明卡片 | ui.RGBA(18,26,42,0.55)，亮像素计数 | ✅ 5732 亮像素（文本渲染出） |
| **鼠标穿透** | 命中测试：开→命中下层窗；关→命中自己；往返 | ✅ `SetIgnoreMouseEvents` 可靠 |
| 拖动 | 按住立绘 DragWindow，16 步合成输入 | ✅ 位移 +192,+112 与输入 1:1 跟手 |
| 全局快捷键 | Ctrl+Alt+T 切穿透（穿透开时按钮点不到的兜底） | ✅ 两次往返成功 |
| 生命周期 | WM_CLOSE 退出 | ✅ exit 0 干净退出 |
| 体积 | `go build` 产物 | ✅ 16MB 单 exe，零 DLL |

自动化验证脚本：`verify/probe_check.py`（纯 ctypes，无第三方依赖），
子命令 `probe`（样式+穿透）/ `pix`（像素采样）/ `drag`（拖动）/ `after`（关窗对比）/ `close`；
**透明判定以 `verify/transparency_check.py` 为准**（红板三段采样，一键全自动：
标定探针位置 → 垫红板 → pre/mid/post 采样 → 全屏截图 `shot3_redboard.png`）。
红板脚本 `verify/redboard.ps1`。全屏截图（人眼复核用）在 `%TEMP%\mygoprobe-shots\`。

## 构建运行

```bash
# 需要 Go 1.27+（本机装在 D:\apps\go）
cd mygo-probe
export GOPROXY=https://goproxy.cn,direct
go build -o mygo-probe.exe .      # 正式版可加 -ldflags -H=windowsgui 去掉控制台
./mygo-probe.exe                  # 从本目录运行
```

操作：按住鲸鱼娘拖动窗口；「切换穿透」按钮或 Ctrl+Alt+T 切换鼠标穿透。

## 局限 / 未验证

- **透明失败**只在「原生 UI 模式 × Windows」测得；Web 模式（WebView2，官方支持
  透明 DefaultBackgroundColor）未测，需要 Bun 构建，是潜在的另一条命。
- 未验证：任务栏隐藏的人眼确认、多屏、双击 DragWindow 最大化、美术主题背景合成
  （只验了 rgba 卡片这一层）。
- 控制台窗口：默认 `go build` 带控制台，探针无所谓，正式出包加 `-H=windowsgui`。
- MyGo v0.8.1，依赖 purego（纯 Go 调系统 API）——交叉编译优势成立的前提成立。
