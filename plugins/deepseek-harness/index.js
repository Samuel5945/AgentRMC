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
  // B3 授权内联答（opt-in，默认关）：开启后插件成为 approval/request 瀑布的
  // 独占应答者——授权改由 RMC 聊天页允许/拒绝，桌面 Web UI 的授权卡不再弹。
  // 默认关保持安全只读（进程内无按 id 应答口，硬抢瀑布会卡住授权）。
  approvalInline: false,
  // 提问内联答·独占模式（opt-in，默认关）：开启后插件用 **prepend** 抢在桌面 GUI
  // 的转发器之前 hold 住 user-questions/request 瀑布，提问改由 RMC 聊天页/桌宠卡
  // 作答，答案**当轮**回给工具调用（真同步），桌面 Web UI 提问卡不再弹。
  // 默认关：不做应答者，答案只能走 steer 旁路（那条路对 legacy 提问救不了——
  // 工具调用正卡在瀑布上，steer 的用户消息要等下一步边界才被认领）。
  // 两道安全门见 questionHoldTimeoutMs 与 nativeAlive()。
  questionInline: false,
  // hold 提问的兜底时长：到点还没等到 RMC 的答案就 next() 放行给桌面 GUI 的
  // 转发器（这时 GUI 提问卡才弹），绝不把工具调用挂死。RMC 之后再答会落到
  // steer 旁路（变成一条普通用户消息，不会重复扣卡）。
  questionHoldTimeoutMs: 180000,
  // hold 授权的兜底时长：同理——到点放行给桌面 GUI（授权卡这时才弹），
  // 绝不把工具调用挂死。
  approvalHoldTimeoutMs: 180000,
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
    approvalInline: asBool(input.approvalInline, DEFAULTS.approvalInline),
    questionInline: asBool(input.questionInline, DEFAULTS.questionInline),
    questionHoldTimeoutMs: asDuration(
      input.questionHoldTimeoutMs,
      DEFAULTS.questionHoldTimeoutMs,
    ),
    approvalHoldTimeoutMs: asDuration(
      input.approvalHoldTimeoutMs,
      DEFAULTS.approvalHoldTimeoutMs,
    ),
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
    // 推了请求卡的事件带 bubble:false：气泡收敛进卡里，不弹第二个浮层。
    if (entry.bubble === false) payload.bubble = false;
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

  // B3 授权内联答的 waiter 表。声明放在 trackRequest 之前：后者（会话事件
  // 监听）要用它把 approval/asked 的 UUID 关联到瀑布按 callId 挂的 waiter 上。
  // `${sid}|${callId}` -> { finish }
  const approvalWaiters = new Map();
  // `${sid}|${approvalUuid}` -> waiter entry（两个 id 不同源，见下方注释）
  const approvalAlias = new Map();

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
  // 「等你选方案」带上题干：气泡只有一句泛话时，人得切回窗口才知道要拍什么板。
  // 题干就在 ask_user_question 的 arguments.questions[0].question 里，截 40 字；
  // 解析不出（形状变了 / arguments 不是合法 JSON）退回泛化文案。
  const QUESTION_STEM_CAP = 40;
  function questionStemOf(data) {
    let args = data && data.arguments;
    if (typeof args === 'string') {
      try { args = JSON.parse(args); } catch { return ''; }
    }
    const qs = args && Array.isArray(args.questions) ? args.questions : [];
    const text = String((qs[0] && (qs[0].question || qs[0].header)) || '')
      .replace(/\s+/g, ' ').trim();
    if (!text) return '';
    return text.length > QUESTION_STEM_CAP
      ? text.slice(0, QUESTION_STEM_CAP - 1) + '…'
      : text;
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
        toolName: String(data.toolName || ''), reason: String(data.reason || ''),
        answerable: !!config.approvalInline });
      changed = true;
      // 把瀑布挂的 waiter（按 callId）关联到这个 approval UUID：RMC 应答带的是
      // UUID，瀑布只给 callId，两个 id 不同源（2026-11-16 真机实测）。
      const askCallId = data.callId || data.subCallId;
      if (askCallId) {
        const entry = approvalWaiters.get(sid + '|' + askCallId);
        if (entry) approvalAlias.set(sid + '|' + data.id, entry);
      }
    } else if (type === 'approval/decided' && data.id) {
      if (byId && byId.delete(data.id)) changed = true;
      const decided = approvalAlias.get(sid + '|' + data.id);
      if (decided) approvalAlias.delete(sid + '|' + data.id);
      approvalWaiters.delete(sid + '|' + data.id);
      if (data.callId) approvalWaiters.delete(sid + '|' + data.callId);
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
        ensure().set(callId, { kind: 'question', id: callId, questions, answerable: true });
        changed = true;
      }
    } else if (type === 'tool/result') {
      const callId = data.message && data.message.toolCallId;
      if (callId && byId && byId.get(callId) &&
          byId.get(callId).kind === 'question') {
        byId.delete(callId);
        changed = true;
      }
      questionWaiters.delete(sid);  // 题已结（别处答/取消），独占 waiter 清账
    } else if (type === 'user/message' && data.source &&
               data.source.kind === 'user-question-reply') {
      const callId = data.source.callId;
      questionWaiters.delete(sid);
      if (callId && byId && byId.delete(callId)) changed = true;
    }
    if (changed) pushRequests(session);
  }
  // 会话 id → session：应答落账后重推该会话请求集要拿原始 session（同对象）。
  // session/event 是 global 监听、所有 (session, event) 都路过，顺手建索引；
  // agent/disposed 清账（无事件可挂）。
  const sessionsById = new Map();

  // B2：内联应答。
  //
  // 谁在 user-questions/request 瀑布链上靠前，谁就能独占这道提问：桌面 GUI 的
  // 转发器（dsh-api-remotes 把这条瀑布转发给浏览器）是普通 ctx.on，一旦轮到它
  // 就把请求塞进远程队列、**不再 next()**（GUI 提问卡因此弹出，答案在浏览器里给）。
  // 插件要抢在它前面，靠的是 `{ prepend: true }`——cordis register() 里
  // `options.prepend ? unshift : push`，与注册先后无关（2026-11-16 读包核实：
  // cordis/lib/types/events.js 的 register、dsh-scope 的 scopeTarget——未打
  // scope 标签的监听器一定放行，所以 questionInline 抢不到不是 scope 问题，是顺序）。
  //
  // 直答路（2026-11-14 读包核实官方 answer 的落点）：一条
  // source.kind="user-question-reply" 的用户消息 steer 进 agent——Web UI 卡片
  // 经投影监听同一事件收卡，agent 认领 reply 当作 ask_user_question 的答复。
  // 官方 uq.answer() 只认 continued 态（timed 提问超时后才有），legacy 提问的
  // fold 不跟踪 → 自己 steer 同形消息；重复答案先查 inbox（官方 REPLY_QUEUED 同款判据）。
  //
  // 旧版这里还有一条「① 有 timed wait 就 wait.completion.resolve(batch)」——
  // 2026-11-16 读包核实那是**无效**的：TimedQuestionWait.completion 只是 wait
  // 自己的结束信号（close() 里 resolve，`done` 给 NO_PROVIDER 分支 await），
  // 答案通道只有 ask() 派发的 waterfall。留着它会把答案吞掉还回 ok:true
  // （原生端显示已答、会话其实还在等），所以整条删掉，改走下面的官方 answer。
  //
  /** ctx.userQuestions：宿主 compose 期把服务实例挂到 ctx 上；兼容多种拿法。 */
  function userQuestionsOf() {
    return ctx.userQuestions ||
      (ctx.get ? ctx.get('userQuestions') : undefined) ||
      (ctx.services ? ctx.services.userQuestions : undefined);
  }
  // —— 原生端活性：决定要不要 hold 提问 ——
  // 提问一旦 hold 住，只有 RMC 能答（GUI 卡被我们挡在后面），所以「桌宠在不在」
  // 必须先确认，否则提问会永久挂住。活性信号只取**轮询成功**记账（应答轮询 1.2s
  // 一发、派发轮询 1.5s 一发），不做现场探测：hold 的注册必须是同步的，一旦在
  // 瀑布里等一次网络往返，原生端抢在那几毫秒里送回来的答案就会因为「还没有
  // waiter」被当成旁路答案丢掉。桌宠不在线时轮询不会成功，缓存自然过期 → 不 hold。
  const NATIVE_ALIVE_MS = 15000;
  let nativeAliveAt = 0;
  function markNativeAlive() { nativeAliveAt = Date.now(); }
  function nativeAlive() { return Date.now() - nativeAliveAt < NATIVE_ALIVE_MS; }
  // 提问·独占模式（questionInline）：插件 hold 住 user-questions/request 瀑布，
  // 桌面 GUI 拿不到请求→提问卡不弹→RMC 的答案当轮回给工具调用（真同步、无残留）。
  // 按会话攒 waiter（legacy 提问的 waterfall request 不带 callId，只能用问题 id
  // 配）。同一会话可能同时 hold 多道题（桌宠现在是待答队列，多张卡都能答），
  // 所以这里是**列表**：答复批里 answers[].id 就是提问的 question id，用它配对，
  // 配不上才退回「只有一条就用它」的老行为。
  const questionWaiters = new Map(); // sid -> [{ finish, ids:Set }]
  function pickQuestionWaiter(sessionId, answers) {
    const list = questionWaiters.get(sessionId);
    if (!list || !list.length) return null;
    const ids = (answers || []).map((answer) => answer && answer.id).filter(Boolean);
    if (ids.length) {
      const exact = list.find((entry) => ids.every((id) => entry.ids.has(id)));
      if (exact) return exact;
    }
    return list.length === 1 ? list[0] : null;
  }
  if (config.questionInline && typeof ctx.on === 'function') {
    ctx.on('user-questions/request', (request, next) => {
      const sid = request && request.agent && request.agent.id;
      if (!sid) return next();
      if (!nativeAlive()) return next();  // 桌宠不在线：让给 GUI 转发器（同步判断）
      return new Promise((resolve) => {
        let settled = false;
        let timer;
        const entry = {
          finish: null,
          ids: new Set((request.questions || []).map((q) => q && q.id).filter(Boolean)),
        };
        const release = () => {
          // 只删自己的那条：同一会话多题并存时别把别人的 waiter 误删。
          const list = questionWaiters.get(sid);
          if (list) {
            const at = list.indexOf(entry);
            if (at >= 0) list.splice(at, 1);
            if (!list.length) questionWaiters.delete(sid);
          }
          clearTimeout(timer);
          if (request.signal) request.signal.removeEventListener('abort', onAbort);
        };
        // 三种收场都只走一次：RMC 答案 / 取消 / 兜底超时（后两者 next() 放行给 GUI）。
        const finish = (value) => {
          if (settled) return;
          settled = true;
          release();
          resolve(value);
        };
        entry.finish = finish;
        const onAbort = () => finish(next());
        timer = setTimeout(() => finish(next()), config.questionHoldTimeoutMs);
        const list = questionWaiters.get(sid) || [];
        list.push(entry);
        questionWaiters.set(sid, list);
        if (request.signal) {
          if (request.signal.aborted) return onAbort();
          request.signal.addEventListener('abort', onAbort, { once: true });
        }
      });
    }, { prepend: true });
  }
  async function answerQuestion(sessionId, callId, answers) {
    const batch = { answers: answers || [] };
    // ⓪ 独占模式：直接 resolve 插件 hold 住的瀑布——答案当轮回给工具调用，
    // GUI 提问卡根本没弹（无「旁路答完 GUI 卡残留」）。同会话多道题并存时按
    // 问题 id 配对，别把 A 的答案塞给 B。
    if (config.questionInline) {
      const w = pickQuestionWaiter(sessionId, batch.answers);
      if (w) {
        w.finish(batch);
        return { ok: true };
      }
      // 没 hold 住（活性门没过 / 已超时放行 / 注册时序）→ 落回下面旁路。
    }
    // ① 主路：steer user-question-reply（与官方 answer 同形同落点）。
    try {
      const agents = ctx.get('agents');
      const agent = agents && agents.get(sessionId);
      if (!agent) {
        return { ok: false, error: '会话不在活表里（未打开？），未应答' };
      }
      if (typeof agent.steer !== 'function' || !agent.inbox) {
        return { ok: false, error: 'Agent 接口不认识（DSH 版本变了？），未应答' };
      }
      // ①-a timed 提问超时后处于 continued 态：官方 answer 是正路（它自己 steer
      // 一条同形消息并记账 REPLY_QUEUED）。legacy 提问 fold 不跟踪 → 返回 false，
      // 落到 ①-b 手搓的同一形态。
      const uq = userQuestionsOf();
      if (uq && typeof uq.answer === 'function') {
        try {
          if (uq.answer(agent, callId, batch)) return { ok: true };
        } catch (e) {
          const code = e && e.code;
          if (code === 'REPLY_QUEUED') {
            return { ok: false, error: '这条回答已经在路上了（别重复提交）' };
          }
          if (code === 'BAD_ANSWER') {
            return { ok: false, error: '答案条目和提问对不上：' + ((e && e.message) || e) };
          }
          // 其它（版本不符等）→ 落回 ①-b。
        }
      }
      // ①-b 手搓直答：重复答案先查 inbox（官方 REPLY_QUEUED 同款判据）。
      const queued = (m) => m && m.source && m.source.kind === 'user-question-reply' &&
        m.source.callId === callId;
      const inTurn = agent.inbox.nextTurn && agent.inbox.nextTurn.some(queued);
      const inStep = agent.inbox.nextStep && agent.inbox.nextStep.some(queued);
      if (inTurn || inStep) {
        return { ok: false, error: '这条回答已经在路上了（别重复提交）' };
      }
      const payload = {
        kind: 'answer_to_pending_question',
        tool: 'ask_user_question',
        callId,
        answers: batch.answers,
      };
      // 带上问题原文（照 Web UI 的 reply 内容形）；拿不到也照送，答案里有 id。
      try {
        const continued = uq && typeof uq.continued === 'function'
          ? uq.continued(agent) : undefined;
        const entry = Array.isArray(continued)
          ? continued.find((q) => q.callId === callId) : undefined;
        if (entry && entry.questions) payload.questions = entry.questions;
      } catch (e) { /* 问题原文可选 */ }
      agent.steer({
        id: randomUUID(),
        role: 'user',
        content: [{ type: 'text', text: JSON.stringify(payload) }],
        source: { kind: 'user-question-reply', callId, outcome: 'answered' },
      });
      return { ok: true };
    } catch (e) {
      return { ok: false, error: '应答失败：' + ((e && e.message) || e) };
    }
  }
  // B3 授权内联答（opt-in 独占）：进程内 Cordis approval 服务只有 request()、
  // 无按 id 应答口；桌面 GUI 自己是 approval/request 瀑布的转发者/应答者。开启
  // approvalInline 后插件用 **prepend** 抢在转发器前面 hold 住请求，等 RMC
  // 聊天页的允许/拒绝（pull 模型成立——我们是应答者，答案何时到都能 resolve），
  // 桌面 GUI 授权卡不再弹。默认关＝不注册，保持安全只读。
  //
  // 2026-11-16：这里原来也是普通 ctx.on，跟 questionInline 同一个病——排在 GUI
  // 转发器后面就永远轮不到，卡片照样弹（那是 approval/asked 会话事件推的），
  // 点「允许」却因为插件没 hold 而找不到 waiter → 看着就是「点了没反应」。
  // 现在与提问同构：prepend + 活性门 + 兜底超时。
  //
  // 另一个更硬的坑（同日真机实测）：瀑布 request 的字段只有
  // agent/toolName/callId/reason/displayReason/signal，**根本没有 id**；而
  // approval/asked 事件与 RMC 应答用的是另一个 approval UUID。以前直接读
  // request.id 拿到空串 → 一路 return next() 放行 → waiter 永远建不起来。
  // 现在按 callId 挂 waiter，再用 approval/asked 的 callId 把 UUID 关联过来。
  if (config.approvalInline && typeof ctx.on === 'function') {
    ctx.on('approval/request', (request, next) => {
      const sid = request && request.agent && request.agent.id;
      const callId = request && request.callId;
      const id = (request && request.id) || callId;
      if (!sid || !id) return next();
      if (!nativeAlive()) return next();  // 桌宠不在线：让给 GUI 转发器（同步判断）
      return new Promise((resolve) => {
        let settled = false;
        let timer;
        const key = sid + '|' + id;
        const entry = { finish: null, callId: callId || '', sid };
        const release = () => {
          if (approvalWaiters.get(key) === entry) approvalWaiters.delete(key);
          for (const [ak, av] of approvalAlias) {
            if (av === entry) approvalAlias.delete(ak);
          }
          clearTimeout(timer);
          if (request.signal) request.signal.removeEventListener('abort', onAbort);
        };
        // 三种收场都只走一次：RMC 裁决 / 取消 / 兜底超时（超时 next() 放行给 GUI）。
        const finish = (value) => {
          if (settled) return;
          settled = true;
          release();
          resolve(value);
        };
        entry.finish = finish;
        const onAbort = () => finish('cancelled');
        timer = setTimeout(() => finish(next()), config.approvalHoldTimeoutMs);
        approvalWaiters.set(key, entry);
        if (request.signal) {
          if (request.signal.aborted) return onAbort();
          request.signal.addEventListener('abort', onAbort, { once: true });
        }
      });
    }, { prepend: true });
  }
  async function answerApproval(sessionId, approvalId, outcome) {
    if (!config.approvalInline) {
      return { ok: false, error: '授权内联答未开启（approvalInline=false），请回电脑应答' };
    }    // 三种取法，覆盖两个 id 空间：
    //   ① 直接命中 callId（approval/asked 的 callId 与瀑布 request.callId 同源）
    //   ② 命中 approval/asked 的 UUID 别名（trackRequest 关联过）
    //   ③ 同会话只有一个在等的授权 → 就是它（兜底，避免关联失败就永久失联）
    let waiter = approvalWaiters.get(sessionId + '|' + approvalId);
    if (!waiter) {
      waiter = approvalAlias.get(sessionId + '|' + approvalId);
    }
    if (!waiter) {
      const sameSession = [];
      for (const entry of approvalWaiters.values()) {
        if (entry.sid === sessionId) sameSession.push(entry);
      }
      if (sameSession.length === 1) waiter = sameSession[0];
    }
    if (!waiter) {
      return {
        ok: false,
        error: '这条授权已不在等待（已应答 / 超时放行到桌面 GUI / 桌宠当时不在线），未生效',
      };
    }
    waiter.finish(outcome === 'allowed-once' ? 'allowed-once' : 'rejected');
    return { ok: true };
  }
  async function handleAnswerMessage(item) {
    // 桥 JSON 是 snake_case（session_id），与派发通道取件同口径。
    const sessionId = String((item && (item.session_id || item.sessionId)) || '');
    const res = item.kind === 'approval'
      ? await answerApproval(sessionId, item.id, item.outcome || 'rejected')
      : await answerQuestion(sessionId, item.id, item.answers || []);
    const sid = sessionId;
    const byId = pendingRequests.get(sid);
    if (res.ok && byId) {
      byId.delete(item.id);
      const session = sessionsById.get(sid);
      if (session) pushRequests(session);
    }
    return res;
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
          // 会话索引：global session/event 所有事件都路过，顺手记住 session 对象，
          // 应答落账后重推该会话请求集要用它（agent/disposed 清账）。
          {
            const sIdx = sessionIdOf(session);
            if (sIdx) sessionsById.set(sIdx, session);
          }
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
              // 卡能代答（提问恒可答、授权随 approvalInline）：气泡收敛进卡里。
              bubble: false,
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
          const stem = questionStemOf(data);
          send({
            event: 'question',
            // 气泡带题干（截 40 字）让人不用切窗口就知道要拍什么板；语音仍只带
            // 宿主名。前缀「等你选方案」保持不动：原生端按它认「选方案」语义
            // （180s 迟到去重也认它），冒号后的题干不影响判定。
            bubble: false,
            message: stem ? '等你选方案：' + stem : '等你选方案',
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
            // 回复第一句随 "reply" 给原生端：待办卡直接用，省掉它在主线程上
            // 全文读会话记录（大会话会卡住事件循环，桌宠卡弹不出来）。
            ...(said ? { reply: said } : {}),
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
          sessionsById.delete(agent.id);
          bridge.post('/requests', { exe: config.raiseExe || '',
            session_id: agent.id, project: '', requests: [] }).catch(() => {});
        } else {
          sessionsById.delete(agent.id);
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
        '中途发现的重要信息等非阻塞提醒；等授权、等选方案、等你回话、' +
        '任务完成的提醒由插件自动发，' +
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
        '回合以问句收尾等用户回答时不用发 blocked——插件会按回复收尾的问句自动提醒。' +
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
        markNativeAlive(); // 有响应就算桌宠在线（questionInline 活性门的缓存）
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

  // B2/B3：待答请求重推（原生端重启后恢复展示）+ 应答收件轮询（手机/桌面下发的
  // choice → wait.settle → ack）。不依赖 dispatch 开关——交互应答与派发是两条事。
  if (typeof ctx.effect === 'function') {
    let answerBusy = false;
    const pollAnswers = async () => {
      if (answerBusy) return;
      answerBusy = true;
      try {
        const res = await bridge.get(
          '/request-answer/pending?exe=' + encodeURIComponent(config.raiseExe || ''),
        );
        if (res.status !== 200) return; // 旧原生端：安静跳过
        markNativeAlive(); // 应答口在 → 提问可以放心 hold 给 RMC 答
        let items;
        try { items = JSON.parse(res.body || '[]'); } catch { return; }
        if (!Array.isArray(items) || items.length === 0) return;
        for (const item of items) {
          let r;
          try { r = await handleAnswerMessage(item); }
          catch (e) { r = { ok: false, error: ((e && e.message) || String(e)) }; }
          try {
            await bridge.post('/request-answer/ack', {
              answer_id: (item && item.answer_id) || '',
              ok: !!(r && r.ok),
              ...(r && r.error ? { error: r.error } : {}),
            });
          } catch (e) { /* 原生没开：60s 取件超时兜底 */ }
        }
      } catch (e) { /* 轮询失败安静 */ }
      finally { answerBusy = false; }
    };
    const rePush = () => {
      for (const [sid, byId] of pendingRequests) {
        bridge.post('/requests', { exe: config.raiseExe || '', session_id: sid,
          project: '', requests: [...byId.values()] }).catch(() => {});
      }
    };
    // 收件 1.2s、重推 10s。unref：不撑住宿主/测试进程的事件循环（DSH 常驻前台
    // 自有其它 ref _handle_保活；测试里 B2 用例自带 keep 计时器）。dispose 时清理。
    // 首拍也挂在 effect 里：假 ctx（测试）在 execute 返回后同步跑 disposer，
    // 裸写的提前 poll 会被随后的 clear 掐灭；effect 生命周期下 setup 必然先跑。
    ctx.effect(() => {
      const ansTimer = setInterval(() => { pollAnswers().catch(() => {}); }, 1200);
      const reqTimer = setInterval(rePush, 10000);
      for (const t of [ansTimer, reqTimer]) {
        if (typeof t.unref === 'function') t.unref();
      }
      pollAnswers().catch(() => {});
      return () => {
        clearInterval(ansTimer);
        clearInterval(reqTimer);
      };
    }, 'agentrmc-bridge answer poll / requests push');
  }
}

export const name = 'agentrmc-bridge';
// 没有 tools 服务的 profile 里不激活（而不是抛错）；skills 是可选的，用 ctx.get 取。
export const inject = ['tools'];

export const version = PLUGIN_VERSION;
