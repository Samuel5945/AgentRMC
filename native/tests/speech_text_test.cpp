#include "core/SpeechText.h"

#include <cstdio>

namespace {

int failures = 0;

void check(const char *name, const QString &actual, const QString &expected) {
    if (actual == expected) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", name,
                    expected.toUtf8().constData(), actual.toUtf8().constData());
        ++failures;
    }
}

QString s(const char *utf8) { return QString::fromUtf8(utf8); }

}  // namespace

int main() {
    check("windows path -> drive + basename",
          SpeechText::friendly(s("D:\\githubrepos\\Agentrmc\\native\\src\\foo.cpp")),
          s("D盘 foo"));
    check("windows path in sentence",
          SpeechText::friendly(s("改好了 D:/repo/foo.cpp 的 bug")),
          s("改好了 D盘 foo 的 bug"));
    check("drive-only path", SpeechText::friendly(s("D:\\")), s("D盘"));
    check("bare drive letter", SpeechText::friendly(s("放在 C: 上")), s("放在 C盘 上"));
    check("relative path -> basename",
          SpeechText::friendly(s("native/src/MainWindow.cpp")), s("MainWindow"));
    check("posix absolute path", SpeechText::friendly(s("/home/user/app.js")), s("app"));
    check("unc path", SpeechText::friendly(s("\\\\server\\share\\foo.txt")), s("foo"));
    check("url removed", SpeechText::friendly(s("https://example.com/a?b=1 看这个")),
          s("看这个"));
    check("session title punctuation",
          SpeechText::friendly(s("「修复 bug（紧急）」这轮跑完了")),
          s("修复 bug 紧急 这轮跑完了"));
    check("branch name hyphens",
          SpeechText::friendly(s("feat/handoff-focus-and-classic-theme")),
          s("handoff focus and classic theme"));
    check("standalone file extension",
          SpeechText::friendly(s("打开 foo.cpp 看看")), s("打开 foo 看看"));
    check("snake_case to words", SpeechText::friendly(s("foo_bar_baz")),
          s("foo bar baz"));
    check("clean label unchanged", SpeechText::friendly(s("已切换到 sensevoice")),
          s("已切换到 sensevoice"));
    check("version kept", SpeechText::friendly(s("版本 0.3.9 好了")),
          s("版本 0.3.9 好了"));
    check("markdown and emoji", SpeechText::friendly(s("**改好了** 😊")), s("改好了"));
    check("empty stays empty", SpeechText::friendly(s("")), s(""));
    check("punctuation only -> empty", SpeechText::friendly(s("「」（）")), s(""));

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
