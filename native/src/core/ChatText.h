#pragma once

#include <QString>

// 对话回复落气泡 / TTS 前的文本清理。
// 模型偶尔会把工具调用协议当正文吐出来（例如
// `<tool call><function=mcpTodoListgetTodoList>...`），必须拦掉。
namespace ChatText {

bool looksLikeToolCall(const QString &text);
QString cleanReply(const QString &text);

}  // namespace ChatText
