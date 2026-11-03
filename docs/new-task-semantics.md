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
| Qoder / Qoder CN | focusComposer + 两段读回（16:21 实测） | ctrl+n → 当前项目新任务页 → 验证粘贴（16:5x 修后接读回） | 语义1 ✔ | 语义2 下拉存在但无自动化 |
| WorkBuddy / WorkBuddyAI | focusComposer + 两段读回（15:51/16:19 同会话实测） | UIA「新建任务」SetFocus+Enter → 无项目落地页 → 验证粘贴（15:22 实测） | 语义3 ✔ | 激活失败回落剪贴板喊人（16:5x 修，不再粘当前会话） |
| ZCode | 未登记 → 直粘当前焦点 | 同左 | 应语义1 | 「新建任务」是**逐项目行按钮**（天然语义2），无单前项目入口；只记录不登记 |
| DeepSeek Harness | 未登记 → 直粘当前焦点 | UIA「新建会话」SetFocus+Enter → 空输入框（P2 实测）→ 验证粘贴 | 语义1 ✔ 已登记 | 输入框定位未实测 → 默认仍直粘 |
| OpenSquilla | 未登记 → 直粘当前焦点 | UIA「新建任务」（无项目）SetFocus+Enter → 空输入框（P2 实测）→ 验证粘贴 | 语义3 ✔ 已登记 | 同上；另有逐项目「新建项目任务」按钮可作语义2 |
| CodeBuddy | 未登记 → 直粘当前焦点 | 同左 | 未分类 | 未实测 |

登记口径：`HostNewTask::controlNames` = 新任务按钮登记（WorkBuddy 系/OpenSquilla
「新建任务」、DSH「新建会话」）；`AgentWindow::chatFocusCombo` = Qoder 系 ctrl+n。
**`measuredChatHost` 改为显式白名单**（WorkBuddy 系 + Qoder 系）：默认续聊的
输入框定位只对这些实测过的宿主启用；登记新任务按钮不会连带启用未实测的
focusComposer（IDE 类第一个 Edit 可能是代码编辑器）。没实测的一律不塞默认值。

## 三、发现（按severity）

- **P1 语义泄漏（已修 16:5x）**：`VoicePet::handOffNewTask` 激活失败原回落
  「粘进当前会话」；16:29 轮实测话进了用户既有会话 22e624a9 并让它重跑一轮。
  现改回落**剪贴板 + 喊人手动发**，不替用户决定续聊。
- **P1 工单错标（已修 16:5x）**：`WorkItems.cpp` upsert 原 loose 合并会把该宿主
  第一个 stop 并进无 session 的 voice 卡（b451726f ← 用户会话 22e624a9）。
  现规则：stop 的 session 已有活跃卡 → 该 stop 归那张卡的 lineage，voice 回执
  直接结案（done）不吸收；session 无活跃卡（新任务首轮/已 ack 的续聊）才吸收。
  WorkItemTest 新增三用例覆盖吸收/不吸收/结案。
- **P2 能力缺口**：ZCode/DSH（语义1）、OpenSquilla（语义3）的新任务原语没实测，
  一次性新任务在这些宿主上退化为直粘，名不副实。
- **P2 语义2 未实现**：Qoder 项目下拉可自动化但未实测；「话里项目名 → 下拉选项」
  的意图层是新功能，需先测下拉的 UIA 可操作性。
- **P3 消费口径不一（已修 16:5x）**：Qoder 热键新任务分支改复用
  `handOffVerifiedPaste`，一次性 new 的消费在两路都有读回证明。

## 四、建议顺序（每项先实测再登记默认）

1. ~~P1 回落改剪贴板~~（已修）。
2. ~~P1 合并收紧~~（已修 + 测试）。
3. ~~P3 Qoder 新路径接读回~~（已修）。
4. P2 逐宿主测新任务原语（ZCode/DSH/OpenSquilla），登记后才谈默认。
5. P2 语义2：测 Qoder 落地页项目下拉 → 设计意图层（话语项目名匹配选项）。

## 五、P2 实测记录（17:28–17:4x，探针 rc_probe/p2_*.py）

- DSH：「新建会话」Button 无 Invoke pattern → SetFocus 落稳 + Enter → 焦点进
  空 composer（「描述你想要构建的内容…」），未发送。登记。
- OpenSquilla：「新建任务」Button SetFocus+Enter → 空 composer（「要发送的消息」
  /占位「分配任务或提出任何问题」），无项目绑定 = 语义3。测量后已点回原会话行
  恢复视图。登记。侧栏另有逐项目「新建项目任务」= 语义2 候选。
- ZCode：最小化恢复后扫描，「新建任务」为每个项目行各一个 Button（x 同、y 随行），
  落地页另有一个 Text 型「新建任务」（不可 Enter 激活）。无「当前项目」单入口 →
  不登记；语义2 在 ZCode 反而天然（点哪行就是哪个项目）。
- Qoder 语义2 下拉：落地页选择器 = Button「切换或清空当前工作区，当前为 X」，
  ExpandCollapse 可用；**选项在主窗树里、类型 MenuItem(50011)**（不是独立 popup
  HWND，也不是 ListItem——按类型过滤会漏）。点击选项可切换、选择器标签读回
  （需 ≥1s 稳定）；实测 lig-site → Agentrmc 往返净效果正确。
- 语义2 意图层设计（待实现）：new 模式且宿主=Qoder 时，ctrl+n 后读选择器标签得
  当前项目；展开读 MenuItem 选项名；话语里精确/归一化匹配到选项名且 ≠ 当前 →
  点击该选项 → 读回标签含该项目才继续粘贴；匹配不到 = 保持当前项目（语义1），
  不猜。读回失败回落剪贴板。OpenSquilla/DSH 的语义2 同思路用逐项目按钮。

## 六、「原本 Qoder 能按语义选其他项目」核对结论

本仓库无此实现。存在的是 Qoder 新任务页的项目下拉（手动）。若印象来自旧版或
别的工具，不在当前代码里；语义2 按新功能立项，不回填「原本就有」的说法。
