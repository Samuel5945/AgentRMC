#!/usr/bin/env node
// Agent hook → AgentRMC 桌宠通知桥。
// 宿主的 hook 调它：
//   node agentrmc-notify.js permission [Agent名] [窗口进程名]   ← 工具等授权
//   node agentrmc-notify.js question   [Agent名] [窗口进程名]   ← AskUserQuestion 等选方案
//        （Qoder 把 AskUserQuestion 走 PreToolUse 而非 PermissionRequest，所以单独
//          一个事件名，payload 里仍按 permission 发——原生只对 stop/permission 出声；
//          ZCode 同理：PermissionRequest 压根不为它发，主路也是 PreToolUse 匹配）
//   node agentrmc-notify.js prompt     [Agent名] [窗口进程名]   ← 弹窗刚出现（准点）
//        （WorkBuddy 的 Notification + matcher:"permission_prompt"：这是唯一在
//          「题目弹出那一刻」就派发的信号；同一件事它的 PreToolUse 要等用户答完
//          才跑，只能当兜底，靠 asked-question.json 记一笔避免重复喊）
//   node agentrmc-notify.js stop       [Agent名] [窗口进程名]   ← 一轮结束
//        （transcript 末句是问句 → 这轮其实没完，改按 permission 发
//          「等你回话」：出声 + 拉前台 + 待我处理卡片；见 trailingQuestion）
//   node agentrmc-notify.js error      [Agent名] [窗口进程名]   ← 一轮以错误中止
//        （挂 Qoder / Qoder CN 的 StopFailure 事件上：模型 429 限流 / 5xx / 上下文满 /
//          登录失效时宿主**不派 Stop**，桌宠过去全程静默——人只能自己盯着屏幕发现。
//          payload 给 error（分类名，见 ERROR_REASONS）+ error_details（原文）。
//          只注册在 hooks-qoder：WorkBuddy 那家的 harness 里 StopFailure 是「Stop 钩子
//          自己执行失败」，语义不同，套上去会把无关故障喊成模型出错。ZCode 的运行时里
//          查不到这个事件，没证据就不注册。）
// 后两个参数**现在一般不用传**：宿主名和要拉前台的进程映像名由 lib/host.js 自检
// （脚本装在哪个配置家就是哪个宿主，Qoder 国际版 → Qoder.exe，Qoder CN →
// Qoder CN.exe，不传则按环境变量认 WorkBuddy / ZCode）。显式传了以传的为准，
// 老的手写配置行为不变。原生端只按 raise 给的进程映像名找窗口，不给就退回找
// ZCode.exe。
// AGENTRMC_DRY_RUN=1 只打印将要发送的 payload 不真发，供测试矩阵断言用。
// stdin 是 hook 事件 JSON（可能为空），脚本从中挑有用的字段拼文案，
// 一律 POST /notify，事件类型原样交给原生端决定怎么提醒：
//   permission → 气泡 + TTS 念出来 + 把 Agent 主窗口拉到前台；
//   stop       → 念「<Agent> 已完成…」+ 拉前台（任务一般很长，完成值得出声）。
// （拉前台曾在更早的版本里用 detached PowerShell 做，运行环境会把 detached
// 子进程连带清掉，从来没生效过；改由常驻的原生端自己做，可靠且省一次
// PowerShell 冷启动。）
// 任何一步失败都静默退出 0——通知是锦上添花，绝不能挡会话。
'use strict';

const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { resolveHost } = require('../lib/host');
const readVersion = require('../lib/version');

const PORT = Number(process.env.AGENTRMC_PORT || 45915);
// AgentRMC 没开时不能把提醒丢掉：先落盘，原生端启动/定时扫描时补投。
const SPOOL_DIR = process.env.AGENTRMC_SPOOL_DIR ||
  path.join(process.env.APPDATA || os.tmpdir(), 'LIG', 'AgentRMCNative');
