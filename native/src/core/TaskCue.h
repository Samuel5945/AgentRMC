#pragma once

#include <QString>

// 对话模式里「这句话该谁接」的判据。纯本地词表、零延迟——判断本身要是还得
// 等一次推理，「说完就有反应」就没了。返回命中的判据名，空串 = 不像任务，
// 留给桌宠自己接。
namespace TaskCue {

// 命中次序（先命中的算数，只报一个成因）：
// 点名（话里点了宿主的名字）> 交接（交给/派给…）> 回指（刚才/上一条…）
// > 干活（原有动词白名单）> 长句（多半是任务描述）。
QString of(const QString &text);

// 这句是在问「你自己会什么」——只有这种句子才值得把整份功能清单塞进这一次
// 请求；平时只带一句索引，别把系统提示词当上下文垃圾桶。
bool wantsList(const QString &text);

}  // namespace TaskCue
