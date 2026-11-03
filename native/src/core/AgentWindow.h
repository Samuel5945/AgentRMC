#pragma once

#include <QString>
#include <QStringList>

// 宿主 Agent 主窗口的探测与前置（Win32）。
// AgentRMC 不绑定具体宿主：hook 发通知时带 raise（进程映像名），这里按名字找窗口。
// 桌宠要判断「宿主 Agent 在不在」也走这里——枚举窗口比等 hook 心跳快且不依赖宿主配合。
namespace AgentWindow {

// 常见宿主的进程映像名，按优先级排；detectAgentExe 逐个试。
QStringList knownExes();

// 进程映像名 → 产品显示名（WorkBuddyAI.exe → WorkBuddy AI）。
// 未知名字去掉 .exe 原样返回，给菜单/气泡用，避免把裸 exe 名甩给用户。
QString displayName(const QString &exe);

// 找到就返回该进程映像名，都不在返回空串。preferred 非空时优先试它。
QString detectAgentExe(const QString &preferred = QString());

// 该映像名对应的宿主是否真活着（严格按这一个名字找窗口，不回落其它宿主）。
bool alive(const QString &exe);

// 该映像名的进程是否在跑（不看窗口）。托盘 / 隐藏窗口的宿主进程活着但没有
// 可交互窗口——alive() 会判否，这里判是。状态展示、Worker 表、「交给」下拉
// 用它；语音分流和 raise 仍必须 alive()（没窗口的话注入会打进别的窗口）。
bool processAlive(const QString &exe);

// 前台窗口的进程映像名（取不到返回空）。
QString foregroundExe();

// 前台窗口若是已知宿主，返回它的映像名；否则空。
// 桌宠靠它判断「用户此刻在哪个 Agent 里说话」——比按候选顺序猜准得多。
QString foregroundAgentExe();

// 宿主是否在线（有可见主窗口）。等价于 detectAgentExe(preferred) 非空。
bool present(const QString &preferred = QString());

// 把该进程的主窗口拉到前台（最小化先还原，ALT 按放一下绕 SetForegroundWindow 限制）。
// exeName 为空时退回 ZCode.exe，兼容老手写 hook。找不到窗口就静默返回。
void raise(const QString &exeName);

}  // namespace AgentWindow
