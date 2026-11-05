#include "SystemControl.h"

#include "core/Profiles.h"

#include <QThread>
#include <QVector>

#include <cmath>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <powrprof.h>
#include <comdef.h>
#include <Wbemidl.h>

// 高 level monitor API 的结构体自己声明，省掉 winuser 里那一大坨头文件依赖。
struct PhysicalMonitor {
    HANDLE handle;
    wchar_t description[128];
};

struct Ddc {
    using GetCountFn = BOOL(WINAPI *)(HMONITOR, LPDWORD);
    using GetMonitorsFn = BOOL(WINAPI *)(HMONITOR, DWORD, PhysicalMonitor *);
    using DestroyFn = BOOL(WINAPI *)(HANDLE);
    using GetVcpFn = BOOL(WINAPI *)(HANDLE, BYTE, LPDWORD, LPDWORD, LPDWORD);
    using SetVcpFn = BOOL(WINAPI *)(HANDLE, BYTE, DWORD);

    GetCountFn getCount = nullptr;
    GetMonitorsFn getMonitors = nullptr;
    DestroyFn destroy = nullptr;
    GetVcpFn getVcp = nullptr;
    SetVcpFn setVcp = nullptr;
    bool ready = false;
};

namespace {

constexpr BYTE kVcpBrightness = 0x10;
constexpr int kShutdownDelaySeconds = 15;

QString tr(const wchar_t *text) {
    return QString::fromWCharArray(text);
}

void ensureCom() {
    // 每个线程各自初始化一次；已被别处按别的模式初始化过就沿用，照样能用。
    static thread_local const bool initialized = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return true;
    }();
    Q_UNUSED(initialized);
}

template <typename T>
class ComPtr {
public:
    ~ComPtr() {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
    }
    T **out() { return &ptr_; }
    T *get() const { return ptr_; }
    T *operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T *ptr_ = nullptr;
};

class Bstr {
public:
    explicit Bstr(const QString &text)
        : value_(SysAllocStringLen(reinterpret_cast<const OLECHAR *>(text.utf16()),
                                   UINT(text.size()))) {}
    ~Bstr() { SysFreeString(value_); }
    BSTR get() const { return value_; }

private:
    BSTR value_ = nullptr;
};

// ---------------------------------------------------------------- 音量

bool endpointVolume(ComPtr<IAudioEndpointVolume> *out) {
    ensureCom();
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void **>(enumerator.out())))) {
        return false;
    }
    ComPtr<IMMDevice> device;
    // 每次现取默认渲染端点：用户切了耳机/音箱，下一条命令就跟着走新的那台。
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                   device.out()))) {
        return false;
    }
    return SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                      nullptr,
                                      reinterpret_cast<void **>(out->out())));
}

bool readVolume(IAudioEndpointVolume *volume, int *percent, bool *muted) {
    float level = 0.0f;
    if (FAILED(volume->GetMasterVolumeLevelScalar(&level))) {
        return false;
    }
    BOOL mute = FALSE;
    volume->GetMute(&mute);
    *percent = qBound(0, int(std::lround(level * 100.0f)), 100);
    *muted = mute != FALSE;
    return true;
}

// ---------------------------------------------------------------- 亮度

Ddc loadDdc(void **library) {
    Ddc ddc;
    if (*library == nullptr) {
        *library = LoadLibraryW(L"dxva2.dll");
    }
    if (*library == nullptr) {
        return ddc;
    }
    HMODULE module = HMODULE(*library);
    ddc.getCount = reinterpret_cast<Ddc::GetCountFn>(GetProcAddress(
        module, "GetNumberOfPhysicalMonitorsFromHMONITOR"));
    ddc.getMonitors = reinterpret_cast<Ddc::GetMonitorsFn>(GetProcAddress(
        module, "GetPhysicalMonitorsFromHMONITOR"));
    ddc.destroy = reinterpret_cast<Ddc::DestroyFn>(
        GetProcAddress(module, "DestroyPhysicalMonitor"));
    ddc.getVcp = reinterpret_cast<Ddc::GetVcpFn>(GetProcAddress(
        module, "GetVCPFeatureAndVCPFeatureReply"));
    ddc.setVcp = reinterpret_cast<Ddc::SetVcpFn>(
        GetProcAddress(module, "SetVCPFeature"));
    ddc.ready = ddc.getCount && ddc.getMonitors && ddc.getVcp && ddc.setVcp;
    return ddc;
}

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto *list = reinterpret_cast<QVector<quintptr> *>(param);
    const quintptr key = quintptr(monitor);
    if (!list->contains(key)) {
        list->append(key);
    }
    return TRUE;
}