const SPOOL_FILE = path.join(SPOOL_DIR, 'pending-events.jsonl');
const DRY_RUN = process.env.AGENTRMC_DRY_RUN === '1';
const event = process.argv[2] || 'message';
// 已加载的插件版本，随 payload 上报：装完不重启宿主还是旧代码，
// 原生端要靠这个数把「盘上已升级」和「进程还在跑旧版」分开。
const PLUGIN_VERSION = readVersion();
const host = resolveHost({
  scriptDir: __dirname,
  argv: process.argv.slice(2),
  env: process.env,
});
const agent = host.agent;
// 拉前台的进程映像名：Qoder 两个变体靠自检区分，别写死成其中一个。
const raise = host.raise;

// —— 念出 Agent 的最终回复 / 判它是不是在等人回话 ——
// Stop 事件里宿主要是给了 transcript_path，就从会话记录里取最后一条 assistant
// 的**原文**（不压句），交给 stop 分支做两件事：压成一句念出来、判收尾是不是问句。
// 拿不到（老宿主/文件读不动/没有正文）返回空，
// 调用方退回原来的「已完成任务」通用文案——这条通路坏掉不能影响提醒本身。
function flattenText(content) {
  if (typeof content === 'string') return content;
  if (!Array.isArray(content)) return '';
  return content
    .filter((c) => c && typeof c.text === 'string')
    .map((c) => c.text)
    .join(' ');
}

