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
  const regs = [];   // {event, listener, options}：options 用来断言 prepend
  const tools = [];
  const skills = [];
  const effects = [];
  const ctx = {
    on(event, listener, options) {
      if (!listeners.has(event)) listeners.set(event, []);
      listeners.get(event).push(listener);
      regs.push({ event, listener, options });
      return () => {};
    },
    effect(execute) {
      const disposer = execute();
      if (typeof disposer === 'function') {
        effects.push(disposer);
      } else if (disposer && typeof disposer.dispose === 'function') {
        effects.push(() => disposer.dispose());
      } else {
        effects.push(() => {});
      }
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
  return { ctx, listeners, tools, skills, effects, regs };
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
  assert.strictEqual(payload.bubble, false, '授权气泡收敛进卡');
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
  assert.strictEqual(payload.bubble, false, '选方案气泡收敛进卡');
  assert.strictEqual(payload.speech, 'DeepSeek Harness 在等你选方案');
  assert.strictEqual(payload.event_id, 'call-2');
  assert.strictEqual(payload.session_id, 'sess-1');
});

await check('tool/call ask_user_question: 气泡带题干，前缀保持「等你选方案」', async () => {
  clearSpool();
  emit('session/event', SESSION, {
    type: 'tool/call',
    data: {
      name: 'ask_user_question', callId: 'call-stem',
      arguments: JSON.stringify({
        questions: [{ question: '升 minor 出 0.5 还是先修完出 0.4.2？', options: [] }],
      }),
    },
  });
  const lines = await waitForSpool(1);
  assert.strictEqual(lines[0].message, '等你选方案：升 minor 出 0.5 还是先修完出 0.4.2？');
  assert.ok(lines[0].message.startsWith('等你选方案'),
    '前缀被改会丢原生端的选方案语义（180s 迟到去重）');
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
      // 取件轮询（B2）与派发取件要回 JSON 数组；别的回工具用的对象。
      res.end(req.url.startsWith('/request-answer/pending') ||
        req.url.startsWith('/dispatch/pending') ? '[]' : '{"ok":true,"id":"x"}');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  try {
    const stub = makeCtx();
    apply(stub.ctx, { port, registerSkill: false, dispatch: false });
    const exec = { agent: AGENT };
    const notify = stub.tools.find((tool) => tool.name === 'pet_notify');
    await notify.execute({ message: '阶段进度' }, exec);
    const work = stub.tools.find((tool) => tool.name === 'pet_work_update');
    await work.execute({ stage: 'ready', message: '做完了' }, exec);

    // B2 收件轮询与工具调用共走一条 HTTP 连接：只看业务 POST 的。
    const relevant = received.filter((r) =>
      r.url === '/notify' || r.url === '/work-items/update');
    const notifyBody = JSON.parse(relevant[0].body);
    assert.strictEqual(relevant[0].url, '/notify');
    assert.strictEqual(notifyBody.session_id, 'sess-1');
    assert.strictEqual(notifyBody.project, 'D:/repo');
    assert.strictEqual(notifyBody.title, 'DeepSeek Harness');

    const workBody = JSON.parse(relevant[1].body);
    assert.strictEqual(relevant[1].url, '/work-items/update');
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

/** 假原生端（B2）：/request-answer/pending 一次性吐完 items，ack 记账；
 *  /requests 推送也记账（验证插件在应答后重推空集）。响应必须 Connection: close。
 *  options.gate = {open:false} 时取件先吐空数组——用来把「答案到达」压在
 *  「插件已经 hold 住瀑布」之后（否则第一拍轮询会把答案抢在 hold 之前消费掉）。 */
function makeAnswerServer(pendingItems, options = {}) {
  const seen = { pushes: [], acks: [], pendingUrls: [] };
  const gate = options.gate;
  const queue = pendingItems.slice();
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (chunk) => { body += chunk; });
    req.on('end', () => {
      res.setHeader('Connection', 'close');
      if (req.url.startsWith('/request-answer/pending')) {
        seen.pendingUrls.push(req.url);
        res.setHeader('Content-Type', 'application/json');
        res.end(JSON.stringify(gate && !gate.open ? [] : queue.splice(0)));
        return;
      }
      if (req.url.startsWith('/request-answer/ack')) {
        seen.acks.push(JSON.parse(body || '{}'));
        res.end('{}');
        return;
      }
      if (req.url.startsWith('/requests')) {
        seen.pushes.push(JSON.parse(body || '{}'));
        res.end('{}');
        return;
      }
      res.end('{}');
    });
  });
  return { server, seen };
}

