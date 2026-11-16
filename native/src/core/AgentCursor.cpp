#include "core/AgentCursor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uiautomation.h>

#include <QDebug>
#include <QJsonValue>
#include <QStringList>

namespace AgentCursor {
namespace {

constexpr int kMaxDurationMs = 4000;
// 绝对值上界：真实桌面坐标不会到这个量级，越界一定是调用方算错了坐标系
// （比如把逻辑坐标当物理坐标传、或漏了屏幕偏移）。宁可 400，也不要画到天外
// 再点到一个随机窗口。
constexpr int kCoordLimit = 100000;

QString backendName(const QString &raw) {
    const QString value = raw.trimmed().toLower();
    if (value == QStringLiteral("uia") || value == QStringLiteral("post")) {
        return value;
    }
    return QStringLiteral("auto");
}

QString buttonName(const QString &raw, bool *ok) {
    const QString value = raw.trimmed().toLower();
    if (value.isEmpty() || value == QStringLiteral("left")) {
        *ok = true;
        return QStringLiteral("left");
    }
    if (value == QStringLiteral("right") || value == QStringLiteral("middle") ||
        value == QStringLiteral("double")) {
        *ok = true;
        return value;
    }
    *ok = false;
    return QString();
}

bool readPoint(const QJsonObject &payload, QPoint *out, QString *error) {
    const QJsonValue x = payload.value(QStringLiteral("x"));
    const QJsonValue y = payload.value(QStringLiteral("y"));
    if (!x.isDouble() || !y.isDouble()) {
        *error = QStringLiteral("缺 x/y：坐标必须是物理像素数字");
        return false;
    }
    const int px = int(x.toDouble());
    const int py = int(y.toDouble());
    if (qAbs(px) > kCoordLimit || qAbs(py) > kCoordLimit) {
        *error = QStringLiteral("坐标 %1,%2 越界（上限 %3）")
                     .arg(px)
                     .arg(py)
                     .arg(kCoordLimit);
        return false;
    }
    *out = QPoint(px, py);
    return true;
}

void readCommon(const QJsonObject &payload, Request *req) {
    req->agent = payload.value(QStringLiteral("agent")).toString().trimmed();
    req->backend =
        backendName(payload.value(QStringLiteral("backend")).toString());
    const QJsonValue duration = payload.value(QStringLiteral("duration_ms"));
    if (duration.isDouble()) {
        req->durationMs = qBound(0, int(duration.toDouble()), kMaxDurationMs);
    }
    // 只认显式给的 click：parseMove 把默认设成「不点」，这里若无条件写 true 会
    // 把它顶掉（第一版就是这么把 move 变成会点击的）。
    if (payload.contains(QStringLiteral("click"))) {
        req->click = payload.value(QStringLiteral("click")).toBool();
    }
}

// ---------------------------------------------------------------- UIA 那条路

IUIAutomation *automation() {
    // 每线程一份：调用方可能在主线程，也可能在点击线程（见 MainWindow 的
    // QThread::create）。UIA 是 apartment 对象，跨线程共享会直接失败。
    thread_local IUIAutomation *ui = nullptr;
    thread_local bool tried = false;
    if (tried) {
        return ui;
    }
    tried = true;
    // RPC_E_CHANGED_MODE（本线程已被别处做成 MTA）不算致命，只是这次拿不到
    // UIA 对象，调用方会退回 PostMessage。
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (init != S_OK && init != S_FALSE && init != RPC_E_CHANGED_MODE) {
        return nullptr;
    }
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui)))) {
        ui = nullptr;
    }
    return ui;
}

bool isClickThrough(HWND hwnd) {
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
}

// WindowFromPoint 会把「点击穿透」的窗口也当命中——而 Agent 指针的覆盖层就是
// 一块全屏 topmost 的穿透窗，直接用它只会点到自己。所以按 z 序往下找第一扇真
// 正吃鼠标的窗口。命中顶层窗后还要往里钻一层（子控件才是真正处理消息的那个）。
HWND windowAtPoint(const QPoint &pt) {
    POINT raw{pt.x(), pt.y()};
    for (HWND hwnd = GetTopWindow(nullptr); hwnd != nullptr;
         hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || isClickThrough(hwnd)) {
            continue;
        }
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect) || !PtInRect(&rect, raw)) {
            continue;
        }
        POINT local = raw;
        ScreenToClient(hwnd, &local);
        HWND child = ChildWindowFromPointEx(
            hwnd, local, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        return child != nullptr ? child : hwnd;
    }
    return WindowFromPoint(raw);
}

QString windowLabel(HWND hwnd) {
    if (hwnd == nullptr) {
        return QString();
    }
    wchar_t cls[256] = {0};
    GetClassNameW(hwnd, cls, 255);
    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 255);
    const QString klass = QString::fromWCharArray(cls);
    const QString text = QString::fromWCharArray(title);
    return text.isEmpty() ? klass : klass + QLatin1Char(':') + text;
}

