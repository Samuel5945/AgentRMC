#pragma once

#include <QLabel>

class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QTimer;

class Toast final : public QLabel {
    Q_OBJECT

public:
    explicit Toast(QWidget *parent);

    void showText(const QString &text, int durationMs = 2200);
    void reposition();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QTimer *hideTimer_ = nullptr;
    QGraphicsOpacityEffect *effect_ = nullptr;
    QPropertyAnimation *fade_ = nullptr;
};
