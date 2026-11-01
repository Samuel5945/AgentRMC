#include "core/ThemeManager.h"

#include "core/Profiles.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <algorithm>

#ifdef Q_OS_WIN
// 子进程是控制台程序（tar.exe），GUI 进程拉起时会闪黑窗，压掉。
#include <qt_windows.h>
#endif

namespace {

QString absolutePath(const QString &base, const QString &relative) {
    if (relative.trimmed().isEmpty()) {
        return {};
    }
    return QDir::cleanPath(base + QLatin1Char('/') + relative.trimmed());
}

QStringList resolveFrames(const QString &base, const QJsonArray &frames) {
    QStringList out;
    for (const QJsonValue &value : frames) {
        const QString path = absolutePath(base, value.toString());
        if (!path.isEmpty() && QFile::exists(path)) {
            out.append(path);
        }
    }
    return out;
}

// 整目录拷贝（rename 跨盘失败时的兜底）。
bool copyDir(const QString &from, const QString &to) {
    const QDir src(from);
    if (!QDir().mkpath(to)) {
        return false;
    }
    for (const QFileInfo &entry :
         src.entryInfoList(QDir::Files | QDir::Hidden)) {
        if (!QFile::copy(entry.absoluteFilePath(), to + QLatin1Char('/') +
                                                    entry.fileName())) {
            return false;
        }
    }
    for (const QFileInfo &entry :
         src.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (!copyDir(entry.absoluteFilePath(),
                     to + QLatin1Char('/') + entry.fileName())) {
            return false;
        }
    }
    return true;
}

QString sanitizeDirName(QString name) {
    static const QRegularExpression bad(R"([<>:"/\\|?*])");
    name.remove(bad);
    name = name.trimmed();
    while (name.startsWith(QLatin1Char('.'))) {
        name.remove(0, 1);
    }
    return name;
}

}  // namespace

ThemeManager &ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