bool readBrightness(const Ddc &ddc, HANDLE handle, int *percent, int *maximum) {
    DWORD current = 0;
    DWORD top = 0;
    DWORD type = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ddc.getVcp(handle, kVcpBrightness, &type, &current, &top)) {
            *maximum = top > 0 ? int(top) : 100;
            *percent = qBound(0, int(current), *maximum);
            return true;
        }
        QThread::msleep(20);
    }
    return false;
}

// 冷启动第一次取到的物理监视器句柄经常读不到（DDC 线要 warm up）：
// 按退避重取句柄再试。读不到还硬写一次，是「亮度没变但报了成功」的来源。
void destroyExtras(const Ddc &ddc, const QVector<PhysicalMonitor> &monitors) {
    if (ddc.destroy == nullptr) {
        return;
    }
    for (int i = 1; i < monitors.size(); ++i) {
        ddc.destroy(monitors[i].handle);
    }
}

// 笔记本内屏往往不支持 DDC/CI，走 WMI 的背光接口。
bool wmiBrightness(bool apply, int target, int *percent, QString *error) {
    ensureCom();
    static const bool securitySet = [] {
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                             RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE,
                             nullptr);
        return true;  // 进程里已经设过就沿用它，失败不影响后面连接
    }();
    Q_UNUSED(securitySet);

    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator,
                                reinterpret_cast<void **>(locator.out())))) {
        *error = tr(L"这台机器取不到背光接口（WMI 不可用）");
        return false;
    }
    ComPtr<IWbemServices> services;
    Bstr nameSpace(QStringLiteral("ROOT\\WMI"));
    if (FAILED(locator->ConnectServer(nameSpace.get(), nullptr, nullptr, nullptr,
                                      0, nullptr, nullptr, services.out()))) {
        *error = tr(L"这台机器取不到背光接口（ROOT\\WMI 连不上）");
        return false;
    }
    CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
    if (!apply) {
        Bstr language(QStringLiteral("WQL"));
        Bstr query(QStringLiteral("SELECT CurrentBrightness FROM WmiMonitorBrightness"));
        ComPtr<IEnumWbemClassObject> items;
        if (FAILED(services->ExecQuery(
                language.get(), query.get(),
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                items.out()))) {
            *error = tr(L"这块屏不支持亮度调节（DDC/CI 与背光接口都没有）");
            return false;
        }
        ComPtr<IWbemClassObject> item;
        ULONG returned = 0;
        if (items->Next(5000, 1, item.out(), &returned) != WBEM_S_NO_ERROR ||
            returned == 0) {
            *error = tr(L"这块屏不支持亮度调节（DDC/CI 与背光接口都没有）");
            return false;
        }
        VARIANT value{};
        VariantInit(&value);
        const HRESULT hr = item->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr);
        bool ok = false;
        if (SUCCEEDED(hr) &&
            SUCCEEDED(VariantChangeType(&value, &value, 0, VT_I4))) {
            *percent = qBound(0, int(value.lVal), 100);
            ok = true;
        }
        VariantClear(&value);
        if (!ok) {
            *error = tr(L"读不到当前亮度");
        }
        return ok;
    }
    Bstr className(QStringLiteral("WmiMonitorBrightnessMethods"));
    ComPtr<IWbemClassObject> klass;
    if (FAILED(services->GetObject(className.get(), 0, nullptr, klass.out(), nullptr))) {
        *error = tr(L"这块屏不支持亮度调节（没有背光控制类）");
        return false;
    }
    Bstr methodName(QStringLiteral("WmiSetBrightness"));
    ComPtr<IWbemClassObject> signature;
    ComPtr<IWbemClassObject> inParams;
    if (FAILED(klass->GetMethod(methodName.get(), 0, signature.out(), nullptr)) ||
        FAILED(signature->SpawnInstance(0, inParams.out()))) {
        *error = tr(L"这块屏不支持亮度调节（背光接口签名取不到）");
        return false;
    }
    VARIANT timeout{};
    timeout.vt = VT_UI4;
    timeout.ulVal = 1;
    VARIANT brightness{};
    brightness.vt = VT_UI4;
    brightness.ulVal = ULONG(qBound(0, target, 100));
    const bool paramsOk =
        SUCCEEDED(inParams->Put(L"Timeout", 0, &timeout, 0)) &&
        SUCCEEDED(inParams->Put(L"Brightness", 0, &brightness, 0));
    if (!paramsOk) {
        *error = tr(L"背光接口参数写不进去");
        return false;
    }
    Bstr language(QStringLiteral("WQL"));
    Bstr query(QStringLiteral("SELECT * FROM WmiMonitorBrightnessMethods"));
    ComPtr<IEnumWbemClassObject> items;
    if (FAILED(services->ExecQuery(
            language.get(), query.get(),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            items.out()))) {
        *error = tr(L"这块屏不支持亮度调节（背光明细取不到）");
        return false;
    }
    int applied = 0;
    while (true) {
        ComPtr<IWbemClassObject> item;
        ULONG returned = 0;
        if (items->Next(5000, 1, item.out(), &returned) != WBEM_S_NO_ERROR ||
            returned == 0) {
            break;
        }
        VARIANT path{};
        VariantInit(&path);
        if (SUCCEEDED(item->Get(L"__RELPATH", 0, &path, nullptr, nullptr)) &&
            path.vt == VT_BSTR && path.bstrVal != nullptr) {
            ComPtr<IWbemClassObject> outParams;
            if (SUCCEEDED(services->ExecMethod(path.bstrVal, methodName.get(), 0,
                                               nullptr, inParams.get(),
                                               outParams.out(), nullptr))) {
                ++applied;
            }
        }
        VariantClear(&path);
    }
    if (applied == 0) {
        *error = tr(L"这块屏不支持亮度调节（背光接口调不动）");
        return false;
    }
    *percent = qBound(0, target, 100);
    return true;
}

