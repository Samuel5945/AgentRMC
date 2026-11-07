// DeepSeek Harness 版插件的回归测试：零依赖，`node test/plugin.test.js` 直接跑。
//
// 两层：
//   A) 纯函数（lib/text.js 的一句话压缩）；
//   B) 用假 ctx 把整个插件跑起来——事件监听、payload 形状、工具注册、skill 注册。
//      桥接端口故意指到 1（必然连不上），于是每条通知都会落进 pending-events.jsonl，
//      断言读的就是「真发出去的那一份内容」，不用 mock HTTP。
//
// 注意：跑之前要把宿主家族的环境变量清干净，否则 AGENTRMC_PORT 之类会被外部影响。

import assert from 'node:assert';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { apply, name as pluginName, inject } from '../index.js';
import { trimSentence, flattenText, trailingQuestion } from '../lib/text.js';

const PLUGIN_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
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

// ---------------------------------------------------------------- A) 纯函数
await check('trimSentence: 去代码块与 markdown 记号，只留第一句', () => {
  const text = trimSentence('**改好了**，`dist` 已重新打包。后面还有一堆细节不用念。');
  assert.match(text, /^改好了/);
  assert.match(text, /dist 已重新打包/);
  assert.doesNotMatch(text, /后面还有一堆细节/, '只留第一句');
  assert.doesNotMatch(text, /[*`]/, 'markdown 记号不念');
});

await check('trimSentence: 长句截断到一句话长度', () => {
  const text = trimSentence('甲'.repeat(200));
  assert.ok(text.length <= 80, '长度 ' + text.length);
  assert.ok(text.startsWith('甲'));
});

await check('trimSentence: 空输入返回空串', () => {
  assert.strictEqual(trimSentence(''), '');
  assert.strictEqual(trimSentence(null), '');
  assert.strictEqual(trimSentence('```js\nconst a = 1;\n```'), '');
});

await check('flattenText: 只取 text 块', () => {
  assert.strictEqual(
    flattenText([
      { type: 'text', text: '甲' },
      { type: 'reasoning', text: '不念' },
      { type: 'text', text: '乙' },
    ]),
    '甲 乙',
  );
});

// —— 问句收尾判定：与 bridge 版 trailingQuestion 同一套规则，两条路行为要一致 ——
await check('trailingQuestion: 问句收尾返回那句问话', () => {
  assert.strictEqual(trailingQuestion('两条路都试了。要不要顺手把 docs 也改了？'), '要不要顺手把 docs 也改了？');
  assert.strictEqual(trailingQuestion('先说结论：测试全绿。再说一句：装不装？'), '再说一句：装不装？');
  assert.strictEqual(trailingQuestion('Both paths work now. Should I mirror this into DSH?'), 'Should I mirror this into DSH?');
});

await check('trailingQuestion: 陈述/问号后还有正文/URL/光问号 都返回空', () => {
  assert.strictEqual(trailingQuestion('改好了，测试全绿。'), '');
  assert.strictEqual(trailingQuestion('要不要我改？我先按 A 做了。'), '');
  assert.strictEqual(trailingQuestion('看这里 https://example.com/docs?tab=env'), '');
  assert.strictEqual(trailingQuestion('？'), '');
  assert.strictEqual(trailingQuestion(''), '');
});

await check('trailingQuestion: 版本号小数点不拦腰截断', () => {
  assert.strictEqual(trailingQuestion('那 0.4.0 要不要现在出包？'), '那 0.4.0 要不要现在出包？');
});

// ------------------------------------------------------- B) 假 ctx 跑真插件
const SPOOL_ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'agentrmc-dsh-'));
const SPOOL_DIR = path.join(SPOOL_ROOT, 'spool');
const SPOOL_FILE = path.join(SPOOL_DIR, 'pending-events.jsonl');

// 端口指到 1：必然连不上，于是每条通知都走「落盘等补投」这条真实路径，
// 断言读到的就是真要发出去的内容。也顺手保证测试不会真去喊用户桌面上的桌宠。
process.env.AGENTRMC_PORT = '1';
process.env.AGENTRMC_SPOOL_DIR = SPOOL_DIR;
delete process.env.AGENTRMC_DRY_RUN;

/** 假 Cordis 上下文：只实现插件真的用到的那几个方法。 */
function makeCtx() {
  const listeners = new Map();
  const tools = [];
  const skills = [];
  const effects = [];
  const ctx = {
    on(event, listener) {
      if (!listeners.has(event)) listeners.set(event, []);
      listeners.get(event).push(listener);
      return () => {};
    },
    effect(execute) {
      const disposer = execute();
      effects.push(disposer);
      return () => {};
    },
    get(service) {
      if (service === 'workingDirectory') {
        return { get: () => 'D:/repo' };
      }
      if (service === 'skills') {
        return {
          register(registration) {
            skills.push(registration);
            return () => {};
          },
        };
      }
      return undefined;
    },
    tools: {
      register(definition) {
        tools.push(definition);
        return () => {};
      },
    },
  };
  return { ctx, listeners, tools, skills, effects };
}

const harness = makeCtx();
apply(harness.ctx, {});

function emit(event, ...args) {
  const list = harness.listeners.get(event) || [];
  const results = [];
  for (const listener of list) results.push(listener(...args));
  return results;
}

function spooled() {
  if (!fs.existsSync(SPOOL_FILE)) return [];
  return fs
    .readFileSync(SPOOL_FILE, 'utf8')
    .split(/\r?\n/)
    .filter(Boolean)
    .map((line) => JSON.parse(line));
}

async function waitForSpool(count) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const lines = spooled();
    if (lines.length >= count) return lines;
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error('等待落盘超时：期望 ' + count + ' 条，实际 ' + spooled().length + ' 条');
}

function clearSpool() {
  fs.rmSync(SPOOL_DIR, { recursive: true, force: true });
}

const AGENT = { id: 'sess-1', session: { id: 'sess-1' } };
// 会话事件里拿到的是 Session 本身（宿主回调的第二个参数），只用到 id。
const SESSION = { id: 'sess-1' };

await check('导出形式: name / inject 是 Host 插件要的那份', () => {
  assert.strictEqual(pluginName, 'agentrmc-bridge');
  assert.deepStrictEqual(inject, ['tools']);
});

await check('approval/asked: 发 permission，带工具名、宿主名、session 与审计 id', async () => {
  clearSpool();
  // 会话事件是 emit：所有监听器都会跑，不存在「被 UI 应答者抢占后轮不到」的问题。
  emit('session/event', SESSION, {
    type: 'approval/asked',
    data: { id: 'ap-1', toolName: 'pwsh', callId: 'call-1' },
  });
  const lines = await waitForSpool(1);
  const payload = lines[0];
  assert.strictEqual(payload.event, 'permission');
  assert.strictEqual(payload.title, 'DeepSeek Harness');
  assert.strictEqual(payload.message, '等你确认 pwsh');
  assert.strictEqual(payload.speech, 'DeepSeek Harness 在等你点确认');
  assert.strictEqual(payload.raise, 'DeepSeek Harness.exe');
  assert.strictEqual(payload.session_id, 'sess-1');
  assert.strictEqual(payload.project, 'D:/repo');
  assert.strictEqual(payload.event_id, 'ap-1');
  assert.ok(payload.plugin_version, 'payload 必须带 plugin_version');
});

await check('tool/call ask_user_question: 发 question（等你选方案）', async () => {
  clearSpool();
  emit('session/event', SESSION, {
    type: 'tool/call',
    data: { name: 'ask_user_question', callId: 'call-2', arguments: '{"questions":[]}' },
  });
  const lines = await waitForSpool(1);
  const payload = lines[0];
  assert.strictEqual(payload.event, 'question');
  assert.strictEqual(payload.message, '等你选方案');
  assert.strictEqual(payload.speech, 'DeepSeek Harness 在等你选方案');
  assert.strictEqual(payload.event_id, 'call-2');
  assert.strictEqual(payload.session_id, 'sess-1');
});

await check('session/event 其它事件静默: 普通过具调用不刷屏', async () => {
  clearSpool();
  emit('session/event', SESSION, { type: 'tool/call', data: { name: 'pwsh', callId: 'c9' } });
  emit('session/event', SESSION, { type: 'turn/end', data: { turn: 1 } });
  emit('session/event', SESSION, { type: 'request/header', data: { header: {} } });
  await new Promise((resolve) => setTimeout(resolve, 400));
  assert.deepStrictEqual(spooled(), [], '只有 approval/asked 与 ask_user_question 该发');
});

await check('agent/inbox/claimed: 同一轮只报一次 turn-start，且不出声', async () => {
  clearSpool();
  emit('agent/inbox/claimed', { agent: AGENT, turn: 1, message: {} });
  emit('agent/inbox/claimed', { agent: AGENT, turn: 1, message: {} });
  const lines = await waitForSpool(1);
  await new Promise((resolve) => setTimeout(resolve, 300));
  const again = spooled();
  assert.strictEqual(again.length, 1, '同一 turn 只该发一条');
  assert.strictEqual(lines[0].event, 'turn-start');
  assert.strictEqual(lines[0].message, '');
  assert.strictEqual(lines[0].speech, undefined);
});

await check('assistant-stream + turn-stopping: 念出本轮最终回复', async () => {
  clearSpool();
  const agent = { id: 'sess-2', session: { id: 'sess-2' } };
  emit('agent/assistant-stream', {
    agent,
    frame: { type: 'start', attemptId: 'a1', revision: 0, turn: 1, step: 1 },
  });
  emit('agent/assistant-stream', {
    agent,
    frame: {
      type: 'chunk',
      attemptId: 'a1',
      revision: 0,
      index: 0,
      time: 1,
      chunk: { type: 'text-delta', index: 0, text: '**改好了**，`dist` 已重新打包。' },
    },
  });
  emit('agent/assistant-stream', {
    agent,
    frame: {
      type: 'end',
      attemptId: 'a1',
      revision: 0,
      index: 1,
      outcome: { kind: 'committed', eventType: 'assistant/message', seq: 7 },
    },
  });
  emit('agent/turn-stopping', { agent, turn: 1, signal: new AbortController().signal });
  const lines = await waitForSpool(1);
  const payload = lines[0];
  assert.strictEqual(payload.event, 'stop');
  assert.match(payload.message, /^DeepSeek Harness：改好了/);
  assert.ok(payload.message.length < 100, '念的文案要短');
});

await check('turn-stopping 兜底: 没有正文时用通用文案', async () => {
  clearSpool();
  const agent = { id: 'sess-3', session: { id: 'sess-3' } };
  emit('agent/turn-stopping', { agent, turn: 1 });
  const lines = await waitForSpool(1);
  assert.strictEqual(lines[0].message, 'DeepSeek Harness 做完了，等你回来看');
  assert.strictEqual(lines[0].speech, undefined);
});

await check('turn-stopping 问句收尾: 改发 permission 的「等你回话」', async () => {
  clearSpool();
  const agent = { id: 'sess-q', session: { id: 'sess-q' } };
  emit('agent/assistant-stream', {
    agent,
    frame: { type: 'start', attemptId: 'a1', revision: 0, turn: 1, step: 1 },
  });
  emit('agent/assistant-stream', {
    agent,
    frame: {
      type: 'chunk',
      attemptId: 'a1',
      revision: 0,
      index: 0,
      time: 1,
      chunk: { type: 'text-delta', index: 0, text: '测试全绿。**要不要现在出包？**' },
    },
  });
  emit('agent/assistant-stream', {
    agent,
    frame: {
      type: 'end',
      attemptId: 'a1',
      revision: 0,
      index: 1,
      outcome: { kind: 'committed', eventType: 'assistant/message', seq: 7 },
    },
  });
  emit('agent/turn-stopping', { agent, turn: 1, signal: new AbortController().signal });
  const lines = await waitForSpool(1);
  const payload = lines[0];
  assert.strictEqual(payload.event, 'permission');
  assert.strictEqual(payload.message, '等你回话：要不要现在出包？');
  assert.strictEqual(payload.speech, 'DeepSeek Harness 在等你回话');
  // 不能撞「等你选方案」前缀，否则原生端套 180s 迟到去重把它吞掉。
  assert.doesNotMatch(payload.message, /^等你选方案/);
});

await check('工具注册: 四个 pet_* 都有合法的 schema 与文本渲染', () => {
  const names = harness.tools.map((tool) => tool.name).sort();
  assert.deepStrictEqual(names, [
    'pet_notify',
    'pet_speak',
    'pet_state',
    'pet_work_update',
  ]);
  for (const tool of harness.tools) {
    assert.ok(tool.description && tool.description.length > 10, tool.name + ' 缺描述');
    assert.strictEqual(tool.parameters.type, 'object', tool.name + ' 参数根必须是对象');
    assert.strictEqual(typeof tool.parameters.properties, 'object');
    assert.ok(tool.output && typeof tool.output.render === 'function', tool.name + ' 缺 render');
    const rendered = tool.output.render({}, 'x');
    assert.deepStrictEqual(rendered, [{ type: 'text', text: 'x' }]);
  }
  const speak = harness.tools.find((tool) => tool.name === 'pet_speak');
  assert.deepStrictEqual(speak.parameters.required, ['message']);
});

await check('工具注册: 参数缺失时拒绝执行（不静默发空气泡）', async () => {
  const notify = harness.tools.find((tool) => tool.name === 'pet_notify');
  await assert.rejects(() => notify.execute({}), /message 不能为空/);
  const work = harness.tools.find((tool) => tool.name === 'pet_work_update');
  await assert.rejects(() => work.execute({ stage: 'done' }), /progress \/ blocked \/ ready/);
});

await check('工具执行: 桌宠没开时抛友好错误而不是卡住', async () => {
  const state = harness.tools.find((tool) => tool.name === 'pet_state');
  await assert.rejects(() => state.execute({}), /AgentRMC 没连上/);
});

// 用一台真的假桥接（随机端口）验工具发出去的 body：hook 的 payload 天生带
// session_id，工具调用只能从 exec.agent 补——补不上，宿主下一轮新对话就找不到
// 这张工单去归档。
await check('工具执行: body 带 session_id / project / agent_exe，与 hook 同字段', async () => {
  const received = [];
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
    });
    req.on('end', () => {
      received.push({ method: req.method, url: req.url, body });
      res.setHeader('Content-Type', 'application/json');
      res.end('{"ok":true,"id":"x"}');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  try {
    const stub = makeCtx();
    apply(stub.ctx, { port, registerSkill: false });
    const exec = { agent: AGENT };
    const notify = stub.tools.find((tool) => tool.name === 'pet_notify');
    await notify.execute({ message: '阶段进度' }, exec);
    const work = stub.tools.find((tool) => tool.name === 'pet_work_update');
    await work.execute({ stage: 'ready', message: '做完了' }, exec);

    const notifyBody = JSON.parse(received[0].body);
    assert.strictEqual(received[0].url, '/notify');
    assert.strictEqual(notifyBody.session_id, 'sess-1');
    assert.strictEqual(notifyBody.project, 'D:/repo');
    assert.strictEqual(notifyBody.title, 'DeepSeek Harness');

    const workBody = JSON.parse(received[1].body);
    assert.strictEqual(received[1].url, '/work-items/update');
    assert.strictEqual(workBody.stage, 'ready');
    assert.strictEqual(workBody.session_id, 'sess-1');
    assert.strictEqual(workBody.agent_exe, 'DeepSeek Harness.exe');
    assert.strictEqual(workBody.raise, 'DeepSeek Harness.exe');
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('skill 注册: 名字、正文、目录资源都给了', () => {
  assert.strictEqual(harness.skills.length, 1);
  const skill = harness.skills[0];
  assert.strictEqual(skill.name, 'agentrmc-pet');
  assert.ok(skill.description.length > 20);
  assert.ok(skill.content.includes('pet_work_update'), '正文要教模型用工具');
  assert.ok(!skill.content.startsWith('---'), 'frontmatter 不该进正文');
  assert.strictEqual(skill.source, 'custom');
  assert.strictEqual(skill.provider, 'agentrmc-bridge');
  assert.deepStrictEqual(skill.invocation, { modelInvocable: true, userInvocable: true });
  assert.strictEqual(skill.resourceBase.kind, 'directory');
  assert.ok(fs.existsSync(skill.resourceBase.path), 'resourceBase 指向的目录必须存在');
});

await check('配置覆盖: agentName / raiseExe / 关掉的提醒都生效', async () => {
  clearSpool();
  const other = makeCtx();
  apply(other.ctx, {
    agentName: '另一个宿主',
    raiseExe: 'other.exe',
    notifyApproval: false,
    registerTools: false,
    registerSkill: false,
  });
  assert.strictEqual(other.tools.length, 0, 'registerTools=false 时不该注册工具');
  assert.strictEqual(other.skills.length, 0, 'registerSkill=false 时不该注册 skill');
  for (const listener of other.listeners.get('session/event') || []) {
    listener(SESSION, { type: 'approval/asked', data: { id: 'ap-2', toolName: 'pwsh' } });
  }
  await new Promise((resolve) => setTimeout(resolve, 300));
  assert.deepStrictEqual(spooled(), [], '关掉的提醒不该发');
});

await check('离线落盘与原版同址同格式: pending-events.jsonl 一行一条 JSON', async () => {
  clearSpool();
  emit('agent/turn-stopping', { agent: { id: 'sess-4', session: { id: 'sess-4' } }, turn: 1 });
  const lines = await waitForSpool(1);
  assert.ok(lines.length > 0);
  for (const line of lines) {
    assert.ok(typeof line.event === 'string' && line.event.length > 0);
    assert.ok(typeof line.title === 'string');
  }
  assert.strictEqual(path.basename(SPOOL_FILE), 'pending-events.jsonl');
  assert.strictEqual(path.dirname(SPOOL_FILE), SPOOL_DIR);
});

await check('spool 上限守卫: 超 1MB 丢最旧留最新，新事件仍在尾部', async () => {
  clearSpool();
  fs.mkdirSync(SPOOL_DIR, { recursive: true });
  const filler = JSON.stringify({ event: 'stop', title: 'filler', message: 'x'.repeat(300) });
  // ~350B/行 × 4000 行 ≈ 1.4MB，超过 1MB 上限
  fs.writeFileSync(SPOOL_FILE, Array(4000).fill(filler).join('\n') + '\n');
  emit('agent/turn-stopping', { agent: { id: 'sess-cap', session: { id: 'sess-cap' } }, turn: 1 });
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const lines = spooled();
    if (lines.length && lines[lines.length - 1].session_id === 'sess-cap') break;
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  const lines = spooled();
  assert.ok(lines.length <= 501, '裁剪后应 ≤501 行，实际 ' + lines.length);
  assert.strictEqual(lines[lines.length - 1].session_id, 'sess-cap', '新事件必须在尾部');
  assert.ok(fs.statSync(SPOOL_FILE).size < 1024 * 1024, '裁剪后应远小于上限');
});

await check('插件入口可读: cordis.patch.yml 的 name 与 package.json 对得上', () => {
  const manifest = JSON.parse(
    fs.readFileSync(path.join(PLUGIN_ROOT, 'package.json'), 'utf8'),
  );
  const patch = fs.readFileSync(path.join(PLUGIN_ROOT, 'cordis.patch.yml'), 'utf8');
  assert.strictEqual(manifest.name, '@local/agentrmc-bridge');
  assert.ok(patch.includes("'" + manifest.name + "'"), 'patch 里的 name 必须与 package.json 一致');
  assert.strictEqual(manifest.dsh.bundle.patch, './cordis.patch.yml');
  assert.ok(manifest.exports['./locale/*.json'], 'metadata 要走 locale 导出');
  assert.ok(manifest.exports['./icon'], 'icon 要导出');
});

// —— 定向派发通道：真 HTTP 轮询取件 → followup 注入 → ack 回执 ——
async function waitUntil(predicate, ms = 6000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (predicate()) return true;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  return false;
}

/** 假原生端：/dispatch/pending 一次性吐完 items（模拟 take-once），/dispatch/ack 记账。
 * 响应必须带 Connection: close——原生端 AgentBridge 就是这么回的；不关的话 node
 * 全局 agent 会池化连接，server.close() 要等 5s 空闲超时，测试被拖死。 */
function makeDispatchServer(pendingItems) {
  const seen = { pendingUrls: [], acks: [] };
  const queue = pendingItems.slice();
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
    });
    req.on('end', () => {
      res.setHeader('Connection', 'close');
      if (req.url.startsWith('/dispatch/pending')) {
        seen.pendingUrls.push(req.url);
        res.setHeader('Content-Type', 'application/json');
        res.end(JSON.stringify(queue.splice(0)));
        return;
      }
      if (req.url.startsWith('/dispatch/ack')) {
        seen.acks.push(JSON.parse(body || '{}'));
        res.end('{"ok":true}');
        return;
      }
      res.end('{}');
    });
  });
  return { server, seen, queue };
}

/** 给假 ctx 补一个 agents 服务（真实插件从 ctx.get('agents') 拿活表）。 */
function applyWithAgents(agents, config) {
  const stub = makeCtx();
  const innerGet = stub.ctx.get;
  stub.ctx.get = (service) => (service === 'agents' ? agents : innerGet(service));
  apply(stub.ctx, { registerSkill: false, registerTools: false, ...config });
  return stub;
}

await check('派发通道: 取件 → followup 注入（UserMessage 与 ACP 同形）→ ack ok', async () => {
  const sid = 'session-aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee';
  const item = {
    dispatch_id: 'd-1',
    session_id: sid,
    project: 'D:/repo',
    text: '定向派发探针 MARK-1：只回复收到',
  };
  const injected = [];
  const agents = {
    get: (id) => (id === sid ? { followup: (m) => injected.push(m) } : undefined),
  };
  const { server, seen } = makeDispatchServer([item]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    applyWithAgents(agents, { port: server.address().port });
    assert.ok(await waitUntil(() => seen.acks.length > 0), 'ack 未到');
    assert.ok(
      seen.pendingUrls[0].includes('exe=DeepSeek%20Harness.exe'),
      '取件要带自己的映像名，实际 ' + seen.pendingUrls[0],
    );
    assert.strictEqual(injected.length, 1);
    assert.strictEqual(injected[0].role, 'user');
    assert.strictEqual(injected[0].source.kind, 'user');
    assert.strictEqual(injected[0].content[0].type, 'text');
    assert.strictEqual(injected[0].content[0].text, item.text);
    assert.ok(injected[0].id, 'UserMessage 要有稳定 id');
    assert.strictEqual(seen.acks[0].dispatch_id, 'd-1');
    assert.strictEqual(seen.acks[0].ok, true);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('派发通道: 会话不在活表 / agents 不可达 → ack ok:false 带原因', async () => {
  // 一台 server 撑两个场景（批量两条走第一个实例；agents 不可达换新实例，
  // 后续 items 由 queue 补进）。不再关了重开：池化 socket 的关-开序列会拖死测试。
  const { server, seen, queue } = makeDispatchServer([
    { dispatch_id: 'd-2', session_id: 'session-missing', project: '', text: 'x' },
    { dispatch_id: 'd-3', session_id: 'session-too', project: '', text: 'y' },
  ]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  try {
    // 实例 A：有 agents 服务但目标不在活表 → 两条都拒。
    const withAgents = makeCtx();
    const innerGet = withAgents.ctx.get;
    withAgents.ctx.get = (service) =>
      service === 'agents' ? { get: () => undefined } : innerGet(service);
    apply(withAgents.ctx, {
      port,
      registerSkill: false,
      registerTools: false,
    });
    assert.ok(await waitUntil(() => seen.acks.length >= 2), 'ack 未到');
    assert.deepStrictEqual(
      seen.acks.map((ack) => ack.dispatch_id),
      ['d-2', 'd-3'],
      '一次取件的两条要逐条 ack',
    );
    for (const ack of seen.acks) {
      assert.strictEqual(ack.ok, false);
      assert.ok(/不在活表/.test(ack.error), ack.error);
    }
    // 实例 B：连 agents 服务都拿不到。先把 A 的轮询器停掉（effect disposer =
    // clearInterval），不然下一条 items 会被 A 抢先消费、报错文本对不上。
    for (const dispose of withAgents.effects) dispose();
    queue.push({ dispatch_id: 'd-4', session_id: 'session-x', project: '', text: 'z' });
    apply(makeCtx().ctx, {
      port,
      registerSkill: false,
      registerTools: false,
    });
    assert.ok(await waitUntil(() => seen.acks.length >= 3), 'ack B 未到');
    assert.strictEqual(seen.acks[2].dispatch_id, 'd-4');
    assert.strictEqual(seen.acks[2].ok, false);
    assert.ok(/agents 服务不可达/.test(seen.acks[2].error), seen.acks[2].error);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('派发通道: followup 抛错 → ack ok:false 带原因，不炸轮询', async () => {
  const agents = {
    get: () => ({
      followup: () => {
        throw new Error('driver disposed');
      },
    }),
  };
  const { server, seen } = makeDispatchServer([
    { dispatch_id: 'd-5', session_id: 'session-boom', project: '', text: 'boom' },
  ]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    applyWithAgents(agents, { port: server.address().port });
    assert.ok(await waitUntil(() => seen.acks.length > 0), 'ack 未到');
    assert.strictEqual(seen.acks[0].ok, false);
    assert.ok(/followup 被拒/.test(seen.acks[0].error), seen.acks[0].error);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('派发通道: dispatch=false 不轮询（关开关就该安静）', async () => {
  const { server, seen } = makeDispatchServer([]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    applyWithAgents({ get: () => undefined }, {
      port: server.address().port,
      dispatch: false,
    });
    await new Promise((resolve) => setTimeout(resolve, 2500));
    assert.deepStrictEqual(seen.pendingUrls, [], '关掉的通道不该发取件请求');
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

fs.rmSync(SPOOL_ROOT, { recursive: true, force: true });

const total = passed + failures.length;
if (failures.length) {
  console.log('\n' + failures.length + ' FAILED of ' + total);
  failures.forEach((item) => console.log('  ' + item));
  process.exit(1);
}
console.log('\nALL PASS (' + passed + '/' + total + ')');
