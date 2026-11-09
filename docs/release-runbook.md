# AgentRMC 发布 Runbook

一次可复用的发版流程。按顺序执行，每步都有可核对读数；`X.Y.Z` 替换成本次版本号。

---

## 0. 原则

- **版本号只在用户明确说「升版」时改**；出包本身不升版（安装器原地覆盖）。
- 版本唯一来源：`plugins/agentrmc-bridge/VERSION`；Native 与插件同号。
- 一次升版只改一次号，改完再出包；改完跑 `sync-plugin-version.ps1` 灌六处清单。

## 1. 前置检查

```bash
git branch --show-current && git log --oneline -5 && git status --porcelain
cat plugins/agentrmc-bridge/VERSION
grep 'project(' native/CMakeLists.txt | head -1
```

- 要发布的代码必须已提交，工作区无未提交源码改动。
- 记录当前装机状态（可选）：`curl --noproxy '*' -s http://127.0.0.1:45915/state`。

## 2. 出包前清 build 残留

`installer.nsi` 用 `File /r` 打包整个 `native/build/Release`，而 `build.bat` 的 robocopy 是 `/E`（不镜像删除），残留会进包：

```bash
rm -f native/build/Release/.guide_seen        # 否则新装用户跳过引导
rm -f native/build/Release/build-main.log
rm -f native/build/Release/themes/xiaoai-whale/main.png \
      native/build/Release/themes/xiaoai-whale/panel.png
```

> 临时验证 exe 也不要留在 `build/Release`：installer 只排除 `*Test.exe`。

## 3. 构建 + 打包

```bash
unset https_proxy http_proxy HTTPS_PROXY HTTP_PROXY
cmd //c "native\\tools\\package.bat"
```

- 在后台执行并等完成通知，不要 `sleep` 轮询。
- 成功后检查日志末尾有 `Done -> dist\AgentRMC-Setup.exe`。
- 版本一致性已由 package.bat 内置的 `check-version.ps1` 检查。

## 4. 静默装机 + 验证

```bash
cmd //c "native\\tools\\install-silent.bat"
powershell -NoProfile -Command "Start-Process 'D:\apps\AgentRMC\AgentRMCNative.exe'"
curl --noproxy '*' -s -m 3 http://127.0.0.1:45915/state
sha256sum native/build/Release/AgentRMCNative.exe D:/apps/AgentRMC/AgentRMCNative.exe
```

通过判据：`/state` 有响应；两个 SHA256 完全一致；进程路径是 `D:\apps\AgentRMC`。

## 5. dist 整理与发布说明

- 旧版本插件 zip 移入 `dist/archive/`，顶层只留当前版本。
- 写 `dist/release-notes-vX.Y.Z.md`（中英双语，GitHub release body 直接复用）。
- 核对：

```bash
ls -la dist/AgentRMC-Setup.exe dist/agentrmc-bridge-X.Y.Z.zip
unzip -l dist/agentrmc-bridge-X.Y.Z.zip | head
diff -r plugins/agentrmc-bridge <解压目录>    # 应无差异
```

## 6. GitHub 发布

```bash
git add ... && git commit -m "..." && git push origin <feature-branch>
git checkout master && git merge --ff-only <feature-branch> && git push origin master
git tag -a vX.Y.Z -m "..." && git push origin vX.Y.Z

gh release create vX.Y.Z \
  --title "..." --notes-file dist/release-notes-vX.Y.Z.md \
  dist/AgentRMC-Setup.exe \
  dist/agentrmc-bridge-X.Y.Z.zip \
  dist/agentrmc-bridge-qoder-X.Y.Z.zip
```

- **资产必须随 `create` 一次传完**；发布后再补会 422，删掉重建会永久占用 tag 名。
- 核对：`gh release view vX.Y.Z --json assets` 应列出三个资产。

## 7. 官网发布

本地检出：`C:\Users\PC\Documents\Qoder\2026-08-28\1b3e82f9\lig-site`（推 `main` 触发 Cloudflare Pages `ligdesign`）。

1. `agentrmc-update.json`：`version / releaseDate / title / titleEn / notes / notesEn`。
2. `agentrmc.html`：版本徽标、轮播 `alt`。
3. `assets/i18n.js`：中英文案（截图 alt、注意 video 素材与版本要一致）。
4. **每次改 `i18n.js` 都把全站 `i18n.js?v=NN` 一起 +1**（`agentrmc/index/liquiddesk/lig-editor/kuaixiu/themes/404`）。
5. 自检与推送：

