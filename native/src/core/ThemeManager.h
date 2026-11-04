#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

class ProfileStore;

// 一个动画状态的精灵帧与程序动画参数；frames 为空表示该状态没有帧。
struct ThemePetState {
    QStringList frames;  // 绝对路径
    int fps = 2;
    int bob = 3;   // 上下浮动幅度（px）
    int tilt = 0;  // 左右摆动幅度（度）
};

// themes/ 下发现的一个主题目录（未必能解析成功）。
struct ThemeInfo {
    QString id;        // 目录名
    QString displayName;
    QString author;
    QString dir;       // 绝对路径
    QString preview;   // preview.png 绝对路径，空 = 没有
    bool valid = true;
    QString error;
};

// 解析后的主题。palette 已与 classic 默认合并；图片走 ThemeManager::cachedImage。
class Theme {
public:
    QString id;
    QString displayName;
    QString author;
    QString characterName;
    QString persona;
    QHash<QString, QString> palette;   // token -> 颜色串（hex/rgba/关键字）
    QHash<QString, ThemePetState> petStates;
    bool ring = true;                  // 精灵模式下是否保留状态环
    QString mainBackground;            // 绝对路径，空 = 没有背景图
    QString panelBackground;
    // 浅色模式变体（theme.json 可选 backgrounds.light / palette.light）：
    // 空 = 该主题不吃配色模式，浅色时整套沿用上面的深色值。
    QString mainBackgroundLight;
    QString panelBackgroundLight;
    QHash<QString, QString> paletteLight;  // 已合并 classicLight 默认；空 = 无浅色变体
    bool hasLightVariant() const {
        return !paletteLight.isEmpty() || !mainBackgroundLight.isEmpty() ||
               !panelBackgroundLight.isEmpty();
    }
    QString bubbleDecor;               // 桌宠气泡装饰贴纸（如大米饭），空 = 没有
    QString bubbleTail;                // 尾巴造型："whale" = 鲸鱼双鳍，空 = 普通气泡尖尾
    qreal dim = 0.55;                  // 背景上的压暗遮罩，保文字可读
    qreal dimLight = 0.08;             // 浅色插画上的白纱（保文字可读），默认很淡
    QString voiceName;                 // 主题音色名（TTS 的 audio.voice 参数）；空 = Chloe
    QString voiceStyle;                // 主题音色描述；用户没自填音色描述时用

    bool hasPetSprite() const { return !petStates.isEmpty(); }
    QString paletteValue(const QString &token) const {
        return palette.value(token);
    }
};

// 主题系统入口：扫描 exeDir/themes/、渲染 QSS、切换与广播。
// classic 是代码内置的零美术主题（= 原配色 + 圆球桌宠），永远存在且兜底。
class ThemeManager final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    static ThemeManager &instance();

    // main.cpp 早期调用：扫描 + 载入 ui.theme 指向的主题 + 应用渲染后的 QSS。
    void init(ProfileStore *store);
    void rescan();

    QList<ThemeInfo> themes() const { return infos_; }
    const Theme &current() const { return current_; }
    QString currentId() const { return current_.id; }
    // 按 id 取解析结果（主题面板画色卡用）；不存在回落 classic。
    Theme themeById(const QString &id) const { return loadTheme(id); }

    static QString themesRoot();

    // —— 外观覆盖：用户在「主题」面板里拖的滑块，持久化在 ui.card_alpha /
    // ui.backdrop_dim / ui.font_scale；没设过就跟随主题自己的值。——
    int cardAlpha() const;             // 卡片不透明度 0-255
    qreal backdropDim() const;         // 背景压暗/白纱 0-1（主窗含可读性下限）
    qreal fontScale() const;           // 全窗字号倍率 0.8-1.5（1.0 = 跟随默认）
    bool hasAppearanceOverride() const;
    void setCardAlpha(int alpha);      // 落盘 + 重刷 QSS + 广播
    void setBackdropDim(qreal dim);    // 落盘 + 广播（主窗自己重绘背景）
    void setFontScale(qreal scale);    // 落盘 + 重刷 QSS + 广播
    void resetAppearance();            // 回到主题默认
    // 代码内联样式 / 自绘控件的字号统一走这里：和 QSS 的 font-size 一起缩放，
    // 不然全局缩了、这几处硬编码的还是老尺寸，一眼看出不齐。
    static int scaledPx(qreal px);

    // "rgba(8, 12, 20, 55)" / "#121722" / 命名色 → QColor；解析不了返回无效色。
    static QColor parsePaletteColor(const QString &value);

    // —— 配色模式：浅色 / 深色（夜间）/ 跟随系统。内置 classic 与带浅色
    // 变体（backgrounds.light / palette.light）的主题都吃这个开关；
    // 没带变体的自定义主题不受影响。——
    QString scheme() const { return scheme_; }
    void setScheme(const QString &scheme);
    // 配色模式解析成浅色（不区分主题，读系统偏好）。
    bool prefersLight() const;
    // 当前主题 + 配色模式解析到浅色（classic 恒可用；自定义主题要有变体）。
    bool isLightActive() const;
    // 按 token 取当前已应用调色板里的颜色：内联样式 / 自绘控件用，
    // 这样它们跟全局 QSS 一起随浅色/深色切换，不会各画各的。
    QColor color(const QString &token) const;
    QString css(const QString &token) const;

    // 跟随系统：Windows 切换深浅色会广播 WM_SETTINGCHANGE，这里收到后
    // 若当前是 auto 且主题吃配色模式就重算并刷新。
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;

