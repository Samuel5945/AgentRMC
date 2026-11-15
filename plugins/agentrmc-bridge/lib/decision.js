'use strict';
// 决策式远程授权（bridge 系 / ZCode，2026-11-15 真机探针三路全通后落地）。
// 流程与传输分离：bridge 参数可注入（hook 用真 http，测试用假桥），流程逻辑
// 只有一份、可进程内单测。返回值不写 stdout、不 exit——那是调用方（hook）的事。
//
// 红线（zcode.cjs 实测契约，由调用方保证）：
//   - exit 2 = 硬 deny：任何失败路径都必须 exit 0 静默放手；
//   - stdout 除决策 JSON 外一个字都不能有（schema 解析失败会被当阻塞错误）。
//
// 安全模型：开关文件在 SPOOL_DIR 下（<SPOOL_DIR>/approval-inline），默认关。
// 开着时，本机能碰到桌宠/聊天页的人都能代答授权——与 DSH approvalInline 同级。

const APPROVAL_POLL_MS = 1200;       // 取件节奏（与 DSH 一致）
const APPROVAL_HEARTBEAT_MS = 10000; // /requests 重推节奏（原生 15s 在线窗内续命）

// 开关判定：标记文件存在即开。fsLike 可注入（测试用假 fs）。
function approvalInlineEnabled(spoolDir, fsLike) {
  const fsMod = fsLike || require('fs');
  try {
    fsMod.statSync(spoolDir + '/approval-inline');
    return true;
  } catch {
    return false;
  }
}

// 拦截条件（hook 的 gate 与测试共用一份判据）：
//   permission 事件、不是 AskUserQuestion（提问答案注入不进阻塞中的工具调用）、
//   非 DRY_RUN、有 session 与 requestId（老宿主没法挂账对号）、开关开着、
//   宿主是 ZCode（其它宿主的事件时序没验证过，不冒险）。
function approvalInlineGate(input) {
  const i = input || {};
  return i.event === 'permission' &&
    i.tool !== 'AskUserQuestion' &&
    !i.dryRun &&
    !!String(i.sessionId || '').trim() &&
    !!String(i.requestId || '').trim() &&
    !!i.enabled &&
    String(i.exe || '').toLowerCase() === 'zcode.exe';
}

// 流程本体。bridge.request(method, path, payloadObj) -> {status,body}|null
//（失败 resolve null，由这里决定退路）。p = {
//   agent, exe, sid, rid, project, toolName, reason, message, speech,
//   eventId, pluginVersion, waitSeconds, pollMs?, heartbeatMs?,
// }。返回 { decision }：decision=null = 超时/桥断（宿主正常弹窗）；
// allow；或 { behavior:'deny', message }。
async function runApprovalInlineFlow(bridge, p) {
  const pollMs = p.pollMs || APPROVAL_POLL_MS;
  const heartbeatMs = p.heartbeatMs || APPROVAL_HEARTBEAT_MS;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const decision = { result: null };
  try {
    // 1) 照常提醒：出声 + 落工单（session_id 必带，桌宠卡片应答靠它挂工单）。
    //    no_raise=true：卡片能代答，别把宿主拽到前台把卡片作废；超时后宿主
    //    自己弹窗抢焦点，不需要这里代劳。
    const notifyPayload = {
      event: 'permission', title: p.agent, message: p.message, no_raise: true,
      // 卡能代答：气泡收敛进卡里，不弹第二个浮层。
      bubble: false,
    };
    if (p.speech) notifyPayload.speech = p.speech;
    if (p.sid) notifyPayload.session_id = p.sid;
    if (p.project) notifyPayload.project = p.project;
    notifyPayload.raise = p.exe;
    if (p.eventId) notifyPayload.event_id = p.eventId;
    if (p.pluginVersion) notifyPayload.plugin_version = p.pluginVersion;
    await bridge.request('POST', '/notify', notifyPayload);
    // 2) 推可答卡（kind=approval；id 就是 Authorization requestId，应答按它对号）
    const reqObj = {
      kind: 'approval', id: p.rid, toolName: p.toolName || '',
      reason: p.reason || '', answerable: true,
    };
    const push = () => bridge.request('POST', '/requests', {
      exe: p.exe, session_id: p.sid, project: p.project, requests: [reqObj],
    });
    await push();
    // 3) 限时轮询：拿到本请求的应答就核销；桥断（桌宠关了）直接放手
    const deadline = Date.now() + p.waitSeconds * 1000;
    let lastPush = Date.now();
    while (Date.now() < deadline) {
      const res = await bridge.request('GET',
        '/request-answer/pending?exe=' + encodeURIComponent(p.exe) +
        '&session=' + encodeURIComponent(p.sid), null);
      if (res === null) break;
      let items = [];
      try { items = JSON.parse(res.body || '[]') || []; } catch { items = []; }
      const mine = items.find((it) => it && String(it.id || '') === p.rid &&
        String(it.session_id || '') === p.sid);
      if (mine) {
        await bridge.request('POST', '/request-answer/ack', {
          answer_id: String(mine.answer_id || ''), ok: true,
        });
        decision.result = mine.outcome === 'allowed-once'
          ? { behavior: 'allow' }
          : { behavior: 'deny', message: '已由用户在桌宠上拒绝' };
        break;
      }
      // 心跳：answerable 挂在原生 15s 在线窗上，重推保活
      if (Date.now() - lastPush >= heartbeatMs) {
        lastPush = Date.now();
        await push();
      }
      await sleep(Math.min(pollMs, Math.max(1, deadline - Date.now())));
    }
  } catch { /* 流程里任何意外都不往外抛：最坏退回宿主弹窗 */ }
  // 4) 撤卡（答完/超时/出错都要撤：请求已处理，卡片别留着）
  try {
    await bridge.request('POST', '/requests', {
      exe: p.exe, session_id: p.sid, project: p.project, requests: [],
    });
  } catch { /* 撤不掉就算了 */ }
  return { decision: decision.result };
}

