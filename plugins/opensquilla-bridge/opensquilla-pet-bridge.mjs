// OpenSquilla → AgentRMC 桌宠联动桥（零依赖，Node 22+ 内置 WebSocket/fetch）。
//
// 作用：订阅 OpenSquilla 网关的会话事件流，在「对话结束」「需要你确认/选方案」时
// 调 AgentRMC 的 AgentBridge（127.0.0.1:45915）：
//   session.event.done / chat.done            → POST /notify event=stop       （出声 + 拉窗口）
//   session.event.approval_required           → POST /notify event=permission （出声 + 拉窗口）
//   session.event.plan_presentation           → POST /notify event=question   （出声 + 拉窗口）
//   session.event.turn_committed              → POST /notify event=turn-start （静默：归档旧验收）
//
// 每条通知都带 session_id（网关的会话 key）：原生端靠它把同一会话的旧「待验收」
// 归档，一轮只留一张卡——不带它的话，同一会话每跑完一轮就多攒一条重复待办
// （2026-11-03 实测踩坑：WebChat 一个会话攒了 3 条一模一样的「待验收」）。
//
// 用法：
//   node opensquilla-pet-bridge.mjs                       # 前台跑，Ctrl+C 退出
//   node opensquilla-pet-bridge.mjs --raise AgentRMCNative.exe   # 改拉哪个窗口（--no-raise 不拉）
//   node opensquilla-pet-bridge.mjs --include-cron --log out.log
//   node opensquilla-pet-bridge.mjs --selftest            # 只测一次 /notify，不连网关
//
// 文案模板（{session}=会话标题，泛标题 WebChat 回落项目目录名；{text}=回复开头，
// 仅 --relay 时用；默认文案宿主名开头，与 hooks 系同口径）：
//   --msg-stop "..."        默认：「{session}」这轮跑完了，回来看看
//   --msg-permission "..."  默认：有个操作在等你确认
//   --msg-plan "..."        默认：「{session}」方案出来了，等你选一个
//   --relay                 结束提醒附带回复正文开头（默认只报标题，TTS 不念长文）
//
// 免打扰：创建 pause 文件即静音（默认 %TEMP%\opensquilla-pet-bridge.pause），
//   删除该文件恢复。--pause-file PATH 可改位置。
//
// 离线补投：AgentRMC 没开时通知不丢——落盘到与 hooks/DSH 同址同格式的
//   pending-events.jsonl（%APPDATA%\LIG\AgentRMCNative，AGENTRMC_SPOOL_DIR 可改），
//   原生端启动/每 5 秒补投；补投只落工单不出声。文件超 1MB 丢最旧留最新。
//
// 假活自愈：网关半开（TCP 不断但收不到事件）时，sessions.list 探针连续 3 次
//   失败就强制重连；握手限时（--hello-timeout，默认 20s）兜底 TCP 黑洞。
//   心跳文件（--heartbeat-file，默认 %TEMP%\pet-bridge.heartbeat）每 30s 写一拍，
//   独立于网关状态——看门狗靠它区分「活着但网关断了」和「进程卡死」。
//
// 退出码：0 正常；1 网关连不上（--selftest 时为桥连不上）。

