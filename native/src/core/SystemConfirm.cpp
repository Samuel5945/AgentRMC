#include "SystemConfirm.h"

void SystemConfirm::arm(const SystemCommand &command, qint64 now, qint64 windowMs) {
    command_ = command;
    until_ = now + windowMs;
}

bool SystemConfirm::expire(qint64 now) {
    if (!command_.matched || now < until_) {
        return false;
    }
    cancel();
    return true;
}

SystemConfirm::Verdict SystemConfirm::offer(const SystemCommand &command,
                                            qint64 now) {
    if (!command_.matched) {
        return Verdict::None;
    }
    if (now >= until_) {
        cancel();
        return Verdict::None;
    }
    // 必须是完整口令（「确认关机」），而且对得上正在等的那一条：
    // 光说「确认」解析不出命令，电视声或残留语音蹭不上。
    if (command.matched && command.confirmed && command.kind == command_.kind) {
        return Verdict::Confirmed;
    }
    cancel();
    return Verdict::Cancelled;
}

SystemCommand SystemConfirm::take() {
    const SystemCommand command = command_;
    cancel();
    return command;
}

void SystemConfirm::cancel() {
    command_ = SystemCommand{};
    until_ = 0;
}
