/**
 * AgentRMC 桌宠联动 —— DeepSeek Harness 版（Host 插件）。
 *
 * 原版插件（plugins/agentrmc-bridge）靠宿主的 hooks 干活：宿主在「等授权 / 等选
 * 方案 / 一轮结束」时 fork 一个 node 脚本，脚本自己发 HTTP 给桌宠。DeepSeek
 * Harness 没有那种 hooks.json，事件是进程内的 Cordis Event，所以这里把同一套
 * 语义换成「Host 插件监听事件 + 注册原生工具」，走的还是同一个本机回环口
 * 127.0.0.1:45915，原生端一行不用改。
 *
 * 事件映射（与原版的对照见 README）：
 *   approval/request       ← PermissionRequest  等工具授权
 *   user-questions/request ← PreToolUse 选方案  等选方案
 *   agent/turn-stopping    ← Stop               一轮结束（念最终回复）
 *   agent/inbox/claimed    ← UserPromptSubmit   新对话开始（归档旧验收）
 *
 * 工具：pet_notify / pet_speak / pet_state / pet_work_update（原版走 MCP stdio，
 * 这里直接注册成 Host 原生工具，省掉一个子进程）。
 *
 * 零依赖：不 import 任何 @deepseek-ai 包，装进哪个 profile 都能加载。
 */

import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';

import { createBridge, resolveSpoolDir } from './lib/bridge.js';
import { trimSentence, trailingQuestion } from './lib/text.js';

const PACKAGE_ROOT = path.dirname(fileURLToPath(import.meta.url));

// 缺省值就是「装在 DeepSeek Harness 里、别的什么都不配」时的正确行为。
// cordis.patch.yml 里的 config 是整块替换：写了哪个字段就用哪个，其余仍走这里。
const DEFAULTS = {
  // AgentBridge 端口，与原版 AGENTRMC_PORT 一致。
  port: 45915,
  // 气泡署名 / 念出来的宿主名。
  agentName: 'DeepSeek Harness',
  // 拉前台的进程映像名：原生端按这个名字精确找窗口。
  raiseExe: 'DeepSeek Harness.exe',
  // 单次 HTTP 超时；超时就落盘等补投，绝不让网络拖住宿主。
  requestTimeoutMs: 1500,
  // 离线补投目录；空 = %APPDATA%\LIG\AgentRMCNative（与原版同一个文件）。
  spoolDir: '',
  // 四类自动提醒的开关。
  notifyApproval: true,
  notifyQuestion: true,
  notifyTurnStart: true,
  notifyStop: true,
  // 一轮结束时把最终回复念出来（取不到就退回通用文案）。
  speakFinalReply: true,
  // 注册 pet_* 工具与 agentrmc-pet skill。
  registerTools: true,
  registerSkill: true,
  // 定向派发通道：轮询原生端取件 → agent.followup() 注入 → ack 回执。
  dispatch: true,
  // 只打印将要发送的 payload，不真发（排查用；与 AGENTRMC_DRY_RUN=1 等价）。
  dryRun: false,
};

function readPackageVersion() {
  try {
    const text = fs.readFileSync(path.join(PACKAGE_ROOT, 'package.json'), 'utf8');
    return String(JSON.parse(text).version || '').trim();
  } catch {
    // 版本读数缺失绝不能拖垮提醒本身。
    return '';
  }
}

const PLUGIN_VERSION = readPackageVersion();

function asBool(value, fallback) {
  if (typeof value === 'boolean') return value;
  if (value === undefined || value === null || value === '') return fallback;
  const text = String(value).trim().toLowerCase();
  if (text === 'false' || text === '0' || text === 'no' || text === 'off') return false;
  if (text === 'true' || text === '1' || text === 'yes' || text === 'on') return true;
  return fallback;
}

function asText(value, fallback) {
  const text = value === undefined || value === null ? '' : String(value).trim();
  return text || fallback;
}

function asPort(value, fallback) {
  const port = Number(value);
  return Number.isInteger(port) && port > 0 && port < 65536 ? port : fallback;
}

function asDuration(value, fallback) {
  const ms = Number(value);
  return Number.isFinite(ms) && ms > 0 ? ms : fallback;
}

