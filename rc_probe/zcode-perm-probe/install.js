#!/usr/bin/env node
// 把探针装进 ZCode 插件缓存 hooks（研究用一次性脚本）。
'use strict';
const fs = require('fs');

const CACHE = 'C:/Users/PC/.zcode/cli/plugins/cache/dev-agentrmc-local/agentrmc-bridge/0.4.1/hooks';
const p = CACHE + '/hooks.json';
const j = JSON.parse(fs.readFileSync(p, 'utf8'));
const root = '${CLAUDE_PLUGIN_ROOT}';

const already = j.hooks.PermissionRequest[0].hooks.some(
  (h) => String(h.command).includes('zcode-perm-probe'),
);
if (!already) {
  j.hooks.PermissionRequest[0].hooks.push({
    type: 'command',
    command: `"${root}/hooks/agentrmc-launch.cmd" "${root}/hooks/zcode-perm-probe.js"`,
    timeout: 15,
  });
  fs.writeFileSync(p, JSON.stringify(j, null, 2));
  console.log('probe registered');
} else {
  console.log('probe already registered');
}
console.log('entries:', j.hooks.PermissionRequest[0].hooks.length);
