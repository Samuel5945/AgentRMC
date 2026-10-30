#pragma once

#include <QDialog>

class QKeyEvent;
class QMouseEvent;
class QWidget;

// 引导页与引导弹窗共用的「键位 + 说明」行。
namespace guide {

QWidget *heading(const QString &text);
QWidget *row(const QString &key, const QString &text, int keyWidth);

}  // namespace guide

class GuideDialog final : public QDialog {
    Q_OBJECT

public:
    explicit GuideDialog(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
};
