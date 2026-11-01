#!/usr/bin/env node
// ZCode hook → AgentRMC 桌宠通知桥。
// ZCode 的 PermissionRequest（等确认）和 Stop（一轮结束）hook 调它：
//   node agentrmc-notify.js permission
//   node agentrmc-notify.js stop
// stdin 是 hook 事件 JSON（可能为空），脚本从中挑有用的字段拼文案，
// POST 到本机 AgentRMC 的通知口（127.0.0.1:45915/notify）。
// 任何一步失败都静默退出 0——通知是锦上添花，绝不能挡会话。
'use strict';

const http = require('http');
const path = require('path');

const PORT = 45915;
const event = process.argv[2] || 'message';

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
  const cwd = hook.cwd || process.env.CLAUDE_PROJECT_DIR || '';
  const project = cwd ? path.basename(String(cwd)) : '';

  let message = '';
  if (event === 'stop') {
    message = project ? `任务完成【${project}】，等你回来验收` : '任务完成，等你回来验收';
  } else if (event === 'permission') {
    const detail = String(input.file_path || input.path || input.command || input.url || '');
    const short = detail.length > 60 ? detail.slice(0, 57) + '…' : detail;
    message = `等你确认${tool ? ' ' + tool : ''}${short ? '：' + short : ''}`;
    if (project) message += `【${project}】`;
  } else {
    message = String(hook.message || '有新消息');
    if (project) message += `【${project}】`;
  }

  const payload = JSON.stringify({ event, title: 'ZCode', message });
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
