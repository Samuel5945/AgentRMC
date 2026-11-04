#include "core/ThemeManager.h"

#include "core/Profiles.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

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

QString rgbaString(const QColor &color, int alpha) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(qBound(0, alpha, 255));
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
    // 气泡渐变第二停 / 描边 / 可点击描边：classic 默认与旧观感完全一致
    // （渐变两停同色=平涂，描边与可点击态都用 w30 的旧值），美术主题单独覆盖。
    m_.insert(QStringLiteral("bubble.bg2"), QStringLiteral("rgba(15, 20, 29, 242)"));
    m_.insert(QStringLiteral("bubble.border"), QStringLiteral("rgba(255, 255, 255, 30)"));
    m_.insert(QStringLiteral("bubble.accent"), QStringLiteral("rgba(255, 255, 255, 30)"));
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

QHash<QString, QString> ThemeManager::classicLightPalette() {
    QHash<QString, QString> m_ = classicPalette();
    // 底与卡片：深底翻成浅灰底 + 白卡，弹层/气泡用白。
    m_.insert(QStringLiteral("bg"), QStringLiteral("#f3f5f8"));
    m_.insert(QStringLiteral("dialog.bg"), QStringLiteral("#f3f5f8"));
    m_.insert(QStringLiteral("card.bg"), QStringLiteral("#ffffff"));
    m_.insert(QStringLiteral("card.bg2"), QStringLiteral("#eef1f5"));
    m_.insert(QStringLiteral("raised"), QStringLiteral("#ffffff"));
    m_.insert(QStringLiteral("msgbox.bg"), QStringLiteral("#ffffff"));
    m_.insert(QStringLiteral("bubble.bg"),
              QStringLiteral("rgba(255, 255, 255, 245)"));
    m_.insert(QStringLiteral("bubble.bg2"),
              QStringLiteral("rgba(255, 255, 255, 245)"));
    m_.insert(QStringLiteral("bubble.border"),
              QStringLiteral("rgba(15, 23, 42, 60)"));
    m_.insert(QStringLiteral("bubble.accent"),
              QStringLiteral("rgba(15, 23, 42, 60)"));
    m_.insert(QStringLiteral("guide.image.bg"), QStringLiteral("#ffffff"));
    // 字色：浅底深字，主次层级与深色版一一对应（text 最重、body 最轻）。
    m_.insert(QStringLiteral("text"), QStringLiteral("#171e2a"));
    m_.insert(QStringLiteral("text.soft"), QStringLiteral("#232b3a"));
    m_.insert(QStringLiteral("text.dim"), QStringLiteral("#3a4453"));
    m_.insert(QStringLiteral("text.body"), QStringLiteral("#55606e"));
    m_.insert(QStringLiteral("muted"), QStringLiteral("#5f6b7a"));
    m_.insert(QStringLiteral("muted.2"), QStringLiteral("#77828f"));
    m_.insert(QStringLiteral("faint"), QStringLiteral("#aeb6c0"));
    m_.insert(QStringLiteral("faint.2"), QStringLiteral("#c2c9d2"));
    m_.insert(QStringLiteral("line"), QStringLiteral("#c9d0d9"));
    // 浅底上的蓝色文字要压深才读得出；主按钮仍是蓝底白字，accent 本身不动。
    m_.insert(QStringLiteral("accent.text"), QStringLiteral("#2f6bdb"));
    m_.insert(QStringLiteral("accent.link"), QStringLiteral("#2f6bdb"));
    // 白色透明阶梯在浅底上改成深色透明阶梯（表面/描边/滚动条）；透明度翻倍，
    // 不然白底上描边几乎看不见。蓝色 a 阶梯不动——选中高亮两种底都成立。
    const QList<int> alphas = {4,  6,  7,  8,  10, 11, 12, 14,
                               16, 18, 20, 22, 24, 26, 30, 40};
    for (int a : alphas) {
        m_.insert(QStringLiteral("w%1").arg(a),
                  QStringLiteral("rgba(15, 23, 42, %1)").arg(qMin(255, a * 2)));
    }
    return m_;
}

