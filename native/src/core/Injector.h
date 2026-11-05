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
    virtual bool launch(const QString &appId) = 0;
    virtual bool copyText(const QString &text) { Q_UNUSED(text); return false; }
    virtual bool pasteText(const QString &text, bool autoEnter) {
        Q_UNUSED(text); Q_UNUSED(autoEnter); return false;
    }
    // 发一个组合键（如 Ctrl+N），逐键校验 SendInput 是否真的注入成功。
    virtual bool sendHotkey(const QStringList &mods, int vk) {
        Q_UNUSED(mods); Q_UNUSED(vk); return false;
    }
    // 把物理上仍按住的 Ctrl/Alt/Shift/Win 全部松开：热键说话后用户可能
    // 还按着修饰键，不清掉的话 Ctrl+V 会变成 Ctrl+Alt+V 之类的组合。
    virtual void releaseModifiers() {}
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
    bool launch(const QString &appId) override;
    bool copyText(const QString &text) override;
    bool pasteText(const QString &text, bool autoEnter) override;
    bool sendHotkey(const QStringList &mods, int vk) override;
    void releaseModifiers() override;

private:
    bool sendKey(int vk, bool up);
};
