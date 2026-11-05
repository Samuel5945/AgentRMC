#include "PopupShape.h"

#include "core/ThemeManager.h"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QPalette>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// 圆角半径：和浮层观感对齐（DSH 紧凑菜单 12px / 普通菜单 8px）。
constexpr int kComboRadius = 12;
constexpr int kMenuRadius = 8;
// 弹层和下拉按钮之间留一条缝（DSH 的 menu 也是离锚点几像素浮着），
// 不再紧贴着按钮下沿。
constexpr int kAnchorGap = 6;

// 弹层容器的父对象是 QComboBox（不同 Qt 版本也可能挂在 view 上），
// 沿父链找比按类名猜稳。
QComboBox *comboOwner(QWidget *widget) {
    if (widget == nullptr || !widget->isWindow() ||
        !(widget->windowFlags() & Qt::Popup)) {
        return nullptr;
    }
    for (QWidget *parent = widget->parentWidget(); parent != nullptr;
         parent = parent->parentWidget()) {
        if (auto *combo = qobject_cast<QComboBox *>(parent)) {
            return combo;
        }
    }
    return nullptr;
}

int popupRadius(QWidget *widget) {
    if (widget == nullptr || !widget->isWindow() ||
        !(widget->windowFlags() & Qt::Popup)) {
        return 0;
    }
    if (qobject_cast<QMenu *>(widget) != nullptr) {
        return kMenuRadius;
    }
    return comboOwner(widget) != nullptr ? kComboRadius : 0;
}

// 弹层窗口底必须是不透明的：主题里的 raised 在美术主题下也可能是
// 半透明 rgba，直接拿去铺窗口底会和黑色窗口底叠成脏色。
QColor popupSurface() {
    QColor surface = ThemeManager::instance().color(QStringLiteral("raised"));
    if (!surface.isValid()) {
        surface = ThemeManager::instance().color(QStringLiteral("card.bg"));
    }
    if (!surface.isValid()) {
        surface = ThemeManager::instance().color(QStringLiteral("bg"));
    }
    if (surface.isValid()) {
        surface.setAlpha(255);
    }
    return surface;
}

// Win10 的 Qt popup 不吃 WA_TranslucentBackground（实测 HWND 没有
// WS_EX_LAYERED），圆角以外只能是黑底；QWidget::setMask 又是逻辑像素的
// 1-bit 区域，2x DPI 下台阶有 2 物理像素、还会把抗锯齿描边剪脏。
// 这里直接用 Win32 SetWindowRgn 按「物理像素」剪圆角：台阶只有 1 物理
// 像素，肉眼是平滑的圆角矩形；圆角以外真正镂空（透出后面的内容）。
// 配合 QSS 去掉描边/圆角（纯平涂），就没有任何脏边。
void applyRoundRegion(QWidget *popup, int radius) {
#ifdef Q_OS_WIN
    const qreal dpr = popup->devicePixelRatioF();
    const QSizeF size = QSizeF(popup->size()) * dpr;
    if (size.width() <= 2 * radius * dpr || size.height() <= 2 * radius * dpr) {
        return;
    }
    // 原生圆角区域（物理像素）：比多边形拟合干净，不会有拟合毛刺像素。
    const int dia = qMax(2, qRound(2 * radius * dpr));
    HRGN region = CreateRoundRectRgn(0, 0, int(size.width()) + 1,
                                     int(size.height()) + 1, dia, dia);
    if (region != nullptr) {
        // SetWindowRgn 之后区域归系统所有，不要 DeleteObject。
        SetWindowRgn(reinterpret_cast<HWND>(popup->winId()), region, TRUE);
    }
#else
    Q_UNUSED(popup);
    Q_UNUSED(radius);
#endif
}

void shapePopup(QWidget *popup, int radius) {
    const QColor surface = popupSurface();
    if (surface.isValid()) {
        QPalette palette = popup->palette();
        palette.setColor(QPalette::Window, surface);
        palette.setColor(QPalette::Base, surface);
        popup->setPalette(palette);
        popup->setAutoFillBackground(true);
    }
    applyRoundRegion(popup, radius);
}

