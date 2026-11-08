#include "core/AgentWindow.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <QPair>
#include <QVector>

#include <string>

namespace {

bool matchesImage(HWND hwnd, const wchar_t *exe) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &size);
    CloseHandle(proc);
    if (!ok) {
        return false;
    }
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    return _wcsicmp(name, exe) == 0;
}

struct FindAgentWindowCtx {
    const wchar_t *exe;
    HWND found = nullptr;
};

BOOL CALLBACK findAgentWindow(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<FindAgentWindowCtx *>(lparam);
    if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    if (matchesImage(hwnd, ctx->exe)) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

struct LargestWindowCtx {
    const wchar_t *exe;
    HWND best = nullptr;
    LONG bestArea = 0;
};

BOOL CALLBACK collectAgentWindows(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<LargestWindowCtx *>(lparam);
    if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    if (!matchesImage(hwnd, ctx->exe)) {
        return TRUE;
    }
    RECT r{};
    GetWindowRect(hwnd, &r);
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) {
        ctx->best = hwnd;
        ctx->bestArea = area;
    }
    return TRUE;  // 继续找更大的
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
    // DeepSeek Harness 是 DSH 桌面端自己的可执行文件（Electron，窗口标题非空）：
    // 它的插件（plugins/deepseek-harness）按这个名字发 raise，放进白名单后
    // 它才能成为编制内的 Worker——否则待办会进列表却没人认领，open_items 对不上。
    // OpenSquilla 排最后：它不走插件包，靠外部桥发同一个 /notify 接口
    // （见 README「OpenSquilla 接入」）。放进白名单只是为了让它成为
    // 编制内的 Worker——否则它的待办会进列表却没人认领，open_items 对不上。
    return {QStringLiteral("WorkBuddyAI.exe"),
            QStringLiteral("WorkBuddy.exe"), QStringLiteral("CodeBuddy.exe"),
            QStringLiteral("ZCode.exe"), QStringLiteral("Qoder.exe"),
            QStringLiteral("Qoder CN.exe"),
            QStringLiteral("DeepSeek Harness.exe"),
            QStringLiteral("OpenSquilla.exe")};
}

QString displayName(const QString &exe) {
    static const QVector<QPair<QString, QString>> kNames = {
        {QStringLiteral("WorkBuddyAI.exe"), QStringLiteral("WorkBuddy AI")},
        {QStringLiteral("WorkBuddy.exe"), QStringLiteral("WorkBuddy")},
        {QStringLiteral("CodeBuddy.exe"), QStringLiteral("CodeBuddy")},
        {QStringLiteral("ZCode.exe"), QStringLiteral("ZCode")},
        {QStringLiteral("Qoder.exe"), QStringLiteral("Qoder")},
        {QStringLiteral("Qoder CN.exe"), QStringLiteral("Qoder CN")},
        {QStringLiteral("DeepSeek Harness.exe"), QStringLiteral("DeepSeek Harness")},
        {QStringLiteral("OpenSquilla.exe"), QStringLiteral("OpenSquilla")},
    };
    for (const auto &entry : kNames) {
        if (entry.first.compare(exe, Qt::CaseInsensitive) == 0) {
            return entry.second;
        }
    }
    QString name = exe;
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    return name;
}

QString exeForDisplayName(const QString &name) {
    static const QVector<QPair<QString, QString>> kNames = {
        {QStringLiteral("WorkBuddyAI.exe"), QStringLiteral("WorkBuddy AI")},
        {QStringLiteral("WorkBuddy.exe"), QStringLiteral("WorkBuddy")},
        {QStringLiteral("CodeBuddy.exe"), QStringLiteral("CodeBuddy")},
        {QStringLiteral("ZCode.exe"), QStringLiteral("ZCode")},
        {QStringLiteral("Qoder.exe"), QStringLiteral("Qoder")},
        {QStringLiteral("Qoder CN.exe"), QStringLiteral("Qoder CN")},
        {QStringLiteral("DeepSeek Harness.exe"), QStringLiteral("DeepSeek Harness")},
        {QStringLiteral("OpenSquilla.exe"), QStringLiteral("OpenSquilla")},
    };
    for (const auto &entry : kNames) {
        if (entry.second.compare(name, Qt::CaseInsensitive) == 0) {
            return entry.first;
        }
    }
    return QString();
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

bool processAlive(const QString &exe) {
    if (exe.isEmpty()) {
        return false;
    }
    // 窗口枚举找不到它，但进程可能还在（收进托盘 / 隐藏窗口）：直接查
    // 进程快照按映像名对。Electron 宿主会有七八个子进程，命中一个就算在跑。
    const std::wstring image = exe.toStdWString();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, image.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

QString imagePath(const QString &exe) {
    if (exe.isEmpty()) {
        return QString();
    }
    const std::wstring image = exe.toStdWString();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return QString();
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    QString found;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, image.c_str()) != 0) {
                continue;
            }
            HANDLE proc =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (proc == nullptr) {
                continue;
            }
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &size);
            CloseHandle(proc);
            if (ok) {
                found = QString::fromWCharArray(path);
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
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

QString chatFocusCombo(const QString &exe) {
    // Qoder 新工作台：Ctrl+N = 「新任务」，打开新的 Chat 输入页，焦点必落在
    // 输入框里——派活正好需要这个语义（一句话 = 一个新任务）。
    // 来源：docs.qoder.com/qoder/keyboard-shortcuts（2026-11-03 核对）。
    // 用户改过键位时用 ui.chat_focus_<exe> 覆盖；填 "none" 显式禁用。
    if (exe.compare(QStringLiteral("Qoder.exe"), Qt::CaseInsensitive) == 0 ||
        exe.compare(QStringLiteral("Qoder CN.exe"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("ctrl+n");
    }
    return QString();
}

void *mainWindow(const QString &exe) {
    if (exe.isEmpty()) {
        return nullptr;
    }
    const std::wstring image = exe.toStdWString();
    LargestWindowCtx ctx{image.c_str(), nullptr, 0};
    EnumWindows(collectAgentWindows, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

}  // namespace AgentWindow