function cleanText(content) {
  return flattenText(content)
    .replace(/```[\s\S]*?```/g, ' ')  // 代码块不念
    .replace(/[#*`>_~|]+/g, ' ')  // markdown 记号
    .replace(/\s+/g, ' ')
    .trim();
}

function trimSentence(text) {
  let t = cleanText(text);
  if (!t) return '';
  // 只念第一句：整段念下来又长又吵。
  const first = t.match(/^[^。！？!?]{1,80}[。！？!?]?/);
  if (first) t = first[0].trim();
  if (t.length > 80) t = t.slice(0, 78) + '…';
  return t;
}

// 问号后面允许跟的东西：空白、markdown 强调、引号括号、破折号、省略号。
// 只要还写了别的字（「…吗？我先按 A 做了」），就不算「以问句收尾」。
const TRAILING_NOISE = /^[\s*_`"'’”」』)\]】…—-]*$/;

// —— 这一轮到底是「说完了」还是「在等我回话」——
// 正文提问没有选项卡片，宿主不会为它派发任何等待事件：Qoder CN 实测（2026-11-04）
// 这种一轮只出一条 stop 工单，桌宠念的是回复的第一句、写成「这轮说完了」，
// 球明明在人这边却喊不到人。能拿到的信号只有 transcript 的收尾文字，就按它判：
// 末句以问号收尾 = 在等人回话。判宽了会把客套问句当成待答，念错比不念更烦，
// 所以只认「最后一个问号后面再无正文」这一种。
function trailingQuestion(text) {
  const clean = cleanText(text);
  if (!clean) return '';
  const at = Math.max(clean.lastIndexOf('？'), clean.lastIndexOf('?'));
  if (at < 0) return '';
  if (!TRAILING_NOISE.test(clean.slice(at + 1))) return '';
  const body = clean.slice(0, at);
  // 取问号所在的最后一句：从最近的句末标点之后开始。英文句点要**后面跟空白**
  // 才算句末，否则「0.4.0 出包吗？」会被小数点拦腰截断。
  const term = /[。；！!？?]|\.(?=\s)/g;
  let cut = 0;  // 命中句末标点就推进到它后面；一个都没命中 = 整段就是这一问
  let m;
  while ((m = term.exec(body)) !== null) cut = m.index + m[0].length;
  const q = body.slice(cut).trim() + clean[at];
  if (q.length < 3) return '';
  return q.length > 48 ? q.slice(0, 45) + '…' : q;
}

function lastAssistantText(hook) {
  const file = hook.transcript_path || hook.transcriptPath || '';
  if (!file || typeof file !== 'string') return '';
  let raw = '';
  try {
    // 会话记录可能很大：只读尾部 512KB，够覆盖最后几轮。
    const cap = 512 * 1024;
    const size = fs.statSync(file).size;
    const start = size > cap ? size - cap : 0;
    const fd = fs.openSync(file, 'r');
    const buf = Buffer.alloc(size - start);
    fs.readSync(fd, buf, 0, buf.length, start);
    fs.closeSync(fd);
    raw = buf.toString('utf8');
  } catch {
    return '';
  }
  const lines = raw.split(/\r?\n/).filter(Boolean);
  for (let i = lines.length - 1; i >= 0; i--) {
    let obj;
    try {
      obj = JSON.parse(lines[i]);
    } catch {
      continue;  // 尾部截断可能从半行开始，跳过就好
    }
    if (!obj) continue;
    const msg = obj.message || obj;
    const role = (msg && msg.role) || obj.type || '';
    if (role !== 'assistant') continue;
    const text = flattenText(msg && msg.content);
    if (cleanText(text)) return text;
  }
  return '';
}

// —— 「等你选方案」只喊一次 ——
// 同一个弹窗会被两个 hook 看到：Notification(permission_prompt) 在弹出时（准），
// PreToolUse 在用户答完后（晚）。准的那条先喊，晚的那条不该再喊一遍——用户等了
// 5 分钟才答，原生端 180 秒的迟到去重兜不住，只能在这里记一笔。
//
// 键分三种（2026-11-04 加固）：
//   tool:<tool_use_id>  一次弹窗的天然唯一 ID。question 分支的主去重键——
//                       同一弹窗无论被派发几次、session 给得一不一致都认它。
//   pending:<scope>     只有 prompt（准点信号）记；question 晚到时**消费掉**它。
//                       一次性：答完后的下一道新题不会被上一题的账吞掉。
//   solo:<scope>        老宿主不给 tool_use_id 时的退路，按 scope 去重。
// 曾经的教训：question 分支按 session 记 30 分钟——Qoder 里同一会话连问两道题，
// 第二道被第一道的账吞掉（实测 55f51dbc 会话 7 问只喊了 2 声）。scope 键只能
// 用于「同一弹窗的两条信号」，不能用于「同一会话的多次提问」。
const ASKED_FILE = path.join(SPOOL_DIR, 'asked-question.json');
const ASKED_TTL_MS = 30 * 60 * 1000;

// 项目目录：两条 hook 可能给得不一样（大小写、斜杠方向、尾斜杠），归一再比。
// 报给原生端的 project 仍用原样，只有去重键走归一化后的形式。
function projectRaw(hook) {
  return String(
    hook.cwd ||
      hook.workspace ||
      hook.project_dir ||
      hook.projectDir ||
      process.env.CLAUDE_PROJECT_DIR ||
      process.env.CODEBUDDY_PROJECT_DIR ||
      '',
  ).trim();
}

function projectOf(hook) {
  return projectRaw(hook)
    .replace(/\\/g, '/')
    .replace(/\/{2,}/g, '/')  // 宿主把路径写成 D:\\a\\b 时别算成两个项目
    .replace(/\/+$/, '')
    .toLowerCase();
}

function toolUseId(hook) {
  return String(hook.tool_use_id || hook.toolUseId || '').trim();
}

function askedScopeKey(hook) {
  const sid = String(hook.session_id || hook.sessionId || '').trim();
  if (sid) return sid;
  // 没有 session（老宿主）：按「宿主身份 + 项目目录」认，宁可多喊一次也别漏。
  // 这里**不能用 process.ppid**：hook 的父进程是宿主每次新起的 bash.exe，
  // pid 天生唯一，两条信号永远对不上，去重形同虚设（2026-11-03 实测复现）。
  // 宿主自己的 pid 也拿不到（中间隔了一层 bash），所以用稳定的宿主标识代替。
  return `host:${(raise || agent || 'agent').toLowerCase()}|${projectOf(hook)}`;
}

function loadAsked() {
  try {
    return JSON.parse(fs.readFileSync(ASKED_FILE, 'utf8')) || {};
  } catch {
    return {};
  }
}

function saveAsked(all) {
  const now = Date.now();
  for (const k of Object.keys(all)) {
    if (!(now - Number(all[k]) < ASKED_TTL_MS)) delete all[k];
  }
  // 原子替换：先写 pid 后缀的临时文件再 rename。两条 hook 并发时读者
  // 不会看到半截 JSON（直接 writeFileSync 写到一半被读走，解析失败按
  // 「没问过」处理，会重复喊）。并发写最坏后写覆盖先写，多喊一声而已。
  const tmp = `${ASKED_FILE}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(all));
  fs.renameSync(tmp, ASKED_FILE);
}

function rememberKeys(keys) {
  try {
    fs.mkdirSync(SPOOL_DIR, { recursive: true });
    const all = loadAsked();
    const now = Date.now();
    for (const k of keys) all[k] = now;
    saveAsked(all);
  } catch {
    // 记不住就记不住：最多多喊一声，不能挡会话。
  }
}

function fresh(all, key) {
  const at = Number(all[key] || 0);
  return at > 0 && Date.now() - at < ASKED_TTL_MS;
}

function seenKey(key) {
  return fresh(loadAsked(), key);
}

// 同 seenKey，但自己带窗口：ASKED_TTL_MS（30 分钟）是给「同一弹窗两条信号」定的，
// 出错提醒要的是「这一次失败只喊一声，连错两次要能再喊」，只能自己掐表。
function seenWithin(key, windowMs) {
  const at = Number(loadAsked()[key] || 0);
  return at > 0 && Date.now() - at < windowMs;
}

// 命中即作废：pending 是「同一弹窗的晚到信号」专用，消费后下一道新题不受影响。
function consumeKey(key) {
  try {
    const all = loadAsked();
    if (!fresh(all, key)) return false;
    delete all[key];
    saveAsked(all);
    return true;
  } catch {
    return false;
  }
}

// 宿主 StopFailure 的 error 分类 → 中文（气泡短语 / 念出来的短语）。
// 念出来不能用宿主给的英文原文（「the model provider is temporarily unavailable」
// 念成一串英文没人听得懂），所以气泡可以带原文，语音只用这张表。
// 分类名来自 Qoder CLI 的 classifyStopFailureError，认不出来一律按「出错了」喊。
const ERROR_REASONS = {
  rate_limit: ['被限流了', '被限流了'],
  overloaded: ['服务繁忙', '服务繁忙了'],
  server_error: ['服务报错', '服务出错了'],
  model_not_found: ['模型不可用', '模型不可用了'],
  authentication_failed: ['登录态失效', '登录态失效了'],
  oauth_org_not_allowed: ['登录态失效', '登录态失效了'],
  billing_error: ['额度用完了', '额度用完了'],
  invalid_request: ['请求被拒', '请求被模型拒了'],
  empty_response: ['没返回内容', '没返回内容'],
  max_output_tokens: ['输出超限', '输出太长被截断了'],
  unknown: ['出错了', '出错了'],
};

// 一次失败只喊一声。窗口要盖住「同一次失败的两条信号」：StopFailure 之后，
// 有的宿主还会把错误原文当成最终回复再发一条 stop（WorkBuddy 实测念出过
// 「429 您的使用量已超出频率限制…」），两条都念就成了自相矛盾。
const ERROR_WINDOW_MS = 2 * 60 * 1000;

// 人自己按停止不算「模型出错」：宿主在「中止且没吐出正文」这条路上也派
// StopFailure（分类落 unknown，原文带 AbortError / Interrupted by user），
// 那种喊出来是凭空一声。
const USER_ABORT_RE =
  /aborterror|operation aborted|interrupted by user|request interrupted|user cancelled|response interrupted/i;

// 把宿主的失败信号拼成「气泡文案 + 念出来的短句」。认不出来就报泛化文案，
// 不能因为分类表没覆盖就静默——出错不喊人正是要修的事。
function describeModelFailure(hook, agentName) {
  const kind = String(hook.error || '').trim().toLowerCase();
  const details = String(hook.error_details || hook.errorDetails || '').trim();
  // 两个都没有 = 这条信号没告诉我们为什么停：宁可不喊，也别在人自己按了停止的
  // 那一轮凭空喊一声「模型出错了」。
  if (!kind && !details) return null;
  if (USER_ABORT_RE.test(kind) || USER_ABORT_RE.test(details)) return null;
  const zh = ERROR_REASONS[kind] || ERROR_REASONS.unknown;
  // 气泡带一句原文好判断（stack 只取第一行，一屏放不下），语音永远只念中文短语：
  // 宿主给的英文原文念出来是「the model provider is temporarily unavailable」这种。
  const line = (details.split(/\r?\n/).find((s) => s && s.trim()) || '').trim();
  const short = line.length > 60 ? line.slice(0, 57) + '…' : line;
  const known = kind && kind !== 'unknown';
  return {
    bubble: known
      ? (short ? `模型出错（${zh[0]}）：${short}` : `模型出错：${zh[0]}`)
      : (short ? `模型出错：${short}` : '模型出错了'),
    speech: known ? `${agentName} 模型出错了，${zh[1]}` : `${agentName} 模型出错了`,
  };
}

let raw = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => {
  raw += chunk;
});
process.stdin.on('end', () => {
  let hook = {};
  try {
    hook = JSON.parse(raw || '{}') || {};
  } catch {
    // stdin 不是 JSON 就按空事件处理
  }

  const tool = hook.tool_name || hook.toolName || '';
  const input = hook.tool_input || hook.toolInput || {};

  let message = '';
  // speech 只给语音用：念出来要短、要带宿主名。工具名和文件路径留在气泡里看，
  // 念出来是噪音（「等你确认 Bash 冒号 D 冒号 反斜杠…」没人听得懂）。
  // 不给 speech 时原生端退回念 message。
  let speech = '';
  // 发给原生端的事件类型：原生端只对 stop / permission 出语音并把窗口拉回前台
  // （MainWindow.cpp 的 notify 分支），其它 kind 只冒气泡。所以 question 复用
  // permission 通道，别在原生端加新类型。
  let outEvent = event;
  if (event === 'prompt') {
    // 「弹窗出现的那一刻」——WorkBuddy 的 Notification(permission_prompt) hook。
    // 这是唯一准时的信号：同一件事它的 PreToolUse 要等用户答完才派发
    // （实测问题 11:24:38 弹出、11:30:19 才 spawn question hook）。
    // 只管 AskUserQuestion：普通授权已经由 PermissionRequest hook 在弹窗前报过，
    // 这里再报就重复了。
    const type = String(hook.notification_type || hook.notificationType || '');
    if (type && type !== 'permission_prompt') process.exit(0);
    const note = String(hook.message || '');
    if (!/AskUserQuestion/i.test(note)) process.exit(0);
    outEvent = 'permission';
    message = '等你选方案';
    speech = `${agent} 在等你选方案`;
    // 准点信号先喊，记一笔「这个 scope 有弹窗喊过了」，等答完后的 question
    // 晚到信号来消费（一次性）。不记 tool 键：Notification 未必带 tool_use_id，
    // 记了反而可能残留到下一道题。
    rememberKeys([`prompt:${askedScopeKey(hook)}`]);
  } else if (event === 'stop') {
    // 刚刚为这一次失败喊过（同 scope 的 error 账还没被新一轮清掉）：别再念
    // 「做完了」。有的宿主会把错误原文当成最终回复再发一条 stop，两条都念就
    // 成了自相矛盾——一声「模型出错了」、一声「XX 做完了，等你回来看」。
    if (seenWithin(`error:${askedScopeKey(hook)}`, ERROR_WINDOW_MS)) process.exit(0);
    // 优先念 Agent 的最终回复（transcript 最后一条 assistant），
    // 拿不到才退回通用文案——宿主不给 transcript 也照样能用。
    const said = lastAssistantText(hook);
    const ask = trailingQuestion(said);
    if (ask) {
      // 用问句收尾的一轮没结束：球在人这边，走「要人动手」那条通道
      // （permission = 出声 + 拉前台 + 待我处理卡片），别报成「这轮说完了」。
      // 复用 permission 而不是 question：原生端对 question 语义的文案套 180s
      // 迟到去重（那是给「同一弹窗两条信号」准备的），正文提问套上去会被吞。
      // 文案也不能写「等你选方案」——原生按这个前缀判是不是选方案，这里没有选项卡片。
      outEvent = 'permission';
      message = `等你回话：${ask}`;
      speech = `${agent} 在等你回话`;
    } else {
      const one = trimSentence(said);
      message = one ? `${agent}：${one}` : `${agent} 做完了，等你回来看`;
    }
  } else if (event === 'question') {
    // 「等你选方案」：实测 Qoder 把 AskUserQuestion 当**普通工具**走 PreToolUse
    // （hook.started 只有 source=runtime 的 SDK 记录，PermissionRequest 根本没发），
    // 所以这条挂在 hooks-qoder 的 PreToolUse + matcher:"AskUserQuestion" 上。
    // matcher 万一不被宿主执行，这里再判一次 tool_name 兜底——PreToolUse 每个工具
    // 都会触发，认不出来就静默，绝不能把授权提醒刷成屏。
    if (tool !== 'AskUserQuestion') process.exit(0);
    // 去重认「弹窗」不认「会话」（2026-11-04 加固）：
    // 1) 同一 tool_use_id 喊过了 → 同一弹窗的重复派发，吞掉（session 给得
    //    一不一致都认它）；
    // 2) prompt 准点信号喊过了 → 我是答完后的晚到回声，消费掉那笔账，吞掉；
    // 3) 老宿主不给 tool_use_id → 退回 scope 键（维持旧行为）。
    // 绝不能按 session 记 30 分钟：同一会话连问多题时，第二题起全被吞
    // （实测 55f51dbc 会话 7 问只喊了 2 声，中间 5 题静默卡住）。
    const tid = toolUseId(hook);
    const scope = askedScopeKey(hook);
    if (tid && seenKey(`tool:${tid}`)) process.exit(0);
    if (consumeKey(`prompt:${scope}`)) process.exit(0);
    if (!tid && seenKey(`solo:${scope}`)) process.exit(0);
    outEvent = 'permission';
    message = '等你选方案';
    speech = `${agent} 在等你选方案`;
    rememberKeys([tid ? `tool:${tid}` : `solo:${scope}`]);
  } else if (event === 'permission') {
    const detail = String(input.file_path || input.path || input.command || input.url || '');
    const short = detail.length > 60 ? detail.slice(0, 57) + '…' : detail;
    if (tool === 'AskUserQuestion') {
      // 少数宿主会把方案选择当成授权请求；念中文比念工具名顺。
      // ZCode 起主路是 PreToolUse(AskUserQuestion)→question（弹窗那一刻就发），
      // PermissionRequest 若也跟一条，后到的按同一弹窗去重吞掉——与 question
      // 分支共用 tool:/solo: 账本，两边谁先到谁喊，另一条静默。
      const tid = toolUseId(hook);
      const scope = askedScopeKey(hook);
      if (tid && seenKey(`tool:${tid}`)) process.exit(0);
      if (!tid && seenKey(`solo:${scope}`)) process.exit(0);
      message = '等你选方案';
      speech = `${agent} 在等你选方案`;
      rememberKeys([tid ? `tool:${tid}` : `solo:${scope}`]);
    } else {
      message = `等你确认${tool ? ' ' + tool : ''}${short ? '：' + short : ''}`;
      speech = `${agent} 在等你点确认`;
    }
  } else if (event === 'error') {
    const said = describeModelFailure(hook, agent);
    if (!said) process.exit(0);  // 认不出错因、或人自己按的停止：不喊
    const key = `error:${askedScopeKey(hook)}`;
    if (seenWithin(key, ERROR_WINDOW_MS)) process.exit(0);
    // 出错这一轮球也在人这边（要他重试/换模型），所以同样走 permission 通道：
    // 出声 + 拉前台 + 待我处理卡片。复用 permission 而不是新增 kind：旧版原生端
    // 只认 stop/permission 出语音，加新类型得让用户重装 App 才生效。
    outEvent = 'permission';
    message = said.bubble;
    speech = said.speech;
    rememberKeys([key]);
  } else if (event === 'turn-start') {
    // 新一轮对话开始：原生端把同 session 的旧 review 自动归档，不出声。
    // 顺手清掉上一次的「出错」账：他改口重问、那一轮正常结束后该念「做完了」，
    // 不能被上一轮出错的去重窗口吞掉。
    consumeKey(`error:${askedScopeKey(hook)}`);
    outEvent = 'turn-start';
    message = '';
  } else {
    message = String(hook.message || '有新消息');
  }

  const sessionId = String(
    hook.session_id || hook.sessionId || '',
  ).trim();
  const project = projectRaw(hook);
  const eventId = String(hook.tool_use_id || hook.toolUseId || '').trim();

  const payloadObj = { event: outEvent, title: agent, message };
  if (speech) payloadObj.speech = speech;
  if (raise) payloadObj.raise = raise;
  if (sessionId) payloadObj.session_id = sessionId;
  if (project) payloadObj.project = project;
  if (eventId) payloadObj.event_id = eventId;
  if (PLUGIN_VERSION) payloadObj.plugin_version = PLUGIN_VERSION;
  const payload = JSON.stringify(payloadObj);
  if (DRY_RUN) {
    // 测试用：把判据一起吐出来，好断言「哪个宿主 → 拉哪个窗」。
    console.log(JSON.stringify({ dry_run: true, source: host.source, payload: payloadObj }));
    process.exit(0);
  }
// 上限守卫：原生端一直不开时别把文件养到几百 MB。超 1MB 丢最旧留最新
// （按行裁，保 JSONL 完整）。hook 可能并发跑，读-改-写没加锁：最坏丢几条
// 旧通知，提醒场景可接受；裁不动就照常追加，不能挡通知。
const SPOOL_CAP_BYTES = 1024 * 1024;
function trimSpool() {
  try {
    if (fs.statSync(SPOOL_FILE).size <= SPOOL_CAP_BYTES) return;
    const lines = fs.readFileSync(SPOOL_FILE, 'utf8').split('\n').filter(Boolean);
    fs.writeFileSync(SPOOL_FILE, lines.slice(-500).join('\n') + '\n');
  } catch { /* 裁不动就照常追加 */ }
}

function spool(payload) {
  try {
    fs.mkdirSync(SPOOL_DIR, { recursive: true });
    trimSpool();
    fs.appendFileSync(SPOOL_FILE, payload + '\n');
  } catch {
    // 落盘也失败就彻底放弃：通知不能挡会话。
  }
}

const req = http.request(
    {
      host: '127.0.0.1',
      port: PORT,
      path: '/notify',
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) },
      timeout: 1500,
    },
    (res) => {
      res.resume();
      res.on('end', () => {
        if (res.statusCode >= 500) spool(payload);
        process.exit(0);
      });
    },
  );
  req.on('timeout', () => {
    spool(payload);
    req.destroy();
  });
  req.on('error', () => {
    spool(payload);
    process.exit(0);
  }); // AgentRMC 没开：落到 spool，等下次补投
  req.end(payload);
  // 兜底：任何情况 2.5 秒内必退，hook 阻塞会拖住会话。
  setTimeout(() => process.exit(0), 2500);
});