import {
  appendFileSync, existsSync, mkdirSync, readFileSync, statSync, writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";

const argv = process.argv.slice(2);
const opt = (name, dflt = "") => {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[i + 1] : dflt;
};
const flag = (name) => argv.includes(`--${name}`);

const CFG = {
  gateway: opt("gateway", "ws://127.0.0.1:18791/ws"),
  bridge: opt("bridge", "http://127.0.0.1:45915").replace(/\/$/, ""),
  raise: flag("no-raise") ? "" : opt("raise", "OpenSquilla.exe"),
  logFile: opt("log", ""),
  includeCron: flag("include-cron"),
  quiet: flag("quiet"),
  relay: flag("relay"),
  refreshMs: Number(opt("refresh", "30000")),
  callTimeoutMs: Number(opt("call-timeout", "15000")),
  // sessions.send 的受理比普通 RPC 慢（网关要落 ingress 账），单给一个上限；
  // 超时口径是「送达未知」，不是被拒。测试可调小。
  sendTimeoutMs: Number(opt("send-timeout", "30000")),
  helloTimeoutMs: Number(opt("hello-timeout", "20000")),
  heartbeatFile: opt(
    "heartbeat-file",
    path.join(process.env.TEMP || process.env.TMP || ".", "pet-bridge.heartbeat"),
  ),
  // 默认文案与 hooks 系同口径：宿主名开头（用户 2026-11-08 指出开头不该是 WebChat）。
  msgStop: opt("msg-stop", "OpenSquilla：这轮跑完了，回来看看"),
  msgPermission: opt("msg-permission", "OpenSquilla：有个操作在等你确认"),
  msgPlan: opt("msg-plan", "OpenSquilla：「{session}」方案出来了，等你选一个"),
  pauseFile: opt(
    "pause-file",
    `${process.env.TEMP || process.env.TMP || "."}\\opensquilla-pet-bridge.pause`,
  ),
};

const log = (msg) => {
  const line = `${new Date().toISOString()} ${msg}`;
  if (!CFG.quiet) console.log(line);
  if (CFG.logFile) {
    try { appendFileSync(CFG.logFile, line + "\n", "utf8"); } catch { /* log 写不进就算了 */ }
  }
};

// 心跳：给看门狗证明「事件循环还活着」。独立于网关状态——网关断了在重连
// 循环里也照写，否则看门狗会把健康但连不上网关的桥误杀。看门狗见文件
// 超 3 分钟没更新（漏 6 拍）才判卡死。周期取 min(30s, refresh)，启动先写一拍。
function touchHeartbeat() {
  try { writeFileSync(CFG.heartbeatFile, String(Date.now()), "utf8"); } catch { /* 写不进就算了 */ }
}

// 同一会话同一件事，15s 内只发一次（done 和 chat.done 会前后脚到）。
// 但「同一会话」不等于「同一件事」：连续两轮完成、连续两次审批都必须各喊一次。
// 事件自带身份（approval_id / turn_id / stream_seq…）时按身份去重；没有身份
// 的老事件才退回会话级窗口。新轮开始（turn_committed）还要清掉该会话的账。
const DEDUPE_MS = 15000;
const recent = new Map();

function eventIdentity(p) {
  const id = p?.event_id ?? p?.eventId ?? p?.approval_id ?? p?.approvalId ??
    p?.request_id ?? p?.requestId ?? p?.turn_id ?? p?.turnId ??
    p?.task_id ?? p?.taskId;
  if (id !== undefined && id !== null && String(id).trim()) {
    return String(id).trim();
  }
  const seq = p?.stream_seq ?? p?.streamSeq;
  return seq === undefined || seq === null ? "" : `seq:${seq}`;
}

const dedupe = (sessionKey, kind, p) => {
  const now = Date.now();
  const identity = eventIdentity(p);
  // 带身份：只按身份判重，不同轮/不同请求互不牵连；同时刷新会话级水位，
  // 让随后不带身份的 chat.done 之类回声仍被压掉（回声只查会话级键）。
  const key = identity
    ? `${sessionKey}|${kind}|${identity}`
    : `${sessionKey}|${kind}`;
  const last = recent.get(key) || 0;
  if (now - last < DEDUPE_MS) return false;
  recent.set(key, now);
  if (identity) recent.set(`${sessionKey}|${kind}`, now);
  if (recent.size > 200) {
    for (const [k, t] of recent) if (now - t > DEDUPE_MS) recent.delete(k);
  }
  return true;
};

// 新轮开始：该会话上一轮的完成/审批账全部作废。否则 done(轮1) →
// turn_committed(轮2) → done(轮2) 里第二条 done 会被 15s 窗口吞掉。
function resetSessionDedupe(sessionKey) {
  const prefix = `${sessionKey}|`;
  for (const key of [...recent.keys()]) {
    if (key.startsWith(prefix)) recent.delete(key);
  }
}

// 离线补投：与 hooks/DSH 同址同格式（pending-events.jsonl 一行一条 JSON），
// 原生端 importSpool() 启动/每 5 秒补投，补投只落工单不出声、不抢前台。
const SPOOL_DIR = process.env.AGENTRMC_SPOOL_DIR ||
  path.join(process.env.APPDATA || os.tmpdir(), "LIG", "AgentRMCNative");
const SPOOL_FILE = path.join(SPOOL_DIR, "pending-events.jsonl");
const SPOOL_CAP_BYTES = 1024 * 1024;

// 上限守卫：原生端一直不开时别把文件养到几百 MB。超 1MB 丢最旧留最新
// （按行裁，保 JSONL 完整）。读-改-写没加锁：并发落盘最坏丢几条旧通知，
// 提醒场景可接受；裁不动就照常追加，不能挡通知。
function trimSpool() {
  try {
    if (statSync(SPOOL_FILE).size <= SPOOL_CAP_BYTES) return;
    const lines = readFileSync(SPOOL_FILE, "utf8").split("\n").filter(Boolean);
    writeFileSync(SPOOL_FILE, lines.slice(-500).join("\n") + "\n", "utf8");
  } catch { /* 裁不动就照常追加 */ }
}

function spool(payload) {
  try {
    mkdirSync(SPOOL_DIR, { recursive: true });
    trimSpool();
    appendFileSync(SPOOL_FILE, JSON.stringify(payload) + "\n", "utf8");
  } catch { /* 落盘也失败就彻底放弃：通知不能挡会话 */ }
}

function buildPayload(kind, message, sessionKey, eventId = "") {
  const body = { event: kind, title: "OpenSquilla", message };
  if (CFG.raise) body.raise = CFG.raise;
  // 会话归属：native 端靠它做「同会话旧验收归档」和工单合并。
  if (sessionKey) body.session_id = sessionKey;
  // 事件身份：native 端按它区分同一会话里的不同弹窗/不同轮次。不带的话
  // 原生会退回「同会话 180s 只喊一次」，第二道题会被第一道吞掉。
  if (eventId) body.event_id = eventId;
  // 项目归属：与 hooks 同口径报原样路径，外壳行身份取目录名（Agentrmc 等）。
  // 不带它卡行只能兜底「会话 <短 id>」（2026-11-08 用户指出）。
  const project = sessionKey ? sessionProjects.get(sessionKey) : "";
  if (project) body.project = project;
  return body;
}

async function notifyPet(kind, message, sessionKey, eventId = "") {
  const res = await fetch(`${CFG.bridge}/notify`, {
    method: "POST",
    headers: { "Content-Type": "application/json; charset=utf-8" },
    body: JSON.stringify(buildPayload(kind, message, sessionKey, eventId)),
    signal: AbortSignal.timeout(5000),
  });
  if (res.status !== 204) throw new Error(`bridge returned ${res.status}`);
}

if (flag("selftest")) {
  try {
    await notifyPet("stop", "联动桥自检：桌宠出声说明桥是通的");
    log("SELFTEST OK");
    process.exit(0);
  } catch (e) {
    log(`SELFTEST FAIL ${e.message}`);
    process.exit(1);
  }
}

let ws = null;
let gatewayUp = false;
let callSeq = 0;
const pending = new Map();
const subscribed = new Set();
let refreshTimer = null;
let helloTimer = null;
let reconnectDelay = 1000;
let shuttingDown = false;
let probeFailures = 0;
let refreshing = false;
const PROBE_FAILURES_MAX = 3;

function call(method, params = {}, timeoutMs = CFG.callTimeoutMs) {
  const id = `b-${++callSeq}`;
  ws.send(JSON.stringify({ type: "req", id, method, params }));
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    setTimeout(() => {
      if (pending.delete(id)) reject(new Error(`${method} timeout`));
    }, timeoutMs);
  });
}

