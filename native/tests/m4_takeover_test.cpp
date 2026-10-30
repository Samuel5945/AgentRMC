#include "core/KeyRouter.h"
#include "core/NativeHooks.h"
#include "core/Profiles.h"

#include <QJsonObject>
#include <QStringList>

#include <atomic>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

namespace {

class FakeInjector final : public Injector {
public:
    void key(const QStringList &mods, int vk, int repeat) override {
        Q_UNUSED(mods);
        Q_UNUSED(repeat);
        keys.push_back(vk);
    }
    void scroll(int) override {}
    void mouseMove(int, int) override {}
    void mouseClick() override {}
    void brightness(int) override {}
    void run(const QString &) override {}

    void clear() { keys.clear(); }
    std::vector<int> keys;
};

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fputs("FAIL: ", stderr);
        std::fputs(message, stderr);
        std::fputc(10, stderr);
    }
    return condition;
}

Profile profile() {
    Profile p;
    p.name = "test";
    p.keys = QJsonObject{{"37", "cursor_left"}, {"38", "scroll_up"},
                         {"27", "back"}, {"93", "menu"}};
    p.actions = QJsonObject{
        {"cursor_left",
         QJsonObject{{"type", "none"}, {"note", QStringLiteral("原生 ←")}}},
        {"scroll_up", QJsonObject{{"type", "scroll"}, {"delta", 300}}},
        {"back", QJsonObject{{"type", "none"}, {"note", "Esc"}}},
        {"menu", QJsonObject{{"type", "none"}, {"note", "Apps"}}},
        {"k37_d_backspace",
         QJsonObject{{"type", "key"}, {"vk", 0x08},
                     {"label", QStringLiteral("退格")}}},
    };
    p.doubleMap = QJsonObject{{"37", "k37_d_backspace"}};
    return p;
}

}  // namespace

int main() {
    FakeInjector fake;
    std::atomic<bool> cursorMode{false};
    std::atomic<double> cursorLast{0};
    KeyRouter router(&fake, {});
    router.setCursorClocks(&cursorMode, &cursorLast);
    router.setProfile(std::make_shared<Profile>(profile()));
    bool ok = true;

    // 接管：直通单击由 Router 补发源键。
    router.setTakeover(true, {37, 38, 27, 93});
    fake.clear();
    router.keyDown(37, 100.0);
    router.tick(100.31);
    ok &= check(fake.keys == std::vector<int>{37}, "takeover pass tap");
    router.keyUp(37, 100.4);

    // 接管：双击只发退格，不再补 ←。
    fake.clear();
    router.keyDown(37, 200.0);
    router.keyUp(37, 200.05);
    router.keyDown(37, 200.2);
    router.keyUp(37, 200.25);
    ok &= check(fake.keys == std::vector<int>{8}, "takeover double");

    // 接管：设备连发逐次补源键。
    fake.clear();
    router.keyDown(37, 300.0);
    router.tick(300.31);
    router.keyDown(37, 300.35);
    ok &= check(fake.keys == std::vector<int>({37, 37}), "takeover repeat");
    router.keyUp(37, 300.4);

    // 关闭接管：none 不再补发。
    router.setTakeover(false, {});
    fake.clear();
    router.keyDown(37, 400.0);
    router.tick(400.31);
    ok &= check(fake.keys.empty(), "no takeover no tap");
    router.keyUp(37, 400.4);

    // 鼠标模式标记由 Router 维护，供钩子读取。
    fake.clear();
    router.keyDown(93, 500.0);
    router.keyUp(93, 500.05);
    router.keyDown(93, 500.2);
    router.keyUp(93, 500.25);
    ok &= check(cursorMode.load(), "cursor mode flag on");
    router.keyDown(27, 500.3);
    router.keyUp(27, 500.35);
    ok &= check(!cursorMode.load(), "cursor mode flag off");

    // 钩子纯判定 / 光标步长。
    ok &= check(nativeShouldSwallow(37, false, true, {37}), "swallow on");
    ok &= check(!nativeShouldSwallow(37, true, true, {37}), "injected pass");
    ok &= check(!nativeShouldSwallow(37, false, false, {37}), "disabled pass");
    ok &= check(!nativeShouldSwallow(38, false, true, {37}), "not mapped pass");
    ok &= check(nativeCursorStep(1) == 8 && nativeCursorStep(2) == 8 &&
                    nativeCursorStep(3) == 10 && nativeCursorStep(5) == 12 &&
                    nativeCursorStep(17) == 24,
                "cursor step accel");

    // 耳机播放键：只吞物理键，注入键放行；开关关闭时不动。
    ok &= check(nativeShouldSwallowMedia(0xB3, false, true, 0xB3),
                "media swallow on");
    ok &= check(!nativeShouldSwallowMedia(0xB3, true, true, 0xB3),
                "media injected pass");
    ok &= check(!nativeShouldSwallowMedia(0xB3, false, false, 0xB3),
                "media disabled pass");
    ok &= check(!nativeShouldSwallowMedia(0xB3, false, true, 0),
                "media unset pass");

    if (!ok) {
        return 1;
    }
    std::fputs("PASS native takeover: pass/double/repeat/flag/guard rules",
               stdout);
    std::fputc(10, stdout);
    return 0;
}
