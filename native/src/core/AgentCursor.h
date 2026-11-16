#pragma once

#include <QJsonObject>
#include <QPoint>
#include <QString>

// Agent 专用虚拟指针：机器上所有 Agent 共用一根自己画的指针，从移动到点击全程
// 不移动系统光标——用户的手可以和 Agent 同时用鼠标（这是它存在的唯一理由）。
//
// 本文件只管两件事：**请求怎么判**（离线可测）与**点怎么落**（Win32 注入）。
// 怎么画（覆盖层、缓动、涟漪）在 ui/CursorOverlay；HTTP 口在 core/AgentBridge。
namespace AgentCursor {

// 坐标一律是「物理像素」：与截图、GetCursorPos、其它自动化工具看到的是同一套
// 数。逻辑坐标换算（逐屏 DPI）归 CursorOverlay，这里不引 QtGui。
struct Request {
    QPoint pos;
    QString agent;         // 调用者署名（DSH / Codex / ...）：决定指针颜色与标签
    QString backend;       // auto | uia | post
    QString button;        // left | right | middle | double
    int durationMs = 240;  // 指针缓动时长；0 = 立刻落点
    bool click = true;     // false = 只把指针挪过去，不点
    bool ok = false;
    QString error;  // ok=false 时给人看的拒绝原因（不猜，直接说哪儿不对）
};

// POST /cursor/move：只要 x/y，其余可选。
Request parseMove(const QJsonObject &payload);
// POST /cursor/click：move 的全部字段 + button / backend / click。
Request parseClick(const QJsonObject &payload);

struct ClickResult {
    bool ok = false;
    QString backend;        // 真用了哪条路：uia / post / none
    bool verified = false;  // uia 走到 Invoke 才算「点到」；post 只能算「递到了」
    QString target;         // 目标窗口「类名:标题」，给调用方判断点对没有
    QString error;
};

// 在 req.pos 上完成一次点击，全程不动系统光标。顺序：UIA(Invoke /
// DoDefaultAction) → PostMessage(WM_*BUTTON*)。都不成才失败——失败就说失败，
// 绝不偷偷改用真光标。
ClickResult performClick(const Request &req);

// 同一个 agent 名恒定同一个颜色（覆盖层与 /cursor/state 共用这一份算法）。
QString agentColor(const QString &agent);

}  // namespace AgentCursor
