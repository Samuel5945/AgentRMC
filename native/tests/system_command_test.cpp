// 语音系统控制自测：解析词表（含必须落空的句子）、二次确认状态机、
// 执行器的确认闸门与分类开关。默认全程离线，不碰真实音量/亮度/电源；
// 加 --live 才做「读 → 设 → 读回 → 还原原值」的真硬件自检。
#include "core/Injector.h"
#include "core/Profiles.h"
#include "core/SystemCommand.h"
#include "core/SystemConfirm.h"
#include "core/SystemControl.h"

#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(const char *name, bool ok) {
    if (ok) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

QString u8(const char *text) {
    return QString::fromUtf8(text);
}

struct Want {
    const char *text;
    const char *kind;
    const char *op;
    int value;
};

void expectCommand(const Want &want) {
    const SystemCommand got = SystemCommandText::parse(u8(want.text));
    const bool ok = got.matched && got.kind == QLatin1String(want.kind) &&
                    got.op == QLatin1String(want.op) && got.value == want.value;
    if (!ok) {
        std::printf("  want %s/%s/%d, got matched=%d %s/%s/%d\n", want.kind,
                    want.op, want.value, int(got.matched), qPrintable(got.kind),
                    qPrintable(got.op), got.value);
    }
    check(want.text, ok);
}

void expectMiss(const char *text) {
    const SystemCommand got = SystemCommandText::parse(u8(text));
    if (got.matched) {
        std::printf("  unexpected match: %s/%s\n", qPrintable(got.kind),
                    qPrintable(got.op));
    }
    check(text, !got.matched);
}

void testParser() {
    const Want hits[] = {
        {"音量调到50", "volume", "set", 50},
        {"音量调到 50%", "volume", "set", 50},
        {"帮我把音量调到百分之五十", "volume", "set", 50},
        {"声音百分之五十", "volume", "set", 50},
        {"音量三十五", "volume", "set", 35},
        {"音量五十", "volume", "set", 50},
        {"音量一百", "volume", "set", 100},
        {"音量零", "volume", "set", 0},
        {"声音大一点", "volume", "step", 5},
        {"音量调大", "volume", "step", 10},
        {"小声一点", "volume", "step", -5},
        {"音量减二十", "volume", "step", -20},
        {"音量加十", "volume", "step", 10},
        {"亮度降低二十", "brightness", "step", -20},
        {"音量最大", "volume", "set", 100},
        {"声音最小", "volume", "set", 0},
        {"现在音量多少", "volume", "query", 0},
        {"静音", "volume", "mute", 0},
        {"取消静音", "volume", "unmute", 0},
        {"解除静音", "volume", "unmute", 0},
        {"亮度调到30", "brightness", "set", 30},
        {"屏幕暗一点", "brightness", "step", -5},
        {"亮度调亮", "brightness", "step", 10},
        {"亮度最亮", "brightness", "set", 100},
        {"亮度最暗", "brightness", "set", 0},
        {"主屏亮度50", "brightness", "set", 50},
        {"所有屏幕亮度暗一点", "brightness", "step", -5},
        {"亮度多少", "brightness", "query", 0},
        {"播放", "media", "play", 0},
        {"暂停一下", "media", "pause", 0},
        {"下一首", "media", "next", 0},
        {"上一曲", "media", "prev", 0},
        {"关屏幕", "screen_off", "off", 0},
        {"锁屏", "lock", "lock", 0},
        {"睡眠", "sleep", "suspend", 0},
        {"休眠", "hibernate", "hibernate", 0},
        {"关机", "shutdown", "off", 0},
        {"重启电脑", "restart", "off", 0},
        {"取消关机", "shutdown_cancel", "cancel", 0},
    };
    for (const Want &want : hits) {
        expectCommand(want);
    }

    const SystemCommand all = SystemCommandText::parse(u8("所有屏幕亮度暗一点"));
    check("all screens scope", all.screen == ScreenScope::All);
    const SystemCommand primary = SystemCommandText::parse(u8("主屏亮度50"));
    check("primary screen scope", primary.screen == ScreenScope::Primary);
    const SystemCommand cursor = SystemCommandText::parse(u8("亮度调到30"));
    check("default scope is cursor screen", cursor.screen == ScreenScope::Cursor);

    // 这些话必须落空，否则会被当命令吞掉、不再听写。
    const char *misses[] = {
        "别调音量",
        "音量不要动",
        "不要把亮度调低",
        "音量大小刚好",
        "亮度变暗了",
        "声音真好听",
        "这个音量是多少来着你知道吗",
        "亮度调到五十好吗",
        "音量一百零五",
        "音量250",
        "今天天气不错",
        "帮我写一段代码",
        "别关机",
        "确认",
        "确认一下",
        "我觉得亮度可以再高一点吗",
        "声音是不是太小了",
    };
    for (const char *text : misses) {
        expectMiss(text);
    }

    const SystemCommand confirm = SystemCommandText::parse(u8("确认关机"));
    check("confirm phrase parses",
          confirm.matched && confirm.confirmed &&
              confirm.kind == QLatin1String("shutdown"));
    check("confirm phrase for sleep",
          SystemCommandText::parse(u8("确认睡眠")).confirmed);
    check("confirm phrase for restart",
          SystemCommandText::parse(u8("确认重启")).confirmed);
    check("confirm volume is not a command",
          !SystemCommandText::parse(u8("确认静音")).matched);

    check("percent chinese", SystemCommandText::percentValue(u8("五十")) == 50);
    check("percent chinese teens",
          SystemCommandText::percentValue(u8("十五")) == 15);
    check("percent chinese hundred",
          SystemCommandText::percentValue(u8("一百")) == 100);
    check("percent digits", SystemCommandText::percentValue(u8("80")) == 80);
    check("percent out of range", SystemCommandText::percentValue(u8("101")) == -1);
    check("percent junk", SystemCommandText::percentValue(u8("音量")) == -1);

    check("needsConfirm shutdown",
          SystemCommandText::needsConfirm(QStringLiteral("shutdown")));
    check("needsConfirm sleep",
          SystemCommandText::needsConfirm(QStringLiteral("sleep")));
    check("volume needs no confirm",
          !SystemCommandText::needsConfirm(QStringLiteral("volume")));
    check("category of lock is power",
          SystemCommandText::category(QStringLiteral("lock")) == QLatin1String("power"));
    check("confirm phrase text",
          SystemCommandText::confirmPhrase(QStringLiteral("shutdown")) == u8("确认关机"));
}

