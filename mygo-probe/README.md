# mygo-probe — MyGo 桌宠生死线探针

用最小代码验证 [MyGo](https://github.com/egoist/mygo)（纯 Go 桌面框架，无 cgo）
能不能承载 AgentRMC 桌宠形态。结论（2026-11-07，Win10 19045，4K 200% DPI）：
**两种模式都没过**——原生 UI 模式透明白底（红板判定实锤），Web 模式 WebView2
控制器创建失败（E_UNEXPECTED）；穿透/拖动/无边框/置顶/快捷键这些 API 面全部通过。

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

## Web 模式（WebView2）——也挂了，且更早

`cmd/webprobe`：静态页纯 Go 嵌入（`mygo.SetFrontend` + go:embed，无 Bun），
窗口选项与原生探针一致。结果：**WebView2 控制器创建失败（E_UNEXPECTED
0x8000FFFF）×3 后放弃，页面永不渲染**，窗口空壳。已排除：

| 假设 | 实验 | 结果 |
| --- | --- | --- |
| `Transparent` 触发 | `-transparent=false` | ❌ 照样 E_UNEXPECTED |
| `Frameless` 触发 | `-frameless=false`（全默认窗） | ❌ 照样 |
| 官方 loader 缺失（走运行时内部导出） | 拷入 AgentRMC 在用的 `WebView2Loader.dll` | ❌ 照样 |
| user data folder 冲突 | `WEBVIEW2_USER_DATA_FOLDER` 指全新目录 | ❌ 照样 |
| 本机 WebView2 坏了 | 反例：同一时刻 AgentRMC / Adobe / Lively / 电脑管家的 WebView2 全部正常（运行时 Evergreen 157.0.4341.42） | 运行时没问题 |

异常状态组合：环境创建**报成功**、浏览器进程**从未被拉起**（进程列表只有
AgentRMC/电脑管家两棵 msedgewebview2 树）、`%LOCALAPPDATA%` 下 **user data
目录从未创建**（MyGo 在 `startEnvironment` 里会先 MkdirAll）、事件查看器无崩溃记录。
三者互相矛盾，指向 MyGo 自身的 purego COM 完成回调路径在本机不工作（`b.env` 疑似
停留在 0，`comCall(0,…)` 正好返回 E_UNEXPECTED），属上游 bug 或运行时 157 兼容问题。

## 结论

**本机（Win10 19045 + WebView2 157）MyGo v0.8.1 两种模式都过不了桌宠生死线**：
原生模式透明白底、Web 模式 WebView2 起不来。穿透/拖动/无边框/置顶这些 API 面
是好的，问题集中在渲染与 WebView2 集成层。对 AgentRMC：议题关闭，除非上游修复
（mygo-probe 即现成复现仓库）。

## 局限 / 未验证

- E_UNEXPECTED 的确切上游根因未深挖（purego COM 回调路径 vs 运行时 157 兼容），
  有兴趣可拿本仓库报 issue。
- 未验证：任务栏隐藏的人眼确认、多屏、双击 DragWindow 最大化、美术主题背景合成
  （只验了 rgba 卡片这一层）。
- 控制台窗口：默认 `go build` 带控制台，探针无所谓，正式出包加 `-H=windowsgui`。
- MyGo v0.8.1，依赖 purego（纯 Go 调系统 API）——交叉编译优势成立的前提成立。
- Web 探针构建：`go build -o webprobe.exe ./cmd/webprobe`（`WebView2Loader.dll`
  已拷在目录里，实测对结果无影响，留作变量已排除的记录）。