// ---------------------------------------------------------------- 电源

bool enablePrivilege(const wchar_t *name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr,
                          nullptr);
    const bool ok = GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

// 返回退出码；-1 = 起不来或超时。直接 CreateProcess 带 CREATE_NO_WINDOW，
// 免掉控制台黑框一闪（QProcess 在 Qt 6.8 没有等价的开关）。
int runSystemTool(const QString &program, const QStringList &args) {
    QString line = QStringLiteral("\"%1\"").arg(program);
    for (const QString &arg : args) {
        line += QStringLiteral(" \"%1\"").arg(arg);
    }
    std::wstring command = line.toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 8000);
    DWORD code = 0;
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        code = DWORD(-1);
    } else {
        GetExitCodeProcess(process.hProcess, &code);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 ? int(code) : -1;
}

// ---------------------------------------------------------------- 媒体键

bool sendMediaKey(int vk) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = WORD(vk);
    down.ki.wScan = WORD(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC));
    down.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
    INPUT up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    // 注入键带 LLKHF_INJECTED，自己的接管钩子会放行（NativeHooks 三处都先看它）。
    if (SendInput(1, &down, sizeof(INPUT)) != 1) {
        return false;
    }
    QThread::msleep(20);
    return SendInput(1, &up, sizeof(INPUT)) == 1;
}

