// 主题系统单测：classic 调色板、palette 合并、清单解析、QSS 渲染。
// 只测纯函数，不碰 QApplication / setStyleSheet。
#include "core/ThemeManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

void writeFile(const QString &path, const QString &content) {
    QFile file(path);
    file.open(QIODevice::WriteOnly | QIODevice::Text);
    file.write(content.toUtf8());
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    const auto classic = ThemeManager::classicPalette();
    check(classic.size() >= 50, "classic palette has all tokens");
    check(classic.value("bg") == QStringLiteral("#0b0e14"),
          "classic bg keeps original value");
    check(classic.value("accent") == QStringLiteral("#4f8cff"),
          "classic accent keeps original value");

    // 合并：覆盖生效、未覆盖的留 classic 值。
    auto merged = ThemeManager::mergePalette(
        QHash<QString, QString>{{"bg", "#101010"}, {"accent", "   "}});
    check(merged.value("bg") == QStringLiteral("#101010"),
          "override applies");
    check(merged.value("accent") == QStringLiteral("#4f8cff"),
          "blank override is ignored");

    QTemporaryDir dir;
    check(dir.isValid(), "temp dir ok");
    const QString themeDir = dir.filePath(QStringLiteral("xiaoai"));
    QDir().mkpath(themeDir + QStringLiteral("/pet"));
    const QString manifest = QStringLiteral(R"({
        "id": "xiaoai-whale",
        "displayName": "小爱 · 鲸鱼娘",
        "author": "test",
        "character": {"name": "小爱", "persona": "你是小爱。"},
        "palette": {"bg": "#0a0d13"},
        "pet": {
            "ring": false,
            "states": {
                "idle": {"frames": ["pet/idle_0.png", "pet/missing.png"],
                         "fps": 4, "bob": 2, "tilt": 3}
            }
        },
        "backgrounds": {"main": "backgrounds/main.png", "dim": 0.7}
    })");
    writeFile(themeDir + QStringLiteral("/theme.json"), manifest);
    writeFile(themeDir + QStringLiteral("/pet/idle_0.png"),
              QStringLiteral("not a real png"));
    QDir().mkpath(themeDir + QStringLiteral("/backgrounds"));
    writeFile(themeDir + QStringLiteral("/backgrounds/main.png"), QStringLiteral("x"));

    QString error;
    const ThemeInfo info = ThemeManager::parseThemeDir(themeDir, &error);
    check(info.valid && error.isEmpty(), "valid manifest parses");
    check(info.id == QStringLiteral("xiaoai") && info.displayName.contains(QStringLiteral("鲸鱼娘")),
          "manifest id/displayName read");

    // 缺 displayName 的清单必须被判坏。
    QDir().mkpath(dir.filePath(QStringLiteral("broken")));
    writeFile(dir.filePath(QStringLiteral("broken/theme.json")),
              QStringLiteral("{\"id\": \"broken\"}"));
    const ThemeInfo broken =
        ThemeManager::parseThemeDir(dir.filePath(QStringLiteral("broken")), &error);
    check(!broken.valid && !error.isEmpty(), "missing displayName rejected");

    // 缺 theme.json 的目录同样判坏。
    QDir().mkpath(dir.filePath(QStringLiteral("empty")));
    const ThemeInfo empty =
        ThemeManager::parseThemeDir(dir.filePath(QStringLiteral("empty")), &error);
    check(!empty.valid, "missing manifest rejected");

    // QSS 渲染：token 全部替换、追加主界面背景规则。
    const QString rendered = ThemeManager::renderQss(
        QStringLiteral("QWidget#root { background: @bg@; color: @text@; }"),
        classic, QStringLiteral("D:/themes/xiaoai/backgrounds/main.png"));
    check(!rendered.contains(QStringLiteral("@")), "all tokens replaced");
    check(rendered.contains(QStringLiteral("#0b0e14")) &&
              rendered.contains(QStringLiteral("#e6edf3")),
          "classic values substituted");
    check(rendered.contains(QStringLiteral("background-image")),
          "root background-image appended");

    // 未覆盖 token 时不得追加背景规则。
    const QString plain = ThemeManager::renderQss(
        QStringLiteral("A { color: @bg@; }"), classic, QString());
    check(!plain.contains(QStringLiteral("background-image")),
          "no background rule without image");

    // —— 主题包导入：用系统 tar 造 zip（Windows 10+ 自带 bsdtar）。 ——
    const QString pkgRoot = dir.filePath(QStringLiteral("pkgsrc"));
    QDir().mkpath(pkgRoot + QStringLiteral("/goodtheme/pet"));
    writeFile(pkgRoot + QStringLiteral("/goodtheme/theme.json"),
              QStringLiteral(R"({"displayName": "好主题", "pet": {"states": {
                  "idle": {"frames": ["pet/idle_0.png"]}}}})"));
    writeFile(pkgRoot + QStringLiteral("/goodtheme/pet/idle_0.png"),
              QStringLiteral("x"));
    const QString themesDir = dir.filePath(QStringLiteral("installed"));
    QDir().mkpath(themesDir);

    QProcess tar;
    tar.setProgram(QStringLiteral("tar"));
    tar.setWorkingDirectory(pkgRoot);
    tar.setArguments({QStringLiteral("-a"), QStringLiteral("-cf"),
                      dir.filePath(QStringLiteral("good.zip")),
                      QStringLiteral("goodtheme")});
    tar.start();
    const bool tarOk = tar.waitForFinished(30000) &&
                       tar.exitStatus() == QProcess::NormalExit &&
                       tar.exitCode() == 0;
    if (tarOk) {
        QString importError;
        const QString id = ThemeManager::importThemePackageTo(
            dir.filePath(QStringLiteral("good.zip")), themesDir, &importError);
        check(id == QStringLiteral("goodtheme") && importError.isEmpty(),
              "zip import returns theme id");
        QString probeError;
        check(ThemeManager::parseThemeDir(themesDir + QStringLiteral("/goodtheme"),
                                          &probeError)
                  .valid,
              "imported theme parses");
        bool tempLeft = false;
        for (const QString &name :
             QDir(themesDir).entryList(QDir::Dirs | QDir::Hidden)) {
            if (name.startsWith(QStringLiteral(".import-"))) {
                tempLeft = true;
            }
        }
        check(!tempLeft, "no temp dirs left behind");

        // 同名再导入 = 更新：改掉 displayName、重新打包后重导，读出来应是新值。
        writeFile(pkgRoot + QStringLiteral("/goodtheme/theme.json"),
                  QStringLiteral(R"({"displayName": "好主题2"})"));
        tar.setArguments({QStringLiteral("-a"), QStringLiteral("-cf"),
                          dir.filePath(QStringLiteral("good.zip")),
                          QStringLiteral("goodtheme")});
        tar.start();
        tar.waitForFinished(30000);
        const QString reId = ThemeManager::importThemePackageTo(
            dir.filePath(QStringLiteral("good.zip")), themesDir, &importError);
        ThemeInfo updated =
            ThemeManager::parseThemeDir(themesDir + QStringLiteral("/goodtheme"),
                                        &probeError);
        check(updated.valid &&
                  updated.displayName == QStringLiteral("好主题2"),
              (QStringLiteral("re-import replaces same-name theme (reId=%1 err=%2 parsed=%3 name=%4)")
                   .arg(reId, importError,
                        updated.valid ? QStringLiteral("1")
                                      : QStringLiteral("0"),
                       updated.displayName)
                   .toUtf8()
                   .constData()));

        // 没有 theme.json 的 zip 必须被拒绝。
        QDir().mkpath(pkgRoot + QStringLiteral("/junk/inner"));
        writeFile(pkgRoot + QStringLiteral("/junk/inner/readme.txt"),
                  QStringLiteral("not a theme"));
        tar.setArguments({QStringLiteral("-a"), QStringLiteral("-cf"),
                          dir.filePath(QStringLiteral("junk.zip")),
                          QStringLiteral("junk")});
        tar.start();
        tar.waitForFinished(30000);
        const QString junkId = ThemeManager::importThemePackageTo(
            dir.filePath(QStringLiteral("junk.zip")), themesDir, &importError);
        check(junkId.isEmpty() && importError.contains(QStringLiteral("theme.json")),
              "zip without theme.json rejected");
    } else {
        std::printf("SKIP theme import tests: tar unavailable\n");
    }

    // 坏 zip（根本不是压缩包）必须报错而不是崩溃。
    writeFile(dir.filePath(QStringLiteral("bad.zip")),
              QStringLiteral("this is not a zip"));
    QString badError;
    const QString badId = ThemeManager::importThemePackageTo(
        dir.filePath(QStringLiteral("bad.zip")), themesDir, &badError);
    check(badId.isEmpty() && !badError.isEmpty(), "corrupt zip rejected");

    if (failures == 0) {
        std::printf("PASS native theme: palette / manifest / qss render\n");
        return 0;
    }
    std::printf("FAIL native theme: %d checks failed\n", failures);
    return 1;
}
