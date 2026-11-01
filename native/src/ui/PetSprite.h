#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QObject>
#include <QString>

class Theme;
struct ThemePetState;

// 桌宠精灵帧播放器：轮播主题帧 + 程序化浮动/摆动。
// 故意不是 QWidget——VoicePet::paintEvent 自己取当前帧来画，
// 状态环、贴边缩条这些层级才不受子窗口影响。
class PetSprite final : public QObject {
    Q_OBJECT

public:
    explicit PetSprite(QObject *parent = nullptr);

    void applyTheme(const Theme &theme);
    // idle/listening/recording/transcribing/chat；没有该状态的帧就回落 idle。
    void setState(const QString &state);
    bool hasFrames() const;
    // VoicePet::tick 每 50ms 推一次时钟。
    void advance(int ms);
    // 当前帧与变换；返回 false = 主题没精灵（调用方画圆球）。
    bool current(QPixmap *pixmap, qreal *rotationDeg, qreal *dy) const;

private:
    const ThemePetState *resolved() const;

    QHash<QString, ThemePetState> states_;
    QString state_ = QStringLiteral("idle");
    // current() 是 const 却要懒加载缓存，用 mutable。
    mutable QHash<QString, QPixmap> pixmaps_;  // 帧路径 -> 位图
    QElapsedTimer clock_;
    qint64 elapsedMs_ = 0;
};