SystemResult failure(const QString &text) {
    SystemResult result;
    result.ok = false;
    result.text = text;
    return result;
}

SystemResult success(const QString &text, int value = -1) {
    SystemResult result;
    result.ok = true;
    result.text = text;
    result.value = value;
    return result;
}

QString screenName(ScreenScope scope) {
    switch (scope) {
    case ScreenScope::Primary:
        return tr(L"主屏");
    case ScreenScope::All:
        return tr(L"全部屏幕");
    case ScreenScope::Cursor:
        break;
    }
    return tr(L"光标所在屏");
}

}  // namespace

WinSystemControl::~WinSystemControl() {
    invalidateMonitors();
    if (dxva2_ != nullptr) {
        FreeLibrary(HMODULE(dxva2_));
        dxva2_ = nullptr;
    }
}

// 取一块逻辑屏的物理监视器句柄；kept 为真时顺手放进缓存。
void *WinSystemControl::acquireMonitor(void *hmonitor, const Ddc &ddc, bool kept) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (kept) {
        auto it = monitors_.find(quintptr(hmonitor));
        if (it != monitors_.end()) {
            return it.value();
        }
    }
    if (!ddc.getMonitors) {
        return nullptr;
    }
    DWORD count = 0;
    if (!ddc.getCount || !ddc.getCount(HMONITOR(hmonitor), &count) || count == 0) {
        return nullptr;
    }
    QVector<PhysicalMonitor> monitors(int(qMin<DWORD>(count, 8)));
    if (!ddc.getMonitors(HMONITOR(hmonitor), DWORD(monitors.size()), monitors.data())) {
        return nullptr;
    }
    // 一块逻辑屏可能挂多个物理屏，这里只调第一个，其余句柄当场还掉免泄漏。
    destroyExtras(ddc, monitors);
    void *handle = monitors[0].handle;
    if (kept) {
        monitors_.insert(quintptr(hmonitor), handle);
    }
    return handle;
}

// 把一块屏的缓存句柄丢掉并还掉（写失败时用，下次重新取）。
void WinSystemControl::dropMonitor(quintptr key, const Ddc &ddc) {
    void *handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = monitors_.find(key);
        if (it == monitors_.end()) {
            return;
        }
        handle = it.value();
        monitors_.erase(it);
    }
    if (handle != nullptr && ddc.destroy != nullptr) {
        ddc.destroy(HANDLE(handle));
    }
}

// 实测 GetPhysicalMonitorsFromHMONITOR 在进程内第一次调用会失败（老实现就卡在这：
// 第一次亮度按键没反应），带退避重取；只有读通了的句柄才进缓存。
void *WinSystemControl::resolveBrightnessTarget(void *hmonitor, const Ddc &ddc,
                                                quintptr *usedKey) {
    auto probe = [&](void *handle) {
        int percent = 0;
        int maximum = 100;
        return handle != nullptr && readBrightness(ddc, HANDLE(handle), &percent,
                                                   &maximum);
    };
    void *cached = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached = monitors_.value(quintptr(hmonitor), nullptr);
    }
    if (probe(cached)) {
        *usedKey = quintptr(hmonitor);
        return HANDLE(cached);
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        void *handle = acquireMonitor(hmonitor, ddc, false);
        if (handle != nullptr) {
            if (probe(handle)) {
                *usedKey = quintptr(hmonitor);
                std::lock_guard<std::mutex> lock(mutex_);
                monitors_.insert(quintptr(hmonitor), handle);
                return handle;
            }
            if (ddc.destroy != nullptr) {
                ddc.destroy(HANDLE(handle));
            }
        }
        // 取句柄和读值都可能是冷的：整轮退避重试，而不是拿到空句柄就放弃。
        QThread::msleep(120 * (attempt + 1));
    }
    if (cached != nullptr) {
        // 缓存里那把已经坏了（多半是插拔过显示器），别留着。
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = monitors_.find(quintptr(hmonitor));
        if (it != monitors_.end() && it.value() == cached) {
            monitors_.erase(it);
            if (ddc.destroy != nullptr) {
                ddc.destroy(HANDLE(cached));
            }
        }
    }
    return nullptr;
}

