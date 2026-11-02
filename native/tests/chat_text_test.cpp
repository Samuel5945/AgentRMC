#include "core/ChatText.h"

#include <cstdio>

namespace {

int failures = 0;

void check(const char *name, bool ok) {
    if (ok) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

}  // namespace

int main() {
    const QString leak =
        QString::fromUtf8("好的，我来检查一下当前的任务状态。"
                          "<tool call><function=mcpTodoListgetTodoList>"
                          "<parameter=includeCompleted>true</parameter>"
                          "</function></tool>");
    check("detects tool-call XML leak", ChatText::looksLikeToolCall(leak));
    check("detects json tool_calls",
          ChatText::looksLikeToolCall(
              QStringLiteral("{\"tool_calls\":[{\"name\":\"getTodoList\"}]}")));
    check("does not flag normal chat",
          !ChatText::looksLikeToolCall(
              QString::fromUtf8("ZCode 是你正在用的 AI 编程助手。")));

    check("strips markdown and emoji",
          ChatText::cleanReply(
              QString::fromUtf8("**改好了** 😊")) == QString::fromUtf8("改好了"));
    check("removes code fences",
          ChatText::cleanReply(QStringLiteral("```code``` 你好")) ==
              QString::fromUtf8("你好"));

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
