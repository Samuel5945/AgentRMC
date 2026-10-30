#include "NativeHooks.h"

#include <algorithm>
#include <chrono>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr DWORD kLlkfInjected = 0x10;
NativeHooks *s_instance = nullptr;

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct KeyboardHookData {
    DWORD vkCode;
    DWORD scanCode;
    DWORD flags;
    DWORD time;
    ULONG_PTR extraInfo;
};

LRESULT CALLBACK guardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == 0 && s_instance != nullptr) {
        auto *kb = reinterpret_cast<KeyboardHookData *>(lParam);
        const bool injected = (kb->flags & kLlkfInjected) != 0;
        const int vk = int(kb->vkCode);
        const bool down =
            wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        if (!injected && s_instance->cursorMode() &&
            s_instance->handleCursor(vk, down)) {
            return 1;
        }
        if (s_instance->shouldSwallowMedia(vk, injected)) {
            if (down) {
                s_instance->notifyMedia();
            }
            return 1;
        }
        if (s_instance->shouldSwallow(vk, injected)) {
            s_instance->feed(vk, down);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK cursorProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == 0 && s_instance != nullptr) {
        auto *kb = reinterpret_cast<KeyboardHookData *>(lParam);
        const bool injected = (kb->flags & kLlkfInjected) != 0;
        if (!injected && s_instance->cursorMode()) {
            const int vk = int(kb->vkCode);
            const bool down =
                wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            if (s_instance->handleCursor(vk, down)) {
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

}  // namespace

bool nativeShouldSwallow(int vk, bool injected, bool enabled,
                         const std::set<int> &vks) {
    return enabled && !injected && vks.count(vk) > 0;
}

bool nativeShouldSwallowMedia(int vk, bool injected, bool enabled,
                              int mediaVk) {
    // 与 nativeShouldSwallow 相反：耳机走 AVRCP，系统只注入带 INJ 标记的 keydown，
    // 没有 keyup 也没有设备名，所以这里必须认 injected。
    return enabled && injected && mediaVk != 0 && vk == mediaVk;
}

int nativeCursorStep(int count) {
    return 8 + std::min(8, std::max(0, (count - 1) / 2)) * 2;
}

NativeHooks::NativeHooks(Injector *injector,
                         std::function<void(int, bool)> feed,
                         std::atomic<bool> *cursorMode,
                         std::atomic<double> *cursorLast,
                         std::function<void()> mediaSink)
    : injector_(injector),
      feed_(std::move(feed)),
      mediaSink_(std::move(mediaSink)),
      cursorMode_(cursorMode),
      cursorLast_(cursorLast) {
    s_instance = this;
}

NativeHooks::~NativeHooks() {
    stop();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void NativeHooks::setTakeover(bool enabled, const std::set<int> &vks) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    takeoverEnabled_ = enabled;
    takeoverVks_ = vks;
}

void NativeHooks::setMediaVoice(int vk, bool enabled) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    mediaVk_ = vk;
    mediaEnabled_ = enabled;
}

bool NativeHooks::shouldSwallow(int vk, bool injected) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return nativeShouldSwallow(vk, injected, takeoverEnabled_, takeoverVks_);
}

bool NativeHooks::shouldSwallowMedia(int vk, bool injected) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return nativeShouldSwallowMedia(vk, injected, mediaEnabled_, mediaVk_);
}

void NativeHooks::notifyMedia() {
    if (mediaSink_) {
        mediaSink_();
    }
}

bool NativeHooks::cursorMode() const {
    return cursorMode_ != nullptr && cursorMode_->load();
}

void NativeHooks::feed(int vk, bool down) {
    if (feed_) {
        feed_(vk, down);
    }
}

bool NativeHooks::handleCursor(int vk, bool down) {
    const bool isCursor =
        vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_RETURN;
    if (!isCursor) {
        return false;
    }
    if (!down) {
        if (vk == cursorVk_) {
            cursorVk_ = 0;
            cursorCount_ = 0;
        }
        return true;
    }

    const double now = nowSeconds();
    if (vk == cursorVk_ && now - cursorTime_ <= 0.5) {
        ++cursorCount_;
    } else {
        cursorCount_ = 1;
    }
    cursorVk_ = vk;
    cursorTime_ = now;
    if (cursorLast_ != nullptr) {
        cursorLast_->store(now);
    }
    if (vk == VK_RETURN) {
        injector_->mouseClick();
        return true;
    }
    const int step = nativeCursorStep(cursorCount_);
    switch (vk) {
    case VK_LEFT: injector_->mouseMove(-step, 0); break;
    case VK_RIGHT: injector_->mouseMove(step, 0); break;
    case VK_UP: injector_->mouseMove(0, -step); break;
    case VK_DOWN: injector_->mouseMove(0, step); break;
    default: break;
    }
    return true;
}

void NativeHooks::threadLoop() {
    guardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, guardProc, nullptr, 0);
    cursorHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, cursorProc, nullptr, 0);
    installed_ = guardHook_ != nullptr && cursorHook_ != nullptr;
    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(readyMutex_);
        ready_ = true;
    }
    readyCv_.notify_all();

    MSG msg{};
    while (!stop_.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (guardHook_ != nullptr) {
        UnhookWindowsHookEx(HHOOK(guardHook_));
        guardHook_ = nullptr;
    }
    if (cursorHook_ != nullptr) {
        UnhookWindowsHookEx(HHOOK(cursorHook_));
        cursorHook_ = nullptr;
    }
    running_.store(false);
}

bool NativeHooks::start() {
    if (thread_.joinable()) {
        return running_.load();
    }
    stop_.store(false);
    {
        std::lock_guard<std::mutex> lock(readyMutex_);
        ready_ = false;
    }
    installed_ = false;
    thread_ = std::thread(&NativeHooks::threadLoop, this);
    std::unique_lock<std::mutex> lock(readyMutex_);
    readyCv_.wait_for(lock, std::chrono::milliseconds(500),
                      [this] { return ready_; });
    if (!installed_) {
        stop();
        return false;
    }
    return true;
}

void NativeHooks::stop() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}