// classic 调色板 = QSS 令牌化迁移前的原值（tools/theme/qss_tokenize.py 生成），
// 缺了任何 token 界面就会露出未着色的 QSS，别手删。
QHash<QString, QString> ThemeManager::classicPalette() {
    QHash<QString, QString> m_;
    m_.insert(QStringLiteral("a110"), QStringLiteral("rgba(79, 140, 255, 110)"));
    m_.insert(QStringLiteral("a120"), QStringLiteral("rgba(79, 140, 255, 120)"));
    m_.insert(QStringLiteral("a130"), QStringLiteral("rgba(79, 140, 255, 130)"));
    m_.insert(QStringLiteral("a150"), QStringLiteral("rgba(79, 140, 255, 150)"));
    m_.insert(QStringLiteral("a16"), QStringLiteral("rgba(79, 140, 255, 16)"));
    m_.insert(QStringLiteral("a24"), QStringLiteral("rgba(79, 140, 255, 24)"));
    m_.insert(QStringLiteral("a26"), QStringLiteral("rgba(79, 140, 255, 26)"));
    m_.insert(QStringLiteral("a28"), QStringLiteral("rgba(79, 140, 255, 28)"));
    m_.insert(QStringLiteral("a36"), QStringLiteral("rgba(79, 140, 255, 36)"));
    m_.insert(QStringLiteral("a58"), QStringLiteral("rgba(79, 140, 255, 58)"));
    m_.insert(QStringLiteral("a64"), QStringLiteral("rgba(79, 140, 255, 64)"));
    m_.insert(QStringLiteral("a70"), QStringLiteral("rgba(79, 140, 255, 70)"));
    m_.insert(QStringLiteral("a72"), QStringLiteral("rgba(79, 140, 255, 72)"));
    m_.insert(QStringLiteral("a80"), QStringLiteral("rgba(79, 140, 255, 80)"));
    m_.insert(QStringLiteral("accent"), QStringLiteral("#4f8cff"));
    m_.insert(QStringLiteral("accent.2"), QStringLiteral("#7c5cff"));
    m_.insert(QStringLiteral("accent.hover"), QStringLiteral("#6299ff"));
    m_.insert(QStringLiteral("accent.link"), QStringLiteral("#7dabff"));
    m_.insert(QStringLiteral("accent.text"), QStringLiteral("#9ec1ff"));
    m_.insert(QStringLiteral("accent2.hover"), QStringLiteral("#8d70ff"));
    m_.insert(QStringLiteral("bg"), QStringLiteral("#0b0e14"));
    m_.insert(QStringLiteral("bubble.bg"), QStringLiteral("rgba(15, 20, 29, 242)"));
    m_.insert(QStringLiteral("card.bg"), QStringLiteral("#121722"));
    // 卡片渐变第二停：主题想给卡片做半透明时单独改它；raised 还被
    // QMenu/QToolTip/下拉弹出层当顶层窗口底色用，不能跟着变透明。
    m_.insert(QStringLiteral("card.bg2"), QStringLiteral("#161d2a"));
    m_.insert(QStringLiteral("danger"), QStringLiteral("#e5484d"));
    m_.insert(QStringLiteral("dialog.bg"), QStringLiteral("#0f141d"));
    m_.insert(QStringLiteral("faint"), QStringLiteral("#46505e"));
    m_.insert(QStringLiteral("faint.2"), QStringLiteral("#3d4653"));
    m_.insert(QStringLiteral("ghost"), QStringLiteral("#6b7684"));
    m_.insert(QStringLiteral("guide.image.bg"), QStringLiteral("#05070c"));
    m_.insert(QStringLiteral("line"), QStringLiteral("#4a5568"));
    m_.insert(QStringLiteral("muted"), QStringLiteral("#8b949e"));
    m_.insert(QStringLiteral("muted.2"), QStringLiteral("#5b6472"));
    m_.insert(QStringLiteral("msgbox.bg"), QStringLiteral("#121722"));
    m_.insert(QStringLiteral("on.accent"), QStringLiteral("white"));
    m_.insert(QStringLiteral("raised"), QStringLiteral("#161d2a"));
    m_.insert(QStringLiteral("text"), QStringLiteral("#e6edf3"));
    m_.insert(QStringLiteral("text.body"), QStringLiteral("#b6c2cf"));
    m_.insert(QStringLiteral("text.dim"), QStringLiteral("#c9d1d9"));
    m_.insert(QStringLiteral("text.soft"), QStringLiteral("#dbe4ee"));
    m_.insert(QStringLiteral("w10"), QStringLiteral("rgba(255, 255, 255, 10)"));
    m_.insert(QStringLiteral("w11"), QStringLiteral("rgba(255, 255, 255, 11)"));
    m_.insert(QStringLiteral("w12"), QStringLiteral("rgba(255, 255, 255, 12)"));
    m_.insert(QStringLiteral("w14"), QStringLiteral("rgba(255, 255, 255, 14)"));
    m_.insert(QStringLiteral("w16"), QStringLiteral("rgba(255, 255, 255, 16)"));
    m_.insert(QStringLiteral("w18"), QStringLiteral("rgba(255, 255, 255, 18)"));
    m_.insert(QStringLiteral("w20"), QStringLiteral("rgba(255, 255, 255, 20)"));
    m_.insert(QStringLiteral("w22"), QStringLiteral("rgba(255, 255, 255, 22)"));
    m_.insert(QStringLiteral("w24"), QStringLiteral("rgba(255, 255, 255, 24)"));
    m_.insert(QStringLiteral("w26"), QStringLiteral("rgba(255, 255, 255, 26)"));
    m_.insert(QStringLiteral("w30"), QStringLiteral("rgba(255, 255, 255, 30)"));
    m_.insert(QStringLiteral("w4"), QStringLiteral("rgba(255, 255, 255, 4)"));
    m_.insert(QStringLiteral("w40"), QStringLiteral("rgba(255, 255, 255, 40)"));
    m_.insert(QStringLiteral("w6"), QStringLiteral("rgba(255, 255, 255, 6)"));
    m_.insert(QStringLiteral("w7"), QStringLiteral("rgba(255, 255, 255, 7)"));
    m_.insert(QStringLiteral("w8"), QStringLiteral("rgba(255, 255, 255, 8)"));
    m_.insert(QStringLiteral("warn"), QStringLiteral("#d29922"));
    return m_;
}

QHash<QString, QString> ThemeManager::mergePalette(
    const QHash<QString, QString> &overrides) {
    QHash<QString, QString> merged = classicPalette();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (!it.value().trimmed().isEmpty()) {
            merged.insert(it.key(), it.value().trimmed());
        }
    }
    return merged;
}

QString ThemeManager::themesRoot() {
    return QDir::cleanPath(QCoreApplication::applicationDirPath() +
                           QStringLiteral("/themes"));
}

ThemeInfo ThemeManager::parseThemeDir(const QString &dir, QString *error) {
    ThemeInfo info;
    info.dir = QDir::cleanPath(dir);
    info.id = QDir(dir).dirName();

    const QString manifest = info.dir + QStringLiteral("/theme.json");
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("缺 theme.json");
        info.valid = false;
        return info;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!doc.isObject()) {
        if (error) {
            *error = parse.error != QJsonParseError::NoError
                         ? QStringLiteral("theme.json 解析失败：%1")
                               .arg(parse.errorString())
                         : QStringLiteral("theme.json 不是 JSON 对象");
        }
        info.valid = false;
        return info;
    }
    const QJsonObject root = doc.object();
    info.displayName = root.value("displayName").toString();
    info.author = root.value("author").toString();
    if (info.displayName.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("displayName 必填");
        info.valid = false;
        return info;
    }
    const QString preview = info.dir + QStringLiteral("/preview.png");
    if (QFile::exists(preview)) {
        info.preview = preview;
    }

    // 主题细节（palette/pet/backgrounds/character）只在 loadTheme 用，
    // 这里只负责列出与校验，避免解析两遍。
    if (error) error->clear();
    info.valid = true;
    return info;
}

