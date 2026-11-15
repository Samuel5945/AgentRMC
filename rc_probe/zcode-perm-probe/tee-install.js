#!/usr/bin/env node
// 给插件缓存里的 agentrmc-notify.js 加 payload 落盘 tee（研究用一次性脚本）。
'use strict';
const fs = require('fs');

const p = 'C:/Users/PC/.zcode/cli/plugins/cache/dev-agentrmc-local/agentrmc-bridge/0.4.1/hooks/agentrmc-notify.js';
const PROBE_DIR = 'D:/githubrepos/Agentrmc/rc_probe/zcode-perm-probe';
let s = fs.readFileSync(p, 'utf8');

if (s.includes('PERM-PROBE-TEE')) {
  console.log('already teed');
  process.exit(0);
}

const anchor = "const tool = hook.tool_name || hook.toolName || '';";
if (!s.includes(anchor)) {
  console.log('anchor missing');
  process.exit(1);
}

const tee = [
  '  // PERM-PROBE-TEE（研究用临时改动）：把宿主派发的原始 payload 落盘供探针分析。',
  '  try {',
  '    fs.mkdirSync(' + JSON.stringify(PROBE_DIR) + ', { recursive: true });',
  '    fs.appendFileSync(' + JSON.stringify(PROBE_DIR + '/capture.jsonl') + ',',
  '      JSON.stringify({ t: new Date().toISOString(), via: "notify-tee",',
  '        event, keys: Object.keys(hook).sort(), hook }) + "\\n");',
  '  } catch {}',
  '',
].join('\n');

s = s.replace(anchor, tee + anchor);
fs.writeFileSync(p, s);

// 自检：改完必须能加载（退出码 0）。
const { spawnSync } = require('child_process');
const chk = spawnSync(process.execPath, [p], { input: '', timeout: 8000 });
console.log('tee added, spawn check exit =', chk.status, chk.stderr ? String(chk.stderr).slice(0, 200) : '');
