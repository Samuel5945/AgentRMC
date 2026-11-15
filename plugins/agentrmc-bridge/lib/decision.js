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
    // 1) 照常提醒：出声 + 落工单（session_id 必带，桌宠卡片应答靠它挂工单）
    const notifyPayload = { event: 'permission', title: p.agent, message: p.message };
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

module.exports = {
  approvalInlineEnabled,
  approvalInlineGate,
  runApprovalInlineFlow,
  APPROVAL_POLL_MS,
  APPROVAL_HEARTBEAT_MS,
};