function subscribeKey(key) {
  return call("sessions.messages.subscribe", { key })
    .then(() => { subscribed.add(key); log(`subscribed ${key}`); })
    .catch((e) => log(`subscribe failed ${key}: ${e.message}`));
}

// sessions.list 是游标分页（v4 契约：view=session-list-v1 → nextCursor）。
// 只取首页会漏掉后面的会话：既订阅不到，project 也缓存不上（2026-11-08 审查）。
// 老网关不认识 view 会退回落单页数组，照样能用。
async function listAllSessions() {
  const out = [];
  let cursor = "";
  for (let page = 0; page < 20; page += 1) {
    const params = { view: "session-list-v1", limit: 200 };
    if (cursor) params.cursor = cursor;
    const res = await call("sessions.list", params);
    if (Array.isArray(res)) {
      out.push(...res);
      break;
    }
    const arr = res?.sessions || res?.items || res?.keys || [];
    out.push(...arr);
    const next = res?.nextCursor || res?.next_cursor || "";
    if (!next || next === cursor) break;
    cursor = next;
  }
  return out;
}

async function refreshSessions() {
  // 防重入：订阅多时一轮可能超过 refresh 周期，探针不许叠着跑，
  // 否则半开判定会被并发的超时计数刷乱。
  if (refreshing) return;
  refreshing = true;
  try {
    const arr = await listAllSessions();
    probeFailures = 0;
    for (const s of arr) {
      const key = s.key || s.session_key || s.id;
      if (!key) continue;
      const title = s.display_name || s.displayName || s.title || s.subject || "";
      if (title) sessionTitles.set(key, String(title).slice(0, 40));
      const ws = s.workspace || s.workspaceDisplayPath || s.workspaceLabel || "";
      if (ws) sessionProjects.set(key, String(ws).slice(0, 200));
      if (subscribed.has(key)) continue;
      if (!CFG.includeCron && key.startsWith("cron:")) continue;
      await subscribeKey(key);
    }
  } catch (e) {
    probeFailures += 1;
    log(`refresh sessions failed (${probeFailures}/${PROBE_FAILURES_MAX}): ${e.message}`);
    if (probeFailures >= PROBE_FAILURES_MAX) {
      forceReconnect("sessions.list 探针连续失败，网关疑似半开");
    }
  } finally {
    refreshing = false;
  }
}

