#include "core/TaskCue.h"

#include <QRegularExpression>

namespace TaskCue {
namespace {

// 宿主名。ASCII 大小写不敏感；中文那几条是 ASR 常听到的同音写法
// （Qoder/Code → 「科德」，WorkBuddy/CodeBuddy → 「巴迪」）。宁可漏判
// （桌宠自己接一句），也不硬凑词表——误判会把闲聊贴进 Agent 会话花它的额度。
const QRegularExpression &hostName() {
    static const QRegularExpression re(
        QStringLiteral(
            "qoder|coder|workbuddy|codebuddy|zcode|deepseek|opensquilla|"
            "科德|克德|阔德|巴迪"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// 交接动词：话要出桌宠的口、进别人的会话。
const QRegularExpression &handOffVerb() {
    static const QRegularExpression re(
        QStringLiteral("交给|派给|转给|丢给|发给|委托"));
    return re;
}

// 回指：接续的是刚才那段工作，不是新起的话题。
const QRegularExpression &callback() {
    static const QRegularExpression re(QStringLiteral(
        "刚才|刚说|上一条|上一句|前面说|之前那句"));
    return re;
}

// 干活动词：原有的白名单，一字未动。
const QRegularExpression &workVerb() {
    static const QRegularExpression re(QStringLiteral(
        "打开|启动|运行|执行|帮我|替我|给我|改一下|改改|修改|修复|构建|编译|"
        "打包|安装|卸载|部署|跑一下|跑个|测试|提交|推送|拉取|合并|搜索|查一下|"
        "查查|看一下|读一下|总结|生成|写一个|新建|创建|删除|重命名|移动|"
        "git |npm |pip |cmake|报错|日志|仓库"));
    return re;
}

// 问桌宠自己会什么——这种句子才配得上一次完整清单。
const QRegularExpression &selfListQuestion() {
    static const QRegularExpression re(QStringLiteral(
        "清单|能干什么|会干什么|能做什么|会做什么|都会什么|会说哪些|"
        "能说什么|有什么功能|哪些功能|功能列表|怎么用|语音操控|操控电脑"));
    return re;
}

}  // namespace

QString of(const QString &text) {
    if (text.isEmpty()) {
        return QString();
    }
    if (hostName().match(text).hasMatch()) {
        return QStringLiteral("点名");
    }
    if (handOffVerb().match(text).hasMatch()) {
        return QStringLiteral("交接");
    }
    if (callback().match(text).hasMatch()) {
        return QStringLiteral("回指");
    }
    if (workVerb().match(text).hasMatch()) {
        return QStringLiteral("干活");
    }
    // 长句多半是任务描述（"帮我看看这个为什么……"），短句多半是闲聊。
    if (text.size() >= 40) {
        return QStringLiteral("长句");
    }
    return QString();
}

bool wantsList(const QString &text) {
    return selfListQuestion().match(text).hasMatch();
}

}  // namespace TaskCue
