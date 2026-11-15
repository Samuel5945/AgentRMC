---
name: agentrmc-pet
description: 与 AgentRMC 桌宠交互：汇报任务阶段进度、让桌宠出声说一句话、查询桌宠状态。当用户要求「告诉桌宠 / 让鲸鱼娘说 / 汇报进度 / 桌宠在不在」，或长任务想在桌宠上留非阻塞提示时使用。注意：等授权、等选方案、等你回话、任务完成、模型出错五类提醒由 hook 自动完成，不要用工具重复发。
---

# AgentRMC 桌宠交互

通过 MCP 工具（`mcp__agentrmc__pet_*`）操作本机 AgentRMC 桌宠。前提：AgentRMC
正在运行；没运行时工具会返回友好错误，如实告知用户即可，不要重试。

## 四个工具

- **pet_notify**：桌宠冒一条气泡（不出声；桌宠收起时走系统托盘）。用于长任务
  的阶段进度（「正在跑测试，12/20」）、中途发现的重要信息。`event` 可选
  （如 `progress`），只影响事件流归类。
- **pet_speak**：桌宠说一句话——冒气泡并 TTS 朗读。用于最终结论、需要用户
  立刻注意的事。
- **pet_state**：查桌宠状态（可见/贴边收纳/监听/录音/转写/对话模式/TTS 就绪）。
  拿不准桌宠在不在、TTS 能不能出声时先查一眼再决定用哪个工具。
- **pet_work_update**：把任务阶段写进「待我处理」列表——`progress`（正在干）/
  `blocked`（卡住，等人）/ `ready`（做完，等验收）。`progress` 只更新工单不出声；
  `blocked` / `ready` 会冒气泡并语音提醒用户（要人动手的事必须喊到），所以阶段
  真的变了才报，别每跑一条命令都报一次。

## 提醒分工：hook 自动提醒 vs 主动汇报

**等授权、等选方案（AskUserQuestion）、等你回话、任务完成、模型出错这五类，
hook 在弹窗/收尾那一刻自动提醒**——桌宠出声、把宿主窗口拉到前台、建待办卡。
一律不要用工具重发：

- 随包各宿主（ZCode / Qoder / WorkBuddy / CodeBuddy）的提醒都在**弹窗那一刻**
  发出。提问、请求授权之前**不要**先 `pet_speak` 或 `pet_work_update
  stage=blocked` 打招呼——补了就是同一件事念两遍。
- 「等你回话」是从 transcript 判的：回复**最后一句以问号收尾**才算。要问用户
  问题就把问题放在最后，**问号后面别再接任何正文**——破折号补充、客套话都会
  让判定失效，桌宠会把这轮念成「做完了」。这种轮次同样不用补
  `pet_work_update stage=blocked`，两边都发就是念两遍。

**主动汇报只用在 hook 盖不到的时刻**：

- 长任务的阶段进度 → `pet_notify`（要人当场看见）或 `pet_work_update
  stage=progress`（留在「待我处理」列表里）。
- 回合还没结束就先卡住（缺凭据、外部服务挂了）→ `pet_work_update
  stage=blocked`，它自己会出声。
- 最终结论、需要用户立刻注意的事 → `pet_speak`。

「模型出错」的自动提醒只有 Qoder / Qoder CN 有（别家宿主没有 StopFailure 事件）。
在别的宿主里碰到 429、provider unavailable 这类终止错误，按
`pet_work_update stage=blocked` 汇报即可（它自己会出声），不要再补 `pet_speak`。

## 礼仪

- 一句话以内；长内容用户来不及看，也念不完。
- 一次任务最多几条提示，别连续调用刷屏；进度更新走 pet_notify /
  pet_work_update，收尾才用 pet_speak。