// 半开自愈：网关半开（睡眠/网络抖）时 TCP 不断、onclose 迟迟不来——进程
// 活着却收不到事件。探针连续失败就 detach 旧 socket（别等它的 close 握手，
// 可能永远不完）、reject 在途调用、立刻开新连接。任何入站帧都是活着的证据，
// 会重置计数（见 onmessage）；握手本身黑洞则由 helloTimer 兜底。
function forceReconnect(reason) {
  log(`forcing reconnect: ${reason}`);
  gatewayUp = false;
  const old = ws;
  ws = null;
  if (old) {
    old.onmessage = old.onclose = old.onerror = null;
    try { old.close(); } catch { /* ignore */ }
  }
  for (const p of pending.values()) p.reject(new Error("reconnect"));
  pending.clear();
  if (refreshTimer) { clearInterval(refreshTimer); refreshTimer = null; }
  if (helloTimer) { clearTimeout(helloTimer); helloTimer = null; }
  subscribed.clear();
  probeFailures = 0;
  reconnectDelay = 1000;
  connect();
}

// 会话标题缓存：done/plan 事件的模板里用 {session}，比念整段回复友好。
const sessionTitles = new Map();
// 会话工作区缓存：sessions.list 的 workspace/workspaceLabel，/notify 带 project。
const sessionProjects = new Map();

