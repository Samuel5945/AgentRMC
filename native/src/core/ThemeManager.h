#pragma once

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
    qreal dim = 0.55;                  // 背景上的压暗遮罩，保文字可读
    QString voiceName;                 // 主题音色名（TTS 的 audio.voice 参数）；空 = Chloe
    QString voiceStyle;                // 主题音色描述；用户没自填音色描述时用

    bool hasPetSprite() const { return !petStates.isEmpty(); }
    QString paletteValue(const QString &token) const {
        return palette.value(token);
    }
};

// 主题系统入口：扫描 exeDir/themes/、渲染 QSS、切换与广播。
// classic 是代码内置的零美术主题（= 原配色 + 圆球桌宠），永远存在且兜底。
class ThemeManager final : public QObject {
    Q_OBJECT

public:
    static ThemeManager &instance();

    // main.cpp 早期调用：扫描 + 载入 ui.theme 指向的主题 + 应用渲染后的 QSS。
    void init(ProfileStore *store);
    void rescan();

    QList<ThemeInfo> themes() const { return infos_; }
    const Theme &current() const { return current_; }
    QString currentId() const { return current_.id; }

    static QString themesRoot();

public slots:
    // 持久化 ui.theme + 立即应用 + 广播 themeChanged。
    void setTheme(const QString &id);

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
    static QHash<QString, QString> mergePalette(
        const QHash<QString, QString> &overrides);
    static ThemeInfo parseThemeDir(const QString &dir, QString *error);
    // theme.json 清单正文 → Theme（palette 已合并 classic 默认）；纯函数供测试。
    static Theme parseThemeBody(const ThemeInfo &info);
    static QString renderQss(const QString &qssTemplate,
                             const QHash<QString, QString> &palette,
                             const QString &rootBackgroundImage);
    static QImage cachedImage(const QString &path);

private:
    Theme loadTheme(const QString &id) const;
    void applyQss();

    ProfileStore *store_ = nullptr;
    QList<ThemeInfo> infos_;
    Theme current_;
};
