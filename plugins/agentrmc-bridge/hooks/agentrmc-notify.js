#!/usr/bin/env node
// Agent hook → AgentRMC 桌宠通知桥。
// 宿主的 PermissionRequest（等确认）和 Stop（一轮结束）hook 调它：
//   node agentrmc-notify.js permission [Agent名] [窗口进程名]
//   node agentrmc-notify.js stop       [Agent名] [窗口进程名]
// 后两个参数省略时按环境变量认宿主（ZCode 的 hooks.json 保持原样不改）；Qoder 那份
// hooks.json 传 `Qoder` + `Qoder CN.exe`——原生端只按 raise 给的进程映像名找窗口，
// 不给就退回找 ZCode.exe。
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
const path = require('path');

const PORT = 45915;
const event = process.argv[2] || 'message';
// 宿主标识：argv 传的优先（Qoder 那份 hooks.json 显式给 Qoder + 进程名），没传
// 就按环境变量认 ZCode / WorkBuddy——两者共用 ./hooks，气泡标题和语音里别把
// 对方叫成自己。
const agent =
  process.argv[3] ||
  process.env.AGENTRMC_NOTIFY_HOST ||
  (process.env.WORKBUDDY_APP_VERSION || process.env.CLIENT_INFO_PRODUCT_NAME === 'WorkBuddy'
    ? 'WorkBuddy'
    : 'ZCode');
const raise = process.argv[4] || '';

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
  const cwd = hook.cwd || process.env.QODER_PROJECT_DIR || process.env.CLAUDE_PROJECT_DIR || '';
  const project = cwd ? path.basename(String(cwd)) : '';

  let message = '';
  if (event === 'stop') {
    // 完成走语音：<Agent> 已完成任务 X，等你回来验收。
    message = project ? `${agent} 已完成任务 ${project}，等你回来验收` : `${agent} 已完成任务，等你回来验收`;
  } else if (event === 'permission') {
    const detail = String(input.file_path || input.path || input.command || input.url || '');
    const short = detail.length > 60 ? detail.slice(0, 57) + '…' : detail;
    if (tool === 'AskUserQuestion') {
      // 方案选择对人是「选哪个」，念中文比念工具名顺。
      message = '等你选方案';
    } else {
      message = `等你确认${tool ? ' ' + tool : ''}${short ? '：' + short : ''}`;
    }
    if (project) message += `【${project}】`;
  } else {
    message = String(hook.message || '有新消息');
    if (project) message += `【${project}】`;
  }

  const payloadObj = { event, title: agent, message };
  if (raise) payloadObj.raise = raise;
  const payload = JSON.stringify(payloadObj);
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
      res.on('end', () => process.exit(0));
    },
  );
  req.on('timeout', () => req.destroy());
  req.on('error', () => process.exit(0)); // AgentRMC 没开就算了
  req.end(payload);
  // 兜底：任何情况 2.5 秒内必退，hook 阻塞会拖住会话。
  setTimeout(() => process.exit(0), 2500);
});