function makeWaitStub() {
  return { completion: { resolve: (v) => { makeWaitStub.resolved = v; } } };
}

// stub ctx 的事件 emit（顶层 harness 的 emit 绑在固定实例上）。
function emitOn(stub, event, ...args) {
  for (const listener of stub.listeners.get(event) || []) listener(...args);
}

/** 等原生端被记成活：应答轮询是「请求 → 记账 → 下一拍」，看到第 2 个取件请求
 *  就证明第 1 个响应已被插件处理（questionInline 的活性门这时才放行 hold）。 */
async function waitNativeAlive(seen) {
  assert.ok(await waitUntil(() => seen.pendingUrls.length >= 2, 6000),
    '等待轮询把原生端记成活（questionInline 活性门）');
}


// —— B2 内联应答：取件 → 独占 hold 的瀑布 resolve（或官方 answer/steer）→ ack ok → 重推空集 ——
// 注：旧版这里断言的是 `wait.completion.resolve(batch)`。2026-11-16 读包核实那条路
// 是无效的（completion 只是 wait 自己的结束信号），已删；continued 态改走官方
// `ctx.userQuestions.answer()`，所以 now 断言 answer() 被调用、wait 不被碰。
await check('B2: timed 提问（continued）走官方 answer()，不再碰 wait.completion', async () => {
  const sid = 'session-bbbb1111-2222-4333-8444-555555555555';
  const callId = 'call_b2test';
  const { server, seen } = makeAnswerServer([
    { answer_id: 'a-1', session_id: sid, kind: 'question', id: callId,
      answers: [{ id: 'q1', selected: ['甲'] }] },
  ]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const wait = { completion: { resolve: (v) => { wait.resolved = v; } } };
    const calls = [];
    const agent = {
      id: sid,
      session: {},
      inbox: { nextTurn: [], nextStep: [] },
      steer: () => { throw new Error('answer() 认下了就不该再手搓 steer'); },
    };
    const agents = { get: (id) => (id === sid ? agent : undefined) };
    const stub = makeCtx();
    const innerGet = stub.ctx.get;
    stub.ctx.get = (service) => (service === 'agents' ? agents : innerGet(service));
    stub.ctx.userQuestions = {
      waits: new Map([[agent, new Map([[callId, wait]])]]),
      answer: (a, cid, batch) => { calls.push({ a, cid, batch }); return true; },
    };
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false, registerTools: false });
      emitOn(stub, 'session/event', { id: sid }, {
        type: 'tool/call',
        data: { name: 'ask_user_question', callId,
          arguments: JSON.stringify({ questions: [{ id: 'q1', question: '选一个', options: [{ label: '甲' }] }] }) },
      });
      assert.ok(await waitUntil(() => seen.acks.length >= 1), 'ack 未到');
      assert.strictEqual(seen.acks[0].answer_id, 'a-1');
      assert.strictEqual(seen.acks[0].ok, true, JSON.stringify(seen.acks[0]));
      assert.strictEqual(calls.length, 1, '应走官方 answer()');
      assert.strictEqual(calls[0].cid, callId);
      assert.deepStrictEqual(calls[0].batch, { answers: [{ id: 'q1', selected: ['甲'] }] });
      assert.strictEqual(wait.resolved, undefined,
        'wait.completion 不是答案通道，任何情况下都不该被 resolve');
      assert.ok(await waitUntil(() => seen.pushes.some(p => p.session_id === sid && p.requests.length === 0)),
        '应答成功后要重推空集让卡片消失');
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B2: legacy 无 wait → steer user-question-reply 直答（与官方 answer 同形）', async () => {
  const sid = 'session-dddd1111-2222-4333-8444-555555555555';
  const callId = 'call_b2steer';
  const { server, seen } = makeAnswerServer([
    { answer_id: 'a-3', session_id: sid, kind: 'question', id: callId,
      answers: [{ id: 'q1', selected: ['乙'] }] },
  ]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const steered = [];
    const agent = {
      id: sid,
      session: {},
      inbox: { nextTurn: [], nextStep: [] },
      steer: (m) => steered.push(m),
    };
    const agents = { get: (id) => (id === sid ? agent : undefined) };
    const stub = makeCtx();
    const innerGet = stub.ctx.get;
    stub.ctx.get = (service) => (service === 'agents' ? agents : innerGet(service));
    stub.ctx.userQuestions = { waits: new Map() };
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false, registerTools: false });
      assert.ok(await waitUntil(() => seen.acks.length >= 1), 'ack 未到');
      assert.strictEqual(seen.acks[0].ok, true, JSON.stringify(seen.acks[0]));
      assert.strictEqual(steered.length, 1);
      const msg = steered[0];
      assert.strictEqual(msg.role, 'user');
      assert.strictEqual(msg.source.kind, 'user-question-reply');
      assert.strictEqual(msg.source.callId, callId);
      assert.strictEqual(msg.source.outcome, 'answered');
      const payload = JSON.parse(msg.content[0].text);
      assert.strictEqual(payload.kind, 'answer_to_pending_question');
      assert.strictEqual(payload.callId, callId);
      assert.deepStrictEqual(payload.answers, [{ id: 'q1', selected: ['乙'] }]);
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B2: 重复答案被挡 → ack ok:false 带原因', async () => {
  const sid = 'session-eeee1111-2222-4333-8444-555555555555';
  const callId = 'call_b2dup';
  const { server, seen } = makeAnswerServer([
    { answer_id: 'a-4', session_id: sid, kind: 'question', id: callId,
      answers: [{ id: 'q1', selected: ['甲'] }] },
  ]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const agent = {
      id: sid,
      session: {},
      inbox: {
        nextTurn: [{ source: { kind: 'user-question-reply', callId } }],
        nextStep: [],
      },
      steer: () => { throw new Error('不该再 steer'); },
    };
    const agents = { get: (id) => (id === sid ? agent : undefined) };
    const stub = makeCtx();
    const innerGet = stub.ctx.get;
    stub.ctx.get = (service) => (service === 'agents' ? agents : innerGet(service));
    stub.ctx.userQuestions = { waits: new Map() };
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false, registerTools: false });
      assert.ok(await waitUntil(() => seen.acks.length >= 1), 'ack 未到');
      assert.strictEqual(seen.acks[0].ok, false);
      assert.ok(/已经在路上/.test(seen.acks[0].error), seen.acks[0].error);
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

