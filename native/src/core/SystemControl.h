#pragma once

#include <QHash>
#include <QString>

#include <mutex>

#include "core/SystemCommand.h"

class ProfileStore;

// dxva2 里那几个 DDC/CI 入口的函数指针，全是 POD，免得到处 include windows.h。
struct Ddc;

struct SystemResult {
    bool ok = false;
    QString text;   // 给人看、也给桌宠念的一句话（失败时说清为什么）
    int value = -1; // 读回的真值（音量/亮度百分比）；没有就是 -1
    bool needsConfirm = false;  // 危险动作没过二次确认，什么都没做
};

class ISystemControl {
public:
    virtual ~ISystemControl() = default;
    // confirmed 只由二次确认流程置真。危险动作缺确认一律不执行。
    virtual SystemResult execute(const SystemCommand &command,
                                 bool confirmed = false) = 0;
};

// 语音、遥控器、HTTP 桥共用这一份实现。execute 可能阻塞几百毫秒（DDC/CI），
// 主线程调用方要自己丢线程池；内部缓存有锁，两条路并发进来是安全的。
class WinSystemControl final : public ISystemControl {
public:
    ~WinSystemControl() override;

    SystemResult execute(const SystemCommand &command,
                         bool confirmed = false) override;
    // 遥控器亮度动作走这里（只有幅度、作用在光标所在屏）。
    SystemResult adjustBrightness(int delta);
    // 插拔显示器/改分辨率后旧句柄会失效，主窗收到 WM_DISPLAYCHANGE 时调。
    void invalidateMonitors();

private:
    SystemResult runVolume(const SystemCommand &command);
    SystemResult runBrightness(const SystemCommand &command);
    SystemResult runMedia(const SystemCommand &command);
    SystemResult runPower(const SystemCommand &command);
    // 取/还物理监视器句柄：读通了的才进缓存，坏句柄不住进来。
    // 句柄类型在 .cpp 里按 void*↔HANDLE 对应（头文件不 include windows.h）。
    void *acquireMonitor(void *hmonitor, const Ddc &ddc, bool kept);
    void *resolveBrightnessTarget(void *hmonitor, const Ddc &ddc,
                                  quintptr *usedKey);
    void dropMonitor(quintptr key, const Ddc &ddc);

    void *dxva2_ = nullptr;
    QHash<quintptr, void *> monitors_;
    std::mutex mutex_;
};

namespace SystemControlSettings {

// 总开关 + 分类开关，默认全开。调用方在主线程判完再 execute——
// execute 会在线程池里跑，不能让它回头读配置。
bool enabled(const ProfileStore *store, const QString &kind);
QString masterKey();
QString categoryKey(const QString &kind);
QString categoryLabel(const QString &category);

}  // namespace SystemControlSettings
