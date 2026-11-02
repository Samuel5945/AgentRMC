#!/usr/bin/env node
// Agent hook → AgentRMC 桌宠通知桥。
// 宿主的 hook 调它：
//   node agentrmc-notify.js permission [Agent名] [窗口进程名]   ← 工具等授权
//   node agentrmc-notify.js question   [Agent名] [窗口进程名]   ← AskUserQuestion 等选方案
//        （Qoder 把 AskUserQuestion 走 PreToolUse 而非 PermissionRequest，所以单独
//          一个事件名，payload 里仍按 permission 发——原生只对 stop/permission 出声）
//   node agentrmc-notify.js stop       [Agent名] [窗口进程名]   ← 一轮结束
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

// —— 念出 Agent 的最终回复 ——
// Stop 事件里宿主要是给了 transcript_path，就从会话记录里取最后一条 assistant
// 文本，压成一句话交给桌宠念。拿不到（老宿主/文件读不动/没有正文）返回空，
// 调用方退回原来的「已完成任务」通用文案——这条通路坏掉不能影响提醒本身。
function flattenText(content) {
  if (typeof content === 'string') return content;
  if (!Array.isArray(content)) return '';
  return content
    .filter((c) => c && typeof c.text === 'string')
    .map((c) => c.text)
    .join(' ');
}

function trimSentence(text) {
  let t = String(text)
    .replace(/```[\s\S]*?```/g, ' ')  // 代码块不念
    .replace(/[#*`>_~|]+/g, ' ')  // markdown 记号
    .replace(/\s+/g, ' ')
    .trim();
  if (!t) return '';
  // 只念第一句：整段念下来又长又吵。
  const first = t.match(/^[^。！？!?]{1,80}[。！？!?]?/);
  if (first) t = first[0].trim();
  if (t.length > 80) t = t.slice(0, 78) + '…';
  return t;
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
    const text = trimSentence(flattenText(msg && msg.content));
    if (text) return text;
  }
  return '';
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
  if (event === 'stop') {
    // 优先念 Agent 的最终回复（transcript 最后一条 assistant），
    // 拿不到才退回通用文案——宿主不给 transcript 也照样能用。
    const said = lastAssistantText(hook);
    message = said ? `${agent}：${said}` : `${agent} 做完了，等你回来看`;
  } else if (event === 'question') {
    // 「等你选方案」：实测 Qoder 把 AskUserQuestion 当**普通工具**走 PreToolUse
    // （hook.started 只有 source=runtime 的 SDK 记录，PermissionRequest 根本没发），
    // 所以这条挂在 hooks-qoder 的 PreToolUse + matcher:"AskUserQuestion" 上。
    // matcher 万一不被宿主执行，这里再判一次 tool_name 兜底——PreToolUse 每个工具
    // 都会触发，认不出来就静默，绝不能把授权提醒刷成屏。
    if (tool !== 'AskUserQuestion') process.exit(0);
    outEvent = 'permission';
    message = '等你选方案';
    speech = `${agent} 在等你选方案`;
  } else if (event === 'permission') {
    const detail = String(input.file_path || input.path || input.command || input.url || '');
    const short = detail.length > 60 ? detail.slice(0, 57) + '…' : detail;
    if (tool === 'AskUserQuestion') {
      // 少数宿主会把方案选择当成授权请求；念中文比念工具名顺。
      message = '等你选方案';
      speech = `${agent} 在等你选方案`;
    } else {
      message = `等你确认${tool ? ' ' + tool : ''}${short ? '：' + short : ''}`;
      speech = `${agent} 在等你点确认`;
    }
  } else if (event === 'turn-start') {
    // 新一轮对话开始：原生端把同 session 的旧 review 自动归档，不出声。
    outEvent = 'turn-start';
    message = '';
  } else {
    message = String(hook.message || '有新消息');
  }

  const sessionId = String(
    hook.session_id || hook.sessionId || '',
  ).trim();
  const project = String(
    hook.cwd ||
      hook.workspace ||
      hook.project_dir ||
      hook.projectDir ||
      process.env.CLAUDE_PROJECT_DIR ||
      process.env.CODEBUDDY_PROJECT_DIR ||
      '',
  ).trim();
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
function spool(payload) {
  try {
    fs.mkdirSync(SPOOL_DIR, { recursive: true });
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