// —— 选方案只读卡（ZCode，2026-11-15）——
// ZCode 的 AskUserQuestion 答案注入不进阻塞中的工具调用（真机探针结论），
// 卡是只读的：题干 + 去聊天页查看入口。生命周期：PreToolUse(question) 推卡
// → PostToolUse(AskUserQuestion)（question-done）撤卡。

// ZCode AskUserQuestion 的 tool_input → DSH 卡片同款 questions 形状。
// ZCode 用 camelCase（multiSelect），DSH 用 multi_select，归一到后者。
function normalizeZcodeQuestions(input) {
  const qs = input && Array.isArray(input.questions) ? input.questions : [];
  return qs.map((q, i) => ({
    id: String((q && q.id) || 'q' + (i + 1)),
    question: String((q && q.question) || ''),
    header: String((q && q.header) || ''),
    options: ((q && Array.isArray(q.options)) ? q.options : []).map((o) => ({
      label: String((o && o.label) || ''),
      description: String((o && o.description) || ''),
    })),
    multi_select: !!(q && (q.multi_select || q.multiSelect)),
  }));
}

// gate：question 事件、非 DRY_RUN、有会话与 tool_use_id、宿主是 ZCode。
// 不挂 approval-inline 开关：只读展示没有代答的安全面。
// 其它宿主不开——WorkBuddy 的 PreToolUse(question) 在答完之后才派发，
// 推了就是一张立刻过期的卡；Qoder 家事件时序没验证过。
function questionCardGate(input) {
  const i = input || {};
  return i.event === 'question' &&
    !i.dryRun &&
    !!String(i.sessionId || '').trim() &&
    !!String(i.requestId || '').trim() &&
    String(i.exe || '').toLowerCase() === 'zcode.exe';
}

// 选方案卡流程：语音 + 推只读卡。返回 { pushed }。
// bubble:false（用户 2026-11-15）：不出「等你选方案」普通气泡——卡片本身就是
// 那条通知（题干在卡上），气泡同锚位晚到会把卡顶掉（实测卡只闪一下）。
async function runQuestionCardFlow(bridge, p) {
  let pushed = false;
  try {
    const notifyPayload = {
      event: 'permission', title: p.agent, message: p.message, bubble: false,
    };
    if (p.speech) notifyPayload.speech = p.speech;
    if (p.sid) notifyPayload.session_id = p.sid;
    if (p.project) notifyPayload.project = p.project;
    notifyPayload.raise = p.exe;
    if (p.eventId) notifyPayload.event_id = p.eventId;
    if (p.pluginVersion) notifyPayload.plugin_version = p.pluginVersion;
    await bridge.request('POST', '/notify', notifyPayload);
    await bridge.request('POST', '/requests', {
      exe: p.exe, session_id: p.sid, project: p.project,
      requests: [{
        kind: 'question', id: p.rid,
        questions: p.questions || [],
        answerable: false,
      }],
    });
    pushed = true;
  } catch { /* 提醒已发，卡推失败就算了 */ }
  return { pushed };
}

// 题已结（PostToolUse）：撤掉该会话的只读卡。
async function runQuestionDone(bridge, p) {
  try {
    await bridge.request('POST', '/requests', {
      exe: p.exe, session_id: p.sid, project: p.project, requests: [],
    });
  } catch { /* 撤不掉就算了 */ }
}

module.exports = {
  approvalInlineEnabled,
  approvalInlineGate,
  runApprovalInlineFlow,
  normalizeZcodeQuestions,
  questionCardGate,
  runQuestionCardFlow,
  runQuestionDone,
  APPROVAL_POLL_MS,
  APPROVAL_HEARTBEAT_MS,
};
