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
    // 调用方可能传完整路径（会话记录/hook 上报的形态不统一）：只比 basename，
    // 否则永远匹配不上，表现就是「宿主明明在跑却说窗口被隐藏」。
    const wchar_t *want = wcsrchr(exe, L'\\');
    want = want != nullptr ? want + 1 : exe;
    return _wcsicmp(name, want) == 0;
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

// 不限可见性：宿主收进托盘（IsWindowVisible=false）时，窗口仍在 EnumWindows
// 列表里，标题也还在。隐藏窗口的 GetWindowRect 可能全 0，所以面积打平时
// 用「先到先得」，保证至少能拿到一个。
BOOL CALLBACK collectAnyAgentWindow(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<LargestWindowCtx *>(lparam);
    if (GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;  // 无标题的多是辅助/工具窗口，不是主窗
    }
    if (!matchesImage(hwnd, ctx->exe)) {
        return TRUE;
    }
    RECT r{};
    GetWindowRect(hwnd, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area <= 0) {
        area = 1;  // 隐藏窗口没有真实几何，给个最小分以便被选中
    }
    if (ctx->best == nullptr || area > ctx->bestArea) {
        ctx->best = hwnd;
        ctx->bestArea = area;
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

// 按窗口标题找主窗：**映像名兜底**。
// 现实里同一个宿主会有两个 exe 名（WorkBuddy.exe / WorkBuddyAI.exe，
// 见 knownExes 注释），而会话记录里的 agentExe 和实际进程映像不一定是同一个；
// 一旦对不上，mainWindow 就永远是 nullptr，遥控派发直接报「宿主窗口被隐藏」
// ——2026-11-19 用户在手机上撞到的就是这个（进程 10 个、主窗 938×786 明明可见）。
// 标题是稳的：Electron 主窗标题就是产品名。用「标题精确等于 + Chromium 窗口类」
// 双条件，避免误命中浏览器标签页。
BOOL CALLBACK findWindowByTitle(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<LargestWindowCtx *>(lparam);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"Chrome_WidgetWin_", 17) != 0) {
        return TRUE;
    }
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    if (_wcsicmp(title, ctx->exe) != 0) {  // ctx->exe 这里装的是标题
        return TRUE;
    }
    RECT r{};
    GetWindowRect(hwnd, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area <= 0) {
        area = 1;
    }
    if (ctx->best == nullptr || area > ctx->bestArea) {
        ctx->best = hwnd;
        ctx->bestArea = area;
    }
    return TRUE;
}

struct DiagCtx {
    const wchar_t *exe;
    int total = 0;
    int visible = 0;
    int titled = 0;
};

BOOL CALLBACK countWindowsByImage(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<DiagCtx *>(lparam);
    if (!matchesImage(hwnd, ctx->exe)) {
        return TRUE;
    }
    ++ctx->total;
    if (IsWindowVisible(hwnd)) {
        ++ctx->visible;
    }
    if (GetWindowTextLengthW(hwnd) > 0) {
        ++ctx->titled;
    }
    return TRUE;
}

QString diagnose(const QString &exe) {
    if (exe.isEmpty()) {
        return QStringLiteral("exe 名为空");
    }
    const std::wstring image = exe.toStdWString();
    DiagCtx ctx{image.c_str()};
    EnumWindows(countWindowsByImage, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.total == 0) {
        return QStringLiteral("该进程没有窗口，多半没启动");
    }
    return QStringLiteral("%1 扇窗口、%2 扇可见、%3 扇有标题")
        .arg(ctx.total)
        .arg(ctx.visible)
        .arg(ctx.titled);
}

void *mainWindowByTitle(const QString &title) {
    if (title.isEmpty()) {
        return nullptr;
    }
    const std::wstring t = title.toStdWString();
    LargestWindowCtx ctx{t.c_str(), nullptr, 0};
    EnumWindows(findWindowByTitle, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

void *mainWindowAny(const QString &exe) {
    if (exe.isEmpty()) {
        return nullptr;
    }
    const std::wstring image = exe.toStdWString();
    LargestWindowCtx ctx{image.c_str(), nullptr, 0};
    EnumWindows(collectAnyAgentWindow, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

bool wake(const QString &exe) {
    HWND hwnd = static_cast<HWND>(mainWindowAny(exe));
    if (hwnd == nullptr) {
        // 映像名对不上时的兜底（见 mainWindowByTitle 注释）。
        hwnd = static_cast<HWND>(mainWindowByTitle(displayName(exe)));
    }
    if (hwnd == nullptr) {
        return false;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd)) {
        // 收进托盘时进程自己 Hide 了窗口，外部 SW_SHOW 能把它叫回来。
        ShowWindow(hwnd, SW_SHOW);
    }
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(hwnd);
    return true;
}

}  // namespace AgentWindow