bool ThemeManager::systemPrefersLight() {
#ifdef Q_OS_WIN
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
                       "CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    // 读不到按深色兜底：classic 历来就是深色，别因缺键突然变浅。
    return settings.value(QStringLiteral("AppsUseLightTheme"), 0).toInt() != 0;
#else
    return false;
#endif
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
                                const QString &rootBackgroundImage,
                                qreal fontScale) {
    QString out = qssTemplate;
    for (auto it = palette.begin(); it != palette.end(); ++it) {
        out.replace(QStringLiteral("@%1@").arg(it.key()), it.value());
    }
    if (!qFuzzyCompare(fontScale, 1.0)) {
        // 字号倍率：把 QSS 里所有 font-size: Npx 等比缩放（取整到整像素，
        // 子像素字号渲染发虚）。最小 7px 保底可读；行高/边距不动。
        static const QRegularExpression re(
            QStringLiteral("font-size\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)px"));
        QString scaled;
        qsizetype pos = 0;
        auto it = re.globalMatch(out);
        while (it.hasNext()) {
            const auto match = it.next();
            scaled += out.mid(pos, match.capturedStart() - pos);
            const qreal px = match.captured(1).toDouble();
            scaled += QStringLiteral("font-size: %1px")
                          .arg(qMax(7, qRound(px * fontScale)));
            pos = match.capturedEnd();
        }
        scaled += out.mid(pos);
        out = scaled;
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

    // 浅色变体（可选）：主题带 backgrounds.light / palette.light 才吃配色
    // 模式；缺文件、缺字段都按「没有变体」处理，浅色时整套沿用深色值。
    const QJsonObject lightBg = backgrounds.value("light").toObject();
    const QString mainBgLight = absolutePath(
        info.dir, lightBg.value("main").toString());
    const QString panelBgLight = absolutePath(
        info.dir, lightBg.value("panel").toString());
    theme.mainBackgroundLight = QFile::exists(mainBgLight) ? mainBgLight : QString();
    theme.panelBackgroundLight = QFile::exists(panelBgLight) ? panelBgLight : QString();
    theme.dimLight = qBound(0.0, lightBg.value("dim").toDouble(0.08), 0.9);
    QHash<QString, QString> lightOverrides;
    const QJsonObject lightPal = root.value("palette").toObject()
                                     .value("light").toObject();
    for (auto it = lightPal.begin(); it != lightPal.end(); ++it) {
        lightOverrides.insert(it.key(), it.value().toString());
    }
    // 只有真给了浅色 token 或浅色插画才算有变体（空对象不激活浅色模式）。
    if (!lightOverrides.isEmpty() || !theme.mainBackgroundLight.isEmpty() ||
        !theme.panelBackgroundLight.isEmpty()) {
        QHash<QString, QString> merged = classicLightPalette();
        for (auto it = lightOverrides.begin(); it != lightOverrides.end(); ++it) {
            if (!it.value().trimmed().isEmpty()) {
                merged.insert(it.key(), it.value().trimmed());
            }
        }
        theme.paletteLight = merged;
    }

    // 气泡装饰贴纸（可选）：桌宠说话气泡里的半透明小气泡素材；
    // 缺文件/缺字段都清空，运行时自动退化为无装饰。
    const QJsonObject bubbleObj = root.value("bubble").toObject();
    const QString decor = absolutePath(
        info.dir, bubbleObj.value("decor").toString());
    theme.bubbleDecor = QFile::exists(decor) ? decor : QString();
    theme.bubbleTail = bubbleObj.value(QStringLiteral("tail")).toString();
    const QJsonObject voiceObj = root.value("voice").toObject();
    theme.voiceName = voiceObj.value("name").toString().trimmed();
    theme.voiceStyle = voiceObj.value("style").toString().trimmed();
    return theme;
}

Theme ThemeManager::buildClassicTheme() const {
    Theme theme;
    theme.id = QStringLiteral("classic");
    theme.displayName = QStringLiteral("经典");
    const bool light =
        scheme_ == QStringLiteral("light") ||
        (scheme_ == QStringLiteral("auto") && systemPrefersLight());
    theme.palette = light ? classicLightPalette() : classicPalette();
    return theme;
}

Theme ThemeManager::loadTheme(const QString &id) const {
    // classic 是代码内置主题，不读 themes/classic 的清单（那儿通常没有 theme.json）。
    // 若也走 parseThemeBody，读不到清单会返回空 palette：渲染出的 QSS 里 @token@
    // 一个都没替换，被 Qt 整张拒收，界面退回系统浅色，而内联的深色字色全看不见。
    if (id != QStringLiteral("classic")) {
        for (const ThemeInfo &info : infos_) {
            if (info.id != id || !info.valid) {
                continue;
            }
            // 浅色模式：优先浅色插画（没带就用深色原图），配色同理。
            // 形象字段在后面 composeCharacter 统一覆盖，这里只管外观。
            Theme theme = parseThemeBody(info);
            if (prefersLight() && theme.hasLightVariant()) {
                if (!theme.mainBackgroundLight.isEmpty()) {
                    theme.mainBackground = theme.mainBackgroundLight;
                }
                if (!theme.panelBackgroundLight.isEmpty()) {
                    theme.panelBackground = theme.panelBackgroundLight;
                }
                if (!theme.paletteLight.isEmpty()) {
                    theme.palette = theme.paletteLight;
                }
                theme.dim = theme.dimLight;
            }
            return theme;
        }
    }
    // classic 永远内置兜底；主题不存在/损坏也回落到它。浅色/深色由配色模式决定。
    return buildClassicTheme();
}

Theme ThemeManager::composeCharacter(Theme base) const {
    if (characterId_.isEmpty()) {
        return base;
    }
    // 形象只管桌宠帧/人设/音色；两张插画背景永远跟主题。
    if (characterId_ == QStringLiteral("none")) {
        base.characterName.clear();
        base.persona.clear();
        base.petStates.clear();
        base.voiceName.clear();
        base.voiceStyle.clear();
        return base;
    }
    const Theme art = loadTheme(characterId_);
    base.characterName = art.characterName;
    base.persona = art.persona;
    base.petStates = art.petStates;
    base.ring = art.ring;
    base.voiceName = art.voiceName;
    base.voiceStyle = art.voiceStyle;
    return base;
}

QColor ThemeManager::color(const QString &token) const {
    return parsePaletteColor(current_.palette.value(token));
}

QString ThemeManager::css(const QString &token) const {
    return current_.palette.value(token);
}

bool ThemeManager::prefersLight() const {
    return scheme_ == QStringLiteral("light") ||
           (scheme_ == QStringLiteral("auto") && systemPrefersLight());
}

bool ThemeManager::isLightActive() const {
    if (!prefersLight()) {
        return false;
    }
    return current_.id == QStringLiteral("classic") || current_.hasLightVariant();
}

void ThemeManager::setScheme(const QString &scheme) {
    QString s = scheme;
    if (s != QStringLiteral("light") && s != QStringLiteral("dark")) {
        s = QStringLiteral("auto");
    }
    if (s == scheme_) {
        return;
    }
    scheme_ = s;
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("color_scheme"), s);
    }
    // classic 与带浅色变体的主题吃配色模式；没变体的只记下偏好。
    // 必须按 current_.id 重载：曾写死 buildClassicTheme()，鲸鱼娘一切
    // 配色就被偷换成 classic（2026-11-04 用户实测）。
    if (current_.id == QStringLiteral("classic") || current_.hasLightVariant()) {
        current_ = composeCharacter(loadTheme(current_.id));
        applyQss();
        emit themeChanged(current_);
    }
}