```bash
node --check assets/i18n.js
node tools/publish.mjs --out dist     # 断链 + 重定向自检必须全绿
git add ... && git commit && git push origin main
```

6. 线上验收（三条都过）：

```bash
# a) Cloudflare 部署成功（API）
#    GET /accounts/{id}/pages/projects/ligdesign/deployments
#    最新 commit 的 latest_stage.status == success
# b) 页面版本号 / 缓存号 / 新文案
curl -H "User-Agent: Mozilla/5.0 ... Chrome/140" \
     "https://ligdesign.win/agentrmc?t=$(date +%s)"
# c) 新图片内容一致（不要只看 200）
curl -H "User-Agent: ..." -o /tmp/x.jpg "https://ligdesign.win/assets/img/xxx.jpg?t=$(date +%s)"
sha256sum /tmp/x.jpg <本地文件>
```

## 8. 截图与素材

脚本（版本号缺省读 `plugins/agentrmc-bridge/VERSION`，输出 `rc_probe/shots<数字>/`）：

```bash
python rc_probe/gen_release_shots.py [X.Y.Z]        # 主窗六页 × 主题矩阵
python rc_probe/gen_release_shots_extra.py [X.Y.Z]  # 使用引导页 + 桌宠语音小窗
```

- WebView2 内容**不能用 `--shot`**（`QWidget::grab` 只出白块），必须真实窗口 + `PrintWindow(PW_RENDERFULLCONTENT)`。
- 页签切换：UIA 找元素 + 物理点击；纯 Qt 对话框/桌宠才用 `--shot-settings` / `--shot-pet`。
- 演示待办：`POST http://127.0.0.1:45915/notify`，字段 `event/title/message/session_id/project`；
  `permission|question` → 等你处理，`stop` → 待验收，`progress` → 进行中。
- 首图讲定向派送：选中 DSH 会话（UIA 找 `DeepSeek Harness：` 行矩形并点击），
  再往「派回这条会话的指令」输入框填一句命令。**Chromium 里 `keybd_event` 逐字发 unicode 会乱码**，
  UIA ValuePattern 也不可用 → 用 PowerShell `Set-Clipboard` + 点框后 Ctrl+V（用完还原剪贴板）。
- 脚本必须「先备份、`finally` 还原」`config.json` 与 `work-items.json`；
  **备份要在任何修改之前**做（否则会把已改脏的状态当原件）。
- 出图为 1600 宽 JPG q88；站点与 README 复用同一份。

## 9. 收尾

```bash
# 演示数据已清、config 已还原（主题/配色/voice_mode 等）
# 删除临时脚本与 *.shot_bak
git status --porcelain              # 只允许预期文件
curl --noproxy '*' -s http://127.0.0.1:45915/work-items   # 活动卡数量回到发布前
```

## 10. 验收清单

| 项 | 通过判据 |
|---|---|
| 版本 | cmake = nsi = exe = VERSION，`check-version.ps1` / `sync-plugin-version.ps1 -Check` 全绿 |
| 安装包 | 时间戳晚于最后一次源码改动；日志 `Done` |
| 装机 | 安装目录 exe SHA256 == build 产物；`/state` 有响应且路径为 `D:\apps\AgentRMC` |
| 插件包 | zip 解压后与 `plugins/agentrmc-bridge` 逐文件一致 |
| GitHub | release 含 Setup + 两个 zip；tag 指向发布提交 |
| 官网 | 部署 commit 的 stage=success；版本号/缓存号/新文案/新图 SHA 都对 |
| 数据 | 演示待办清零，config / work-items 恢复原值 |

## 11. 已知坑

- **cmd 中文注释**：CP936 下连续中文 `rem` 注释会触发解析偏移，打出假的「不是内部或外部命令」。`build.bat` 已全改英文注释。
- **`.guide_seen` 进包**：会让新装用户跳过引导，出包前必须删。
- **robocopy `/E` 不删旧文件**：主题等目录的陈旧素材会一直被打进包，出包前清 `build/Release`。
- plan/探索类文档与截图脚本放 `rc_probe/`，发布产物放 `dist/`（`dist` 被 gitignore）。
- `git show <rev>:<path>` 在 Git Bash 需 `MSYS_NO_PATHCONV=1`。
- curl 本机必须带 `--noproxy '*'`（代理会拦 127.0.0.1）。