// —— B3 授权内联答（opt-in 独占应答者）——
await check('B3: approvalInline 开启 → 独占应答者 hold，RMC 允许 → resolve allowed-once', async () => {
  const sid = 'session-ffff1111-2222-4333-8444-555555555555';
  const approvalId = 'ap-b3';
  const gate = { open: false };
  const { server, seen } = makeAnswerServer([
    { answer_id: 'a-b3', session_id: sid, kind: 'approval', id: approvalId,
      outcome: 'allowed-once' },
  ], { gate });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const stub = makeCtx();
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false,
        registerTools: false, approvalInline: true });
      // 先让轮询把桌宠记成活（活性门）；答案由 gate 压住，免得抢在 hold 之前被取走。
      await waitNativeAlive(seen);
      // 模拟宿主发起授权瀑布：插件应答者 hold 住（不 next）。
      const handler = (stub.listeners.get('approval/request') || [])[0];
      assert.ok(handler, 'approvalInline 开启应注册 approval/request 应答者');
      let nextCalled = false;
      const outcomePromise = handler(
        { agent: { id: sid }, id: approvalId },
        () => { nextCalled = true; return Promise.resolve('unavailable'); });
      gate.open = true;   // 放行答案：RMC 这时才「点了允许」
      // 推一条 approval/asked 进账（answerable 应为 true）。
      emitOn(stub, 'session/event', { id: sid }, {
        type: 'approval/asked', data: { id: approvalId, toolName: 'pwsh', reason: 'rm' } });
      assert.ok(await waitUntil(() => seen.pushes.some(p =>
        p.session_id === sid && p.requests.some(r => r.kind === 'approval' && r.answerable === true))),
        'approval 推送应带 answerable:true');
      assert.ok(await waitUntil(() => seen.acks.length >= 1), 'ack 未到');
      assert.strictEqual(seen.acks[0].ok, true, JSON.stringify(seen.acks[0]));
      assert.strictEqual(await outcomePromise, 'allowed-once');
      assert.strictEqual(nextCalled, false, '独占应答者不应 next() 放行');
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B3: 瀑布只给 callId（无 id）→ 按 callId 挂 waiter，UUID 应答也能命中', async () => {
  // 2026-11-16 真机实测的真实形状：approval/request 的 request 只有
  // agent/toolName/callId/reason/displayReason/signal，**没有 id**；而
  // approval/asked 与 RMC 应答用的是另一个 approval UUID。老代码读 request.id
  // 得到空串就 return next()，waiter 永远建不起来 → 点允许只会得到「不在等待」。
  // 这个用例钉住：callId 挂 waiter + UUID 经 approval/asked 关联 + 应答命中。
  const sid = 'session-real111-2222-4333-8444-666666666666';
  const callId = 'call_00_realshape';
  const approvalUuid = 'dcc85259-1c5a-439d-99a8-eb10c110e857';
  const gate = { open: false };
  const { server, seen } = makeAnswerServer([
    { answer_id: 'a-real', session_id: sid, kind: 'approval', id: approvalUuid,
      outcome: 'allowed-once' },
  ], { gate });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const stub = makeCtx();
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false,
        registerTools: false, approvalInline: true });
      await waitNativeAlive(seen);
      const handler = (stub.listeners.get('approval/request') || [])[0];
      assert.ok(handler, 'approvalInline 开启应注册 approval/request');
      let nextCalled = false;
      // 关键：只给 callId，没有 id —— 真实形状。
      const outcomePromise = handler(
        { agent: { id: sid }, callId, toolName: 'pwsh', reason: 'rm' },
        () => { nextCalled = true; return Promise.resolve('unavailable'); });
      // approval/asked 报 UUID + 同一个 callId：插件据此把两者关联起来。
      emitOn(stub, 'session/event', { id: sid }, {
        type: 'approval/asked',
        data: { id: approvalUuid, callId, toolName: 'pwsh', reason: 'rm' } });
      gate.open = true;   // 放行答案：RMC 这时才「点了允许」
      assert.ok(await waitUntil(() => seen.acks.length >= 1), 'ack 未到');
      assert.strictEqual(seen.acks[0].ok, true,
        'UUID 应答应命中 callId waiter：' + JSON.stringify(seen.acks[0]));
      assert.strictEqual(await outcomePromise, 'allowed-once');
      assert.strictEqual(nextCalled, false, '独占应答者不应 next() 放行');
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B3: 必须 prepend 注册（否则授权卡点了没反应）', async () => {
  const stub = makeCtx();
  apply(stub.ctx, { port: 1, registerSkill: false, registerTools: false, approvalInline: true });
  const reg = stub.regs.find((r) => r.event === 'approval/request');
  assert.ok(reg, 'approvalInline 开启应注册 approval/request');
  // 卡片是 approval/asked 会话事件推的，跟瀑布无关；不 prepend 就抢不到瀑布，
  // 于是「卡片能弹、点了找不到 waiter」——2026-11-16 真机就是这个问题。
  assert.strictEqual(reg.options && reg.options.prepend, true,
    '授权应答者必须 { prepend: true }，实际 ' + JSON.stringify(reg.options));
  for (const dispose of stub.effects) dispose();
});

