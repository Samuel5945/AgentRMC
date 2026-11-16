// Agent 专用虚拟指针的离线 + 真机回归：
//   1. 请求解析（缺坐标 / 越界 / 非法 button / 时长钳制）；
//   2. 点击真的送到目标窗口，且**跳过全屏穿透覆盖层**（否则指针自己就把点击吃
//      了，这是这套东西最容易踩的坑）；
//   3. 全程 GetCursorPos 一动不动——这是「不跟用户抢鼠标」唯一的硬证明。
#include "core/AgentCursor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QCoreApplication>
#include <QJsonObject>
#include <QString>

#include <cstdio>

namespace {

int failures = 0;
int checks = 0;

bool check(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL cursor: %s\n", message);
    }
    return condition;
}

int g_down = 0;
int g_up = 0;

LRESULT CALLBACK probeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN) {
        ++g_down;
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        ++g_up;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 假目标窗（吃鼠标）与假覆盖层（点击穿透）——后者用来证明 windowAtPoint 会跳过
// 自己这一类窗口。
HWND createProbe(const wchar_t *cls, int x, int y, int w, int h,
                 bool clickThrough) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = probeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                     (clickThrough ? (WS_EX_TRANSPARENT | WS_EX_LAYERED) : 0);
    HWND hwnd = CreateWindowExW(ex, cls, L"cursor probe", WS_POPUP, x, y, w, h,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd != nullptr) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

void pump(int ms) {
    const DWORD end = GetTickCount() + DWORD(ms);
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

QJsonObject movePayload() {
    return QJsonObject{{QStringLiteral("x"), 640}, {QStringLiteral("y"), 480}};
}

void checkParsing() {
    const AgentCursor::Request plain = AgentCursor::parseMove(movePayload());
    check(plain.ok, "move parses with x/y only");
    check(!plain.click, "move never clicks");
    check(plain.durationMs == 240, "move default duration");
    check(plain.backend == QLatin1String("auto"), "move default backend");
    check(plain.pos == QPoint(640, 480), "move keeps physical coordinates");

    QJsonObject clamped = movePayload();
    clamped.insert(QStringLiteral("duration_ms"), 999999);
    check(AgentCursor::parseMove(clamped).durationMs == 4000,
          "duration clamped to 4s");

    QJsonObject negative = movePayload();
    negative.insert(QStringLiteral("duration_ms"), -50);
    check(AgentCursor::parseMove(negative).durationMs == 0,
          "negative duration treated as instant");

    const AgentCursor::Request missing =
        AgentCursor::parseMove(QJsonObject{{QStringLiteral("x"), 10}});
    check(!missing.ok && !missing.error.isEmpty(), "missing y rejected with reason");

    // 注意别把变量叫 far/near：windows.h 里那两个是遗留宏，会把这行拆得面目全非。
    const AgentCursor::Request absurd = AgentCursor::parseMove(
        QJsonObject{{QStringLiteral("x"), 1000000}, {QStringLiteral("y"), 0}});
    check(!absurd.ok, "absurd coordinates rejected");

    const AgentCursor::Request left =
        AgentCursor::parseClick(movePayload());
    check(left.ok && left.button == QLatin1String("left"),
          "click defaults to left button");
    check(left.click, "click clicks");

    QJsonObject right = movePayload();
    right.insert(QStringLiteral("button"), QStringLiteral("double"));
    check(AgentCursor::parseClick(right).button == QLatin1String("double"),
          "double click accepted");

    QJsonObject weird = movePayload();
    weird.insert(QStringLiteral("button"), QStringLiteral("sideways"));
    check(!AgentCursor::parseClick(weird).ok, "unknown button rejected");

    QJsonObject backend = movePayload();
    backend.insert(QStringLiteral("backend"), QStringLiteral("telepathy"));
    check(AgentCursor::parseClick(backend).backend == QLatin1String("auto"),
          "unknown backend falls back to auto");

    // 非法请求绝不允许走到「点一下」那一步。
    const AgentCursor::ClickResult refused =
        AgentCursor::performClick(missing);
    check(!refused.ok && refused.backend == QLatin1String("none"),
          "invalid request never clicks");
}

void checkColors() {
    const QString dsh = AgentCursor::agentColor(QStringLiteral("DSH"));
    check(dsh.startsWith(QLatin1Char('#')) && dsh.size() == 7,
          "agent color is #RRGGBB");
    check(dsh == AgentCursor::agentColor(QStringLiteral("dsh")),
          "agent color is case-insensitive and stable");
    check(!AgentCursor::agentColor(QString()).isEmpty(),
          "empty agent still gets a color");
}

void checkRealClick() {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND probe = createProbe(L"AgentRMCCursorProbe", 200, 200, 200, 200, false);
    HWND veil = createProbe(L"AgentRMCCursorVeil", vx, vy, vw, vh, true);
    if (probe == nullptr || veil == nullptr) {
        check(false, "probe windows created");
        return;
    }
    // 覆盖层必须压在目标窗上面，才算真的验了「跳过穿透层」。
    SetWindowPos(probe, HWND_TOPMOST, 200, 200, 200, 200,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(veil, HWND_TOPMOST, vx, vy, vw, vh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    pump(120);

    POINT before{};
    GetCursorPos(&before);

    AgentCursor::Request req =
        AgentCursor::parseClick(QJsonObject{{QStringLiteral("x"), 300},
                                            {QStringLiteral("y"), 300},
                                            {QStringLiteral("agent"),
                                             QStringLiteral("cursor-test")},
                                            {QStringLiteral("backend"),
                                             QStringLiteral("post")}});
    check(req.ok, "probe click request parses");
    const AgentCursor::ClickResult posted = AgentCursor::performClick(req);
    // 立刻采样：后面 `pump(400)` 里用户自己可能动鼠标，那笔账不能算到我们头上。
    // 要证的是「点击这条路径不碰光标」，采样窗口就得贴着调用。
    POINT rightAfter{};
    GetCursorPos(&rightAfter);
    pump(400);
    check(posted.ok, "click is delivered");
    check(posted.backend == QLatin1String("post"),
          "post backend used for a plain window");
    check(g_down >= 1 && g_up >= 1,
          "target window got the button messages despite the transparent veil");
    check(posted.target.contains(QLatin1String("Probe")),
          "result names the window it hit");
    check(!posted.verified, "post click is honest about not being verified");

    // auto 也要能落到同一个窗口。走 uia 还是 post 取决于该点上有没有可 Invoke 的
    // 元素——两条都算成功，断言只要求「点到了」，别锁死后端（锁死后 UIA 一旦能
    // 用，测试反而会红）。
    g_down = 0;
    g_up = 0;
    AgentCursor::Request autoReq =
        AgentCursor::parseClick(QJsonObject{{QStringLiteral("x"), 300},
                                            {QStringLiteral("y"), 300}});
    const AgentCursor::ClickResult automatic =
        AgentCursor::performClick(autoReq);
    pump(400);
    check(automatic.ok, "auto backend also delivers");
    check(automatic.backend == QLatin1String("uia") ||
              automatic.backend == QLatin1String("post"),
          "auto backend reports which path it took");

    check(before.x == rightAfter.x && before.y == rightAfter.y,
          "system cursor never moved (the whole point)");

    DestroyWindow(veil);
    DestroyWindow(probe);
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    checkParsing();
    checkColors();
    checkRealClick();

    if (failures == 0) {
        std::printf(
            "PASS native cursor: parse / click-through veil / cursor untouched "
            "(%d checks)\n",
            checks);
        return 0;
    }
    std::printf("FAIL native cursor: %d of %d checks failed\n", failures, checks);
    return 1;
}
