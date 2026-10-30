#pragma once

#include <QLabel>

class QPaintEvent;
class QPropertyAnimation;
class QTimer;

class Toast final : public QLabel {
    Q_OBJECT

public:
    explicit Toast(QWidget *parent);

    void showText(const QString &text, int durationMs = 2200);
    void reposition();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QTimer *hideTimer_ = nullptr;
    QPropertyAnimation *fade_ = nullptr;
};
