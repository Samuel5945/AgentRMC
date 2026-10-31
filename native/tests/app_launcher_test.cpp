#include "core/AppLauncher.h"
#include "core/Presets.h"
#include "core/Profiles.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QJsonObject>
#include <QTemporaryDir>

#include <QFile>

#include <cstdio>

namespace {

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fputs("FAIL: ", stderr);
        std::fputs(message, stderr);
        std::fputc(10, stderr);
    }
    return condition;
}

bool isApp(const QString &text, const QString &expectId) {
    const AppVoiceMatch match = AppLauncher::matchVoice(text);
    return match.matched && match.appId == expectId;
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    // --launch <appId>：在真机上当场启动一次，用来验解析和启动是否真的成立。
    const QStringList args = app.arguments();
    if (args.size() > 2 && args.at(1) == QStringLiteral("--launch")) {
        QString error;
        const bool launched = AppLauncher::launch(args.at(2), &error);
        std::printf(launched ? "launched" : "failed: ");
        std::puts(error.toLocal8Bit().constData());
        return launched ? 0 : 1;
    }
    // --match <text>：看一句话到底命中了谁，排查误启动用。
    if (args.size() > 2 && args.at(1) == QStringLiteral("--match")) {
        const AppVoiceMatch m = AppLauncher::matchVoice(args.at(2));
        std::printf("matched=%d builtin=%d resolved=%d appId=",
                    int(m.matched), int(m.builtin), int(m.resolved));
        std::puts(m.appId.isEmpty() ? "-" : m.appId.toLocal8Bit().constData());
        if (args.size() > 3 && args.at(3) == QStringLiteral("-v")) {
            for (const AppEntry &entry : AppLauncher::catalog()) {
                std::printf("  %s\n", entry.id.toLocal8Bit().constData());
            }
        }
        return 0;
    }

    bool ok = true;

    ok &= check(AppLauncher::normalize(QStringLiteral("打开 Qoder CN！")) ==
                   QStringLiteral("打开qodercn"),
                "normalize strips spaces/punctuation");

    // 内置应用：说法要能覆盖中英混说和口语填充词。
    ok &= check(isApp(QStringLiteral("打开浏览器"), QStringLiteral("browser")),
                "browser");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("上网")).matched,
                "no verb means dictation, not a command");
    ok &= check(isApp(QStringLiteral("帮我打开剪映吧"), QStringLiteral("jianying")),
                "jianying with fillers");
    ok &= check(isApp(QStringLiteral("我要打开剪映专业版"),
                      QStringLiteral("jianying")),
                "jianying full name");
    ok &= check(isApp(QStringLiteral("打开 Qoder"), QStringLiteral("qoder")),
                "qoder");
    ok &= check(isApp(QStringLiteral("打开Qoder CN"), QStringLiteral("qodercn")),
                "qoder cn wins over qoder");
    ok &= check(isApp(QStringLiteral("开一下WorkBuddy"), QStringLiteral("workbuddy")),
                "workbuddy short verb");
    ok &= check(isApp(QStringLiteral("启动 work buddy"), QStringLiteral("workbuddy")),
                "workbuddy spaced");
    ok &= check(isApp(QStringLiteral("打开deepseek"), QStringLiteral("deepseek")),
                "deepseek");
    ok &= check(isApp(QStringLiteral("打开codex"), QStringLiteral("codex")),
                "codex");
    ok &= check(isApp(QStringLiteral("启动ChatGPT"), QStringLiteral("codex")),
                "chatgpt maps to codex");
    ok &= check(isApp(QStringLiteral("打开zcode"), QStringLiteral("zcode")),
                "zcode");
    ok &= check(isApp(QStringLiteral("打开chrome"), QStringLiteral("chrome")),
                "chrome");
    ok &= check(isApp(QStringLiteral("打开微软浏览器"), QStringLiteral("edge")),
                "edge");
    ok &= check(isApp(QStringLiteral("切换到剪映"), QStringLiteral("jianying")),
                "switch verb");

    // SenseVoice 会把 Qoder 听成 "coder"，还常带个字母尾巴。
    ok &= check(isApp(QStringLiteral("打开 CODER C"), QStringLiteral("qoder")),
                "latin alias tolerates a short asr tail");
    ok &= check(isApp(QStringLiteral("打开coder cn"), QStringLiteral("qodercn")),
                "cn alias still wins");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开coder设置")).matched,
                "chinese tail after a latin name is dictation");

    // 中英混说被整个听岔（真机事件流里的实际转写）：模糊档要能救回来。
    ok &= check(isApp(QStringLiteral("打开 WOKBER"), QStringLiteral("workbuddy")),
                "garbled workbuddy (WOKBER)");
    ok &= check(isApp(QStringLiteral("打开WRK八点"), QStringLiteral("workbuddy")),
                "garbled workbuddy (WRK八点)");
    ok &= check(isApp(QStringLiteral("打开 DIP SCK"), QStringLiteral("deepseek")),
                "garbled deepseek (DIP SCK)");
    ok &= check(isApp(QStringLiteral("打开 DEEP SHCK"), QStringLiteral("deepseek")),
                "garbled deepseek (DEEP SHCK)");

    // 中文应用名出别字（真机事件流：SenseVoice 把「剪映」听成这些）。
    ok &= check(isApp(QStringLiteral("打开剪易"), QStringLiteral("jianying")),
                "garbled jianying (剪易)");
    ok &= check(isApp(QStringLiteral("打开简验"), QStringLiteral("jianying")),
                "garbled jianying (简验)");
    ok &= check(isApp(QStringLiteral("打开解验"), QStringLiteral("jianying")),
                "garbled jianying (解验)");
    ok &= check(isApp(QStringLiteral("打开检验"), QStringLiteral("jianying")),
                "garbled jianying (检验)");
    ok &= check(isApp(QStringLiteral("打开剪印"), QStringLiteral("jianying")),
                "garbled jianying (剪印)");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开简面")).matched,
                "jianmian is below the pinyin bar (accepted miss)");
    // 模糊档不许误伤：数数、没装的应用、形近但首字母不同的名、纯口述。
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开第四个")).matched,
                "counting words are not an app");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开链机")).matched,
                "lianji must not fuzzy into liulanqi (browser)");
    ok &= check(!isApp(QStringLiteral("打开vscode"), QStringLiteral("zcode")),
                "vscode must not fuzzy into zcode");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开这个应用")).matched,
                "dictation like 这个应用 must not fuzzy into zcode");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开剪映设置")).matched,
                "app name followed by content is dictation");

    // 普通口述不能被当成指令。
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开浏览器以后帮我查一下天气"))
                     .matched,
                "sentence longer than an app name is dictation");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("这段代码打开后报错")).matched,
                "verb in the middle is dictation");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("打开")).matched,
                "verb alone does nothing");
    ok &= check(!AppLauncher::matchVoice(QStringLiteral("今天心情不错")).matched,
                "unrelated text");

    // 浏览器永远有目标；其余按这台电脑上的实际安装情况走。
    const AppTarget browser = AppLauncher::resolve(QStringLiteral("browser"));
    ok &= check(browser.found && !browser.target.isEmpty(), "browser resolves");

    // 内置表之外的应用走开始菜单兜底：命中就不是内置应用，但一定有快捷方式。
    const AppVoiceMatch generic =
        AppLauncher::matchVoice(QStringLiteral("打开notepad"));
    ok &= check(!generic.matched || (generic.resolved && !generic.builtin &&
                                     generic.target.kind ==
                                         QStringLiteral("shortcut")),
                "start-menu fallback");


    // 按键映射里的「启动应用」预设。
    const QJsonObject action =
        Presets::actionFor(QStringLiteral("app_jianying"), 36, QStringLiteral("single"));
    ok &= check(action.value("type").toString() == QStringLiteral("launch"),
                "preset type is launch");
    ok &= check(action.value("app").toString() == QStringLiteral("jianying"),
                "preset carries app id");
    ok &= check(action.value("label").toString() ==
                    QStringLiteral("打开 剪映"),
                "preset label");
    ok &= check(Presets::describeAction(action, false) ==
                    QStringLiteral("打开 剪映"),
                "describe launch");
    bool hasGroup = false;
    for (const auto &group : Presets::groups()) {
        if (group.first == QStringLiteral("启动应用")) {
            hasGroup = group.second.size() == AppLauncher::appIds().size();
        }
    }
    ok &= check(hasGroup, "launch-app group lists every app");
    ok &= check(!Presets::appKinds().isEmpty() &&
                    Presets::appKinds().first() == QStringLiteral("app_browser"),
                "app kinds are prefixed");

    // 「启动应用」预设要能写回 profiles.json，并原样读回来给 Router 用。
    QTemporaryDir dir;
    ok &= check(dir.isValid(), "temp dir");
    const QString path = dir.filePath(QStringLiteral("profiles.json"));
    const QJsonObject defaultProfile{
        {QStringLiteral("name"), QStringLiteral("default")},
        {QStringLiteral("keys"),
         QJsonObject{{QStringLiteral("36"), QStringLiteral("home")}}},
        {QStringLiteral("actions"),
         QJsonObject{{QStringLiteral("home"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("none")}}}}},
    };
    const QJsonDocument doc{
        QJsonObject{{QStringLiteral("profiles"), QJsonArray{defaultProfile}}}};
    {
        QFile file(path);
        ok &= check(file.open(QIODevice::WriteOnly), "write temp profiles");
        file.write(doc.toJson(QJsonDocument::Indented));
    }
    ProfileStore store(path);
    ok &= check(store.load(), "store loads");
    ok &= check(store.apply(QStringLiteral("default"), 36, QStringLiteral("double"),
                            QStringLiteral("app_qoder")),
                "apply launch preset");
    const Profile *saved = store.find(QStringLiteral("default"));
    const QString sem = saved->doubleMap.value(QStringLiteral("36")).toString();
    const QJsonObject written = saved->actions.value(sem).toObject();
    ok &= check(written.value("type").toString() == QStringLiteral("launch"),
                "written action is launch");
    ok &= check(written.value("app").toString() == QStringLiteral("qoder"),
                "written app id");

    if (!ok) {
        return 1;
    }
    std::fputs("PASS native AppLauncher: voice phrases / app presets", stdout);
    std::fputc(10, stdout);
    return 0;
}
