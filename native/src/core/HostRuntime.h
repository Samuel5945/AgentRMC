#pragma once

#include <QString>

// 宿主 Electron exe 的 Node 运行时询盘。
// 机器上没有 node 时，hooks 只能把宿主自己的 Electron exe 当 Node 用
// （ELECTRON_RUN_AS_NODE=1）。宿主装在哪个盘不固定，所以 App 从运行中的进程里问出
// 完整路径，落盘给 hooks/agentrmc-launch.cmd 直接读——省掉启动器每次去问进程祖先
// 的开销（实测约 700ms）。候选名单见 HostRuntime.cpp，只收实测可用的宿主。
namespace HostRuntime {

// 重新询盘并更新结果文件；没有已知宿主在跑时删掉旧结果。
// 内容未变不重写（调用方按 5 秒一拍，别让磁盘跟着转）。
void refreshHint();

// 结果文件：%LOCALAPPDATA%\AgentRMC\runtime\node.txt（一行，绝对路径）。
QString hintPath();

}  // namespace HostRuntime