void WinSystemControl::invalidateMonitors() {
    const Ddc ddc = loadDdc(&dxva2_);
    QVector<quintptr> keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = monitors_.constBegin(); it != monitors_.constEnd(); ++it) {
            keys.append(it.key());
        }
    }
    for (quintptr key : keys) {
        dropMonitor(key, ddc);
    }
}

SystemResult WinSystemControl::adjustBrightness(int delta) {
    SystemCommand command;
    command.matched = true;
    command.kind = QStringLiteral("brightness");
    command.op = QStringLiteral("step");
    command.value = delta;
    command.screen = ScreenScope::Cursor;
    return runBrightness(command);
}

SystemResult WinSystemControl::execute(const SystemCommand &command,
                                       bool confirmed) {
    if (!command.matched) {
        return failure(tr(L"没这条命令"));
    }
    if (SystemCommandText::needsConfirm(command.kind) && !confirmed) {
        SystemResult result = failure(
            tr(L"要%1吗？说「%2」执行")
                .arg(SystemCommandText::label(command),
                     SystemCommandText::confirmPhrase(command.kind)));
        result.needsConfirm = true;
        return result;
    }
    const QString kind = command.kind;
    if (kind == QStringLiteral("volume")) {
        return runVolume(command);
    }
    if (kind == QStringLiteral("brightness")) {
        return runBrightness(command);
    }
    if (kind == QStringLiteral("media")) {
        return runMedia(command);
    }
    return runPower(command);
}

SystemResult WinSystemControl::runVolume(const SystemCommand &command) {
    ComPtr<IAudioEndpointVolume> volume;
    if (!endpointVolume(&volume)) {
        return failure(tr(L"取不到系统音量端点（没有默认输出设备？）"));
    }
    int current = 0;
    bool muted = false;
    if (!readVolume(volume.get(), &current, &muted)) {
        return failure(tr(L"读不到当前音量"));
    }
    const QString op = command.op;
    if (op == QStringLiteral("query")) {
        return success(muted ? tr(L"音量 %1%（已静音）").arg(current)
                             : tr(L"音量 %1%").arg(current),
                       current);
    }
    if (op == QStringLiteral("mute")) {
        if (FAILED(volume->SetMute(TRUE, nullptr))) {
            return failure(tr(L"静音没设上"));
        }
        return success(tr(L"已静音"), current);
    }
    if (op == QStringLiteral("unmute")) {
        if (FAILED(volume->SetMute(FALSE, nullptr))) {
            return failure(tr(L"取消静音没设上"));
        }
        return success(tr(L"已取消静音 · 音量 %1%").arg(current), current);
    }
    const int target = op == QStringLiteral("step")
                           ? qBound(0, current + command.value, 100)
                           : qBound(0, command.value, 100);
    if (FAILED(volume->SetMasterVolumeLevelScalar(float(target) / 100.0f,
                                                  nullptr))) {
        return failure(tr(L"音量没调上"));
    }
    // 明确说了要多少却还静音着，用户要的是听见声音。
    if (muted && target > 0) {
        volume->SetMute(FALSE, nullptr);
    }
    int readback = target;
    bool nowMuted = false;
    if (readVolume(volume.get(), &readback, &nowMuted)) {
        return success(tr(L"音量 %1%").arg(readback), readback);
    }
    return success(tr(L"音量 %1%").arg(target), target);
}

