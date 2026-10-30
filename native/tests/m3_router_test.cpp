#include "core/KeyRouter.h"
#include "core/Profiles.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <cstdio>
#include <memory>
#include <vector>

namespace {

struct InjectedKey {
    QString mods;
    int vk = 0;
    int repeat = 1;
};

class FakeInjector final : public Injector {
public:
    void key(const QStringList &mods, int vk, int repeat) override {
        keys.push_back({mods.join('+'), vk, repeat});
    }
    void scroll(int delta) override { scrolls.push_back(delta); }
    void mouseMove(int dx, int dy) override { moves.emplace_back(dx, dy); }
    void mouseClick() override { ++clicks; }
    void brightness(int delta) override { brightnesses.push_back(delta); }
    void run(const QString &cmd) override { runs.push_back(cmd); }

    void clear() {
        keys.clear();
        scrolls.clear();
        moves.clear();
        clicks = 0;
        brightnesses.clear();
        runs.clear();
    }

    std::vector<InjectedKey> keys;
    std::vector<int> scrolls;
    std::vector<std::pair<int, int>> moves;
    int clicks = 0;
    std::vector<int> brightnesses;
    std::vector<QString> runs;
};

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fputs("FAIL: ", stderr);
        std::fputs(message, stderr);
        std::fputc(10, stderr);
    }
    return condition;
}

QJsonObject keyAction(int vk, const QString &label) {
    return QJsonObject{{"type", "key"}, {"vk", vk}, {"label", label}};
}

Profile makeProfile() {
    Profile profile;
    profile.name = "test";
    profile.keys = QJsonObject{
        {"38", "scroll_up"}, {"40", "scroll_down"},
        {"37", "cursor_left"}, {"39", "cursor_right"},
        {"13", "send"}, {"27", "back"}, {"93", "menu"}, {"36", "home"}};
    profile.actions = QJsonObject{
        {"scroll_up", QJsonObject{{"type", "scroll"}, {"delta", 300},
                                  {"hold_repeat", 100}, {"hold_accel", true},
                                  {"label", QStringLiteral("向上滚动")}}},
        {"scroll_down", QJsonObject{{"type", "scroll"}, {"delta", -300}}},
        {"cursor_left", QJsonObject{{"type", "none"},
                                    {"note", QStringLiteral("原生 ←")}}},
        {"cursor_right", QJsonObject{{"type", "none"},
                                     {"note", QStringLiteral("原生 →")}}},
        {"send", QJsonObject{{"type", "none"}, {"note", "Enter"}}},
        {"back", QJsonObject{{"type", "none"}, {"note", "Esc"}}},
        {"menu", QJsonObject{{"type", "key"}, {"vk", 0x0D},
                             {"mods", QJsonArray{"shift"}},
                             {"label", QStringLiteral("换行")}}},
        {"home", QJsonObject{{"type", "scroll"}, {"delta", 9000},
                             {"repeat", 3},
                             {"label", QStringLiteral("滚到顶")}}},
        {"k37_d_backspace", keyAction(0x08, QStringLiteral("退格"))},
        {"k37_h_page_up", keyAction(0x21, "PageUp")},
        {"nav_next", keyAction(0x09, "Tab")},
    };
    profile.doubleMap = QJsonObject{{"37", "k37_d_backspace"}};
    profile.hold = QJsonObject{{"37", "k37_h_page_up"}};
    profile.nav = QJsonObject{
        {"keys", QJsonObject{{"39", "nav_next"}}}, {"exit_key", "36"}};
    return profile;
}

int lastKey(const FakeInjector &fake) {
    return fake.keys.empty() ? -1 : fake.keys.back().vk;
}

}  // namespace

