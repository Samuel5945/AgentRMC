#pragma once

#include <QString>

// Qoder 系宿主的 Node 运行时询盘。
// Qoder 既不带 node.exe 也没有托管运行时（ZCode / WorkBuddy 自带），它的 hooks 只能
// 把自己的 Electron exe 当 Node 用（ELECTRON_RUN_AS_NODE=1）。装在哪台机器、哪个盘都
// 不固定，所以 App 从运行中的进程里问出完整路径，落盘给
// hooks/agentrmc-launch.cmd 直接读——省掉启动器每次去问进程祖先的开销。
namespace HostRuntime {

// 重新询盘并更新结果文件；没有 Qoder 在跑时删掉旧结果。
// 内容未变不重写（调用方按 5 秒一拍，别让磁盘跟着转）。
void refreshHint();

// 结果文件：%LOCALAPPDATA%\AgentRMC\runtime\node.txt（一行，绝对路径）。
QString hintPath();

}  // namespace HostRuntime