// {session} 取值：会话标题；泛标题 WebChat 不念，回落项目目录名（Agentrmc 等）。
function sessionLabel(sessionKey) {
  const t = (sessionTitles.get(sessionKey) || "").trim();
  if (t && t.toLowerCase() !== "webchat") return t;
  const p = sessionProjects.get(sessionKey) || "";
  return p.split(/[\\/]/).filter(Boolean).pop() || "这个会话";
}

function renderTemplate(tpl, sessionKey, replyText) {
  const title = sessionLabel(sessionKey);
  let out = tpl.replaceAll("{session}", title);
  if (tpl.includes("{text}")) return out.replaceAll("{text}", replyText || "");
  // --relay 且模板没写 {text}：回复正文附在模板后（卡面带回复正文，
  // 2026-11-08 用户定）；空回复不附，permission 等不带空尾巴。
  return replyText ? `${out} 回复：${replyText}` : out;
}

function eventKind(evName) {
  if (evName === "session.event.done" || evName === "chat.done" || evName === "task.succeeded") {
    return { kind: "stop", tpl: CFG.msgStop };
  }
  if (evName === "session.event.approval_required") {
    return { kind: "permission", tpl: CFG.msgPermission };
  }
  if (evName === "session.event.plan_presentation") {
    // question 类：原生端按事件类型做「等选方案」迟到去重，自定义 --msg-plan 也不失配。
    return { kind: "question", tpl: CFG.msgPlan };
  }
  if (evName === "session.event.turn_committed") {
    // 新一轮开始：只发归档信号，不出声、不建卡（免打扰时也照发——
    // 人已经回来开新轮了，旧验收本来就该翻篇）。
    return { kind: "turn-start", tpl: "" };
  }
  return null;
}

async function onFrame(frame) {
  const name = frame.event || "";
  const p = frame.payload || {};

  // 新会话出现时立刻订阅：等 30s 轮询会错过已经结束的短回合。
  if (name === "sessions.changed") {
    const key = p.key;
    if (key && !subscribed.has(key) && (CFG.includeCron || !key.startsWith("cron:"))) {
      subscribeKey(key);
    }
    return;
  }

  // 事件名在 frame.event 顶层；个别版本会塞进 payload.event。
  const evName = p.event || name;
  const mapped = eventKind(evName);
  if (!mapped) return;

  const sessionKey = p.session_key || p.key || "?";
  if (!CFG.includeCron && String(sessionKey).startsWith("cron:")) return;
  const identity = eventIdentity(p);
  if (mapped.kind === "turn-start") {
    // 新轮开始：清掉该会话上一轮的完成/审批账（归档信号本身不去重）。
    if (sessionKey !== "?") resetSessionDedupe(sessionKey);
  } else if (!dedupe(sessionKey, mapped.kind, p)) {
    return;
  }

  if (mapped.kind === "turn-start") {
    if (sessionKey === "?") return;
    try {
      await notifyPet("turn-start", "新一轮对话开始", sessionKey);
      log(`TURNSTART sess=${sessionKey}`);
    } catch (e) {
      spool(buildPayload("turn-start", "新一轮对话开始", sessionKey));
      log(`turn-start failed: ${e.message} — 已落盘等补投`);
    }
    return;
  }

  if (existsSync(CFG.pauseFile)) {
    log(`paused (${mapped.kind} suppressed, delete ${CFG.pauseFile} to resume)`);
    return;
  }

  const rawText = (p.message || p.text || "").toString().trim();
  const reply = CFG.relay ? rawText.slice(0, 80) : "";
  let tpl = mapped.tpl;
  // stop 有回复正文：与 hooks 系同口径「宿主名：正文」，回复当主文案
  //（用户 2026-11-08 指出主文案不该是收尾句）；自定义 --msg-stop 不干预；
  // 没回复才念收尾句「这轮跑完了，回来看看」。
  if (mapped.kind === "stop" && reply && !flag("msg-stop") && !tpl.includes("{text}")) {
    tpl = "OpenSquilla：{text}";
  }
  const text = renderTemplate(tpl, sessionKey, reply).slice(0, 200);
  const target = sessionKey === "?" ? "" : sessionKey;
  try {
    await notifyPet(mapped.kind, text, target, identity);
    log(`NOTIFY ${mapped.kind} sess=${sessionKey} raise=${CFG.raise || "(none)"} msg=${text.slice(0, 40)}`);
  } catch (e) {
    // AgentRMC 没在跑：落盘等补投，别把提醒丢了（与 hooks/DSH 同一条通路）。
    spool(buildPayload(mapped.kind, text, target, identity));
    log(`notify failed (${mapped.kind}): ${e.message} — 已落盘等补投`);
  }
}

