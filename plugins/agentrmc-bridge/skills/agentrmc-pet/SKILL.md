---
name: agentrmc-pet
description: 与 AgentRMC 桌宠交互：汇报任务阶段进度、让桌宠出声说一句话、查询桌宠状态。当用户要求「告诉桌宠 / 让鲸鱼娘说 / 汇报进度 / 桌宠在不在」，或长任务想在桌宠上留非阻塞提示时使用。注意：等授权、等选方案、等你回话（回复以问句收尾）、任务完成、模型出错（StopFailure，仅 Qoder）五类提醒由 hook 自动完成，不要用工具重复发。
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

## 礼仪

- 一句话以内；长内容用户来不及看，也念不完。
- 一次任务最多几条提示，别连续调用刷屏；进度更新用 pet_notify（要人当场看见）
  或 pet_work_update（把状态留在列表里），收尾才用 pet_speak。
- 等工具授权（PermissionRequest）、等选方案（AskUserQuestion）、**等你回话**（一轮
  回答以问句收尾）、任务完成（Stop）和**模型出错**（StopFailure，念「<宿主> 模型
  出错了，被限流了」）的提醒都由本插件的 hook 自动发：桌宠念一句
  「<宿主> 等你确认…／在等你选方案／在等你回话／已完成…／模型出错了…」并把宿主窗口
  拉到前台。五者都不要再用 pet_notify / pet_speak 重复发。
- 「模型出错」只有 Qoder / Qoder CN 有这条自动提醒（别家宿主没有对应事件）。在别的
  宿主里碰到 429、provider unavailable 这类终止错误，仍按 `pet_work_update stage=blocked`
  汇报（它自己会出声），不要再补 pet_speak。
- 「等你回话」是从 transcript 判的：**回复最后一句带问号**才算。所以打算问用户问题时，
  把问题写在最后就行，不用再 `pet_work_update stage=blocked` 补一遍（blocked 是给
  「回合还没结束就先卡住」用的，比如缺凭据、要你拍板方向）；两边都发就是同一件事念两遍。
- 明确卡在用户身上（缺凭据、要拍板）时，用 `pet_work_update stage=blocked` 就够——
  它自己会冒泡 + 语音；再补一条 pet_notify 就是同一件事说两遍。
- **要用户回答之前先喊一声**：WorkBuddy 已改挂 `Notification(permission_prompt)`
  ——题目弹出那一刻就喊，不用你操心；但 Qoder 之类的宿主还是**用户答完之后**才
  派发 hook，只靠它就会出现「我都选完了才听到语音」。所以在调用
  `AskUserQuestion`（或其它会阻塞等人的工具）**之前**，先 `pet_work_update
  stage=blocked` 或 `pet_speak` 说一句，让人抬头；宿主随后那条会被去重，
  不会喊两遍。
  （2026-11-04 实测补记：Qoder CN 的这条 hook 是**弹窗那一刻**就派发的
  12:07:06 弹出即起跑，不用补——补了就是两遍。WorkBuddy 的 PreToolUse 仍然要等
  答完，但它另有 Notification 准点信号，也不用补。真要补的只剩 ZCode 这类没有
  question 钩子的宿主。）
