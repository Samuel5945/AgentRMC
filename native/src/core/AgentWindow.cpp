#include "core/AgentWindow.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace {

struct FindAgentWindowCtx {
    const wchar_t *exe;
    HWND found = nullptr;
};

BOOL CALLBACK findAgentWindow(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<FindAgentWindowCtx *>(lparam);
    if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return TRUE;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &size);
    CloseHandle(proc);
    if (ok) {
        const wchar_t *name = wcsrchr(path, L'\\');
        name = name != nullptr ? name + 1 : path;
        if (_wcsicmp(name, ctx->exe) == 0) {
            ctx->found = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

HWND windowFor(const QString &exeName) {
    if (exeName.isEmpty()) {
        return nullptr;
    }
    const std::wstring image = exeName.toStdWString();
    FindAgentWindowCtx ctx{image.c_str(), nullptr};
    EnumWindows(findAgentWindow, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

}  // namespace

namespace AgentWindow {

QStringList knownExes() {
    // 顺序即优先级：WorkBuddy 是本项目主场景，其余是常见宿主。
    // WorkBuddy 的进程映像是 WorkBuddyAI.exe（插件侧 host.js 的兜底 raise
    // 同名），WorkBuddy.exe 留给改名/旧版安装。
    // 别把 Code.exe 放进来——VS Code 遍地都是，会误判成 Agent 在线。
    return {QStringLiteral("WorkBuddyAI.exe"),
            QStringLiteral("WorkBuddy.exe"), QStringLiteral("CodeBuddy.exe"),
            QStringLiteral("ZCode.exe"), QStringLiteral("Qoder.exe"),
            QStringLiteral("Qoder CN.exe")};
}

QString detectAgentExe(const QString &preferred) {
    if (!preferred.isEmpty() && windowFor(preferred) != nullptr) {
        return preferred;
    }
    for (const QString &exe : knownExes()) {
        if (exe.compare(preferred, Qt::CaseInsensitive) == 0) {
            continue;  // 上面已经试过
        }
        if (windowFor(exe) != nullptr) {
            return exe;
        }
    }
    return QString();
}

bool alive(const QString &exe) {
    return !exe.isEmpty() && windowFor(exe) != nullptr;
}

QString foregroundExe() {
    const HWND fg = GetForegroundWindow();
    if (fg == nullptr) {
        return QString();
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0) {
        return QString();
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return QString();
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &size);
    CloseHandle(proc);
    if (!ok) {
        return QString();
    }
    const wchar_t *name = wcsrchr(path, L'\\');
    return QString::fromWCharArray(name != nullptr ? name + 1 : path);
}

QString foregroundAgentExe() {
    const QString fg = foregroundExe();
    if (fg.isEmpty()) {
        return QString();
    }
    for (const QString &exe : knownExes()) {
        if (exe.compare(fg, Qt::CaseInsensitive) == 0) {
            return exe;
        }
    }
    return QString();
}

bool present(const QString &preferred) {
    return !detectAgentExe(preferred).isEmpty();
}

void raise(const QString &exeName) {
    // 没给 raise 的（老手写 hook）仍按 ZCode 找，行为不变。
    const QString target =
        exeName.isEmpty() ? QStringLiteral("ZCode.exe") : exeName;
    const HWND found = windowFor(target);
    if (found == nullptr) {
        return;
    }
    if (IsIconic(found)) {
        ShowWindow(found, SW_RESTORE);
    }
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(found);
}

}  // namespace AgentWindow