function connect() {
  ws = new WebSocket(CFG.gateway);
  // 握手看门狗：TCP 黑洞时连接可能永远停在 CONNECTING，onclose 也不来。
  // hello-ok 限时没到就换一条新连接（hello-ok / onclose 都会清掉它）。
  if (helloTimer) clearTimeout(helloTimer);
  helloTimer = setTimeout(() => {
    if (!shuttingDown) forceReconnect(`${CFG.helloTimeoutMs}ms 内未收到 hello-ok`);
  }, CFG.helloTimeoutMs);

  ws.onmessage = (ev) => {
    const raw = typeof ev.data === "string" ? ev.data : ev.data.toString();
    let f;
    try { f = JSON.parse(raw); } catch { return; }
    probeFailures = 0; // 任何入站帧都证明链路活着，探针计数清零

    if (f.type === "event" && f.event === "connect.challenge") {
      ws.send(JSON.stringify({
        type: "req", id: `c-${++callSeq}`, method: "connect",
        params: {
          minProtocol: 1, maxProtocol: 3, role: "operator",
          scopes: ["operator.admin"],
          client: { id: "agentrmc-pet-bridge", version: "1.0.0", platform: process.platform, mode: "tui" },
        },
      }));
      return;
    }
    if (f.type === "hello-ok") {
      if (helloTimer) { clearTimeout(helloTimer); helloTimer = null; }
      gatewayUp = true;
      log(`connected ${CFG.gateway} (protocol ${f.protocol ?? "?"})`);
      reconnectDelay = 1000;
      refreshSessions();
      if (refreshTimer) clearInterval(refreshTimer);
      refreshTimer = setInterval(refreshSessions, CFG.refreshMs);
      return;
    }
    if (f.type === "res" && pending.has(f.id)) {
      const p = pending.get(f.id);
      pending.delete(f.id);
      f.ok ? p.resolve(f.payload) : p.reject(new Error(JSON.stringify(f.error)));
      return;
    }
    if (f.type === "event") onFrame(f);
  };

  ws.onclose = () => {
    if (helloTimer) { clearTimeout(helloTimer); helloTimer = null; }
    gatewayUp = false;
    if (shuttingDown) return;
    if (refreshTimer) { clearInterval(refreshTimer); refreshTimer = null; }
    subscribed.clear();
    log(`gateway closed, reconnect in ${reconnectDelay}ms`);
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 30000);
  };

  ws.onerror = () => { /* onclose 会接管重连 */ };
}