await check('B3: 桌宠不在线 → 不 hold，next() 放行给 GUI', async () => {
  const stub = makeCtx();
  apply(stub.ctx, { port: 1, registerSkill: false, registerTools: false, approvalInline: true });
  const handler = (stub.listeners.get('approval/request') || [])[0];
  let nextCalled = false;
  const value = await handler(
    { agent: { id: 'sess-offline' }, id: 'ap-offline' },
    () => { nextCalled = true; return Promise.resolve('unavailable'); });
  assert.strictEqual(nextCalled, true, '桌宠没开时绝不能 hold：授权会永久挂住');
  assert.strictEqual(value, 'unavailable');
  for (const dispose of stub.effects) dispose();
});

await check('B3: 兜底超时到点 → next() 放行给 GUI，不挂死工具调用', async () => {
  const { server, seen } = makeAnswerServer([]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const stub = makeCtx();
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false,
        registerTools: false, approvalInline: true, approvalHoldTimeoutMs: 80 });
      await waitNativeAlive(seen);
      const handler = (stub.listeners.get('approval/request') || [])[0];
      let nextCalled = false;
      const p = handler(
        { agent: { id: 'sess-ap-timeout' }, id: 'ap-timeout' },
        () => { nextCalled = true; return Promise.resolve('unavailable'); });
      assert.strictEqual(nextCalled, false, 'hold 期间不该放行');
      assert.strictEqual(await p, 'unavailable');
      assert.strictEqual(nextCalled, true);
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B3: approvalInline 默认关 → 不注册应答者，answerApproval 明确拒绝', async () => {
  const stub = makeCtx();
  apply(stub.ctx, { port: 1, registerSkill: false, registerTools: false });
  assert.strictEqual((stub.listeners.get('approval/request') || []).length, 0,
    '默认关不应注册 approval/request 应答者');
});

