#include "Injector.h"

#include "core/AppLauncher.h"
#include "core/SystemControl.h"

#include <QHash>
#include <QProcess>
#include <QThread>

#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr int kInputKeyboard = 1;
constexpr DWORD kKeyUp = 0x0002;
constexpr DWORD kScancode = 0x0008;
constexpr DWORD kExtended = 0x0001;

int modVk(const QString &name) {
    static const QHash<QString, int> table = {
        {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"shift", VK_SHIFT},
        {"win", VK_LWIN},
    };
    return table.value(name.toLower(), 0);
}

bool isExtended(int vk) {
    switch (vk) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_DIVIDE:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_MEDIA_NEXT_TRACK:
    case VK_MEDIA_PREV_TRACK:
    case VK_MEDIA_PLAY_PAUSE:
    case VK_VOLUME_MUTE:
    case VK_VOLUME_DOWN:
    case VK_VOLUME_UP:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool WinInjector::sendKey(int vk, bool up) {
    INPUT input{};
    input.type = kInputKeyboard;
    input.ki.wVk = WORD(vk);
    input.ki.wScan = WORD(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC));
    input.ki.dwFlags = kScancode;
    if (isExtended(vk)) {
        input.ki.dwFlags |= kExtended;
    }
    if (up) {
        input.ki.dwFlags |= kKeyUp;
    }
    // 返回 0 = 被拦（UIPI/权限不一致等），调用方必须知道，不能装成功。
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

void WinInjector::key(const QStringList &mods, int vk, int repeat) {
    for (int i = 0; i < qMax(1, repeat); ++i) {
        for (const QString &mod : mods) {
            const int mv = modVk(mod);
            if (mv) {
                sendKey(mv, false);
            }
        }
        sendKey(vk, false);
        QThread::msleep(20);
        sendKey(vk, true);
        for (auto it = mods.crbegin(); it != mods.crend(); ++it) {
            const int mv = modVk(*it);
            if (mv) {
                sendKey(mv, true);
            }
        }
        QThread::msleep(50);
    }
}

void WinInjector::keyDown(const QStringList &mods, int vk) {
    for (const QString &mod : mods) {
        const int mv = modVk(mod);
        if (mv) {
            sendKey(mv, false);
        }
    }
    sendKey(vk, false);
}

void WinInjector::keyUp(const QStringList &mods, int vk) {
    sendKey(vk, true);
    for (auto it = mods.crbegin(); it != mods.crend(); ++it) {
        const int mv = modVk(*it);
        if (mv) {
            sendKey(mv, true);
        }
    }
}

bool WinInjector::copyText(const QString &text) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!OpenClipboard(nullptr)) {
            QThread::msleep(20);
            continue;
        }
        EmptyClipboard();
        const size_t bytes = (size_t(text.size()) + 1) * sizeof(char16_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory == nullptr) {
            CloseClipboard();
            return false;
        }
        void *target = GlobalLock(memory);
        if (target == nullptr) {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }
        std::memcpy(target, text.utf16(), bytes);
        GlobalUnlock(memory);
        if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }
        CloseClipboard();
        return true;
    }
    return false;
}

bool WinInjector::pasteText(const QString &text, bool autoEnter) {
    if (text.isEmpty() || !copyText(text)) {
        return false;
    }
    releaseModifiers();
    QThread::msleep(50);
    bool ok = sendKey(VK_CONTROL, false);
    ok = sendKey('V', false) && ok;
    QThread::msleep(20);
    ok = sendKey('V', true) && ok;
    ok = sendKey(VK_CONTROL, true) && ok;
    if (autoEnter) {
        QThread::msleep(80);
        ok = sendKey(VK_RETURN, false) && ok;
        QThread::msleep(20);
        ok = sendKey(VK_RETURN, true) && ok;
    }
    return ok;
}

void WinInjector::releaseModifiers() {
    // 只松真的还按着的键，避免凭空发 keyup 干扰宿主自己的快捷键状态。
    const int mods[] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN};
    for (const int vk : mods) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            sendKey(vk, true);
        }
    }
}

bool WinInjector::sendHotkey(const QStringList &mods, int vk) {
    if (vk == 0) {
        return false;
    }
    releaseModifiers();
    QThread::msleep(30);
    bool ok = true;
    for (const QString &mod : mods) {
        const int mv = modVk(mod);
        if (mv) {
            ok = sendKey(mv, false) && ok;
        }
    }
    ok = sendKey(vk, false) && ok;
    QThread::msleep(30);
    ok = sendKey(vk, true) && ok;
    for (auto it = mods.crbegin(); it != mods.crend(); ++it) {
        const int mv = modVk(*it);
        if (mv) {
            ok = sendKey(mv, true) && ok;
        }
    }
    return ok;
}

void WinInjector::scroll(int delta) {
    INPUT input{};
    input.type = 0;  // MOUSE
    input.mi.dwFlags = 0x0800;  // MOUSEEVENTF_WHEEL
    input.mi.mouseData = DWORD(delta);
    SendInput(1, &input, sizeof(INPUT));
}

void WinInjector::mouseMove(int dx, int dy) {
    INPUT input{};
    input.type = 0;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = 0x0001;  // MOUSEEVENTF_MOVE
    SendInput(1, &input, sizeof(INPUT));
}

void WinInjector::mouseClick() {
    INPUT down{};
    down.type = 0;
    down.mi.dwFlags = 0x0002;  // LEFTDOWN
    SendInput(1, &down, sizeof(INPUT));
    QThread::msleep(20);
    INPUT up{};
    up.type = 0;
    up.mi.dwFlags = 0x0004;  // LEFTUP
    SendInput(1, &up, sizeof(INPUT));
}

void WinInjector::brightness(int delta) {
    // 语音命令和遥控器共用一份实现（光标所在屏 + WMI 回退）。函数内静态：
    // 多个 WinInjector 实例共享同一份显示器句柄缓存，析构时统一还句柄。
    static WinSystemControl control;
    control.adjustBrightness(delta);
}

void WinInjector::run(const QString &cmd) {
    if (!cmd.trimmed().isEmpty()) {
        QProcess::startDetached("cmd.exe", QStringList{"/c", cmd});
    }
}

bool WinInjector::launch(const QString &appId) {
    return AppLauncher::launch(appId);
}