// —— 定向派发通道（2026-11-08 接线）：native 队列 → 本桥轮询取件 →
// 网关 sessions.send 注入 → ack 回执。受理是 durable admission（网关
// turn_ingress_receipts 落账），ack ok 就是「确实到了那条会话」的回执；
// 守卫在取件后：sessions.resolve 命中活表才注入（裁决 6），拒绝原因走
// ack ok:false，原生端按派发失败上卡。与 DSH 插件同款通道；桥在技能目录，
// 不占插件版本。--no-dispatch 可关。
let dispatchBusy = false;
let dispatchLogAt = 0;
const dispatchWarn = (msg) => {
  const now = Date.now();
  if (now - dispatchLogAt > 60000) { dispatchLogAt = now; log(msg); }
};
// 超时/重连 = 不知道对面收没收到（可能已被网关受理）；真实的 RPC 拒绝会带
// 网关错误对象（NOT_FOUND / busy…）。两种必须给用户不同的口径。
const isUncertain = (msg) => /timeout|reconnect/i.test(String(msg));
async function pollDispatches() {
  if (dispatchBusy || flag("no-dispatch")) return;
  dispatchBusy = true;
  try {
    const res = await fetch(`${CFG.bridge}/dispatch/pending?exe=OpenSquilla.exe`, {
      signal: AbortSignal.timeout(5000),
    });
    if (res.status !== 200) return; // 旧原生端没有这条路由：安静跳过
    let items;
    try { items = JSON.parse(await res.text()); } catch { return; }
    if (!Array.isArray(items) || items.length === 0) return;
    for (const item of items) {
      const dispatchId = String(item?.dispatch_id || "");
      const sessionId = String(item?.session_id || "");
      const text = String(item?.text || "");
      let error = "";
      if (!dispatchId || !sessionId || !text) {
        error = "取件缺字段（dispatch_id/session_id/text）";
      } else if (!gatewayUp) {
        error = "网关没连上，未注入";
      } else {
        try {
          await call("sessions.resolve", { key: sessionId });
        } catch (e) {
          const msg = (e && e.message) || String(e);
          error = isUncertain(msg)
            ? `会话 ${sessionId.slice(0, 24)}… 归属未确认：${msg}（未注入）`
            : `会话 ${sessionId.slice(0, 24)}… 不在活表（resolve 拒绝），未注入`;
        }
        if (!error) {
          // 幂等标识由 dispatch_id 派生：同一件重取/重试都带同一个
          // client_message_id，网关能认重；随机 id 会让重试变成第二条消息。
          const cmid = `agentrmc-${dispatchId.replaceAll("-", "")}`;
          try {
            const accepted = await call("sessions.send", {
              key: sessionId,
              message: text,
              attachments: [],
              client_message_id: cmid,
              _source: {
                caller_kind: "cli", channel_kind: "cli",
                channel_id: "cli:agentrmc-bridge", source_kind: "cli",
                source_name: "agentrmc-bridge", client_message_id: cmid,
              },
            }, CFG.sendTimeoutMs);
            if (!accepted || !(accepted.turn_id || accepted.task_id)) {
              error = "送达未知：网关受理应答缺 turn_id，未确认（可能已受理）";
            }
          } catch (e) {
            const msg = (e && e.message) || String(e);
            error = isUncertain(msg)
              ? `送达未知：${msg}（网关可能已受理，先去会话里确认再重派）`
              : "sessions.send 被拒：" + msg;
          }
        }
      }
      try {
        await fetch(`${CFG.bridge}/dispatch/ack`, {
          method: "POST",
          headers: { "Content-Type": "application/json; charset=utf-8" },
          body: JSON.stringify({
            dispatch_id: dispatchId, ok: error === "", ...(error ? { error } : {}),
          }),
          signal: AbortSignal.timeout(5000),
        });
      } catch (e) {
        // ack 发不回去：原生端 60s 取件超时会兜底取消，节流记一笔。
        dispatchWarn("dispatch ack failed: " + e.message);
      }
      log(`DISPATCH ${dispatchId.slice(0, 8)} sess=${sessionId} ok=${error === ""}${error ? " " + error : ""}`);
    }
  } catch (e) {
    // 原生端没开：轮询失败保持安静，60s 记一次。
    dispatchWarn("dispatch poll failed: " + e.message);
  } finally {
    dispatchBusy = false;
  }
}
setInterval(pollDispatches, 1500);

process.on("SIGINT", () => {
  shuttingDown = true;
  log("shutting down");
  try { ws && ws.close(); } catch { /* ignore */ }
  process.exit(0);
});

log(`pet-bridge start: raise=${CFG.raise || "(none)"} bridge=${CFG.bridge} cron=${CFG.includeCron ? "on" : "off"}`);
touchHeartbeat();
setInterval(touchHeartbeat, Math.min(30000, CFG.refreshMs));
connect();
