// OpenSquilla 桥端到端回归：零依赖，`node test/bridge.test.js` 直接跑（Node 22+）。
//
// 手摇一个最小 WebSocket 假网关（http upgrade + 帧编解码，协议与真网关一致：
// connect.challenge → connect → hello-ok → sessions.list），把真桥当子进程跑，
// 另一头是假 AgentBridge（真 HTTP server）或死端口（必然连不上 → 走落盘）。
//
// 覆盖：
//   A) 桥活着：done→stop 带 session_id；15s 内重复 done 去重；plan→question；
//      免打扰时 done 被吞但 turn-start 照发；turn-start 不吃 15s 去重。
//   B) 桥死了：done/turn-start 落盘 pending-events.jsonl，与 hooks 同格式；
//      15s 内两条 turn-start 都要落盘（幂等归档信号不去重）。
//   C) spool 上限守卫：超 1MB 丢最旧留最新。
//   D) --selftest：通 exit 0，不通 exit 1，且自检不落盘（探针不该污染补投队列）。

import assert from 'node:assert';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SCRIPT = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)), '../opensquilla-pet-bridge.mjs');
const WS_MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'agentrmc-osq-'));

let passed = 0;
const failures = [];
async function check(label, fn) {
  try {
    await fn();
    passed += 1;
    console.log('PASS ' + label);
  } catch (error) {
    failures.push(label + ': ' + error.message);
    console.log('FAIL ' + label + ' :: ' + error.message);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function waitFor(fn, label, timeoutMs = 6000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const value = fn();
    if (value) return value;
    await sleep(25);
  }
  throw new Error('等待超时：' + label);
}

// ------------------------------------------------------------ 最小 WS 网关
function encodeFrame(text) {
  const payload = Buffer.from(text, 'utf8');
  let header;
  if (payload.length < 126) {
    header = Buffer.from([0x81, payload.length]);
  } else if (payload.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x81;
    header[1] = 126;
    header.writeUInt16BE(payload.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x81;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(payload.length), 2);
  }
  return Buffer.concat([header, payload]);
}

function decodeFrame(buf) {
  if (buf.length < 2) return null;
  const opcode = buf[0] & 0x0f;
  const masked = (buf[1] & 0x80) !== 0;
  let len = buf[1] & 0x7f;
  let off = 2;
  if (len === 126) {
    if (buf.length < 4) return null;
    len = buf.readUInt16BE(2);
    off = 4;
  } else if (len === 127) {
    if (buf.length < 10) return null;
    len = Number(buf.readBigUInt64BE(2));
    off = 10;
  }
  let mask = null;
  if (masked) {
    if (buf.length < off + 4) return null;
    mask = buf.subarray(off, off + 4);
    off += 4;
  }
  if (buf.length < off + len) return null;
  let payload = buf.subarray(off, off + len);
  if (mask) {
    payload = Buffer.from(payload);
    for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
  }
  return { opcode, payload, consumed: off + len };
}

const sockets = new Set();
let currentSend = null;
let gatewayReady = false; // 收到 sessions.list 应答 = hello-ok 已被桥处理
let upgrades = 0;
let blackholeUpgrade = 0; // 这条连接只握手、不应答 sessions.list（模拟半开）
let stallUpgrade = 0;     // 这条连接连 101 都不回（模拟 TCP 黑洞）

const gateway = http.createServer();
gateway.on('upgrade', (req, socket) => {
  upgrades += 1;
  const myUpgrade = upgrades;
  gatewayReady = false;
  sockets.add(socket);
  socket.on('close', () => sockets.delete(socket));
  socket.on('error', () => sockets.delete(socket));
  if (myUpgrade === stallUpgrade) return; // 握手黑洞：什么都不写
  const accept = crypto.createHash('sha1')
    .update(req.headers['sec-websocket-key'] + WS_MAGIC).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  const send = (obj) => {
    if (!socket.destroyed) socket.write(encodeFrame(JSON.stringify(obj)));
  };
  currentSend = send;
  let buf = Buffer.alloc(0);
  socket.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      const frame = decodeFrame(buf);
      if (!frame) break;
      buf = buf.subarray(frame.consumed);
      if (frame.opcode === 0x8) { socket.end(); return; }
      if (frame.opcode === 0x9) { continue; } // ping 不回也不影响测试
      if (frame.opcode !== 0x1) continue;
      let msg;
      try { msg = JSON.parse(frame.payload.toString('utf8')); } catch { continue; }
      if (msg.type !== 'req') continue;
      if (msg.method === 'connect') {
        send({ type: 'hello-ok', protocol: 3 });
      } else if (msg.method === 'sessions.list') {
        if (myUpgrade === blackholeUpgrade) continue; // 半开：收得到请求，永不回应
        gatewayReady = true;
        send({ type: 'res', id: msg.id, ok: true, payload: [] });
      } else {
        send({ type: 'res', id: msg.id, ok: true, payload: {} });
      }
    }
  });
  // 握手完成即发挑战，桥会回 connect 请求
  send({ type: 'event', event: 'connect.challenge' });
});
await new Promise((resolve) => gateway.listen(0, '127.0.0.1', resolve));
const GW = `ws://127.0.0.1:${gateway.address().port}/ws`;

