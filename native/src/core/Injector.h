#pragma once

#include <QString>
#include <QStringList>

class Injector {
public:
    virtual ~Injector() = default;
    virtual void key(const QStringList &mods, int vk, int repeat = 1) = 0;
    virtual void scroll(int delta) = 0;
    virtual void mouseMove(int dx, int dy) = 0;
    virtual void mouseClick() = 0;
    virtual void brightness(int delta) = 0;
    virtual void run(const QString &cmd) = 0;
};

class WinInjector final : public Injector {
public:
    void key(const QStringList &mods, int vk, int repeat = 1) override;
    void scroll(int delta) override;
    void mouseMove(int dx, int dy) override;
    void mouseClick() override;
    void brightness(int delta) override;
    void run(const QString &cmd) override;

private:
    void sendKey(int vk, bool up);
    void *brightnessMonitor();
    void *dxva2_ = nullptr;
    void *monitor_ = nullptr;
};
