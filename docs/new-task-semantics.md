# 新任务语义复核（2026-11-03，用户定义三语义后全链路理清）

## 一、三种语义（用户 16:32 / 16:35 定义）

1. **当前项目新任务**：新会话继承宿主当前项目/工作区。
   宿主：Qoder、Qoder CN、ZCode、DeepSeek Harness。
2. **其他项目新任务**：建任务时指定别的项目。Qoder 新任务页有项目下拉
   （「在 Agentrmc ⌄」，截图证据）可**手动**选；「按话语语义选项目」在本仓库
   **没有任何实现**（agentrc_gui.py / legacy / docs / native 全 grep 空）。
3. **无项目新任务**：新会话不绑项目。宿主：WorkBuddy、WorkBuddyAI、OpenSquilla。
   WorkBuddy 落地页「选择工作空间」默认未选 = 无项目（可手动选，默认不算）。

## 二、现状矩阵（代码 + 真机证据，见 rc_probe/E2E_HANDOFF_2026-11-03.md）

| 宿主 | 默认（续当前会话） | 一次性新任务 | 语义归属 | 缺口 |
|---|---|---|---|---|
| Qoder / Qoder CN | focusComposer + 两段读回（16:21 实测） | ctrl+n → 当前项目新任务页 → 粘贴+Enter（14:44 实测） | 语义1 ✔ | 语义2 下拉存在但无自动化；新路径无读回 |
| WorkBuddy / WorkBuddyAI | focusComposer + 两段读回（15:51/16:19 同会话实测） | UIA「新建任务」SetFocus+Enter → 无项目落地页 → 验证粘贴（15:22 实测） | 语义3 ✔ | 激活失败回落「粘进当前会话」= 语义泄漏（16:29 实测） |
| ZCode / DeepSeek Harness | 未登记 → 直粘当前焦点 | 同左（**不会开新任务**） | 应语义1 | 新任务原语未实测未登记 |
| OpenSquilla | 未登记 → 直粘当前焦点 | 同左 | 应语义3 | 新会话原语未实测未登记 |
| CodeBuddy | 未登记 → 直粘当前焦点 | 同左 | 未分类 | 未实测 |

登记口径：`HostNewTask::controlNames`（WorkBuddy 系「新建任务」）与
`AgentWindow::chatFocusCombo`（Qoder 系 ctrl+n）；`measuredChatHost` = 两者之一。
没实测的一律不塞默认值（项目约定）。

## 三、发现（按severity）

- **P1 语义泄漏**：`VoicePet::handOffNewTask` 激活失败时回落「粘进当前会话」。
  16:29 轮实测：mode=new 但激活没成，话进了用户既有会话 22e624a9 并让它重跑一轮。
  新任务意图失败时应回落**剪贴板 + 喊人**，不该替用户决定续聊。
- **P1 工单错标**：`WorkItems.cpp` upsert 的 loose 合并（session 空 + 同宿主）
  会把该宿主第一个 stop 并进 voice 卡：b451726f 被标上用户会话 22e624a9。
  voice 卡 session 为空时不应 loose 合并 stop（或要求 project/消息匹配）。
- **P2 能力缺口**：ZCode/DSH（语义1）、OpenSquilla（语义3）的新任务原语没实测，
  一次性新任务在这些宿主上退化为直粘，名不副实。
- **P2 语义2 未实现**：Qoder 项目下拉可自动化但未实测；「话里项目名 → 下拉选项」
  的意图层是新功能，需先测下拉的 UIA 可操作性。
- **P3 消费口径不一**：一次性 new 在 `handOffDelivered` 消费；WorkBuddy 路经
  读回证明交付，Qoder 路经 `handOffPaste`（SendInput 成功即算交付，无读回）。
  应让 Qoder 新路径也复用 `handOffVerifiedPaste`。

## 四、建议顺序（每项先实测再登记默认）

1. P1 回落改剪贴板（小改 + 真机一轮）。
2. P1 合并收紧（小改 + WorkItem/Store 测试）。
3. P3 Qoder 新路径接读回（复用现成函数）。
4. P2 逐宿主测新任务原语（ZCode/DSH/OpenSquilla），登记后才谈默认。
5. P2 语义2：测 Qoder 落地页项目下拉 → 设计意图层（话语项目名匹配选项）。

## 五、「原本 Qoder 能按语义选其他项目」核对结论

本仓库无此实现。存在的是 Qoder 新任务页的项目下拉（手动）。若印象来自旧版或
别的工具，不在当前代码里；语义2 按新功能立项，不回填「原本就有」的说法。