/** config 没有 schema 校验（本插件不导出 schemastery 的 Config），所以在这里兜底。 */
function normalizeConfig(raw) {
  const input = raw && typeof raw === 'object' ? raw : {};
  const dryRun =
    process.env.AGENTRMC_DRY_RUN === '1' || asBool(input.dryRun, DEFAULTS.dryRun);
  return {
    port: asPort(input.port, asPort(process.env.AGENTRMC_PORT, DEFAULTS.port)),
    agentName: asText(input.agentName, DEFAULTS.agentName),
    raiseExe: asText(input.raiseExe, DEFAULTS.raiseExe),
    requestTimeoutMs: asDuration(input.requestTimeoutMs, DEFAULTS.requestTimeoutMs),
    spoolDir: resolveSpoolDir(asText(input.spoolDir, process.env.AGENTRMC_SPOOL_DIR || '')),
    notifyApproval: asBool(input.notifyApproval, DEFAULTS.notifyApproval),
    notifyQuestion: asBool(input.notifyQuestion, DEFAULTS.notifyQuestion),
    notifyTurnStart: asBool(input.notifyTurnStart, DEFAULTS.notifyTurnStart),
    notifyStop: asBool(input.notifyStop, DEFAULTS.notifyStop),
    speakFinalReply: asBool(input.speakFinalReply, DEFAULTS.speakFinalReply),
    registerTools: asBool(input.registerTools, DEFAULTS.registerTools),
    registerSkill: asBool(input.registerSkill, DEFAULTS.registerSkill),
    dispatch: asBool(input.dispatch, DEFAULTS.dispatch),
    dryRun,
  };
}

function warn(message) {
  console.error('[agentrmc-bridge] ' + message);
}

/** 工具输出统一渲染成一段文本；schema 用空表（不受约束的 JSON），由注册处校验。 */
function renderText(_args, value) {
  return [{ type: 'text', text: typeof value === 'string' ? value : JSON.stringify(value) }];
}

function makeTool(spec) {
  const required = Array.isArray(spec.required) ? spec.required : [];
  return {
    name: spec.name,
    description: spec.description,
    parameters: {
      type: 'object',
      properties: spec.properties || {},
      ...(required.length ? { required } : {}),
      additionalProperties: false,
    },
    output: { schema: {}, render: renderText },
    // 四个工具都是互不依赖的 HTTP 调用，可以和别的工具并行。
    isConcurrencySafe: () => true,
    timeoutMs: 5000,
    // exec 里有调用它的 Agent：session_id 要从这里拿——hook 的 payload 天生带
    // session_id，工具调用没有，只能靠 exec 补，否则工具建的工单没有会话归属，
    // 宿主下一轮新对话时「归档旧验收」就找不到它。
    async execute(args, exec) {
      return spec.execute(args && typeof args === 'object' ? args : {}, exec || {});
    },
  };
}

const SKILL_NAME = 'agentrmc-pet';
const SKILL_DESCRIPTION =
  '与 AgentRMC 桌宠交互：汇报任务阶段进度、让桌宠出声说一句话、查询桌宠状态。' +
  '当用户要求「告诉桌宠 / 让桌宠说 / 汇报进度 / 桌宠在不在」，' +
  '或长任务想在桌宠上留非阻塞提示时使用。注意：等授权、等选方案、等你回话' +
  '（回复以问句收尾）、一轮结束四类提醒' +
  '由插件自动完成，不要用工具重复发。';

/** 读 skill 正文：去掉 SKILL.md 的 YAML frontmatter（元数据已经在 description 里）。 */
function readSkillBody() {
  try {
    const text = fs.readFileSync(
      path.join(PACKAGE_ROOT, 'skills', SKILL_NAME, 'SKILL.md'),
      'utf8',
    );
    return text.replace(/^---\r?\n[\s\S]*?\r?\n---\r?\n/, '').trim();
  } catch {
    return '';
  }
}

/**
 * Host 插件入口。
 * @param {object} ctx Cordis 上下文。
 * @param {object} [rawConfig] cordis.patch.yml 里那一行的 config。
 */
