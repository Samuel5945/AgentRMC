'use strict';
// MCP server 回归测试：零依赖，`node test/mcp.test.js` 直接跑。
// 起一台假 AgentBridge（真 HTTP server，可切 204/400），把真 mcp/server.js
// 当子进程跑，stdin 喂 JSON-RPC、stdout 逐行断言响应。
// 重点：4xx 必须报 isError（原先 pet_notify/pet_speak 收到 400 也说「已发送」）。

const assert = require('assert');
const fs = require('fs');
const http = require('http');
const path = require('path');
const { spawn } = require('child_process');

const PLUGIN_ROOT = path.resolve(__dirname, '..');
const SERVER = path.join(PLUGIN_ROOT, 'mcp', 'server.js');
const VERSION = fs.readFileSync(path.join(PLUGIN_ROOT, 'VERSION'), 'utf8').trim();

let passed = 0;
const failures = [];
async function check(name, fn) {
  try {
    await fn();
    passed += 1;
    console.log('PASS ' + name);
  } catch (e) {
    failures.push(name + ': ' + e.message);
    console.log('FAIL ' + name + ' :: ' + e.message);
  }
}

(async () => {
  // —— 假 AgentBridge：mode=ok 回 204，mode=bad 回 400 ——
  let mode = 'ok';
  const received = [];
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', () => {
      received.push({ url: req.url, body });
      if (req.url === '/state') {
        res.setHeader('Content-Type', 'application/json');
        res.end('{"pet_visible":true}');
        return;
      }
      if (mode === 'bad') {
        res.statusCode = 400;
        res.end('{"ok":false,"error":"测试拒绝"}');
        return;
      }
      res.statusCode = 204;
      res.end();
    });
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;

  // 宿主家族变量清掉：resolveHost 在任何宿主会话里跑结果一致（落到 ZCode 兜底，
  // 本测试不断言 title，只断言状态码语义）。
  const env = Object.assign({}, process.env, { AGENTRMC_BRIDGE_PORT: String(port) });
  for (const key of Object.keys(env)) {
    if (/^(QODER_|QODERCN_|CLAUDE_|WORKBUDDY|CODEBUDDY)/.test(key)) delete env[key];
  }
  const child = spawn(process.execPath, [SERVER], { env, stdio: ['pipe', 'pipe', 'pipe'] });
  child.stderr.resume(); // ready 诊断行，不阻塞

  let buf = '';
  const waiters = new Map();
  child.stdout.on('data', (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (!line) continue;
      let msg;
      try { msg = JSON.parse(line); } catch { continue; }
      const w = waiters.get(msg.id);
      if (w) {
        waiters.delete(msg.id);
        clearTimeout(w.timer);
        w.resolve(msg);
      }
    }
  });

  let seq = 0;
  function rpc(method, params) {
    const id = ++seq;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        waiters.delete(id);
        reject(new Error(method + ' 响应超时'));
      }, 6000);
      waiters.set(id, { resolve, timer });
      child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    });
  }
  const callTool = (name, args) => rpc('tools/call', { name, arguments: args });
  const textOf = (resp) => resp.result.content[0].text;

  await check('initialize: serverInfo 版本跟随 VERSION 文件', async () => {
    const resp = await rpc('initialize', { protocolVersion: '2024-11-05' });
    assert.strictEqual(resp.result.serverInfo.name, 'agentrmc-mcp');
    assert.strictEqual(resp.result.serverInfo.version, VERSION);
  });

  await check('tools/list: 四个 pet_* 工具', async () => {
    const resp = await rpc('tools/list', {});
    const names = resp.result.tools.map((t) => t.name).sort();
    assert.deepStrictEqual(names, ['pet_notify', 'pet_speak', 'pet_state', 'pet_work_update']);
  });

  await check('pet_notify 成功: 204 → 非 isError，payload 带 plugin_version', async () => {
    mode = 'ok';
    const resp = await callTool('pet_notify', { message: '阶段进度' });
    assert.ok(!resp.result.isError, '成功不该 isError');
    assert.match(textOf(resp), /204/);
    const sent = received[received.length - 1];
    assert.strictEqual(sent.url, '/notify');
    const body = JSON.parse(sent.body);
    assert.strictEqual(body.event, 'progress');
    assert.strictEqual(body.message, '阶段进度');
    assert.strictEqual(body.plugin_version, VERSION);
  });

  await check('pet_notify 被拒: 400 → isError 且带状态码（不再谎报已发送）', async () => {
    mode = 'bad';
    const resp = await callTool('pet_notify', { message: '会被拒的提示' });
    assert.strictEqual(resp.result.isError, true);
    assert.match(textOf(resp), /400/);
    assert.doesNotMatch(textOf(resp), /已发送/);
  });

  await check('pet_speak 被拒: 400 → isError；成功: 204 → 非 isError', async () => {
    mode = 'bad';
    const bad = await callTool('pet_speak', { message: '会被拒的话' });
    assert.strictEqual(bad.result.isError, true);
    assert.match(textOf(bad), /400/);
    mode = 'ok';
    const ok = await callTool('pet_speak', { message: '正常的话' });
    assert.ok(!ok.result.isError);
    assert.match(textOf(ok), /204/);
    assert.strictEqual(received[received.length - 1].url, '/speak');
  });

  await check('pet_state: 原样返回 /state 的 body', async () => {
    const resp = await callTool('pet_state', {});
    assert.strictEqual(textOf(resp), '{"pet_visible":true}');
  });

  await check('参数缺失与未知工具: 拒绝且不落到 HTTP', async () => {
    const before = received.length;
    const empty = await callTool('pet_notify', {});
    assert.strictEqual(empty.result.isError, true);
    assert.match(textOf(empty), /message 不能为空/);
    const unknown = await callTool('pet_nonexistent', {});
    assert.strictEqual(unknown.error.code, -32602);
    assert.strictEqual(received.length, before, '非法调用不该发 HTTP');
  });

  // —— 收尾 ——
  child.kill();
  await new Promise((r) => child.on('exit', r));
  await new Promise((r) => server.close(r));

  const total = passed + failures.length;
  if (failures.length) {
    console.log('\n' + failures.length + ' FAILED of ' + total);
    failures.forEach((item) => console.log('  ' + item));
    process.exit(1);
  }
  console.log('\nALL PASS (' + total + '/' + total + ')');
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
