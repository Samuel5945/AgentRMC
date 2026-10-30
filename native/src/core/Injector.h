#pragma once

#include <QString>
#include <QStringList>

class Injector {
public:
    virtual ~Injector() = default;
    virtual void key(const QStringList &mods, int vk, int repeat = 1) = 0;
    virtual void keyDown(const QStringList &mods, int vk) { Q_UNUSED(mods); Q_UNUSED(vk); }
    virtual void keyUp(const QStringList &mods, int vk) { Q_UNUSED(mods); Q_UNUSED(vk); }
    virtual void scroll(int delta) = 0;
    virtual void mouseMove(int dx, int dy) = 0;
    virtual void mouseClick() = 0;
    virtual void brightness(int delta) = 0;
    virtual void run(const QString &cmd) = 0;
    virtual bool copyText(const QString &text) { Q_UNUSED(text); return false; }
    virtual bool pasteText(const QString &text, bool autoEnter) {
        Q_UNUSED(text); Q_UNUSED(autoEnter); return false;
    }
};

class WinInjector final : public Injector {
public:
    void key(const QStringList &mods, int vk, int repeat = 1) override;
    void keyDown(const QStringList &mods, int vk) override;
    void keyUp(const QStringList &mods, int vk) override;
    void scroll(int delta) override;
    void mouseMove(int dx, int dy) override;
    void mouseClick() override;
    void brightness(int delta) override;
    void run(const QString &cmd) override;
    bool copyText(const QString &text) override;
    bool pasteText(const QString &text, bool autoEnter) override;

private:
    void sendKey(int vk, bool up);
    void *brightnessMonitor();
    void *dxva2_ = nullptr;
    void *monitor_ = nullptr;
};
