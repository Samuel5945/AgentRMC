#pragma once

#include <QObject>

// 下拉列表和右键菜单都是独立的 popup 窗口，默认不透明：QSS 画圆角时圆角
// 以外会露出系统窗口底色（黑角）；Win10 的 Qt 又不给 popup 开透明底
// （实测 HWND 无 WS_EX_LAYERED），QWidget::setMask 的逻辑像素 1-bit 区域
// 在 2x DPI 下台阶粗、还会把抗锯齿描边剪脏。
//
// 这里的做法：
//   1. ChildAdded 时去掉 Win10 的 CS_DROPSHADOW（系统 SysShadow 窗口就是
//      弹层外面那圈脏阴影）；
//   2. show/resize 时用 Win32 SetWindowRgn 按物理像素剪圆角（台阶 1 物理
//      像素，肉眼平滑），圆角以外真正镂空透出后面内容；窗口底刷成主题
//      浮层色，QSS 只纯平涂不写描边/圆角 —— 一块干净的纯圆角矩形；
//   3. 下拉弹层和按钮之间留 6px 的缝，不再紧贴按钮下沿。
// 不做投影：自绘阴影层要替换 Qt 整套弹出机制，成本另算（已确认不做）。
class PopupShape final : public QObject {
    Q_OBJECT

public:
    // 应用启动时装一次；重复调用无副作用。
    static void install();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit PopupShape(QObject *parent);
};