bool ThemeManager::nativeEventFilter(const QByteArray &eventType, void *message,
                                     qintptr *result) {
    Q_UNUSED(eventType);
    Q_UNUSED(result);
#ifdef Q_OS_WIN
    if (scheme_ == QStringLiteral("auto") &&
        (current_.id == QStringLiteral("classic") || current_.hasLightVariant())) {
        const MSG *msg = static_cast<const MSG *>(message);
        // 系统深浅色切换广播 WM_SETTINGCHANGE，lParam 是 "ImmersiveColorSet"。
        if (msg->message == WM_SETTINGCHANGE && msg->lParam != 0) {
            const QString area = QString::fromWCharArray(
                reinterpret_cast<const wchar_t *>(msg->lParam));
            if (area == QStringLiteral("ImmersiveColorSet")) {
                // 延后一拍：别在系统消息回调里直接 setStyleSheet 重刷整窗。
                QTimer::singleShot(0, this, [this] {
                    if (scheme_ != QStringLiteral("auto") ||
                        (current_.id != QStringLiteral("classic") &&
                         !current_.hasLightVariant())) {
                        return;
                    }
                    const Theme rebuilt = composeCharacter(loadTheme(current_.id));
                    if (rebuilt.palette.value(QStringLiteral("bg")) !=
                            current_.palette.value(QStringLiteral("bg")) ||
                        rebuilt.mainBackground != current_.mainBackground ||
                        rebuilt.panelBackground != current_.panelBackground) {
                        current_ = rebuilt;
                        applyQss();
                        emit themeChanged(current_);
                    }
                });
            }
        }
    }
#endif
    return false;
}

