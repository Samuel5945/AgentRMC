#include "InputEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>
#include <QTimer>

#include "core/VoiceSettings.h"

#include <chrono>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

LRESULT CALLBACK rawWndProc(HWND hwnd, UINT message, WPARAM wparam,
                            LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

bool InputEngineFilter::nativeEventFilter(const QByteArray &eventType,
                                          void *message, qintptr *result) {
    Q_UNUSED(eventType);
    Q_UNUSED(result);
    auto *msg = static_cast<MSG *>(message);
    if (msg != nullptr && msg->message == WM_INPUT) {
        engine_->handleRaw(reinterpret_cast<void *>(msg->lParam));
    }
    return false;
}

InputEngine::InputEngine(ProfileStore *store, QObject *parent)
    : QObject(parent), store_(store) {
    router_ = std::make_unique<KeyRouter>(
        &injector_,
        [this](const QString &kind, const QString &name,
               const QString &profile, const QString &detail, int vk) {
            const QString k = kind;
            const QString n = name;
            const QString p = profile;
            const QString d = detail;
            QMetaObject::invokeMethod(
                this,
                [this, k, n, p, d, vk] { emit eventReady(k, n, p, d, vk); },
                Qt::QueuedConnection);
        });

    router_->setCursorClocks(&cursorMode_, &cursorLast_);

    profileTimer_ = new QTimer(this);
    profileTimer_->setInterval(300);
    connect(profileTimer_, &QTimer::timeout, this, &InputEngine::refreshProfile);
}

InputEngine::~InputEngine() {
    stop();
}

bool InputEngine::start(QString *error) {
    if (running_.load()) {
        return true;
    }

    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\AgentRMC.SingleInstance");
    if (mutex_ == nullptr) {
        if (error) *error = QStringLiteral("创建单实例互斥量失败");
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(HANDLE(mutex_));
        mutex_ = nullptr;
        if (error) {
            *error = QStringLiteral(
                "AgentRMC 已在运行（Python 版或另一个原生实例）；先停止它再开始监听");
        }
        return false;
    }

    deviceMark_ = store_->config().value("device_mark").toString().toLower();
    if (deviceMark_.isEmpty()) {
        deviceMark_ = QStringLiteral("01000d_pid&3838");
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = rawWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AgentRMCNativeRaw";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error) *error = QStringLiteral("注册 Raw Input 窗口类失败");
        CloseHandle(HANDLE(mutex_));
        mutex_ = nullptr;
        return false;
    }

    HWND hwnd = CreateWindowExW(0, L"AgentRMCNativeRaw", L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        if (error) *error = QStringLiteral("创建 Raw Input 窗口失败");
        CloseHandle(HANDLE(mutex_));
        mutex_ = nullptr;
        return false;
    }
    hwnd_ = hwnd;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x06;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        if (error) *error = QStringLiteral("RegisterRawInputDevices 失败");
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        CloseHandle(HANDLE(mutex_));
        mutex_ = nullptr;
        return false;
    }

    filter_ = new InputEngineFilter(this);
    QCoreApplication::instance()->installNativeEventFilter(filter_);

    hooks_ = new NativeHooks(
        &injector_, [this](int vk, bool down) { pushRaw({vk, down}); },
        &cursorMode_, &cursorLast_, [this] { notifyMedia(); });
    if (!hooks_->start()) {
        if (error) *error = QStringLiteral("低层键盘钩子安装失败");
        cleanup();
        return false;
    }

    refreshProfile();
    running_.store(true);
    worker_ = std::thread(&InputEngine::workerLoop, this);
    profileTimer_->start();
    emit runningChanged(true, QStringLiteral("监听中"));
    return true;
}

void InputEngine::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cleanup();
    emit runningChanged(false, QStringLiteral("已停止"));
}