void testConfirm() {
    const SystemCommand shutdown = SystemCommandText::parse(u8("关机"));
    const SystemCommand confirmShutdown = SystemCommandText::parse(u8("确认关机"));
    const SystemCommand confirmSleep = SystemCommandText::parse(u8("确认睡眠"));
    const SystemCommand volume = SystemCommandText::parse(u8("音量调到50"));
    const SystemCommand nothing = SystemCommandText::parse(u8("确认"));

    SystemConfirm confirm;
    check("starts empty", !confirm.pending());
    check("offer without pending is None",
          confirm.offer(confirmShutdown, 1000) == SystemConfirm::Verdict::None);

    confirm.arm(shutdown, 1000, 20000);
    check("armed is pending", confirm.pending());
    check("bare 确认 does not confirm",
          confirm.offer(nothing, 1500) == SystemConfirm::Verdict::Cancelled);
    check("cancelled clears pending", !confirm.pending());

    confirm.arm(shutdown, 1000, 20000);
    check("other utterance cancels",
          confirm.offer(volume, 1500) == SystemConfirm::Verdict::Cancelled);
    check("other utterance cleared pending", !confirm.pending());

    confirm.arm(shutdown, 1000, 20000);
    check("wrong phrase cancels",
          confirm.offer(confirmSleep, 1500) == SystemConfirm::Verdict::Cancelled);

    confirm.arm(shutdown, 1000, 20000);
    check("exact phrase confirms",
          confirm.offer(confirmShutdown, 2000) == SystemConfirm::Verdict::Confirmed);
    const SystemCommand taken = confirm.take();
    check("take returns the armed command",
          taken.kind == QLatin1String("shutdown") && taken.matched);
    check("take clears pending", !confirm.pending());
    check("second take is empty", !confirm.take().matched);

    confirm.arm(shutdown, 1000, 20000);
    check("not expired yet", !confirm.expire(20999));
    check("expires at deadline", confirm.expire(21000));
    check("expire clears pending", !confirm.pending());
    check("expire twice is false", !confirm.expire(30000));

    confirm.arm(shutdown, 1000, 20000);
    check("offer after deadline does not confirm",
          confirm.offer(confirmShutdown, 21000) == SystemConfirm::Verdict::None);
    check("offer after deadline cleared pending", !confirm.pending());

    confirm.arm(shutdown, 1000, 20000);
    confirm.cancel();
    check("cancel clears pending", !confirm.pending());
}