void ThemeManager::applyQss() {
    QFile tpl(QStringLiteral(":/style.qss"));
    const QString qss = tpl.open(QIODevice::ReadOnly | QIODevice::Text)
                            ? QString::fromUtf8(tpl.readAll())
                            : QString();
    // 用户拖过「卡片不透明度」就覆盖主题给的两个卡片色，其余 token 原样保留。
    QHash<QString, QString> palette = current_.palette;
    if (cardAlpha_ >= 0) {
        for (const char *key : {"card.bg", "card.bg2"}) {
            const QString token = QString::fromLatin1(key);
            const QColor color = parsePaletteColor(palette.value(token));
            if (color.isValid()) {
                palette.insert(token, rgbaString(color, cardAlpha_));
            }
        }
    }
    qApp->setStyleSheet(
        renderQss(qss, palette, current_.mainBackground, fontScale()));
}

QColor ThemeManager::parsePaletteColor(const QString &value) {
    const QString text = value.trimmed();
    if (text.isEmpty()) {
        return {};
    }
    if (text.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
        const QString inner =
            text.section(QLatin1Char('('), 1).section(QLatin1Char(')'), 0, 0);
        const QStringList parts =
            inner.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            QColor color(parts.at(0).trimmed().toInt(),
                         parts.at(1).trimmed().toInt(),
                         parts.at(2).trimmed().toInt());
            if (parts.size() >= 4) {
                color.setAlpha(qBound(0, parts.at(3).trimmed().toInt(), 255));
            }
            if (color.isValid()) {
                return color;
            }
        }
        return {};
    }
    const QColor color(text);
    return color.isValid() ? color : QColor();
}

int ThemeManager::cardAlpha() const {
    if (cardAlpha_ >= 0) {
        return cardAlpha_;
    }
    const QColor color =
        parsePaletteColor(current_.palette.value(QStringLiteral("card.bg")));
    return color.isValid() ? color.alpha() : 255;
}

qreal ThemeManager::backdropDim() const {
    if (backdropDim_ >= 0) {
        return backdropDim_;
    }
    // 浅色插画的遮罩是白纱：压暗下限不适用（主题作者自己调 dim.light）。
    if (isLightActive() && !current_.mainBackground.isEmpty()) {
        return qBound<qreal>(0.0, current_.dim, 0.9);
    }
    // 主窗信息密度高：主题写的 dim 只当「想更暗」的加成，保底 0.38。
    return qBound<qreal>(0.38, current_.dim, 0.9);
}