// —— B2 提问·独占模式（questionInline）：hold 瀑布，RMC 答案 resolve，无 GUI 残留 ——
await check('B2独占: questionInline 开启 → hold user-questions/request，RMC 答案 resolve', async () => {
  const sid = 'session-abcd1111-2222-4333-8444-555555555555';
  const callId = 'call-q-excl';
  const gate = { open: false };
  const { server, seen } = makeAnswerServer([
    { answer_id: 'a-qx', session_id: sid, kind: 'question', id: callId,
      answers: [{ id: 'q1', selected: ['甲'] }] },
  ], { gate });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const stub = makeCtx();
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false,
        registerTools: false, questionInline: true });
      // 先让轮询把桌宠记成活（活性门）；答案由 gate 压住，免得抢在 hold 之前被取走。
      await waitNativeAlive(seen);
      const handler = (stub.listeners.get('user-questions/request') || [])[0];
      assert.ok(handler, 'questionInline 开启应注册 user-questions/request 应答者');
      let nextCalled = false;
      const outcomePromise = handler(
        { agent: { id: sid }, questions: [{ id: 'q1' }] },
        () => { nextCalled = true; return Promise.resolve({ answers: [] }); });
      gate.open = true;   // 放行答案：RMC 这时才「点了选择」
      assert.ok(await waitUntil(() => seen.acks.length >= 1), 'ack 未到');
      assert.strictEqual(seen.acks[0].ok, true, JSON.stringify(seen.acks[0]));
      const resolved = await outcomePromise;
      assert.deepStrictEqual(resolved, { answers: [{ id: 'q1', selected: ['甲'] }] });
      assert.strictEqual(nextCalled, false, '独占应答者不应 next() 放行（Web UI 卡不弹）');
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B2独占: 必须 prepend 注册（否则抢不到 GUI 的瀑布转发器）', async () => {
  const stub = makeCtx();
  apply(stub.ctx, { port: 1, registerSkill: false, registerTools: false, questionInline: true });
  const reg = stub.regs.find((r) => r.event === 'user-questions/request');
  assert.ok(reg, 'questionInline 开启应注册 user-questions/request');
  // cordis register(): options.prepend ? unshift : push —— 桌面 GUI 的转发器是普通
  // ctx.on 且轮到它就不再 next()，不 prepend 就永远轮不到我们。
  assert.strictEqual(reg.options && reg.options.prepend, true,
    '提问应答者必须 { prepend: true }，实际 ' + JSON.stringify(reg.options));
  for (const dispose of stub.effects) dispose();
});

await check('B2独占: 默认关 → 不注册提问应答者（保持只做提醒的原行为）', async () => {
  const stub = makeCtx();
  apply(stub.ctx, { port: 1, registerSkill: false, registerTools: false });
  assert.strictEqual((stub.listeners.get('user-questions/request') || []).length, 0,
    '默认关不该注册 user-questions/request 应答者');
  for (const dispose of stub.effects) dispose();
});

await check('B2独占: 桌宠不在线 → 不 hold，next() 放行给 GUI', async () => {
  const stub = makeCtx();
  apply(stub.ctx, { port: 1, registerSkill: false, registerTools: false, questionInline: true });
  const handler = (stub.listeners.get('user-questions/request') || [])[0];
  let nextCalled = false;
  const value = await handler(
    { agent: { id: 'sess-offline' }, questions: [{ id: 'q1' }] },
    () => { nextCalled = true; return Promise.resolve({ answers: [] }); });
  assert.strictEqual(nextCalled, true, '桌宠没开时绝不能 hold：提问会永久挂住');
  assert.deepStrictEqual(value, { answers: [] });
  for (const dispose of stub.effects) dispose();
});