void testExecutorGate() {
    WinSystemControl control;
    const SystemCommand shutdown = SystemCommandText::parse(u8("关机"));
    const SystemResult blocked = control.execute(shutdown, false);
    check("dangerous without confirm is blocked", !blocked.ok && blocked.needsConfirm);
    check("blocked result carries the phrase", blocked.text.contains(u8("确认关机")));

    const SystemResult sleepBlocked =
        control.execute(SystemCommandText::parse(u8("睡眠")), false);
    check("sleep without confirm is blocked",
          !sleepBlocked.ok && sleepBlocked.needsConfirm);

    SystemCommand unmatched;
    check("unmatched command is rejected", !control.execute(unmatched, true).ok);
}

void testSettingsGate() {
    QTemporaryDir dir;
    if (!dir.isValid()) {
        check("temp dir", false);
        return;
    }
    ProfileStore store(dir.filePath(QStringLiteral("profiles.json")));
    const QString volume = QStringLiteral("volume");
    const QString shutdown = QStringLiteral("shutdown");
    check("default enabled", SystemControlSettings::enabled(&store, volume));
    check("power default enabled", SystemControlSettings::enabled(&store, shutdown));

    store.setUiBool(SystemControlSettings::categoryKey(volume), false);
    check("category off disables volume",
          !SystemControlSettings::enabled(&store, volume));
    check("category off keeps power",
          SystemControlSettings::enabled(&store, shutdown));

    store.setUiBool(SystemControlSettings::categoryKey(volume), true);
    store.setUiBool(SystemControlSettings::masterKey(), false);
    check("master off disables everything",
          !SystemControlSettings::enabled(&store, volume) &&
              !SystemControlSettings::enabled(&store, shutdown));
    check("null store is disabled",
          !SystemControlSettings::enabled(static_cast<ProfileStore *>(nullptr),
                                          volume));
}

// 「你会说什么」的清单必须和解析词表同源：清单里给的每个直说口令 parse() 都得
// 认（有人手抄一份新文案、或从表里删了说法，这里就会红）。
void testSpokenList() {
    const QString list = SystemCommandText::spokenList();
    static const char *kSpoken[] = {
        "播放",   "暂停",     "播放暂停", "下一首",   "上一首",   "关屏幕",
        "锁屏",   "睡眠",     "休眠",     "关机",     "重启",     "取消关机",
        "静音",   "取消静音",
    };
    for (const char *spoken : kSpoken) {
        const QString word = u8(spoken);
        char name[96];
        std::snprintf(name, sizeof(name), "list keeps spoken command [%s]",
                      spoken);
        check(name, list.contains(word) &&
                        SystemCommandText::parse(word).matched);
    }
    check("list covers volume nouns",
          list.contains(u8("音量")) && list.contains(u8("大声")));
    check("list covers brightness nouns",
          list.contains(u8("亮度")) && list.contains(u8("显示器")));
    check("list covers spoken percent", list.contains(u8("百分之三十")));
    check("list covers query", list.contains(u8("音量多少")));
    check("list covers screen scope", list.contains(u8("主屏")));
    check("list warns about confirm",
          list.contains(u8("确认关机")) && list.contains(u8("危险")));
    check("list does not invent capabilities",
          !list.contains(u8("风扇")) && !list.contains(u8("蓝牙")));
}

int runOfflineTests() {
    testParser();
    testConfirm();
    testExecutorGate();
    testSettingsGate();
    testSpokenList();
    std::printf(failures == 0 ? "\nALL PASS system\n" : "\n%d FAILED\n", failures);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}

void say(const char *what, const SystemResult &result) {
    std::printf("%-14s ok=%d value=%3d  %s\n", what, int(result.ok), result.value,
                qPrintable(result.text));
    std::fflush(stdout);
}

SystemCommand setVolume(int percent) {
    return SystemCommandText::parse(
        u8("音量调到") + QString::number(percent));
}