// QComboBox 默认把弹层顶边贴在按钮下沿（空间不够时贴在上沿）。这里把它
// 挪开 kAnchorGap：只在「贴着」的容差范围内动，避免干扰 Qt 自己做的
// 屏幕边缘翻转；挪完再夹回屏幕可用区，别把弹层推出屏幕。
void applyAnchorGap(QWidget *popup, QComboBox *combo) {
    const QRect c(combo->mapToGlobal(QPoint(0, 0)), combo->size());
    const QRect p = popup->geometry();
    const int cTop = c.top();
    const int cBottom = c.top() + c.height();
    const int pTop = p.top();
    const int pBottom = p.top() + p.height();

    QScreen *screen = QGuiApplication::screenAt(c.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect avail =
        screen != nullptr ? screen->availableGeometry() : QRect();

    int y = pTop;
    if (pTop >= cBottom - 4 && pTop <= cBottom + kAnchorGap + 4) {
        // 弹层在按钮下方：顶边 = 按钮下沿 + 缝。
        y = cBottom + kAnchorGap;
        if (!avail.isNull() && y + p.height() > avail.bottom() + 1) {
            // 加了缝之后下方放不下：翻到按钮上方。
            y = cTop - kAnchorGap - p.height();
        }
    } else if (pBottom <= cTop + 4 && pBottom >= cTop - kAnchorGap - 4) {
        // 弹层在按钮上方（Qt 因屏幕空间翻转过）：底边 = 按钮上沿 - 缝。
        y = cTop - kAnchorGap - p.height();
    } else {
        return;  // 不是贴着锚点的摆法，不动。
    }
    if (!avail.isNull()) {
        y = qBound(avail.top(), y, avail.bottom() + 1 - p.height());
    }
    if (y != pTop) {
        popup->move(p.left(), y);
    }
}

}  // namespace

PopupShape::PopupShape(QObject *parent) : QObject(parent) {}

void PopupShape::install() {
    if (qApp == nullptr) {
        return;
    }
    if (qApp->findChild<PopupShape *>(QStringLiteral("popupShape"),
                                      Qt::FindDirectChildrenOnly) != nullptr) {
        return;
    }
    auto *shape = new PopupShape(qApp);
    shape->setObjectName(QStringLiteral("popupShape"));
    qApp->installEventFilter(shape);
}

bool PopupShape::eventFilter(QObject *watched, QEvent *event) {
    // 弹层容器 / QMenu 挂到父控件上时还没创建原生窗口：这时去掉 Win10 的
    // CS_DROPSHADOW（系统 SysShadow 窗口就是弹层外那圈脏阴影）。
    if (event->type() == QEvent::ChildAdded) {
        auto *child = static_cast<QChildEvent *>(event)->child();
        if (auto *widget = qobject_cast<QWidget *>(child);
            widget != nullptr && widget->isWindow() &&
            (widget->windowFlags() & Qt::Popup)) {
            widget->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        }
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::Show || event->type() == QEvent::Resize) {
        auto *widget = qobject_cast<QWidget *>(watched);
        const int radius = popupRadius(widget);
        if (radius > 0) {
            if (event->type() == QEvent::Show) {
                // Show 时几何已定、原生窗口还没上屏：同步剪圆角+留缝不闪。
                shapePopup(widget, radius);
                if (auto *combo = comboOwner(widget)) {
                    applyAnchorGap(widget, combo);
                }
            }
            // 高度可能在 showEvent 里才定下来，下一拍再校一次（幂等）。
            QTimer::singleShot(0, widget, [widget, radius] {
                shapePopup(widget, radius);
                if (auto *combo = comboOwner(widget)) {
                    applyAnchorGap(widget, combo);
                }
            });
        }
    }
    return QObject::eventFilter(watched, event);
}
