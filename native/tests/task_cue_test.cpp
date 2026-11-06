#include "core/TaskCue.h"

#include <cstdio>

namespace {

int failures = 0;

void check(const char *name, const QString &actual, const QString &expected) {
    if (actual == expected) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", name,
                    expected.toUtf8().constData(), actual.toUtf8().constData());
        ++failures;
    }
}

QString s(const char *utf8) { return QString::fromUtf8(utf8); }

void check(const char *name, bool actual, bool expected) {
    if (actual == expected) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s (want %d, got %d)\n", name, int(expected),
                   int(actual));
        ++failures;
    }
}

}  // namespace

int main() {
    // 点名：真机踩到的三条都在这里（ASR 把 Qoder 听成「科德」）。
    check("spoken homophone host", TaskCue::of(s("在科德里查。")), s("点名"));
    check("ascii host", TaskCue::of(s("你自动判断不应该把我刚才的话交给Qoder吗？")),
          s("点名"));
    check("buddy host", TaskCue::of(s("让巴迪跑一下")), s("点名"));

    // 没点名但话要出桌宠的口。
    check("hand-off verb", TaskCue::of(s("这条派给隔壁")), s("交接"));

    // 接续刚才那段工作。
    check("back reference", TaskCue::of(s("刚才那句再说一遍")), s("回指"));

    // 原有白名单不动。
    check("work verb", TaskCue::of(s("帮我改一下登录页")), s("干活"));

    const QString longNoVerb =
        s("今天天气不错我们一起去楼下那家面馆吃一碗面再加一个卤蛋和一份凉拌黄瓜"
          "可以吗我觉得有点饿了");
    check("long sentence still counts", TaskCue::of(longNoVerb), s("长句"));

    // 闲聊：一句都不许贴进 Agent 会话。
    check("short chitchat", TaskCue::of(s("羡慕一耳光呢！")), s(""));
    check("statement about the app", TaskCue::of(s("项目已有功能。")), s(""));
    check("quoting a UI label is not a host",
          TaskCue::of(s("你直接在「继续绘画」里输入「查询」，不就行了？")), s(""));
    check("empty in -> empty out", TaskCue::of(s("")), s(""));

    // 已知代价（用户裁决「加指向性判据」时说明过）：光靠「列出」这类
    // 没进白名单的动词，短句仍留在桌宠自己接。
    check("documented gap: bare list verb",
          TaskCue::of(s("列出当前语音操控电脑清单。")), s(""));

    // 按需注入：整份功能清单只跟着「真在要清单」的那一句走。
    check("inventory on list request",
          TaskCue::wantsList(s("列出当前语音操控电脑清单。")), true);
    check("inventory on capability question",
          TaskCue::wantsList(s("你都能干什么？")), true);
    check("inventory on how-to-use", TaskCue::wantsList(s("语音怎么用")), true);
    check("no inventory for chitchat", TaskCue::wantsList(s("羡慕一耳光呢！")),
          false);
    check("no inventory for a task", TaskCue::wantsList(s("帮我改一下登录页")),
          false);
    check("no inventory for empty", TaskCue::wantsList(s("")), false);

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