function inject(eventName, sessionKey, extra = {}) {
  assert.ok(currentSend, '网关还没有活跃连接');
  currentSend({ type: 'event', event: eventName, payload: { session_key: sessionKey, ...extra } });
}

// ------------------------------------------------------- 假 AgentBridge
const received = [];
const fakeBridge = http.createServer((req, res) => {
  let body = '';
  req.on('data', (chunk) => { body += chunk; });
  req.on('end', () => {
    received.push({ url: req.url, body: JSON.parse(body) });
    res.statusCode = 204;
    res.end();
  });
});
await new Promise((resolve) => fakeBridge.listen(0, '127.0.0.1', resolve));
const BRIDGE = `http://127.0.0.1:${fakeBridge.address().port}`;
const DEAD = 'http://127.0.0.1:1'; // 必然连不上 → 走落盘

const PAUSE = path.join(TMP, 'pause');
function spawnBridge(bridgeUrl, spoolDir, extraArgs = []) {
  // opt() 认第一个同名 flag：extraArgs 要覆盖的默认值不能重复出现在前面。
  const has = (name) => extraArgs.includes(`--${name}`);
  const args = [SCRIPT, '--gateway', GW, '--bridge', bridgeUrl, '--pause-file', PAUSE];
  if (!has('refresh')) args.push('--refresh', '600000');
  if (!has('heartbeat-file')) args.push('--heartbeat-file', path.join(TMP, 'default.heartbeat'));
  const child = spawn(process.execPath, [...args, ...extraArgs], {
    env: { ...process.env, AGENTRMC_SPOOL_DIR: spoolDir },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let out = '';
  child.stdout.on('data', (c) => { out += c.toString(); });
  child.stderr.on('data', (c) => { out += c.toString(); });
  return { child, out: () => out };
}
const exitOf = (child) => new Promise((resolve) => child.on('exit', (code) => resolve(code)));
async function stopBridge(handle) {
  handle.child.kill();
  await exitOf(handle.child);
  currentSend = null;
  gatewayReady = false;
}
function spooled(spoolDir) {
  const file = path.join(spoolDir, 'pending-events.jsonl');
  if (!fs.existsSync(file)) return [];
  return fs.readFileSync(file, 'utf8').split(/\r?\n/).filter(Boolean).map((l) => JSON.parse(l));
}

// ------------------------------------------------------------ A) 桥活着
await check('done → POST stop：带 session_id / title / raise', async () => {
  const handle = spawnBridge(BRIDGE, path.join(TMP, 'spool-a'));
  try {
    await waitFor(() => gatewayReady, '网关就绪');
    inject('session.event.done', 's1');
    await waitFor(() => received.length >= 1, 'stop 通知');
    const body = received[0].body;
    assert.strictEqual(received[0].url, '/notify');
    assert.strictEqual(body.event, 'stop');
    assert.strictEqual(body.title, 'OpenSquilla');
    assert.strictEqual(body.session_id, 's1');
    assert.strictEqual(body.raise, 'OpenSquilla.exe');
  } finally { await stopBridge(handle); }
});

await check('15s 内重复 done 去重；plan_presentation → question', async () => {
  received.length = 0;
  const handle = spawnBridge(BRIDGE, path.join(TMP, 'spool-a2'));
  try {
    await waitFor(() => gatewayReady, '网关就绪');
    inject('session.event.done', 's2');
    await waitFor(() => received.length >= 1, '第一条 stop');
    inject('session.event.done', 's2'); // 15s 内重复 → 该被吞
    inject('session.event.plan_presentation', 's3');
    await waitFor(() => received.length >= 2, 'question 通知');
    await sleep(400);
    assert.strictEqual(received.length, 2, '重复 done 不该多发');
    assert.strictEqual(received[1].body.event, 'question');
    assert.strictEqual(received[1].body.session_id, 's3');
  } finally { await stopBridge(handle); }
});

await check('turn-start 不吃 15s 去重：连发两条都到', async () => {
  received.length = 0;
  const handle = spawnBridge(BRIDGE, path.join(TMP, 'spool-a3'));
  try {
    await waitFor(() => gatewayReady, '网关就绪');
    inject('session.event.turn_committed', 's4');
    await waitFor(() => received.length >= 1, '第一条 turn-start');
    inject('session.event.turn_committed', 's4');
    await waitFor(() => received.length >= 2, '第二条 turn-start（不被去重吞）');
    assert.strictEqual(received[0].body.event, 'turn-start');
    assert.strictEqual(received[1].body.event, 'turn-start');
    assert.strictEqual(received[1].body.session_id, 's4');
  } finally { await stopBridge(handle); }
});

await check('免打扰：done 被吞，turn-start 照发（人回来开新轮就该归档）', async () => {
  received.length = 0;
  fs.writeFileSync(PAUSE, '');
  const handle = spawnBridge(BRIDGE, path.join(TMP, 'spool-a4'));
  try {
    await waitFor(() => gatewayReady, '网关就绪');
    inject('session.event.done', 's5');
    inject('session.event.turn_committed', 's5');
    await waitFor(() => received.length >= 1, 'turn-start 穿过免打扰');
    await sleep(400);
    assert.strictEqual(received.length, 1, '免打扰时 done 不该发');
    assert.strictEqual(received[0].body.event, 'turn-start');
  } finally {
    fs.rmSync(PAUSE, { force: true });
    await stopBridge(handle);
  }
});

// ------------------------------------------------------------ B) 桥死了
await check('离线落盘：done/turn-start 进 pending-events.jsonl，与 hooks 同格式', async () => {
  const dir = path.join(TMP, 'spool-b');
  const handle = spawnBridge(DEAD, dir);
  try {
    await waitFor(() => gatewayReady, '网关就绪');
    inject('session.event.done', 's6');
    await waitFor(() => spooled(dir).length >= 1, 'stop 落盘');
    inject('session.event.turn_committed', 's7');
    inject('session.event.turn_committed', 's7');
    await waitFor(() => spooled(dir).length >= 3, '两条 turn-start 都落盘（不去重）');
    const lines = spooled(dir);
    assert.strictEqual(lines[0].event, 'stop');
    assert.strictEqual(lines[0].title, 'OpenSquilla');
    assert.strictEqual(lines[0].session_id, 's6');
    assert.strictEqual(lines[1].event, 'turn-start');
    assert.strictEqual(lines[2].event, 'turn-start');
    assert.strictEqual(lines[2].session_id, 's7');
  } finally { await stopBridge(handle); }
});

// ------------------------------------------------------------ C) 上限守卫
await check('spool 上限守卫：超 1MB 丢最旧留最新，新事件仍在尾部', async () => {
  const dir = path.join(TMP, 'spool-c');
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, 'pending-events.jsonl');
  const filler = JSON.stringify({ event: 'stop', title: 'filler', message: 'x'.repeat(300) });
  // ~350B/行 × 4000 行 ≈ 1.4MB，超过 1MB 上限
  fs.writeFileSync(file, Array(4000).fill(filler).join('\n') + '\n');
  const handle = spawnBridge(DEAD, dir);
  try {
    await waitFor(() => gatewayReady, '网关就绪');
    inject('session.event.done', 's-cap');
    await waitFor(() => {
      const lines = spooled(dir);
      return lines.length && lines[lines.length - 1].session_id === 's-cap';
    }, '新事件落盘');
    const lines = spooled(dir);
    assert.ok(lines.length <= 501, '裁剪后应 ≤501 行，实际 ' + lines.length);
    assert.ok(fs.statSync(file).size < 1024 * 1024, '裁剪后应远小于上限');
  } finally { await stopBridge(handle); }
});

// ------------------------------------------------------------ C2) 假活自愈
await check('半开自愈：探针连续 3 次无应答 → 强制换新连接并恢复', async () => {
  blackholeUpgrade = upgrades + 1;
  const handle = spawnBridge(BRIDGE, path.join(TMP, 'spool-e'), [
    '--refresh', '100', '--call-timeout', '300', '--hello-timeout', '3000',
  ]);
  try {
    // 第一条连接握手正常但 sessions.list 永不回应；桥应在 ~1s 内自己换连接，
    // 第二条连接恢复正常应答 → gatewayReady 重新变真，全程不用重启进程。
    await waitFor(
      () => upgrades >= blackholeUpgrade + 1 && gatewayReady,
      '半开后重连恢复', 10000);
    assert.ok(handle.out().includes('forcing reconnect'), '日志该有强制重连记录');
  } finally {
    await stopBridge(handle);
    blackholeUpgrade = 0;
  }
});

await check('握手黑洞：hello-ok 限时没到也换新连接（TCP 黑洞兜底）', async () => {
  stallUpgrade = upgrades + 1;
  const handle = spawnBridge(BRIDGE, path.join(TMP, 'spool-f'), [
    '--refresh', '100', '--call-timeout', '300', '--hello-timeout', '600',
  ]);
  try {
    await waitFor(
      () => upgrades >= stallUpgrade + 1 && gatewayReady,
      '握手超时后重连恢复', 10000);
    assert.ok(handle.out().includes('forcing reconnect'), '日志该有强制重连记录');
  } finally {
    await stopBridge(handle);
    stallUpgrade = 0;
  }
});

await check('heartbeat：启动即写、按周期更新（时间戳是近期且会前进）', async () => {
  const hb = path.join(TMP, 'hb');
  const handle = spawnBridge(BRIDGE, path.join(TMP, 'spool-g'), [
    '--refresh', '100', '--heartbeat-file', hb,
  ]);
  try {
    await waitFor(() => fs.existsSync(hb), '心跳文件出现');
    const first = Number(fs.readFileSync(hb, 'utf8'));
    assert.ok(Number.isFinite(first) && Date.now() - first < 5000, '心跳应是近期时间戳');
    await waitFor(() => Number(fs.readFileSync(hb, 'utf8')) > first, '心跳更新');
  } finally { await stopBridge(handle); }
});

// ------------------------------------------------------------ D) selftest
await check('--selftest：桥通 exit 0；桥死 exit 1 且不落盘（探针不污染补投）', async () => {
  const dir = path.join(TMP, 'spool-d');
  const ok = spawn(process.execPath, [SCRIPT, '--selftest', '--bridge', BRIDGE], {
    env: { ...process.env, AGENTRMC_SPOOL_DIR: dir }, stdio: 'ignore',
  });
  assert.strictEqual(await exitOf(ok), 0);
  const bad = spawn(process.execPath, [SCRIPT, '--selftest', '--bridge', DEAD], {
    env: { ...process.env, AGENTRMC_SPOOL_DIR: dir }, stdio: 'ignore',
  });
  assert.strictEqual(await exitOf(bad), 1);
  assert.deepStrictEqual(spooled(dir), [], '自检失败不该落盘');
});

// ------------------------------------------------------------ 收尾
for (const socket of sockets) socket.destroy();
await new Promise((resolve) => gateway.close(resolve));
await new Promise((resolve) => fakeBridge.close(resolve));
fs.rmSync(TMP, { recursive: true, force: true });

const total = passed + failures.length;
if (failures.length) {
  console.log('\n' + failures.length + ' FAILED of ' + total);
  failures.forEach((item) => console.log('  ' + item));
  process.exit(1);
}
console.log('\nALL PASS (' + passed + '/' + total + ')');
