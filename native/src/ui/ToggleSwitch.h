#pragma once

#include <QWidget>

class ToggleSwitch final : public QWidget {
    Q_OBJECT

public:
    explicit ToggleSwitch(bool checked = false, QWidget *parent = nullptr);

    bool isChecked() const { return checked_; }
    void setChecked(bool on);
    QSize sizeHint() const override { return {36, 21}; }

signals:
    void toggled(bool on);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void flip();

    bool checked_ = false;
    bool hover_ = false;
};