// expected 是 windowAtPoint 认定的目标窗口。UIA 的 ElementFromPoint 走另一套命中
// 逻辑，可能落在别的窗口上（实测：探针窗被压住时它会去 Invoke 别处的元素并自称
// verified）。点错窗口比点不动严重得多，所以元素不属于同一棵窗口树就整条路放弃。
bool invokeAt(IUIAutomation *ui, const QPoint &pt, HWND expected,
              QString *detail) {
    if (ui == nullptr) {
        return false;
    }
    POINT raw{pt.x(), pt.y()};
    IUIAutomationElement *el = nullptr;
    if (FAILED(ui->ElementFromPoint(raw, &el)) || el == nullptr) {
        return false;
    }
    if (expected != nullptr) {
        UIA_HWND owner = 0;
        if (FAILED(el->get_CurrentNativeWindowHandle(&owner)) ||
            owner == nullptr) {
            el->Release();
            if (detail != nullptr) {
                *detail = QStringLiteral("UIA 元素没有原生窗口句柄");
            }
            return false;
        }
        // 不要求句柄完全相等：Chromium/Electron 里 UIA 元素常报顶层
        // Chrome_WidgetWin_1，而这里拿到的是更深的子窗。同一棵窗口树（祖先/后代）
        // 就算同一目标；跨应用一律拒绝。
        const HWND ownerHwnd = HWND(owner);
        const bool sameTree = ownerHwnd == expected ||
                              IsChild(ownerHwnd, expected) ||
                              IsChild(expected, ownerHwnd);
        if (!sameTree) {
            el->Release();
            if (detail != nullptr) {
                *detail = QStringLiteral("UIA 命中的元素不属于目标窗口");
            }
            return false;
        }
    }
    // 上溯父级要借 tree walker：IUIAutomationElement 自己没有「当前父级」getter
    // （只有 GetCachedParent），这是 UIA 里少数反直觉的地方。
    IUIAutomationTreeWalker *walker = nullptr;
    ui->get_ControlViewWalker(&walker);
    bool invoked = false;
    for (int hop = 0; hop < 4 && el != nullptr && !invoked; ++hop) {
        IUIAutomationInvokePattern *invoke = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId,
                                              IID_PPV_ARGS(&invoke))) &&
            invoke != nullptr) {
            invoked = SUCCEEDED(invoke->Invoke());
            invoke->Release();
        }
        if (!invoked) {
            IUIAutomationLegacyIAccessiblePattern *legacy = nullptr;
            if (SUCCEEDED(el->GetCurrentPatternAs(
                    UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy))) &&
                legacy != nullptr) {
                invoked = SUCCEEDED(legacy->DoDefaultAction());
                legacy->Release();
            }
        }
        if (invoked) {
            break;
        }
        if (walker == nullptr) {
            break;
        }
        IUIAutomationElement *parent = nullptr;
        if (FAILED(walker->GetParentElement(el, &parent)) ||
            parent == nullptr) {
            break;
        }
        el->Release();
        el = parent;
    }
    if (el != nullptr) {
        el->Release();
    }
    if (walker != nullptr) {
        walker->Release();
    }
    if (!invoked && detail != nullptr) {
        *detail = QStringLiteral("该点上的 UIA 元素没有可调用的默认动作");
    }
    return invoked;
}

// ------------------------------------------------------- PostMessage 那条路

struct ButtonMessages {
    UINT down = 0;
    UINT up = 0;
    UINT dbl = 0;
    WPARAM downFlags = 0;
};

ButtonMessages messagesFor(const QString &button) {
    ButtonMessages m;
    if (button == QStringLiteral("right")) {
        m.down = WM_RBUTTONDOWN;
        m.up = WM_RBUTTONUP;
        m.dbl = WM_RBUTTONDBLCLK;
        m.downFlags = MK_RBUTTON;
    } else if (button == QStringLiteral("middle")) {
        m.down = WM_MBUTTONDOWN;
        m.up = WM_MBUTTONUP;
        m.dbl = WM_MBUTTONDBLCLK;
        m.downFlags = MK_MBUTTON;
    } else {
        m.down = WM_LBUTTONDOWN;
        m.up = WM_LBUTTONUP;
        m.dbl = WM_LBUTTONDBLCLK;
        m.downFlags = MK_LBUTTON;
    }
    return m;
}