public slots:
    // 持久化 ui.theme + 立即应用 + 广播 themeChanged。
    void setTheme(const QString &id);
    // 形象与主题分离：ui.character 空 = 跟随主题，none = 无形象，
    // 其余 = 主题包 id（只取它的桌宠帧/人设/音色；配色与插画背景仍跟主题）。
    void setCharacter(const QString &id);

public:
    QString characterId() const { return characterId_; }

signals:
    void themeChanged(const Theme &theme);

public:
    // 导入主题包（.zip）：解压、找 theme.json、校验、装进 themes/（同名覆盖=更新）。
    // 返回主题目录名（即主题 id）；失败返回空串并给出 error。
    QString importThemePackage(const QString &zipPath, QString *error);
    // 纯函数版本：显式指定目标 themes 目录，便于测试；不触发 rescan。
    static QString importThemePackageTo(const QString &zipPath,
                                        const QString &themesDir,
                                        QString *error);

public:
    // —— 纯函数，测试直接调用 ——
    static QHash<QString, QString> classicPalette();
    // classic 的浅色版：同一套 token，浅底深字；夜间深色版就是 classicPalette。
    static QHash<QString, QString> classicLightPalette();
    // 系统（Windows）当前偏好浅色还是深色；读不到时按深色兜底。
    static bool systemPrefersLight();
    static QHash<QString, QString> mergePalette(
        const QHash<QString, QString> &overrides);
    static ThemeInfo parseThemeDir(const QString &dir, QString *error);
    // theme.json 清单正文 → Theme（palette 已合并 classic 默认）；纯函数供测试。
    static Theme parseThemeBody(const ThemeInfo &info);
    static QString renderQss(const QString &qssTemplate,
                             const QHash<QString, QString> &palette,
                             const QString &rootBackgroundImage,
                             qreal fontScale = 1.0);
    static QImage cachedImage(const QString &path);

private:
    Theme loadTheme(const QString &id) const;
    // 按当前配色模式（浅色/深色/跟随系统）组装内置 classic。
    Theme buildClassicTheme() const;
    // 配色来自主题、形象字段来自 characterId_ 指向的包（或清空）。
    Theme composeCharacter(Theme base) const;
    void applyQss();

    ProfileStore *store_ = nullptr;
    QList<ThemeInfo> infos_;
    Theme current_;
    // light / dark / auto；auto = 跟随系统。持久化在 ui.color_scheme。
    QString scheme_ = QStringLiteral("auto");
    // 空 = 跟随主题；none = 无形象；其余 = 形象包 id。持久化在 ui.character。
    QString characterId_;
    bool nativeFilterInstalled_ = false;
    // -1 = 跟随主题。用户拖过滑块才有值。
    int cardAlpha_ = -1;
    qreal backdropDim_ = -1.0;
    // 0 = 跟随默认 100%。持久化在 ui.font_scale（存百分比整数字符串）。
    qreal fontScale_ = 0.0;
};
