#!/usr/bin/env node
// ZCode PermissionRequest 决策路真机探针（研究用，不随插件分发）。
//
// 目的：验证 ZCode 运行时的 PermissionRequest hook 是否真能替人做授权决定
// （源码 zcode.cjs 已读出契约：exit 0 + stdout JSON
//   {"hookSpecificOutput":{"hookEventName":"PermissionRequest",
//    "decision":{"behavior":"allow"|"deny",...}}}
//  → allow 直接放行不弹窗 / deny 直接拒 / 无输出回落宿主弹窗；
//  exit 2 = 硬 deny；超时 = 无决策回落）。
//
// 安全设计：探针**只对带魔法标记的 Bash 授权**出手，其余一律记录后静默直通，
// 不改变任何正常会话的授权行为：
//   命令含 PROBE-ALLOW-ME → 返回 allow（预期：不弹窗直接执行）
//   命令含 PROBE-DENY-ME → 返回 deny（预期：不弹窗，工具收到拒绝消息）
//   命令含 PROBE-SLOW-ME → 先记录再睡 25s（探针 hooks.json timeout=15s，
//                           预期：被超时杀掉 → 宿主正常弹窗）
// 每条收到的 payload 都追加进 capture.jsonl（含探针走的分支），供断言用。
//
// 安装：复制本文件到插件缓存 hooks/ 下，并在缓存 hooks.json 的
// PermissionRequest matcher 里加一条 {"type":"command","command":"node ...probe.js",
// "timeout":15}。实验完删条目删文件。
'use strict';

const fs = require('fs');
const path = require('path');

// 探针自己的数据目录（工作区内；ZCode 启动的子进程写工作区没有沙箱限制）。
const PROBE_DIR = 'D:\\githubrepos\\Agentrmc\\rc_probe\\zcode-perm-probe';
const CAPTURE = path.join(PROBE_DIR, 'capture.jsonl');

function capture(entry) {
  try {
    fs.mkdirSync(PROBE_DIR, { recursive: true });
    fs.appendFileSync(
      CAPTURE,
      JSON.stringify(Object.assign({ t: new Date().toISOString() }, entry)) + '\n',
    );
  } catch { /* 记录失败不影响主流程 */ }
}

let raw = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (c) => { raw += c; });
process.stdin.on('end', () => {
  let payload = {};
  try { payload = JSON.parse(raw || '{}') || {}; } catch { /* 空按空处理 */ }

  const tool = String(payload.tool_name || payload.toolName || '');
  const input = payload.tool_input || payload.toolInput || {};
  const command = String(input.command || '');

  let branch = 'passthrough';
  let decision = null;

  if (tool === 'Bash' && /PROBE-ALLOW-ME/.test(command)) {
    branch = 'allow';
    decision = { behavior: 'allow' };
  } else if (tool === 'Bash' && /PROBE-DENY-ME/.test(command)) {
    branch = 'deny';
    decision = { behavior: 'deny', message: '探针拒绝（真机试验：决策路 DENY）' };
  } else if (tool === 'Bash' && /PROBE-SLOW-ME/.test(command)) {
    branch = 'slow-then-timeout';
  }

  capture({
    branch,
    tool,
    keys: Object.keys(payload).sort(),
    command: command.slice(0, 120),
    toolName: payload.toolName,
    session_id: payload.session_id || payload.sessionId || '',
    request_id: payload.requestId || '',
    mode: payload.mode,
    riskLevel: payload.riskLevel,
    reason: payload.reason,
  });

  if (branch === 'slow-then-timeout') {
    // 已记录，故意拖过 hooks.json 里的 timeout（15s）：预期被运行时按 TimedOut
    // 处理 → 没有决策 → 回落宿主自己的授权弹窗。
    const start = Date.now();
    while (Date.now() - start < 25000) {
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 500);
    }
    process.exit(0);
  }

  if (decision) {
    // 决策必须走 stdout JSON，且不能有其它输出（解析失败会被当阻塞错误）。
    process.stdout.write(JSON.stringify({
      hookSpecificOutput: {
        hookEventName: 'PermissionRequest',
        decision,
      },
    }));
  }
  process.exit(0);
});