// 真硬件自检：只动音量和亮度，且最后必须还原回原值。
int runLiveTests(int argc, char **argv) {
    WinSystemControl control;
    const bool withMedia =
        argc > 2 && QString::fromLocal8Bit(argv[2]) == QLatin1String("--media");
    const bool withPower =
        argc > 2 && QString::fromLocal8Bit(argv[2]) == QLatin1String("--power");
    const bool everyScope =
        argc > 2 && QString::fromLocal8Bit(argv[2]) == QLatin1String("--scopes");

    const SystemResult volumeBefore =
        control.execute(SystemCommandText::parse(u8("现在音量多少")));
    say("音量 原值", volumeBefore);
    if (volumeBefore.ok && volumeBefore.value >= 0) {
        say("音量 +10", control.execute(SystemCommandText::parse(u8("音量加十"))));
        say("音量 回原", control.execute(setVolume(volumeBefore.value)));
        say("静音", control.execute(SystemCommandText::parse(u8("静音"))));
        say("取消静音", control.execute(SystemCommandText::parse(u8("取消静音"))));
        const SystemResult restored =
            control.execute(SystemCommandText::parse(u8("现在音量多少")));
        say("音量 校验", restored);
        if (restored.value != volumeBefore.value) {
            std::printf("  !! 没还原回原值：%d -> %d\n", volumeBefore.value,
                        restored.value);
        }
    }

    const SystemResult lumBefore =
        control.execute(SystemCommandText::parse(u8("亮度多少")));
    say("亮度 原值", lumBefore);
    if (everyScope) {
        // 三屏机器上分清每一档是哪块屏（只读，不动亮度）。
        const struct {
            const char *name;
            const char *text;
            ScreenScope scope;
        } scopes[] = {
            {"光标屏", "亮度多少", ScreenScope::Cursor},
            {"主屏", "主屏亮度多少", ScreenScope::Primary},
            {"全部", "所有屏幕亮度多少", ScreenScope::All},
        };
        for (const auto &entry : scopes) {
            SystemCommand command = SystemCommandText::parse(u8(entry.text));
            command.screen = entry.scope;
            say(entry.name, control.execute(command));
        }
    }
    if (lumBefore.ok && lumBefore.value >= 0) {
        say("亮度 -10", control.execute(SystemCommandText::parse(u8("亮度减十"))));
        say("亮度 +10", control.execute(SystemCommandText::parse(u8("亮度加十"))));
        say("亮度 校验", control.execute(SystemCommandText::parse(u8("亮度多少"))));
    }
    // 遥控器亮度动作走的是 WinInjector::brightness → 同一个 SystemControl，
    // 这里验它没退化：按下去要真的变暗，再按回来。
    {
        WinInjector injector;
        const int before = control.execute(SystemCommandText::parse(u8("亮度多少"))).value;
        injector.brightness(-10);
        const int after = control.execute(SystemCommandText::parse(u8("亮度多少"))).value;
        injector.brightness(10);
        const int back = control.execute(SystemCommandText::parse(u8("亮度多少"))).value;
        std::printf("遥控器亮度      %d -> %d -> %d (%s)\n", before, after, back,
                    (after < before && back >= before) ? "生效" : "没变化");
        std::fflush(stdout);
    }

    if (withMedia) {
        say("暂停", control.execute(SystemCommandText::parse(u8("暂停"))));
        say("播放", control.execute(SystemCommandText::parse(u8("播放"))));
    }
    if (withPower) {
        // 关机只验闸门拦得住，不真排程。
        const SystemResult gate =
            control.execute(SystemCommandText::parse(u8("关机")), false);
        std::printf("关机闸门       needsConfirm=%d  %s\n", int(gate.needsConfirm),
                    qPrintable(gate.text));
        say("关屏幕", control.execute(SystemCommandText::parse(u8("关屏幕"))));
        std::printf("  （动一下鼠标屏幕就回来）\n");
    }
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--live")) {
        return runLiveTests(argc, argv);
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--list")) {
        // 给验收用：把喂给对话模型的功能清单原样打出来，看它会不会念顺。
        std::printf("%s\n", SystemCommandText::spokenList().toUtf8().constData());
        return 0;
    }
    return runOfflineTests();
}