int main() {
    FakeInjector fake;
    std::vector<QString> events;
    KeyRouter router(&fake, [&](const QString &kind, const QString &name,
                                const QString &profile, const QString &detail,
                                int vk) {
        Q_UNUSED(profile);
        Q_UNUSED(vk);
        events.push_back(kind + '|' + name + '|' + detail);
    });
    router.setProfile(std::make_shared<Profile>(makeProfile()));
    bool ok = true;

    // 单次滚动。
    fake.clear();
    router.keyDown(38, 100.0);
    ok &= check(fake.scrolls == std::vector<int>{300}, "scroll single");
    router.keyUp(38, 100.1);

    // 按住连滚。
    fake.clear();
    router.keyDown(38, 200.0);
    router.tick(200.1);
    ok &= check(fake.scrolls.size() == 2, "scroll repeat");
    router.keyUp(38, 200.15);

    // 直通单击：双击窗口后补动作（M3 没有原生接管，none 只记事件）。
    fake.clear();
    events.clear();
    router.keyDown(37, 300.0);
    router.keyUp(37, 300.05);
    router.tick(300.31);
    ok &= check(fake.keys.empty(), "none single no injection");
    ok &= check(!events.empty() &&
                    events.back().contains("cursor_left"),
                "none single event");

    // 双击：只发退格，不发第一次单击。
    fake.clear();
    events.clear();
    router.keyDown(37, 400.0);
    router.keyUp(37, 400.05);
    router.keyDown(37, 400.2);
    router.keyUp(37, 400.25);
    ok &= check(fake.keys.size() == 1 && lastKey(fake) == 0x08,
                "double backspace");
    ok &= check(events.size() == 1 && events.back().contains("k37_d_backspace"),
                "double event");

    // 长按：双击窗口后接 PageUp。
    fake.clear();
    router.keyDown(37, 500.0);
    router.tick(500.31);
    router.tick(500.62);
    ok &= check(fake.keys.size() == 1 && lastKey(fake) == 0x21,
                "hold pageup");
    router.keyUp(37, 500.7);

    // 媒体模式：双击返回进入，↑ 调音量，再按返回退出。
    fake.clear();
    events.clear();
    router.keyDown(27, 600.0);
    router.keyUp(27, 600.05);
    router.keyDown(27, 600.2);
    router.keyUp(27, 600.25);
    ok &= check(!events.empty() &&
                    events.back().contains(QString::fromUtf8("音量/亮度")),
                "media status");
    router.keyDown(38, 600.3);
    ok &= check(lastKey(fake) == 0xAF, "media volume");
    router.keyUp(38, 600.35);
    router.keyDown(27, 600.4);
    router.keyUp(27, 600.45);
    ok &= check(router.mode().isEmpty(), "media exit");
    router.tick(650.0);

    // 导航模式：双击主页进入，→ 走 nav.keys，主页退出。
    fake.clear();
    events.clear();
    router.keyDown(36, 700.0);
    router.keyUp(36, 700.05);
    router.keyDown(36, 700.2);
    router.keyUp(36, 700.25);
    ok &= check(!events.empty() && events.back().contains("导航模式"),
                "nav status");
    router.keyDown(39, 700.3);
    ok &= check(lastKey(fake) == 0x09, "nav key");
    router.keyUp(39, 700.35);
    router.keyDown(36, 700.4);
    router.keyUp(36, 700.45);

    // 鼠标模式：双击菜单进入，↑ 移动鼠标、OK 点击、返回退出。
    fake.clear();
    events.clear();
    router.keyDown(93, 800.0);
    router.keyUp(93, 800.05);
    router.keyDown(93, 800.2);
    router.keyUp(93, 800.25);
    router.keyDown(38, 800.3);
    ok &= check(!fake.moves.empty() && fake.moves.back() == std::make_pair(0, -8),
                "cursor move");
    router.keyUp(38, 800.35);
    router.keyDown(13, 800.4);
    ok &= check(fake.clicks == 1, "cursor click");
    router.keyUp(13, 800.45);
    router.keyDown(27, 800.5);
    router.keyUp(27, 800.55);
    ok &= check(router.mode().isEmpty(), "cursor exit");

    if (!ok) {
        return 1;
    }
    std::fputs("PASS native KeyRouter: scroll / tap / double / hold / modes",
               stdout);
    std::fputc(10, stdout);
    return 0;
}
