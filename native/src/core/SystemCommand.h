#pragma once

#include <QString>

// 语音/桥接共用的系统控制命令。parse() 是纯函数：不碰系统状态、不发按键，
// 便于表驱动测试。宁可不命中也不能误命中——没命中的话还要照原样听写下去。
enum class ScreenScope {
    Cursor,   // 光标所在那块屏
    Primary,  // 主屏
    All,
};

struct SystemCommand {
    bool matched = false;
    QString kind;  // volume / brightness / media / screen_off / lock /
                   // sleep / hibernate / shutdown / restart / shutdown_cancel
    QString op;    // query / set / step / mute / unmute / toggle / play / pause /
                   // next / prev / off / lock / suspend / hibernate / cancel
    int value = 0;            // set = 目标百分比；step = 带符号的幅度
    ScreenScope screen = ScreenScope::Cursor;
    bool confirmed = false;   // 这句话本身就是「确认关机」这样的确认口令
};

namespace SystemCommandText {

SystemCommand parse(const QString &text);
// 关机/重启/睡眠/休眠：执行前要过二次确认。
bool needsConfirm(const QString &kind);
// 开关按类存：ui.voice_system_<category>。
QString category(const QString &kind);
QString label(const SystemCommand &command);
QString confirmPhrase(const QString &kind);
// 「你会用语音干什么」的清单，直接从上面那张解析词表生成——桌宠的对话模型
// 只有照着它答才不会编，硬抄一份文案迟早和真能听懂的词分叉。
QString spokenList();
// 「五十」「一百」「三十五」→ 数值；解不出或超出 0~100 返回 -1。
int percentValue(const QString &text);

}  // namespace SystemCommandText