// 把点击「递」给目标窗口：坐标换成它自己的客户区坐标，消息异步投递。不抢焦点、
// 不动光标。递到 ≠ 生效（Chromium 系、DirectInput 游戏会自己再判），所以调用方
// 看到的 verified 是 false。
bool postClick(HWND hwnd, const QPoint &pt, const QString &button) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }
    POINT local{pt.x(), pt.y()};
    if (!ScreenToClient(hwnd, &local)) {
        return false;
    }
    // 点落在客户区外，说明这层窗不吃这个位置（钻出来的子窗尺寸常对不上），
    // 交给调用方往上一层试。
    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        return false;
    }
    if (local.x < client.left || local.y < client.top ||
        local.x >= client.right || local.y >= client.bottom) {
        return false;
    }
    const LPARAM where = MAKELPARAM(local.x, local.y);
    const ButtonMessages m = messagesFor(button);
    const int clicks = button == QStringLiteral("double") ? 2 : 1;
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, where);
    for (int i = 0; i < clicks; ++i) {
        PostMessageW(hwnd, m.down, m.downFlags, where);
        PostMessageW(hwnd, m.up, 0, where);
        if (i == 0 && clicks == 2 && m.dbl != 0) {
            PostMessageW(hwnd, m.dbl, m.downFlags, where);
        }
    }
    return true;
}

ClickResult fail(const QString &error) {
    ClickResult result;
    result.backend = QStringLiteral("none");
    result.error = error;
    return result;
}

}  // namespace

Request parseMove(const QJsonObject &payload) {
    Request req;
    req.click = false;
    QString error;
    if (!readPoint(payload, &req.pos, &error)) {
        req.error = error;
        return req;
    }
    readCommon(payload, &req);
    req.ok = true;
    return req;
}

Request parseClick(const QJsonObject &payload) {
    Request req;
    QString error;
    if (!readPoint(payload, &req.pos, &error)) {
        req.error = error;
        return req;
    }
    readCommon(payload, &req);
    bool buttonOk = false;
    req.button =
        buttonName(payload.value(QStringLiteral("button")).toString(), &buttonOk);
    if (!buttonOk) {
        req.error = QStringLiteral("button 只认 left / right / middle / double");
        return req;
    }
    req.ok = true;
    return req;
}

ClickResult performClick(const Request &req) {
    if (!req.ok) {
        return fail(req.error.isEmpty() ? QStringLiteral("请求不合法") : req.error);
    }
    HWND hwnd = windowAtPoint(req.pos);
    if (hwnd == nullptr) {
        return fail(QStringLiteral("该坐标上没有窗口"));
    }
    ClickResult result;
    result.target = windowLabel(hwnd);

    const bool wantUia = req.backend == QStringLiteral("auto") ||
                         req.backend == QStringLiteral("uia");
    if (wantUia) {
        QString detail;
        if (invokeAt(automation(), req.pos, hwnd, &detail)) {
            result.ok = true;
            result.backend = QStringLiteral("uia");
            result.verified = true;
            return result;
        }
        if (req.backend == QStringLiteral("uia")) {
            result.backend = QStringLiteral("none");
            result.error = detail.isEmpty()
                               ? QStringLiteral("该点上没有可用的 UIA 元素")
                               : detail;
            return result;
        }
    }
    // PostMessage 沿窗口链往上试：钻到的最深子窗有时已经不在了（目标程序刚重建
    // 过控件），或者它根本不处理鼠标消息，这时交给父窗/顶层窗才有意义。实测踩过：
    // 只投最深子窗，回执报「消息没递出去」而点击其实生效了——既不可信也没法用。
    HWND attempt = hwnd;
    bool posted = false;
    QString trail;
    for (int hop = 0; hop < 4 && attempt != nullptr && !posted; ++hop) {
        posted = postClick(attempt, req.pos, req.button);
        trail += QStringLiteral("%1[%2:%3] ")
                     .arg(posted ? QStringLiteral("+") : QStringLiteral("-"))
                     .arg(hop)
                     .arg(windowLabel(attempt));
        if (!posted) {
            attempt = GetParent(attempt);
        }
    }
    qInfo().noquote() << "[cursor] post trail:" << trail;
    if (!posted) {
        result.backend = QStringLiteral("none");
        result.error =
            QStringLiteral("消息没递出去（目标窗口可能已消失）：%1").arg(trail);
        return result;
    }
    result.ok = true;
    result.backend = QStringLiteral("post");
    // 递到了，但「有没有真触发」在目标程序手里——不撒谎说 verified。
    result.verified = false;
    result.error = QStringLiteral("已把 %1 点击投递给「%2」，是否生效取决于目标程序")
                       .arg(req.button, result.target);
    return result;
}

QString agentColor(const QString &agent) {
    // 固定调色板：每个 Agent 一色，用户一眼能看出「这根指针是谁的」。
    static const char *kPalette[] = {"#4da3ff", "#ffb454", "#8ce99a", "#ff8787",
                                     "#b197fc", "#63e6be", "#ffd43b", "#f783ac"};
    constexpr int kCount = int(sizeof(kPalette) / sizeof(kPalette[0]));
    const QString key = agent.trimmed().toLower();
    if (key.isEmpty()) {
        return QString::fromLatin1(kPalette[0]);
    }
    uint hash = 2166136261u;
    for (const QChar ch : key) {
        hash = (hash ^ uint(ch.unicode())) * 16777619u;
    }
    return QString::fromLatin1(kPalette[hash % kCount]);
}

}  // namespace AgentCursor
