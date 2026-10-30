#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

#include "core/Injector.h"

bool nativeShouldSwallow(int vk, bool injected, bool enabled,
                         const std::set<int> &vks);
int nativeCursorStep(int count);

class NativeHooks {
public:
    NativeHooks(Injector *injector, std::function<void(int, bool)> feed,
                std::atomic<bool> *cursorMode,
                std::atomic<double> *cursorLast);
    ~NativeHooks();

    bool start();
    void stop();
    bool running() const { return running_.load(); }
    void setTakeover(bool enabled, const std::set<int> &vks);

    bool shouldSwallow(int vk, bool injected) const;
    bool handleCursor(int vk, bool down);
    void feed(int vk, bool down);
    bool cursorMode() const;
    Injector *injector() const { return injector_; }

private:
    void threadLoop();

    Injector *injector_ = nullptr;
    std::function<void(int, bool)> feed_;
    std::atomic<bool> *cursorMode_ = nullptr;
    std::atomic<double> *cursorLast_ = nullptr;
    mutable std::mutex stateMutex_;
    bool takeoverEnabled_ = false;
    std::set<int> takeoverVks_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    bool ready_ = false;
    bool installed_ = false;
    void *guardHook_ = nullptr;
    void *cursorHook_ = nullptr;
    int cursorVk_ = 0;
    int cursorCount_ = 0;
    double cursorTime_ = 0;
};