void InputEngine::cleanup() {
    running_.store(false);
    if (hooks_ != nullptr) {
        hooks_->stop();
        delete hooks_;
        hooks_ = nullptr;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (profileTimer_ != nullptr) {
        profileTimer_->stop();
    }
    if (filter_ != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(filter_);
        delete filter_;
        filter_ = nullptr;
    }
    if (hwnd_ != nullptr) {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x06;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        DestroyWindow(HWND(hwnd_));
        hwnd_ = nullptr;
    }
    if (mutex_ != nullptr) {
        CloseHandle(HANDLE(mutex_));
        mutex_ = nullptr;
    }
}

void InputEngine::refreshProfile() {
    if (store_ == nullptr) {
        return;
    }
    const Profile current = store_->current(true);
    if (lastProfileName_ != current.name) {
        lastProfileName_ = current.name;
        emit profileChanged(current.name);
    }
    auto snapshot = std::make_shared<Profile>(current);
    {
        std::lock_guard<std::mutex> lock(profileMutex_);
        profile_ = std::move(snapshot);
    }

    std::set<int> vks;
    // 跟 Python 版一样从合并后的 profile 取表，extends 的键也要接管。
    for (const QJsonObject *object : {&current.keys, &current.doubleMap,
                                      &current.hold}) {
        for (auto it = object->begin(); it != object->end(); ++it) {
            bool okNumber = false;
            const int vk = it.key().toInt(&okNumber);
            if (okNumber) {
                vks.insert(vk);
            }
        }
    }

    const bool enabled = store_->uiBool("native_takeover", false) &&
                         !vks.empty();
    if (hooks_ != nullptr) {
        hooks_->setTakeover(enabled, vks);
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        takeoverEnabled_ = enabled;
        takeoverVks_ = vks;
    }

    const VoiceOptions voice =
        resolveVoiceOptions(store_->config(), current.voice,
                            QCoreApplication::applicationDirPath());
    voiceVk_.store(voice.voiceVk);
    mediaVk_.store(voice.mediaVk);
    // 生效范围：配置里写了 swallow_in 就按名单，否则凡是配了语音的场景都接管。
    const bool mediaGate =
        voice.mediaEnabled && (voice.mediaScopeExplicit
                                   ? voice.mediaProfiles.contains(
                                         current.name.toLower())
                                   : !current.voice.isEmpty());
    mediaGate_.store(mediaGate);
    if (hooks_ != nullptr) {
        hooks_->setMediaVoice(voice.mediaVk, mediaGate);
    }
}

void InputEngine::workerLoop() {
    while (running_.load()) {
        RawEvent event;
        bool hasEvent = false;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (!queue_.empty()) {
                event = queue_.front();
                queue_.pop_front();
                hasEvent = true;
            }
        }

        std::shared_ptr<const Profile> snapshot;
        {
            std::lock_guard<std::mutex> lock(profileMutex_);
            snapshot = profile_;
        }
        bool takeover = false;
        std::set<int> takeoverVks;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            takeover = takeoverEnabled_;
            takeoverVks = takeoverVks_;
        }
        if (snapshot) {
            router_->setProfile(snapshot);
        }
        router_->setTakeover(takeover, takeoverVks);
        if (hasEvent) {
            if (event.vk == voiceVk_.load()) {
                QMetaObject::invokeMethod(
                    this,
                    [this, down = event.down] {
                        if (down) {
                            emit voiceDown();
                        } else {
                            emit voiceUp();
                        }
                    },
                    Qt::QueuedConnection);
            } else if (event.vk == mediaVk_.load() && mediaGate_.load()) {
                notifyMedia();
            } else if (event.down) {
                router_->keyDown(event.vk);
            } else {
                router_->keyUp(event.vk);
            }
        }
        router_->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

QString InputEngine::deviceName(void *handle) {
    const auto cached = deviceNames_.constFind(handle);
    if (cached != deviceNames_.constEnd()) {
        return cached.value();
    }
    UINT size = 0;
    GetRawInputDeviceInfoW(HANDLE(handle), RIDI_DEVICENAME, nullptr, &size);
    if (size == 0) {
        return {};
    }
    std::wstring buffer(size, L'\0');
    GetRawInputDeviceInfoW(HANDLE(handle), RIDI_DEVICENAME, buffer.data(), &size);
    const QString name = QString::fromWCharArray(buffer.c_str());
    deviceNames_.insert(handle, name);
    return name;
}

void InputEngine::handleRaw(void *lparam) {
    UINT size = 0;
    GetRawInputData(HRAWINPUT(lparam), RID_INPUT, nullptr, &size,
                    sizeof(RAWINPUTHEADER));
    if (size == 0) {
        return;
    }
    std::vector<BYTE> buffer(size);
    if (GetRawInputData(HRAWINPUT(lparam), RID_INPUT, buffer.data(), &size,
                        sizeof(RAWINPUTHEADER)) != size) {
        return;
    }
    auto *raw = reinterpret_cast<RAWINPUT *>(buffer.data());
    if (raw->header.dwType != RIM_TYPEKEYBOARD) {
        return;
    }
    const QString path = deviceName(raw->header.hDevice).toLower();
    if (!path.contains(deviceMark_)) {
        return;
    }
    const RAWKEYBOARD &keyboard = raw->data.keyboard;
    if (keyboard.VKey == 0 || keyboard.VKey == 0xFF) {
        return;
    }
    RawEvent event;
    event.vk = int(keyboard.VKey);
    event.down = keyboard.Message == WM_KEYDOWN || keyboard.Message == WM_SYSKEYDOWN;
    pushRaw(event);
}

void InputEngine::pushRaw(const RawEvent &event) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (queue_.size() < 256) {
        queue_.push_back(event);
    }
}

void InputEngine::notifyMedia() {
    // 低层钩子和 Raw Input 可能都送来同一个 B3，200ms 内只认一次。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 previous = mediaLastMs_.exchange(now);
    if (now - previous < 200) {
        return;
    }
    QMetaObject::invokeMethod(
        this, [this] { emit mediaVoice(); }, Qt::QueuedConnection);
}