QImage ThemeManager::cachedImage(const QString &path) {
    static QHash<QString, QImage> cache;
    auto it = cache.constFind(path);
    if (it != cache.constEnd()) {
        return it.value();
    }
    const QImage image(path);
    cache.insert(path, image);
    return image;
}

QString ThemeManager::renderQss(const QString &qssTemplate,
                                const QHash<QString, QString> &palette,
                                const QString &rootBackgroundImage) {
    QString out = qssTemplate;
    for (auto it = palette.begin(); it != palette.end(); ++it) {
        out.replace(QStringLiteral("@%1@").arg(it.key()), it.value());
    }
    if (!rootBackgroundImage.isEmpty()) {
        // 路径必须正斜杠：QSS 的 url() 里反斜杠会被当 CSS 转义序列吃掉。
        out += QStringLiteral(
                   "\nQWidget#root {\n"
                   "    background-image: url(\"%1\");\n"
                   "    background-position: center;\n"
                   "    background-repeat: no-repeat;\n"
                   "}\n")
                   .arg(QDir::fromNativeSeparators(rootBackgroundImage));
    }
    return out;
}

void ThemeManager::rescan() {
    infos_.clear();
    ThemeInfo classic;
    classic.id = QStringLiteral("classic");
    classic.displayName = QStringLiteral("经典");
    classic.dir = themesRoot() + QStringLiteral("/classic");
    infos_.append(classic);

    QDir root(themesRoot());
    const QStringList dirs =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : dirs) {
        QString error;
        ThemeInfo info = parseThemeDir(root.filePath(name), &error);
        if (name == QStringLiteral("classic")) {
            continue;  // classic 由代码内置，目录里的同名清单忽略
        }
        infos_.append(info);
    }
}

Theme ThemeManager::parseThemeBody(const ThemeInfo &info) {
    Theme theme;
    QFile file(info.dir + QStringLiteral("/theme.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return theme;
    }
    const QJsonObject root =
        QJsonDocument::fromJson(file.readAll()).object();

    theme.id = info.id;
    theme.displayName = info.displayName;
    theme.author = info.author;

    QHash<QString, QString> overrides;
    const QJsonObject palette = root.value("palette").toObject();
    for (auto it = palette.begin(); it != palette.end(); ++it) {
        overrides.insert(it.key(), it.value().toString());
    }
    theme.palette = mergePalette(overrides);

    const QJsonObject character = root.value("character").toObject();
    theme.characterName = character.value("name").toString();
    theme.persona = character.value("persona").toString();

    const QJsonObject pet = root.value("pet").toObject();
    theme.ring = pet.value("ring").toBool(true);
    const QJsonObject states = pet.value("states").toObject();
    for (auto it = states.begin(); it != states.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        ThemePetState state;
        state.frames = resolveFrames(info.dir, entry.value("frames").toArray());
        if (state.frames.isEmpty()) {
            continue;
        }
        state.fps = qBound(1, entry.value("fps").toInt(2), 12);
        state.bob = qBound(0, entry.value("bob").toInt(3), 12);
        state.tilt = qBound(0, entry.value("tilt").toInt(0), 15);
        theme.petStates.insert(it.key(), state);
    }

    const QJsonObject backgrounds = root.value("backgrounds").toObject();
    const QString mainBg = absolutePath(
        info.dir, backgrounds.value("main").toString());
    const QString panelBg = absolutePath(
        info.dir, backgrounds.value("panel").toString());
    theme.mainBackground = QFile::exists(mainBg) ? mainBg : QString();
    theme.panelBackground = QFile::exists(panelBg) ? panelBg : QString();
    theme.dim = qBound(0.0, backgrounds.value("dim").toDouble(0.55), 0.9);
    const QJsonObject voiceObj = root.value("voice").toObject();
    theme.voiceName = voiceObj.value("name").toString().trimmed();
    theme.voiceStyle = voiceObj.value("style").toString().trimmed();
    return theme;
}

Theme ThemeManager::loadTheme(const QString &id) const {
    for (const ThemeInfo &info : infos_) {
        if (info.id != id || !info.valid) {
            continue;
        }
        return parseThemeBody(info);
    }
    // classic 永远内置兜底；主题不存在/损坏也回落到它。
    Theme theme;
    theme.id = QStringLiteral("classic");
    theme.displayName = QStringLiteral("经典");
    theme.palette = classicPalette();
    return theme;
}

void ThemeManager::applyQss() {
    QFile tpl(QStringLiteral(":/style.qss"));
    const QString qss = tpl.open(QIODevice::ReadOnly | QIODevice::Text)
                            ? QString::fromUtf8(tpl.readAll())
                            : QString();
    qApp->setStyleSheet(
        renderQss(qss, current_.palette, current_.mainBackground));
}

void ThemeManager::init(ProfileStore *store) {
    store_ = store;
    rescan();
    const QString id =
        store_ != nullptr ? store_->uiString(QStringLiteral("theme"),
                                            QStringLiteral("classic"))
                          : QStringLiteral("classic");
    current_ = loadTheme(id);
    applyQss();
}

void ThemeManager::setTheme(const QString &id) {
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("theme"), id);
    }
    current_ = loadTheme(id);
    applyQss();
    emit themeChanged(current_);
}

