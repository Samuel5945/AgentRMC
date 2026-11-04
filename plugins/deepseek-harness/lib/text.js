// 文案压缩：把模型的最终回复压成「桌宠念得完」的一句话。
//
// 原版（hooks/agentrmc-notify.js）从宿主 transcript 文件里捞最后一条 assistant
// 文本；DeepSeek Harness 版不落 transcript，直接从 agent/assistant-stream 的
// text-delta 里攒，所以这里只保留「压成一句」这一步，两个宿主共用同一套规则：
//   - 代码块、markdown 记号不念（念出来是噪音）；
//   - 只取第一句（整段念下来又长又吵）；
//   - 超过 80 字截断加省略号。
// 另外提供 `trailingQuestion`（判「回复是不是用问句收尾」），与 bridge 版
// hooks/agentrmc-notify.js 里的同名判据保持一致——两条路都要有「等你回话」。
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
  let value = cleanText(text);
  if (!value) return '';
  const first = value.match(/^[^。！？!?]{1,80}[。！？!?]?/);
  if (first) value = first[0].trim();
  if (value.length > 80) value = value.slice(0, 78) + '…';
  return value;
}

/** 净化成一行可念的文本：去代码块、去 markdown 记号、压空白。 */
export function cleanText(text) {
  return flattenText(text)
    .replace(/```[\s\S]*?```/g, ' ') // 代码块不念
    .replace(/[#*`>_~|]+/g, ' ') // markdown 记号
    .replace(/\s+/g, ' ')
    .trim();
}

// 问号后面允许跟的东西：空白、markdown 强调、引号括号、破折号、省略号。
// 只要还写了别的字（「…吗？我先按 A 做了」），就不算「以问句收尾」。
const TRAILING_NOISE = /^[\s*_`"'’”」』)\]】…—-]*$/;

/**
 * 这一轮是「说完了」还是「在等我回话」：回复最后一个问号后面再无正文 → 返回那句问话
 * （≤48 字），否则空串。与 bridge 版 `trailingQuestion` 同一套判据，两边行为要一致。
 * 只认「以问句收尾」这一种：判宽了会把客套问句当成待答，念错比不念更烦。
 */
export function trailingQuestion(text) {
  const clean = cleanText(text);
  if (!clean) return '';
  const at = Math.max(clean.lastIndexOf('？'), clean.lastIndexOf('?'));
  if (at < 0) return '';
  if (!TRAILING_NOISE.test(clean.slice(at + 1))) return '';
  const body = clean.slice(0, at);
  // 取问号所在的最后一句：从最近的句末标点之后开始。英文句点要**后面跟空白**
  // 才算句末，否则「0.4.0 出包吗？」会被小数点拦腰截断。
  const term = /[。；！!？?]|\.(?=\s)/g;
  let cut = 0; // 命中句末标点就推进到它后面；一个都没命中 = 整段就是这一问
  let match;
  while ((match = term.exec(body)) !== null) cut = match.index + match[0].length;
  const question = body.slice(cut).trim() + clean[at];
  if (question.length < 3) return '';
  return question.length > 48 ? question.slice(0, 45) + '…' : question;
}
