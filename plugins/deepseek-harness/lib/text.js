// 文案压缩：把模型的最终回复压成「桌宠念得完」的一句话。
//
// 原版（hooks/agentrmc-notify.js）从宿主 transcript 文件里捞最后一条 assistant
// 文本；DeepSeek Harness 版不落 transcript，直接从 agent/assistant-stream 的
// text-delta 里攒，所以这里只保留「压成一句」这一步，两个宿主共用同一套规则：
//   - 代码块、markdown 记号不念（念出来是噪音）；
//   - 只取第一句（整段念下来又长又吵）；
//   - 超过 80 字截断加省略号。
'use strict';

/** 把 content 数组 / 字符串里的 text 块拼成一段纯文本（reasoning 块不算正文）。 */
export function flattenText(content) {
  if (typeof content === 'string') return content;
  if (!Array.isArray(content)) return '';
  return content
    .filter(
      (block) =>
        block &&
        (block.type === undefined || block.type === 'text') &&
        typeof block.text === 'string',
    )
    .map((block) => block.text)
    .join(' ');
}

/** 压成一句：去代码块、去 markdown 记号、只留第一句、限长 80。 */
export function trimSentence(text) {
  let value = flattenText(text)
    .replace(/```[\s\S]*?```/g, ' ') // 代码块不念
    .replace(/[#*`>_~|]+/g, ' ') // markdown 记号
    .replace(/\s+/g, ' ')
    .trim();
  if (!value) return '';
  const first = value.match(/^[^。！？!?]{1,80}[。！？!?]?/);
  if (first) value = first[0].trim();
  if (value.length > 80) value = value.slice(0, 78) + '…';
  return value;
}