SystemResult WinSystemControl::runBrightness(const SystemCommand &command) {
    const bool absolute = command.op == QStringLiteral("set");
    const bool query = command.op == QStringLiteral("query");
    QVector<quintptr> targets;
    if (command.screen == ScreenScope::All) {
        EnumDisplayMonitors(nullptr, nullptr, collectMonitor,
                            reinterpret_cast<LPARAM>(&targets));
    } else {
        POINT point{0, 0};
        HMONITOR monitor = nullptr;
        if (command.screen == ScreenScope::Primary) {
            monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        } else {
            GetCursorPos(&point);
            monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        }
        if (monitor != nullptr) {
            targets.append(quintptr(monitor));
        }
    }
    if (targets.isEmpty()) {
        return failure(tr(L"找不到显示器"));
    }

    QString firstError;
    const Ddc ddc = loadDdc(&dxva2_);
    if (!ddc.ready) {
        firstError = tr(L"这台机器没有 DDC/CI 接口（dxva2 不可用）");
    }
    int done = 0;
    int reported = -1;
    for (const quintptr key : targets) {
        if (!ddc.ready) {
            break;
        }
        quintptr usedKey = 0;
        HANDLE handle = resolveBrightnessTarget(reinterpret_cast<void *>(key), ddc,
                                                &usedKey);
        if (handle == nullptr) {
            firstError = tr(L"这块屏不支持亮度调节（DDC/CI 读不到）");
            continue;
        }
        int percent = 0;
        int maximum = 100;
        if (!readBrightness(ddc, handle, &percent, &maximum)) {
            firstError = tr(L"这块屏不支持亮度调节（DDC/CI 读不到）");
            continue;
        }
        if (query) {
            const int value = qRound(percent * 100.0 / maximum);
            if (reported < 0) {
                reported = value;
            }
            ++done;
            continue;
        }
        const int wanted = absolute
                               ? qBound(0, command.value, 100)
                               : qBound(0, qRound(percent * 100.0 / maximum) +
                                               command.value,
                                        100);
        const DWORD raw = DWORD(qRound(wanted * maximum / 100.0));
        if (!ddc.setVcp(handle, kVcpBrightness, raw)) {
            // 写不进去的句柄别留在缓存里，下次重新取。
            dropMonitor(usedKey, ddc);
            firstError = tr(L"这块屏的亮度写不进去");
            continue;
        }
        // DDC 写入是异步的，回读一次才敢报数值；读不回来就报到手的目标值。
        QThread::msleep(60);
        int after = percent;
        int afterMax = maximum;
        const int value = readBrightness(ddc, handle, &after, &afterMax)
                              ? qRound(after * 100.0 / afterMax)
                              : wanted;
        if (reported < 0) {
            reported = value;
        }
        ++done;
    }

    if (done == 0) {
        // 外接屏走不通就试笔记本背光；两条都没有才算真不支持。
        int percent = 0;
        QString error = firstError.isEmpty()
                            ? tr(L"这块屏不支持亮度调节")
                            : firstError;
        if (query) {
            if (!wmiBrightness(false, 0, &percent, &error)) {
                return failure(error);
            }
            return success(tr(L"亮度 %1%").arg(percent), percent);
        }
        int current = 0;
        QString readError;
        const bool haveCurrent = wmiBrightness(false, 0, &current, &readError);
        const int wanted = absolute
                               ? qBound(0, command.value, 100)
                               : qBound(0, (haveCurrent ? current : 50) +
                                               command.value,
                                        100);
        if (!wmiBrightness(true, wanted, &percent, &error)) {
            return failure(error);
        }
        return success(tr(L"亮度 %1%").arg(percent), percent);
    }
    const QString name = screenName(command.screen);
    if (query) {
        return success(tr(L"%1亮度 %2%").arg(name).arg(reported), reported);
    }
    if (done < targets.size()) {
        return success(tr(L"亮度 %1% · %2（%3/%4 块调好，其余不支持）")
                           .arg(reported)
                           .arg(name)
                           .arg(done)
                           .arg(int(targets.size())),
                       reported);
    }
    return success(tr(L"亮度 %1% · %2").arg(reported).arg(name), reported);
}

