#pragma once

#include <QString>

#include "core/SystemCommand.h"

// 危险动作（关机/重启/睡眠/休眠）的二次确认状态机。时间由调用方喂进来，
// 所以能离线断言「超时零执行」「光说确认不算」「说别的就作废」。
class SystemConfirm {
public:
    enum class Verdict {
        None,       // 没有待确认的命令，这句话按普通流程走
        Confirmed,  // 口令对上了，take() 拿走命令去执行
        Cancelled,  // 说的是别的话：待确认作废，这句话仍按普通流程走
    };

    void arm(const SystemCommand &command, qint64 now, qint64 windowMs);
    // 到点作废；返回 true 表示刚刚超时（调用方据此提示一句）。
    bool expire(qint64 now);
    Verdict offer(const SystemCommand &command, qint64 now);
    SystemCommand take();
    void cancel();
    bool pending() const { return command_.matched; }
    QString kind() const { return command_.kind; }

private:
    SystemCommand command_;
    qint64 until_ = 0;
};
