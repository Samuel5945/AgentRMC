#pragma once

#include <QSlider>

// 主题面板用的滑块，整条自绘。
// QSS 的 QSlider::handle 在 Windows 11 风格下会被 groove 的绘制区裁掉一截
// （用户 2026-11-05 截图：白色圆球顶部被削平），而且 4px 轨道 + 12px 小球
// 的观感偏旧。这里改成：6px 轨道 + 渐变完成段 + 16px 白色手柄（带投影、
// 悬停光环、焦点环），手柄左右各留半个半径，任何 DPI / 字号倍率下都不裁切。
class ThemeSlider final : public QSlider {
    Q_OBJECT

public:
    explicit ThemeSlider(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    // 轨道两端内缩的手柄半径，保证手柄贴在 0/100 时不越界。
    QRectF trackRect() const;
    qreal handleCenterX() const;
    int valueFromX(qreal x) const;

    bool hover_ = false;
    // 只有键盘/Tab 进来的焦点才画焦点环；鼠标点出来的不画。
    bool keyboardFocus_ = false;
};
