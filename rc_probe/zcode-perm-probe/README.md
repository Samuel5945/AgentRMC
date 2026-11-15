# zcode-perm-probe —— ZCode 决策式 PermissionRequest 真机探针（2026-11-15）

研究记录：结论与落地蓝图见 [docs/prompt-optimization-plan.md §4](../../docs/prompt-optimization-plan.md)。
探针已卸载，插件缓存已还原；本目录留档脚本与原始 capture。

## 文件

- `probe.js` —— 探针本体：读 stdin 的 hook payload，落盘 `capture.jsonl`，
  仅对命令含 `PROBE-ALLOW-ME / PROBE-DENY-ME / PROBE-SLOW-ME` 的 Bash 授权返回
  allow / deny / 睡 25s（超时），其余一律静默直通（不改变正常授权行为）。
- `capture.jsonl` —— 真机捕获：27 条，含 UserPromptSubmit / stop / turn-start /
  PermissionRequest（build 模式关沙箱 Bash，riskLevel=high）的完整 payload。
- `install.js` / `tee-install.js` —— 装进插件缓存 hooks.json 的两处注册与
  notify 脚本 tee（一次性脚本，已执行完毕）。

## 实测判定（详见计划文档 §4）

| 分支 | 结果 |
|---|---|
| allow | 工具直接执行，不弹窗 |
| deny | 工具被拒，message 原文到达 Agent（测试会话 model-io 留证） |
| 超时(15s) | 回落宿主正常弹窗 |
| yolo 模式 | 不派 PermissionRequest，探针全程沉默 |

## 复现要点

1. probe.js 拷进插件缓存 `hooks/`，hooks.json 的 PermissionRequest /
   UserPromptSubmit matcher 各加一条 `agentrmc-launch.cmd zcode-perm-probe.js`，
   `timeout: 15`（秒）。
2. **新开会话**才会加载新 hooks 条目（会话启动快照）；notify 脚本内容改动实时生效。
3. 测试命令需真触发授权：普通模式（build）下关沙箱执行；yolo 不派事件。
4. 卸载：还原 `hooks.json` 与 `agentrmc-notify.js`（装前备份 `*.probe-bak`），
   删探针 js。
