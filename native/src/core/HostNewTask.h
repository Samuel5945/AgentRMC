#pragma once

#include <QString>
#include <QStringList>

// 没有自己的「新任务」快捷键的宿主（WorkBuddy 系）走这里：用 UIA 按控件名
// 找到「新建任务」，把焦点交给它，由调用方发回车激活，再验收焦点是否落进
// 输入框。实测两家各 5 轮全通过、且全程不动鼠标（2026-11-03，见 PRD FR-07）。
//
// 为什么不用 Ctrl+N：WorkBuddy 的 Ctrl+N 是「新建文件」，会把文件对话框弹出来。
// 为什么不用 InvokePattern：该控件是 TabItem，不实现 InvokePattern（UIA 自己
// 报的 pattern 列表里没有 10000，同窗口的 Button 都有）——只剩 SetFocus + 按键。
namespace HostNewTask {

struct Attempt {
    bool focused = false;    // 焦点确实落在目标控件上
    QString landed;          // 焦点控件名
    int landedType = 0;      // 焦点控件 UIA control type
    int namedNodes = 0;      // 当时读到的具名节点数（判断可访问性树有没有建好）
    QString fail;            // 失败成因；focused 为 true 时为空
};

// 该宿主的「新建任务」控件名候选。空 = 没有实测依据，不走这条路
// （Qoder 家族用 ctrl+n；未测过的宿主一律留空，别拿未验证的键位糊上去）。
QStringList controlNames(const QString &exe);

// 找到控件并把焦点交给它。内部会：主窗太小先用协议唤醒 → 轮询等可访问性树
// 建好 → 按候选名精确匹配 + 可见矩形 → SetFocus → 复查焦点落点。
// 阻塞至多 timeoutMs（热态实测 ~100–300ms）。
Attempt focusControl(const QString &exe, const QStringList &names,
                     int timeoutMs);

// 回车之后的验收：焦点在目标宿主进程内、且控件类型是可编辑框。
// 必须按进程限定——焦点还留在自己 App 里也算「可编辑」的话就是假成功。
bool focusIsEditable(const QString &exe);

// 输入框当前文本（第一个 Edit 的 Value pattern）。读不到 = 空串。
// 派活「粘贴后读回」用：字真进了输入框才算粘贴成功，SendInput 返回 1 不算。
QString composerValue(const QString &exe);

// 消息树里有没有名字恰为 text 的节点（已发送气泡/会话标题）。
// 与 composerValue 合起来证明「真发送」：输入框不再含这句话且树里出现了它。
bool treeHasText(const QString &exe, const QString &text);

// 续当前会话模式：把焦点交给该宿主主窗里第一个可见 Edit（当前会话输入框），
// 并复查焦点真落在宿主进程内的 Edit 上。找不到/落不稳 = false，调用方回落
// 剪贴板，不赌焦点。与「新建任务」路互斥，由 ui.dispatch_mode_<exe> 选择。
bool focusComposer(const QString &exe);

}  // namespace HostNewTask