export function apply(ctx, rawConfig) {
  const config = normalizeConfig(rawConfig);
  const bridge = createBridge({
    port: config.port,
    spoolDir: config.spoolDir,
    timeoutMs: config.requestTimeoutMs,
    dryRun: config.dryRun,
    log: warn,
  });

  // 只有 debug 场景才需要每条的判定来源；正常运行时闭嘴。
  if (config.dryRun) {
    warn(
      'dry run | bridge 127.0.0.1:' +
        config.port +
        ' | host ' +
        config.agentName +
        ' | raise ' +
        (config.raiseExe || '(空)') +
        ' | spool ' +
        config.spoolDir,
    );
  }

  /** 拼一条 /notify 的 payload（字段名与原生端 AgentBridge.h 对齐）。 */
  function payloadFor(entry) {
    const payload = { event: entry.event, title: config.agentName, message: entry.message || '' };
    if (entry.speech) payload.speech = entry.speech;
    if (config.raiseExe) payload.raise = config.raiseExe;
    if (entry.sessionId) payload.session_id = String(entry.sessionId);
    if (entry.project) payload.project = String(entry.project);
    if (entry.eventId) payload.event_id = String(entry.eventId);
    if (PLUGIN_VERSION) payload.plugin_version = PLUGIN_VERSION;
    return payload;
  }

  function send(entry) {
    try {
      bridge.notify(payloadFor(entry));
    } catch (error) {
      // 通知是锦上添花，绝不能挡住会话。
      warn('notify failed: ' + (error && error.message));
    }
  }

  /** 会话 id：工具调用没有 hook 那样的 payload，只能从 exec.agent 取。 */
  function sessionOf(agent) {
    return agent && agent.id ? String(agent.id) : '';
  }

  /** 会话 id（从 Session 对象取；不同版本字段名不一样，两个都试）。 */
  function sessionIdOf(session) {
    if (!session) return '';
    const id = session.id || (session.header && session.header.id) || '';
    return id ? String(id) : '';
  }

  /** 会话工作目录：给原生端的「待我处理」分组用，取不到就空着。 */
  function projectFor(session) {
    try {
      if (!session) return '';
      const workingDirectory =
        typeof ctx.get === 'function' ? ctx.get('workingDirectory') : undefined;
      const directory =
        workingDirectory && typeof workingDirectory.get === 'function'
          ? workingDirectory.get(session)
          : '';
      return typeof directory === 'string' ? directory : '';
    } catch {
      return '';
    }
  }

  function projectOf(agent) {
    return projectFor(agent && agent.session);
  }

  // —— 助手流：攒「这一轮最后说了什么」 ——
  // 原版从宿主 transcript 文件里捞最后一条 assistant；DSH 的等价物是
  // agent/assistant-stream。只认 committed 的 assistant/message（即最后一条不带
  // 工具调用的回复），中间的阶段性输出不会被念出来。
  const liveStreams = new Map(); // agentId -> { attemptId, parts: Map<index, text> }
  const lastReply = new Map(); // agentId -> 本轮最后一段正文
  const lastTurnStarted = new Map(); // agentId -> 已上报过的 turn

  // —— B1：待答请求跟踪（只读展示用）——
  // 授权（approval/asked→decided）与提问（ask_user_question→tool/result/reply）的
  // 当前待答集，按会话攒，变化即推给原生端 /requests，聊天页据此渲染卡片。
  // 这里只「看」不「答」（应答是 B2/B3），不碰 waterfall，零风险。
  const pendingRequests = new Map(); // sessionId -> Map(requestId -> request)
  function pushRequests(session) {
    const sid = sessionIdOf(session);
    if (!sid) return;
    const byId = pendingRequests.get(sid);
    bridge.post('/requests', {
      exe: config.raiseExe || '',
      session_id: sid,
      project: projectFor(session),
      requests: byId ? [...byId.values()] : [],
    }).catch(() => {});  // 原生没开就静默；变化/定时再推
  }
  function trackRequest(session, type, data) {
    const sid = sessionIdOf(session);
    if (!sid) return;
    let byId = pendingRequests.get(sid);
    const ensure = () => {
      if (!byId) { byId = new Map(); pendingRequests.set(sid, byId); }
      return byId;
    };
    let changed = false;
    if (type === 'approval/asked' && data.id) {
      ensure().set(data.id, { kind: 'approval', id: data.id,
        toolName: String(data.toolName || ''), reason: String(data.reason || '') });
      changed = true;
    } else if (type === 'approval/decided' && data.id) {
      if (byId && byId.delete(data.id)) changed = true;
    } else if ((type === 'tool/call' || type === 'tool/ptc-dispatch') &&
               data.name === 'ask_user_question') {
      const callId = data.callId || data.subCallId;
      let questions = [];
      try {
        const args = typeof data.arguments === 'string'
          ? JSON.parse(data.arguments) : (data.arguments || {});
        questions = (args.questions || []).map((q) => ({
          id: q.id, question: q.question || '', header: q.header || '',
          options: (q.options || []).map((o) => ({
            label: o.label || '', description: o.description || '' })),
          multi_select: !!q.multi_select,
        }));
      } catch (e) { /*  arguments 解析失败仍记一个空提问卡 */ }
      if (callId) {
        ensure().set(callId, { kind: 'question', id: callId, questions });
        changed = true;
      }
    } else if (type === 'tool/result') {
      const callId = data.message && data.message.toolCallId;
      if (callId && byId && byId.get(callId) &&
          byId.get(callId).kind === 'question') {
        byId.delete(callId);
        changed = true;
      }
    } else if (type === 'user/message' && data.source &&
               data.source.kind === 'user-question-reply') {
      const callId = data.source.callId;
      if (callId && byId && byId.delete(callId)) changed = true;
    }
    if (changed) pushRequests(session);
  }

  if (typeof ctx.on === 'function') {
    // 等授权 / 等选方案：挂**持久会话事件**，不挂那两个瀑布。
    //
    //   approval/asked   授权服务在问人之前先落这条审计事件
    //   tool/call        ask_user_question 被调用时落这条
    //
    // 为什么不监听 approval/request 与 user-questions/request：Web UI 的应答者
    // 本身就是这两个瀑布的监听器，它一旦作答就不再调 next()，排在它后面的监听器
    // 永远轮不到（实测：桌面端点完选方案，桌宠一声没响，/notify 一条没到）。
    // session/event 是 emit，所有监听器都会跑，顺序无关。
    ctx.on(
      'session/event',
      (session, event) => {
        try {
          const type = event && event.type;
          const data = (event && event.data) || {};
          // 可选事件追踪（默认关）：设 AGENTRMC_TRACE_EVENTS=<文件路径> 后，把每条
          // session/event 追加进去，用于排查「选方案/等授权偶发不发声」——监听器
          // 偶尔停收事件时，对照日志即可区分是事件没发还是通知没送。不开则零开销。
          if (process.env.AGENTRMC_TRACE_EVENTS) {
            try {
              fs.appendFileSync(process.env.AGENTRMC_TRACE_EVENTS,
                JSON.stringify({ t: Date.now(), type, name: data && data.name,
                  callId: data && data.callId, sid: sessionIdOf(session) }) + '\n');
            } catch (e) {}
          }
          // B1：先跟踪待答请求（与通知开关无关，展示始终可用），再发通知。
          trackRequest(session, type, data);
          if (type === 'approval/asked') {
            if (!config.notifyApproval) return;
            const toolName = String(data.toolName || '').trim();
            send({
              event: 'permission',
              // 气泡带工具名（"等你确认 Bash"看着有用），语音只带宿主名——
              // 「等你确认 Bash 冒号 D 冒号反斜杠…」念出来没人听得懂。
              message: toolName ? '等你确认 ' + toolName : '等你确认工具授权',
              speech: config.agentName + ' 在等你点确认',
              sessionId: sessionIdOf(session),
              project: projectFor(session),
              eventId: data.id,
            });
            return;
          }
          if (!config.notifyQuestion) return;
          const isQuestionCall =
            (type === 'tool/call' || type === 'tool/ptc-dispatch') &&
            data.name === 'ask_user_question';
          if (!isQuestionCall) return;
          send({
            event: 'question',
            // 与原版一致：气泡和语音都只说「等你选方案」。问题原文可能很长，
            // 塞进来既念不完也看不清——用户看屏幕上的卡片就行。
            message: '等你选方案',
            speech: config.agentName + ' 在等你选方案',
            sessionId: sessionIdOf(session),
            project: projectFor(session),
            eventId: data.callId || data.subCallId,
          });
        } catch (error) {
          warn('session event notify failed: ' + (error && error.message));
        }
      },
      { global: true },
    );

    // 新一轮对话开始：原生端把同一 session 旧的「待验收」归档，不出声。
    ctx.on(
      'agent/inbox/claimed',
      (payload) => {
        try {
          const agent = payload && payload.agent;
          const turn = Number(payload && payload.turn);
          if (!agent || !agent.id) return;
          if (lastTurnStarted.get(agent.id) === turn) return; // 同一轮多条消息只报一次
          lastTurnStarted.set(agent.id, turn);
          if (!config.notifyTurnStart) return;
          send({
            event: 'turn-start',
            message: '',
            sessionId: agent.id,
            project: projectOf(agent),
          });
        } catch (error) {
          warn('turn-start notify failed: ' + (error && error.message));
        }
      },
      { global: true },
    );

    // 攒正文。
    ctx.on(
      'agent/assistant-stream',
      (payload) => {
        try {
          const agent = payload && payload.agent;
          const frame = payload && payload.frame;
          if (!agent || !agent.id || !frame) return;
          if (frame.type === 'start') {
            liveStreams.set(agent.id, { attemptId: frame.attemptId, parts: new Map() });
            return;
          }
          const live = liveStreams.get(agent.id);
          if (!live || live.attemptId !== frame.attemptId) return;
          if (frame.type === 'chunk') {
            const chunk = frame.chunk || {};
            if (chunk.type === 'text-delta') {
              live.parts.set(chunk.index, (live.parts.get(chunk.index) || '') + chunk.text);
            } else if (
              chunk.type === 'block-end' &&
              chunk.block &&
              chunk.block.type === 'text' &&
              typeof chunk.block.text === 'string'
            ) {
              // block-end 带的是完整正文，比拼 delta 可靠，覆盖掉同一 index 的累计值。
              live.parts.set(chunk.index, chunk.block.text);
            }
            return;
          }
          if (frame.type === 'end') {
            const outcome = frame.outcome || {};
            if (outcome.kind === 'committed' && outcome.eventType === 'assistant/message') {
              const text = [...live.parts.entries()]
                .sort((a, b) => a[0] - b[0])
                .map(([, part]) => part)
                .join(' ');
              if (text.trim()) lastReply.set(agent.id, text);
            }
            liveStreams.delete(agent.id);
          }
        } catch (error) {
          warn('assistant-stream failed: ' + (error && error.message));
        }
      },
      { global: true },
    );

    // 一轮结束：念最终回复，并把气泡留着（原生端 stop 分支不抢前台，气泡可点）。
    // 例外：回复以问句收尾 = 这轮没结束，球在人这边 → 改发「等你回话」走 permission
    // （出声 + 抢前台 + 待我处理卡）。正文提问没有选项卡片，宿主不会为它派发任何
    // 等待事件，只能从攒下来的回复文字判——和 bridge 版 Stop 分支同一套规则。
    ctx.on(
      'agent/turn-stopping',
      (payload) => {
        try {
          const agent = payload && payload.agent;
          if (!agent || !agent.id || !config.notifyStop) return;
          const raw = lastReply.get(agent.id) || '';
          lastReply.delete(agent.id);
          const ask = config.speakFinalReply ? trailingQuestion(raw) : '';
          if (ask) {
            send({
              event: 'permission',
              // 不能写「等你选方案」开头：原生端按这个前缀判是不是选方案，套 180s
              // 迟到去重，这里没有选项卡片，被吞掉就白喊了。
              message: '等你回话：' + ask,
              speech: config.agentName + ' 在等你回话',
              sessionId: agent.id,
              project: projectOf(agent),
            });
            return;
          }
          const said = config.speakFinalReply ? trimSentence(raw) : '';
          send({
            event: 'stop',
            message: said ? config.agentName + '：' + said : config.agentName + ' 做完了，等你回来看',
            sessionId: agent.id,
            project: projectOf(agent),
          });
        } catch (error) {
          warn('stop notify failed: ' + (error && error.message));
        }
      },
      { global: true },
    );

    // 会话销毁：清掉按 agent 攒的状态，长时间运行不会越攒越多。
    ctx.on(
      'agent/disposed',
      (payload) => {
        const agent = payload && payload.agent;
        if (!agent || !agent.id) return;
        liveStreams.delete(agent.id);
        lastReply.delete(agent.id);
        lastTurnStarted.delete(agent.id);
        if (pendingRequests.delete(agent.id)) {
          bridge.post('/requests', { exe: config.raiseExe || '',
            session_id: agent.id, project: '', requests: [] }).catch(() => {});
        }
      },
      { global: true },
    );
  }

  // —— 工具：与原版 MCP server 的四个工具一一对应 ——
  async function callBridge(route, payload) {
    try {
      const result = payload ? await bridge.post(route, payload) : await bridge.get(route);
      return result;
    } catch (error) {
      throw new Error(
        'AgentRMC 没连上（' +
          (error && error.message) +
          '）。请确认 AgentRMC 正在运行；没有它，桌宠交互工具不可用，' +
          '但自动提醒本来就只在它运行时才有意义。',
      );
    }
  }

  const tools = [
    makeTool({
      name: 'pet_notify',
      description:
        '在 AgentRMC 桌宠上冒一条气泡提示（不出声）。适合长任务的阶段进度、' +
        '中途发现的重要信息等非阻塞提醒；等授权、等选方案、等你回话（回复以问句收尾）、' +
        '一轮结束的提醒由插件自动发，' +
        '不要用这个工具重复发。message 一句话以内，太长的信息用户来不及看。',
      properties: {
        message: { type: 'string', description: '气泡文案，一句话以内' },
        event: { type: 'string', description: '可选事件标记（如 progress），只影响事件流归类' },
      },
      required: ['message'],
      async execute(args, exec) {
        const message = String(args.message || '').trim();
        if (!message) throw new Error('message 不能为空');
        const sessionId = sessionOf(exec.agent);
        const project = projectOf(exec.agent);
        const res = await callBridge('/notify', {
          event: String(args.event || 'progress'),
          title: config.agentName,
          message,
          ...(config.raiseExe ? { raise: config.raiseExe } : {}),
          ...(sessionId ? { session_id: sessionId } : {}),
          ...(project ? { project } : {}),
          ...(PLUGIN_VERSION ? { plugin_version: PLUGIN_VERSION } : {}),
        });
        if (res.status >= 500) {
          throw new Error('AgentRMC 暂时出错（HTTP ' + res.status + '），气泡没发出去');
        }
        return '已发送（HTTP ' + res.status + '）';
      },
    }),
    makeTool({
      name: 'pet_speak',
      description:
        '让 AgentRMC 桌宠说一句话：冒气泡并用 TTS 念出来（用户开 TTS 时出声）。' +
        '适合把最终结论、需要用户立刻注意的事念给用户。一次一句，别连续调用刷屏。',
      properties: {
        message: { type: 'string', description: '要说的话，一句话以内' },
      },
      required: ['message'],
      async execute(args) {
        const message = String(args.message || '').trim();
        if (!message) throw new Error('message 不能为空');
        const res = await callBridge('/speak', { message });
        if (res.status >= 500) {
          throw new Error('AgentRMC 暂时出错（HTTP ' + res.status + '），这句话没说出去');
        }
        return '已让桌宠说（HTTP ' + res.status + '）';
      },
    }),
    makeTool({
      name: 'pet_state',
      description:
        '查询 AgentRMC 桌宠当前状态：pet_visible/pet_enabled（桌宠可见/开关）、' +
        'docked（贴边收纳）、listening/recording/transcribing（语音监听/录音/转写中）、' +
        'chat_mode（对话模式）、tts_ready（TTS 可用）。想在桌宠上提示前可先查一眼。',
      properties: {},
      async execute() {
        const res = await callBridge('/state', null);
        return res.body || '{}';
      },
    }),
    makeTool({
      name: 'pet_work_update',
      description:
        '把当前任务的阶段汇报给 AgentRMC 的「待我处理」列表：' +
        'progress（正在干）、blocked（卡住了，等人）、ready（做完了，等验收）。' +
        'progress 只更新工单不出声；blocked 和 ready 会冒气泡并用语音提醒用户' +
        '（要人动手的事必须喊到），所以别拿它们当进度条刷——阶段真的变了才报。' +
        '想额外说一句话请用 pet_speak。',
      properties: {
        stage: {
          type: 'string',
          enum: ['progress', 'blocked', 'ready'],
          description: 'progress 正在干 / blocked 卡住等人 / ready 做完等验收',
        },
        message: {
          type: 'string',
          description: '一句话说明在做什么、卡在哪、做完了什么；省略时按阶段给默认文案',
        },
        project: {
          type: 'string',
          description: '可选，工作目录，用来区分同名的多个任务',
        },
      },
      required: ['stage'],
      async execute(args, exec) {
        const stage = String(args.stage || '').trim().toLowerCase();
        if (stage !== 'progress' && stage !== 'blocked' && stage !== 'ready') {
          throw new Error('stage 只支持 progress / blocked / ready');
        }
        const sessionId = sessionOf(exec.agent);
        const res = await callBridge('/work-items/update', {
          stage,
          message: String(args.message || '').trim(),
          title: config.agentName,
          // 进程映像名直接给：原生端按它把工单归到 Worker 行。
          agent_exe: config.raiseExe || '',
          raise: config.raiseExe || '',
          // 会话归属：宿主下一轮新对话（turn-start）靠它归档旧的「待验收」。
          ...(sessionId ? { session_id: sessionId } : {}),
          project: String(args.project || projectOf(exec.agent) || '').trim(),
          ...(PLUGIN_VERSION ? { plugin_version: PLUGIN_VERSION } : {}),
        });
        if (res.status >= 400) {
          throw new Error(
            'AgentRMC 拒绝了这次汇报（HTTP ' + res.status + '）：' + (res.body || ''),
          );
        }
        return '已汇报 stage=' + stage + '（HTTP ' + res.status + '）' + (res.body || '');
      },
    }),
  ];

  if (config.registerTools && typeof ctx.tools?.register === 'function') {
    ctx.effect(
      () => {
        const disposers = tools.map((tool) => ctx.tools.register(tool));
        return () => {
          for (const dispose of disposers) {
            try {
              dispose();
            } catch (error) {
              warn('tool dispose failed: ' + (error && error.message));
            }
          }
        };
      },
      'agentrmc-bridge tools',
    );
  }

  // —— skill：教模型什么时候该用这几个工具、怎么不刷屏 ——
  // 装了 skills 服务的 profile 才有；没有就跳过，不影响工具与提醒。
  if (config.registerSkill) {
    try {
      const skills = typeof ctx.get === 'function' ? ctx.get('skills') : undefined;
      const body = readSkillBody();
      if (skills && typeof skills.register === 'function' && body) {
        ctx.effect(
          () =>
            skills.register({
              name: SKILL_NAME,
              description: SKILL_DESCRIPTION,
              content: body,
              source: 'custom',
              provider: 'agentrmc-bridge',
              invocation: { modelInvocable: true, userInvocable: true },
              resourceBase: {
                kind: 'directory',
                path: path.join(PACKAGE_ROOT, 'skills', SKILL_NAME),
              },
            }),
          'agentrmc-bridge skill',
        );
      }
    } catch (error) {
      warn('skill registration skipped: ' + (error && error.message));
    }
  }

  // —— 定向派发通道（2026-11-07 接线）：原生端下发 → 本插件轮询取件 →
  // agent.followup() 注入 → ack 回执。followup 同步落 durable inbox，ack ok
  // 就是「确实到了那条会话」的回执（与 WorkBuddy 的 jsonl user 行同级证明）；
  // 守卫在取件后：agents.get(session_id) 命中才注入——证明不了目标就不发，
  // 拒绝原因走 ack ok:false，原生端按派发失败上卡（裁决 6）。
  let dispatchBusy = false;
  let dispatchLogAt = 0;
  if (config.dispatch && typeof ctx.get === 'function') {
    const pollDispatches = async () => {
      if (dispatchBusy) return;
      dispatchBusy = true;
      try {
        const res = await bridge.get(
          '/dispatch/pending?exe=' + encodeURIComponent(config.raiseExe || ''),
        );
        if (res.status !== 200) return; // 旧原生端没有这条路由：安静跳过
        let items;
        try {
          items = JSON.parse(res.body || '[]');
        } catch {
          return;
        }
        if (!Array.isArray(items) || items.length === 0) return;
        const agents = ctx.get('agents');
        for (const item of items) {
          const dispatchId = String((item && item.dispatch_id) || '');
          const sessionId = String((item && item.session_id) || '');
          const text = String((item && item.text) || '');
          let error = '';
          if (!agents || typeof agents.get !== 'function') {
            error = 'agents 服务不可达（ctx 拿不到，DSH 版本不匹配？）';
          } else if (!dispatchId || !sessionId || !text) {
            error = '取件缺字段（dispatch_id/session_id/text）';
          } else {
            const agent = agents.get(sessionId);
            if (!agent || typeof agent.followup !== 'function') {
              error =
                '会话 ' + sessionId.slice(0, 13) + '… 不在活表里（未打开），未注入';
            } else {
              // 手搓 UserMessage，与 ACP 桥同形（kind:'user' 源 + 单 text 块），
              // 不 import 任何 @deepseek-ai 包。
              try {
                agent.followup({
                  id: randomUUID(),
                  role: 'user',
                  content: [{ type: 'text', text }],
                  source: { kind: 'user' },
                });
              } catch (e) {
                error = 'followup 被拒：' + ((e && e.message) || e);
              }
            }
          }
          try {
            await bridge.post('/dispatch/ack', {
              dispatch_id: dispatchId,
              ok: error === '',
              ...(error ? { error } : {}),
            });
          } catch (e) {
            // ack 发不回去：原生端 60s 取件超时会兜底取消，这里节流记一笔。
            const now = Date.now();
            if (now - dispatchLogAt > 60_000) {
              dispatchLogAt = now;
              warn('dispatch ack failed: ' + ((e && e.message) || e));
            }
          }
        }
      } catch (e) {
        // 原生端没开：轮询失败保持安静，60s 记一次，别把 Host 日志刷满。
        const now = Date.now();
        if (now - dispatchLogAt > 60_000) {
          dispatchLogAt = now;
          warn('dispatch poll failed: ' + ((e && e.message) || e));
        }
      } finally {
        dispatchBusy = false;
      }
    };
    const dispatchTimer = setInterval(pollDispatches, 1500);
    // 别撑住宿主/测试进程的事件循环：通道纯靠原生端下发，不需要保活。
    if (typeof dispatchTimer.unref === 'function') dispatchTimer.unref();
    if (typeof ctx.effect === 'function') {
      ctx.effect(
        () => () => clearInterval(dispatchTimer),
        'agentrmc-bridge dispatch poll',
      );
    }
  }

  // B1：定时重推待答请求（原生端重启后恢复展示），不依赖 dispatch 开关。
  if (typeof ctx.effect === 'function') {
    const reqTimer = setInterval(() => {
      for (const [sid, byId] of pendingRequests) {
        bridge.post('/requests', { exe: config.raiseExe || '', session_id: sid,
          project: '', requests: [...byId.values()] }).catch(() => {});
      }
    }, 10000);
    if (typeof reqTimer.unref === 'function') reqTimer.unref();
    ctx.effect(() => () => clearInterval(reqTimer), 'agentrmc-bridge requests re-push');
  }
}

export const name = 'agentrmc-bridge';
// 没有 tools 服务的 profile 里不激活（而不是抛错）；skills 是可选的，用 ctx.get 取。
export const inject = ['tools'];

export const version = PLUGIN_VERSION;