qreal ThemeManager::fontScale() const {
    return fontScale_ > 0 ? fontScale_ : 1.0;
}

int ThemeManager::scaledPx(qreal px) {
    return qMax(7, qRound(px * instance().fontScale()));
}

bool ThemeManager::hasAppearanceOverride() const {
    return cardAlpha_ >= 0 || backdropDim_ >= 0 || fontScale_ > 0;
}

void ThemeManager::setCardAlpha(int alpha) {
    cardAlpha_ = qBound(0, alpha, 255);
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("card_alpha"),
                            QString::number(cardAlpha_));
    }
    applyQss();
    emit themeChanged(current_);
}

void ThemeManager::setBackdropDim(qreal dim) {
    backdropDim_ = qBound<qreal>(0.0, dim, 1.0);
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("backdrop_dim"),
                            QString::number(qRound(backdropDim_ * 100)));
    }
    emit themeChanged(current_);
}

void ThemeManager::setFontScale(qreal scale) {
    const qreal clamped = qBound<qreal>(0.8, scale, 1.5);
    if (qFuzzyCompare(clamped, fontScale_)) {
        return;
    }
    fontScale_ = clamped;
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("font_scale"),
                            QString::number(qRound(fontScale_ * 100)));
    }
    applyQss();
    emit themeChanged(current_);
}

void ThemeManager::resetAppearance() {
    cardAlpha_ = -1;
    backdropDim_ = -1.0;
    fontScale_ = 0.0;
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("card_alpha"), QString());
        store_->setUiString(QStringLiteral("backdrop_dim"), QString());
        store_->setUiString(QStringLiteral("font_scale"), QString());
    }
    applyQss();
    emit themeChanged(current_);
}

void ThemeManager::init(ProfileStore *store) {
    store_ = store;
    rescan();
    if (store_ != nullptr) {
        const QString scheme =
            store_->uiString(QStringLiteral("color_scheme"));
        scheme_ = (scheme == QStringLiteral("light") ||
                   scheme == QStringLiteral("dark"))
                      ? scheme
                      : QStringLiteral("auto");
    }
    if (store_ != nullptr) {
        characterId_ = store_->uiString(QStringLiteral("character"));
    }
    const QString id =
        store_ != nullptr ? store_->uiString(QStringLiteral("theme"),
                                            QStringLiteral("classic"))
                          : QStringLiteral("classic");
    current_ = composeCharacter(loadTheme(id));
    if (store_ != nullptr) {
        const QString alpha = store_->uiString(QStringLiteral("card_alpha"));
        if (!alpha.isEmpty()) {
            cardAlpha_ = qBound(0, alpha.toInt(), 255);
        }
        const QString dim = store_->uiString(QStringLiteral("backdrop_dim"));
        if (!dim.isEmpty()) {
            backdropDim_ = qBound<qreal>(0.0, dim.toDouble() / 100.0, 1.0);
        }
        const QString fontScale = store_->uiString(QStringLiteral("font_scale"));
        if (!fontScale.isEmpty()) {
            const qreal value = fontScale.toDouble() / 100.0;
            if (value > 0) {
                fontScale_ = qBound<qreal>(0.8, value, 1.5);
            }
        }
    }
    applyQss();
    // 跟随系统要用：收到 WM_SETTINGCHANGE 才能在系统切深浅色时即时跟上。
    if (!nativeFilterInstalled_ && qApp != nullptr) {
        qApp->installNativeEventFilter(this);
        nativeFilterInstalled_ = true;
    }
}

void ThemeManager::setTheme(const QString &id) {
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("theme"), id);
    }
    current_ = composeCharacter(loadTheme(id));
    applyQss();
    emit themeChanged(current_);
}

void ThemeManager::setCharacter(const QString &id) {
    if (id == characterId_) {
        return;
    }
    characterId_ = id;
    if (store_ != nullptr) {
        store_->setUiString(QStringLiteral("character"), id);
    }
    current_ = composeCharacter(loadTheme(current_.id));
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