await check('B2独占: hold 期间被取消 → next() 放行，不重复 next', async () => {
  const { server, seen } = makeAnswerServer([]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const stub = makeCtx();
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false,
        registerTools: false, questionInline: true, questionHoldTimeoutMs: 1000 });
      await waitNativeAlive(seen);
      const handler = (stub.listeners.get('user-questions/request') || [])[0];
      const controller = new AbortController();
      let nextCalls = 0;
      const p = handler(
        { agent: { id: 'sess-abort' }, questions: [{ id: 'q1' }], signal: controller.signal },
        () => { nextCalls += 1; return Promise.resolve('deferred'); });
      controller.abort();
      assert.strictEqual(await p, 'deferred');
      await new Promise((resolve) => setTimeout(resolve, 60));
      assert.strictEqual(nextCalls, 1, 'next() 只能被调一次（cordis 再调会抛）');
      for (const dispose of stub.effects) dispose();
    } finally { /* 无 keep 计时器：hold 已在 abort 时收场 */ }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B2独占: 兜底超时到点 → next() 放行给 GUI，不挂死工具调用', async () => {
  const { server, seen } = makeAnswerServer([]);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const stub = makeCtx();
    const keep = setInterval(() => {}, 300);
    try {
      apply(stub.ctx, { port: server.address().port, registerSkill: false,
        registerTools: false, questionInline: true, questionHoldTimeoutMs: 80 });
      await waitNativeAlive(seen);
      const handler = (stub.listeners.get('user-questions/request') || [])[0];
      let nextCalled = false;
      const p = handler(
        { agent: { id: 'sess-timeout' }, questions: [{ id: 'q1' }] },
        () => { nextCalled = true; return Promise.resolve('deferred-to-gui'); });
      assert.strictEqual(nextCalled, false, 'hold 期间不该放行');
      assert.strictEqual(await p, 'deferred-to-gui');
      assert.strictEqual(nextCalled, true);
      assert.deepStrictEqual(seen.acks, [], '没人应答就不该有 ack');
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

await check('B2独占: 同会话两题并存 → 按问题 id 配对，不串答案', async () => {
  const sid = 'session-pair1111-2222-4333-8444-555555555555';
  const gate = { open: false };
  const { server, seen } = makeAnswerServer([
    { answer_id: 'a-pair', session_id: sid, kind: 'question', id: 'call-first',
      answers: [{ id: 'qA', selected: ['甲'] }] },
  ], { gate });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const stub = makeCtx();
    const keep = setInterval(() => {}, 300);
    try {
      // hold 超时调小：第二道题在本用例里靠超时收场，别让测试等 180s；但要
      // 大于一轮取件（1.2s），否则第二道会先超时，配对的断言就没意义了。
      apply(stub.ctx, { port: server.address().port, registerSkill: false,
        registerTools: false, questionInline: true, questionHoldTimeoutMs: 2500 });
      await waitNativeAlive(seen);
      const handler = (stub.listeners.get('user-questions/request') || [])[0];
      let nextCalls = 0;
      const answerNext = () => { nextCalls += 1; return Promise.resolve('deferred'); };
      const first = handler({ agent: { id: sid }, questions: [{ id: 'qA' }] }, answerNext);
      const second = handler({ agent: { id: sid }, questions: [{ id: 'qB' }] }, answerNext);
      gate.open = true;   // 只放行针对 qA 的那条答案
      assert.ok(await waitUntil(() => seen.acks.length >= 1), 'ack 未到');
      assert.strictEqual(seen.acks[0].ok, true, JSON.stringify(seen.acks[0]));
      assert.deepStrictEqual(await first, { answers: [{ id: 'qA', selected: ['甲'] }] },
        '答案必须落在 qA 那一道（列表按问题 id 配对）');
      assert.strictEqual(nextCalls, 0, '另一道不许被串答、也不许放行');
      assert.strictEqual(await second, 'deferred', '第二道到点该自己放行给 GUI');
      assert.strictEqual(nextCalls, 1);
    } finally {
      clearInterval(keep);
      for (const dispose of stub.effects) dispose();
    }
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