SystemResult WinSystemControl::runMedia(const SystemCommand &command) {
    static const QHash<QString, int> keys = {
        {QStringLiteral("play"), VK_MEDIA_PLAY_PAUSE},
        {QStringLiteral("pause"), VK_MEDIA_PLAY_PAUSE},
        {QStringLiteral("toggle"), VK_MEDIA_PLAY_PAUSE},
        {QStringLiteral("next"), VK_MEDIA_NEXT_TRACK},
        {QStringLiteral("prev"), VK_MEDIA_PREV_TRACK},
    };
    const int vk = keys.value(command.op, 0);
    if (vk == 0) {
        return failure(tr(L"不认识这个媒体动作"));
    }
    if (!sendMediaKey(vk)) {
        return failure(tr(L"媒体键没发出去（被权限拦了）"));
    }
    // 播放/暂停共用一个切换键，系统不回状态，所以只报「已发送」。
    if (command.op == QStringLiteral("play") ||
        command.op == QStringLiteral("pause") ||
        command.op == QStringLiteral("toggle")) {
        return success(tr(L"已发送播放/暂停"));
    }
    return success(tr(L"已切到%1").arg(SystemCommandText::label(command)));
}

SystemResult WinSystemControl::runPower(const SystemCommand &command) {
    const QString kind = command.kind;
    if (kind == QStringLiteral("screen_off")) {
        SendMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        return success(tr(L"已关屏幕，动一下鼠标就回来"));
    }
    if (kind == QStringLiteral("lock")) {
        if (!LockWorkStation()) {
            return failure(tr(L"锁屏没成功"));
        }
        return success(tr(L"已锁屏"));
    }
    if (kind == QStringLiteral("sleep") || kind == QStringLiteral("hibernate")) {
        const bool hibernate = kind == QStringLiteral("hibernate");
        enablePrivilege(SE_SHUTDOWN_NAME);
        if (!SetSuspendState(hibernate, FALSE, FALSE)) {
            return failure(hibernate ? tr(L"这台机器没开休眠，进不去")
                                     : tr(L"睡眠没成功（可能被电源策略拦了）"));
        }
        return success(hibernate ? tr(L"已休眠") : tr(L"已睡眠"));
    }
    if (kind == QStringLiteral("shutdown") || kind == QStringLiteral("restart")) {
        const bool restart = kind == QStringLiteral("restart");
        const int code = runSystemTool(
            QStringLiteral("shutdown.exe"),
            {restart ? QStringLiteral("/r") : QStringLiteral("/s"),
             QStringLiteral("/t"), QString::number(kShutdownDelaySeconds)});
        if (code != 0) {
            return failure(tr(L"%1没排上（shutdown 返回 %2）")
                               .arg(SystemCommandText::label(command))
                               .arg(code));
        }
        return success(tr(L"%1 秒后%2，说「取消关机」撤销")
                           .arg(kShutdownDelaySeconds)
                           .arg(SystemCommandText::label(command)));
    }
    if (kind == QStringLiteral("shutdown_cancel")) {
        const int code = runSystemTool(QStringLiteral("shutdown.exe"),
                                       {QStringLiteral("/a")});
        if (code != 0) {
            return failure(tr(L"当前没有待执行的关机/重启"));
        }
        return success(tr(L"已取消关机"));
    }
    return failure(tr(L"不认识这个电源动作"));
}

namespace SystemControlSettings {

QString masterKey() {
    return QStringLiteral("voice_system");
}

QString categoryKey(const QString &kind) {
    return QStringLiteral("voice_system_") + SystemCommandText::category(kind);
}

QString categoryLabel(const QString &category) {
    static const QHash<QString, QString> names = {
        {QStringLiteral("volume"), tr(L"音量")},
        {QStringLiteral("brightness"), tr(L"亮度")},
        {QStringLiteral("media"), tr(L"媒体")},
        {QStringLiteral("power"), tr(L"电源")},
    };
    return names.value(category, category);
}

bool enabled(const ProfileStore *store, const QString &kind) {
    if (store == nullptr) {
        return false;
    }
    return store->uiBool(masterKey(), true) &&
           store->uiBool(categoryKey(kind), true);
}

}  // namespace SystemControlSettings