QString ThemeManager::importThemePackageTo(const QString &zipPath,
                                           const QString &themesDir,
                                           QString *error) {
    if (error) error->clear();
    if (!QFile::exists(zipPath)) {
        if (error) *error = QStringLiteral("找不到文件：%1").arg(zipPath);
        return {};
    }
    QDir root(themesDir);
    if (!root.exists() && !root.mkpath(QStringLiteral("."))) {
        if (error) *error = QStringLiteral("建不了主题目录：%1").arg(themesDir);
        return {};
    }
    // 解压到 themes/.import-<时间戳>，装完即删；同盘 rename 保证原子感。
    const QString tmp =
        themesDir + QStringLiteral("/.import-") +
        QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir(tmp).removeRecursively();
    if (!root.mkpath(QFileInfo(tmp).fileName())) {
        if (error) *error = QStringLiteral("建不了临时目录");
        return {};
    }

    QProcess proc;
    proc.setProgram(QStringLiteral("tar"));
    proc.setArguments({QStringLiteral("-xf"),
                       QDir::toNativeSeparators(zipPath),
                       QStringLiteral("-C"), QDir::toNativeSeparators(tmp)});
#ifdef Q_OS_WIN
    proc.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
    proc.start();
    if (!proc.waitForFinished(120000) || proc.exitStatus() != QProcess::NormalExit ||
        proc.exitCode() != 0) {
        const QString detail = QString::fromLocal8Bit(
            proc.readAllStandardError().trimmed());
        QDir(tmp).removeRecursively();
        if (error) {
            *error = detail.isEmpty()
                         ? QStringLiteral("解压失败（需要 Windows 10 自带的 tar）")
                         : QStringLiteral("解压失败：%1").arg(detail);
        }
        return {};
    }

    // 找 theme.json：zip 根、或套了一层/多层文件夹里的最浅一个。
    QStringList manifests;
    QDirIterator it(tmp, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileName() == QStringLiteral("theme.json")) {
            manifests.append(QDir::cleanPath(it.filePath()));
        }
    }
    if (manifests.isEmpty()) {
        QDir(tmp).removeRecursively();
        if (error) *error = QStringLiteral("包里没有 theme.json：不是主题包");
        return {};
    }
    std::sort(manifests.begin(), manifests.end(),
              [](const QString &a, const QString &b) {
                  return a.count(QLatin1Char('/')) < b.count(QLatin1Char('/'));
              });
    QString manifestDir;
    QString parseError;
    for (const QString &candidate : manifests) {
        ThemeInfo probe = parseThemeDir(QFileInfo(candidate).absolutePath(),
                                        &parseError);
        if (probe.valid) {
            manifestDir = QFileInfo(candidate).absolutePath();
            break;
        }
    }
    if (manifestDir.isEmpty()) {
        QDir(tmp).removeRecursively();
        if (error) {
            *error = QStringLiteral("主题包无效：%1").arg(parseError);
        }
        return {};
    }

    // 目标目录名：zip 里那个文件夹的名字；直接铺在根就用 zip 文件名。
    QString name = manifestDir == tmp
                       ? QFileInfo(zipPath).completeBaseName()
                       : QFileInfo(manifestDir).fileName();
    name = sanitizeDirName(name);
    if (name.isEmpty()) {
        name = QFileInfo(zipPath).completeBaseName();
    }
    const QString target = themesDir + QLatin1Char('/') + name;
    QDir(target).removeRecursively();  // 同名 = 更新
    if (!QDir().rename(manifestDir, target)) {
        if (!copyDir(manifestDir, target)) {
            QDir(tmp).removeRecursively();
            if (error) *error = QStringLiteral("装进 themes 失败");
            return {};
        }
    }
    QDir(tmp).removeRecursively();
    return name;
}

QString ThemeManager::importThemePackage(const QString &zipPath,
                                         QString *error) {
    const QString id = importThemePackageTo(zipPath, themesRoot(), error);
    if (!id.isEmpty()) {
        rescan();
    }
    return id;
}
