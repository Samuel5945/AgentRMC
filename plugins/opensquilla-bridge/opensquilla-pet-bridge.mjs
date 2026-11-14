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
//   node opensquilla-pet-bridge.mjs --selftest-mapping    # 内联应答纯逻辑自检，不连网关/桥
//
// 内联应答（与 DSH 插件同款契约，--no-answer 可关，默认开）：
//   待审批/待澄清按会话攒账，变化即 POST {bridge}/requests（手机/桌面聊天页据此
//   渲染可答卡片），另每 10s 重推（推送同时是插件在线心跳，原生 15s 没收到就
//   把卡片退回只读）。每 1.2s GET {bridge}/request-answer/pending?exe=OpenSquilla.exe
//   取用户在聊天页点的答案：审批 → exec/plugin.approval.resolve（按 namespace），
//   澄清/方案 → chat.clarify_submit，然后 POST {bridge}/request-answer/ack 回执。
//   免打扰只静音提醒，不影响攒账/应答——人不在电脑前也能在手机上批。
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
  // 内联应答（攒账推 /requests + 轮询答案调网关 RPC）默认开，--no-answer 整体关掉。
  answerInline: !flag("no-answer"),
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

// 内联应答纯逻辑自检：只跑「事件→请求卡」「答案→网关 RPC」的映射函数，
// 不连网关、不碰桥 HTTP（函数声明提升，定义在文件末尾的内联应答段）。
if (flag("selftest-mapping")) {
  try {
    selftestMapping();
    log("SELFTEST-MAPPING OK");
    process.exit(0);
  } catch (e) {
    log(`SELFTEST-MAPPING FAIL ${e.message}`);
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
  const sessionKey = p.session_key || p.key || "?";
  // 内联应答账本：先于 notify 的去重/免打扰记账——静音只压出声，
  // 不能吞掉可答卡片（免打扰时人也能在手机上批审批/答澄清）。
  if (CFG.answerInline) trackSessionEvent(evName, sessionKey, p);
  const mapped = eventKind(evName);
  if (!mapped) return;

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

// —— 内联应答通道（B2/B3，与 DSH 插件同款契约）：会话事件 → 待答账本 →
// POST /requests 推可答卡片；手机/桌面聊天页的答案 → pollAnswers 取件 →
// 网关 RPC 应答 → /request-answer/ack 回执。字段映射依据蛋仔实现：
//   approval_required payload：approval_id/namespace/tool_name/command/warning/destructive
//     （Opensquilla desktop/pet/main.js:185-199）
//   plan_presentation payload：request_id/message|text|summary/options|choices（main.js:206-217）
//   交互式 user_input：搭载 session.event.tool_result，result.kind=user_input 且
//     paused=true，request_id + clarify_schema.fields（main.js:161-175、pet/events.py:108-135）
//   RPC 形状：exec/plugin.approval.resolve {id,approved,choice?}（gateway.js:261-266）、
//     chat.clarify_submit {sessionKey,requestId,fields}（gateway.js:268-270），
//     fields 是「字段名→值」平铺对象（renderer/pet.js:215-217）。
// --no-answer 整体关闭；关掉后 /requests、/request-answer 零流量。

// 待答账本：sessionKey -> Map(requestId -> 请求卡)。清空后外层键暂不删——
// 10s 定时器还要补推一阵空集让原生端撤卡（见 emptySince）。
const pendingRequests = new Map();
const emptySince = new Map();

// 推某会话当前待答集。fire-and-forget：原生没开就静默，变化/定时会再推；
// 推送本身也是插件在线心跳（原生 15s 没收到就把卡片退回只读）。
function pushRequests(sessionKey) {
  const byId = pendingRequests.get(sessionKey);
  fetch(`${CFG.bridge}/requests`, {
    method: "POST",
    headers: { "Content-Type": "application/json; charset=utf-8" },
    body: JSON.stringify({
      exe: "OpenSquilla.exe",
      session_id: sessionKey,
      project: sessionProjects.get(sessionKey) || "",
      requests: byId ? [...byId.values()] : [],
    }),
    signal: AbortSignal.timeout(5000),
  }).catch(() => { /* 原生没开就静默：变化/定时再推 */ });
}

// 取第一个能拿去做 RPC 应答的非空 id；seq:N 是去重水位不是业务 id，
// 拿它去 resolve 只会得到网关 NOT_FOUND，宁可不记账。
function trackableId(...candidates) {
  for (const c of candidates) {
    const s = String(c ?? "").trim();
    if (s && !s.startsWith("seq:")) return s;
  }
  return "";
}

// 选项归一成 {label,description}（聊天页按 label 渲染、按 label 回传）；
// 蛋仔选项 value 可能与 label 不同（renderer/pet.js:142-144），不同才留 value
// 供应答时映射回去，原生端忽略多余键。
function optionLabelDesc(o) {
  if (typeof o === "string") return { label: o, description: "" };
  const label = String(o?.label ?? o?.title ?? o?.value ?? "");
  const out = { label, description: String(o?.description || "") };
  if (o?.value !== undefined && o?.value !== null && String(o.value) !== label) {
    out.value = String(o.value);
  }
  return out;
}

// approval_required → 审批卡。reason 先 command（要执行什么）再 warning；
// destructive 追加不可逆警示（蛋仔 renderer/pet.js:118 同款文案）。
function approvalRequestFrom(p) {
  const id = trackableId(p.approval_id, p.approvalId, eventIdentity(p));
  if (!id) return null;
  const command = String(p.command || p.display_target || "");
  const warning = String(p.warning || "");
  let reason = command && warning ? `${command}（⚠ ${warning}）` : (command || warning);
  if (p.destructive) reason += (reason ? "；" : "") + "⚠ 该操作可能不可逆，请谨慎确认";
  return {
    kind: "approval", id, answerable: true,
    toolName: String(p.tool_name || ""),
    reason,
    // resolve 按 namespace 分流（gateway.js:262）；原生端忽略多余键。
    namespace: String(p.namespace || "exec"),
  };
}

// tool_result 的 result/user_input_request 可能是对象或 JSON 字符串（main.js:162-166）。
function userInputResult(p) {
  let result = p?.result;
  if (typeof result === "string") { try { result = JSON.parse(result); } catch { return null; } }
  if (!result || typeof result !== "object") result = p?.user_input_request;
  if (typeof result === "string") { try { result = JSON.parse(result); } catch { return null; } }
  if (!result || typeof result !== "object" || result.kind !== "user_input") return null;
  return result;
}

// clarify_schema.fields → 问题列表（蛋仔 renderer/pet.js:134-145 的字段形状）：
// name|key 是字段名，prompt|label|title 是问题，options|choices|enum 是选项；
// boolean 字段没选项，合成 是/否（网关 user_input_broker._coerce_bool 认「是/否」）。
function clarifyQuestionsFrom(result, schema) {
  if (Array.isArray(result.questions) && result.questions.length) {
    // plan_control 等工具直接带归一化 questions（plan_control.py:300-337），优先用。
    return result.questions.map((q) => ({
      id: String(q?.id || ""),
      question: String(q?.question || q?.header || ""),
      options: (Array.isArray(q?.options) ? q.options : []).map(optionLabelDesc),
      multi_select: !!q?.multi_select,
    })).filter((q) => q.id);
  }
  return schema.fields.map((f) => {
    const name = String(f?.name || f?.key || "");
    const type = String(f?.type || "string").toLowerCase();
    const rawOpts = (Array.isArray(f?.options) && f.options.length)
      ? f.options : (f?.choices || f?.enum || []);
    let options = (Array.isArray(rawOpts) ? rawOpts : []).map(optionLabelDesc);
    if ((type === "boolean" || type === "bool") && !options.length) {
      options = [{ label: "是", description: "" }, { label: "否", description: "" }];
    }
    return {
      id: name,
      question: String(f?.prompt || f?.label || f?.title || name),
      options,
      // OpenSquilla 澄清逐字段单值（user_input_broker 逐字段校验），没有多选。
      multi_select: !!f?.multi_select,
    };
  }).filter((q) => q.id);
}

// plan_presentation / 交互式 user_input → 提问卡；不是待答表单返回 null。
function questionRequestFrom(evName, p) {
  if (evName === "session.event.plan_presentation") {
    const id = trackableId(p.request_id, p.requestId, p.clientRequestId, eventIdentity(p));
    if (!id) return null;
    const rawOpts = Array.isArray(p.options) ? p.options
      : (Array.isArray(p.choices) ? p.choices : []);
    return {
      kind: "question", id, answerable: true,
      questions: [{
        id, question: String(p.message || p.text || p.summary || "有个方案等你选择"),
        options: rawOpts.map(optionLabelDesc), multi_select: false,
      }],
    };
  }
  // 交互式 user_input 搭载 tool_result（main.js:224-236）：paused=true 且
  // status 缺省或 input_required 才是待答表单。
  const result = userInputResult(p);
  if (!result || result.paused !== true) return null;
  if (result.status != null && result.status !== "input_required") return null;
  const id = trackableId(result.request_id, result.requestId);
  if (!id) return null;
  let schema = result.clarify_schema;
  if (typeof schema === "string") { try { schema = JSON.parse(schema); } catch { schema = null; } }
  if (!schema || !Array.isArray(schema.fields) || !schema.fields.length) return null;
  const questions = clarifyQuestionsFrom(result, schema);
  if (!questions.length) return null;
  return { kind: "question", id, answerable: true, questions };
}

// 同一 requestId 的 user_input 结果不再 paused = 已在别处应答/取消/过期 → 给销账用。
function resolvedUserInputId(p) {
  const result = userInputResult(p);
  if (!result || result.paused === true) return "";
  return trackableId(result.request_id, result.requestId);
}

// 聊天页答案 [{id,selected:[labels],custom?}] → 蛋仔 submitClarify 的 fields 平铺对象
// （renderer/pet.js:215-217）。OpenSquilla enum 的 choices 就是 label 本身
// （plan_control.py:299）；custom（其他）优先；选项带独立 value 时按 label 映射回去。
// bool/int 字段传字符串即可，网关 user_input_broker._coerce_field 会自己转
// （「是/否」、数字字符串都认）。
function clarifyFieldsFrom(questions, answers) {
  const byQid = new Map((questions || []).map((q) => [q.id, q]));
  const fields = {};
  for (const a of answers || []) {
    const qid = String(a?.id || "");
    if (!qid) continue;
    const custom = String(a?.custom || "").trim();
    let value = custom;
    if (!value) {
      const labels = (Array.isArray(a?.selected) ? a.selected : []).map(String).filter(Boolean);
      if (!labels.length) continue;
      const opt = byQid.get(qid)?.options?.find((o) => o.label === labels[0]);
      // 澄清逐字段单值：多选也只取首项（网关不收数组）。
      value = opt && opt.value !== undefined ? opt.value : labels[0];
    }
    fields[qid] = value;
  }
  return fields;
}

// 应答件 → 网关 RPC。审批按账本记的 namespace 分流（plugin → plugin.approval.resolve，
// gateway.js:262）；账本丢失（桥重启）退回 exec，网关不认会带原因回执。
function answerRpcFrom(item, tracked) {
  if (item.kind === "approval") {
    const method = tracked?.namespace === "plugin" ? "plugin.approval.resolve" : "exec.approval.resolve";
    const params = { id: String(item.id), approved: item.outcome === "allowed-once" };
    if (item.choice) params.choice = String(item.choice);
    return { method, params };
  }
  return {
    method: "chat.clarify_submit",
    params: {
      sessionKey: String(item.session_id),
      requestId: String(item.id),
      fields: clarifyFieldsFrom(tracked?.questions, item.answers),
    },
  };
}

// 会话事件 → 账本增删 + 变化即推。审批已解决（exec/plugin.approval.resolved）是
// 全局广播（approval_events.py:363-367），session_key 可能缺失，按 approval_id 全账找。
function trackSessionEvent(evName, sessionKey, p) {
  if (!CFG.includeCron && String(sessionKey).startsWith("cron:")) return;
  if (/\.approval\.resolved$/.test(evName)) {
    const id = trackableId(p.approval_id, p.approvalId, p.id);
    if (!id) return;
    const keys = sessionKey !== "?" ? [sessionKey] : [...pendingRequests.keys()];
    for (const key of keys) {
      const byId = pendingRequests.get(key);
      if (byId && byId.delete(id)) pushRequests(key);
    }
    return;
  }
  if (sessionKey === "?") return;
  let byId = pendingRequests.get(sessionKey);
  const ensure = () => {
    if (!byId) { byId = new Map(); pendingRequests.set(sessionKey, byId); }
    return byId;
  };
  let changed = false;
  if (evName === "session.event.approval_required") {
    const req = approvalRequestFrom(p);
    if (req) { ensure().set(req.id, req); changed = true; }
  } else if (evName === "session.event.plan_presentation" || evName === "session.event.tool_result") {
    const req = questionRequestFrom(evName, p);
    if (req) {
      ensure().set(req.id, req); changed = true;
    } else if (evName === "session.event.tool_result") {
      const rid = resolvedUserInputId(p);
      if (rid && byId && byId.delete(rid)) changed = true;
    }
  } else if (evName === "session.event.turn_committed" || evName === "session.event.done" ||
             evName === "chat.done" || evName === "task.succeeded") {
    // 新轮开始/本轮结束：整会话销账（蛋仔 done 时也收掉 permission/plan/clarify 气泡，main.js:246）。
    if (byId && byId.size) { byId.clear(); changed = true; }
  }
  if (changed) pushRequests(sessionKey);
}

// 取应答：与 pollDispatches 同款——busy 防重入、非 200 安静（旧原生端没这条路由）、
// 超时/重连是「送达未知」口径、真实拒绝带网关错误。ok 才销账重推，失败留卡给聊天页重试。
let answerBusy = false;
let answerLogAt = 0;
const answerWarn = (msg) => {
  const now = Date.now();
  if (now - answerLogAt > 60000) { answerLogAt = now; log(msg); }
};
async function pollAnswers() {
  if (answerBusy || !CFG.answerInline) return;
  answerBusy = true;
  try {
    const res = await fetch(`${CFG.bridge}/request-answer/pending?exe=OpenSquilla.exe`, {
      signal: AbortSignal.timeout(5000),
    });
    if (res.status !== 200) return;
    let items;
    try { items = JSON.parse(await res.text()); } catch { return; }
    if (!Array.isArray(items) || items.length === 0) return;
    for (const item of items) {
      const answerId = String(item?.answer_id || "");
      const sessionId = String(item?.session_id || "");
      const reqId = String(item?.id || "");
      const tracked = pendingRequests.get(sessionId)?.get(reqId);
      let error = "";
      if (!answerId || !sessionId || !reqId) {
        error = "取件缺字段（answer_id/session_id/id）";
      } else if (!gatewayUp) {
        error = "网关没连上，未应答";
      } else {
        const rpc = answerRpcFrom(item, tracked);
        if (item.kind === "question" && !Object.keys(rpc.params.fields).length) {
          error = "提问应答缺 answers，未提交";
        } else {
          try {
            await call(rpc.method, rpc.params);
          } catch (e) {
            const msg = (e && e.message) || String(e);
            error = isUncertain(msg)
              ? `应答送达未知：${msg}（网关可能已受理，先去会话里确认再重试）`
              : `网关拒绝应答：${msg}`;
          }
        }
      }
      if (!error) {
        const byId = pendingRequests.get(sessionId);
        if (byId && byId.delete(reqId)) pushRequests(sessionId);
      }
      try {
        await fetch(`${CFG.bridge}/request-answer/ack`, {
          method: "POST",
          headers: { "Content-Type": "application/json; charset=utf-8" },
          body: JSON.stringify({
            answer_id: answerId, ok: error === "", ...(error ? { error } : {}),
          }),
          signal: AbortSignal.timeout(5000),
        });
      } catch (e) {
        // ack 发不回去：原生端 60s 取件超时会兜底取消，节流记一笔。
        answerWarn("answer ack failed: " + e.message);
      }
      log(`ANSWER ${answerId.slice(0, 8)} sess=${sessionId} ${item.kind} ok=${error === ""}${error ? " " + error : ""}`);
    }
  } catch (e) {
    // 原生端没开：轮询失败保持安静，60s 记一次。
    answerWarn("answer poll failed: " + e.message);
  } finally {
    answerBusy = false;
  }
}
setInterval(pollAnswers, 1200);

// 10s 定时重推：原生端重启/丢账后恢复展示；有账才推——清空的账补推 30s 空集
// （确保原生端撤卡，也兜住单次推送失败），之后丢弃，长期无账零流量。
setInterval(() => {
  if (!CFG.answerInline || pendingRequests.size === 0) return;
  const now = Date.now();
  for (const [key, byId] of [...pendingRequests]) {
    if (byId.size > 0) {
      emptySince.delete(key);
    } else {
      const t = emptySince.get(key) || now;
      emptySince.set(key, t);
      if (now - t > 30000) { pendingRequests.delete(key); emptySince.delete(key); continue; }
    }
    pushRequests(key);
  }
}, 10000);

// --selftest-mapping：纯逻辑自检（不连网关/桥）。用例按蛋仔 payload 形状构造。
function selftestMapping() {
  const eq = (got, want, msg) => {
    const g = JSON.stringify(got); const w = JSON.stringify(want);
    if (g !== w) throw new Error(`${msg}: got ${g} want ${w}`);
  };
  // 审批卡：approval_id/namespace/tool_name/command/warning（main.js:185-199）。
  const ap = approvalRequestFrom({
    approval_id: "ap-1", namespace: "exec", tool_name: "shell",
    command: "rm -rf x", warning: "会删文件", destructive: true,
  });
  eq(ap.kind, "approval", "approval kind");
  eq(ap.id, "ap-1", "approval id");
  eq(ap.answerable, true, "approval answerable");
  eq(ap.toolName, "shell", "approval toolName");
  eq(ap.namespace, "exec", "approval namespace");
  if (!ap.reason.includes("rm -rf x") || !ap.reason.includes("会删文件")) {
    throw new Error("approval reason 应含 command 与 warning: " + ap.reason);
  }
  // seq 水位不是业务 id：不记账。
  eq(approvalRequestFrom({ stream_seq: 7 }), null, "seq-only approval not trackable");
  // 审批应答：allowed-once→approved:true；namespace 分流 plugin（gateway.js:261-266）。
  eq(answerRpcFrom({ kind: "approval", id: "ap-1", outcome: "allowed-once" }, ap),
    { method: "exec.approval.resolve", params: { id: "ap-1", approved: true } }, "exec resolve");
  eq(answerRpcFrom({ kind: "approval", id: "ap-2", outcome: "rejected", choice: "no" },
    { namespace: "plugin" }),
    { method: "plugin.approval.resolve", params: { id: "ap-2", approved: false, choice: "no" } },
    "plugin resolve + choice");
  // 交互式 user_input：tool_result + JSON 字符串 result（main.js:161-175）。
  const q = questionRequestFrom("session.event.tool_result", {
    session_key: "s",
    result: JSON.stringify({
      kind: "user_input", paused: true, status: "input_required", request_id: "rq-1",
      clarify_schema: { fields: [{ name: "q1", prompt: "选一个", type: "enum", choices: ["甲", "乙"], allow_other: true }] },
    }),
  });
  eq(q.kind, "question", "question kind");
  eq(q.id, "rq-1", "question requestId");
  eq(q.questions, [{ id: "q1", question: "选一个", options: [{ label: "甲", description: "" }, { label: "乙", description: "" }], multi_select: false }], "schema fields → questions");
  // 答案映射：selected label 即值；custom 优先（pet.js:194/215-217）。
  eq(answerRpcFrom({ kind: "question", session_id: "s", id: "rq-1", answers: [{ id: "q1", selected: ["乙"] }] }, q),
    { method: "chat.clarify_submit", params: { sessionKey: "s", requestId: "rq-1", fields: { q1: "乙" } } },
    "clarify selected");
  eq(clarifyFieldsFrom(q.questions, [{ id: "q1", selected: ["甲"], custom: "自己写" }]),
    { q1: "自己写" }, "clarify custom overrides");
  // 账本丢失时退回直接取 label 当值（OpenSquilla choices 即 label，plan_control.py:299）。
  eq(clarifyFieldsFrom(undefined, [{ id: "q1", selected: ["乙"] }]), { q1: "乙" }, "clarify without tracked");
  // 已应答（paused=false）不再是待答表单，且可用来销账。
  eq(questionRequestFrom("session.event.tool_result", {
    result: { kind: "user_input", paused: false, request_id: "rq-1" },
  }), null, "resolved user_input not pending");
  eq(resolvedUserInputId({ result: { kind: "user_input", paused: false, request_id: "rq-1" } }),
    "rq-1", "resolved user_input id");
  // plan_presentation：options/choices + message（main.js:206-217）。
  const plan = questionRequestFrom("session.event.plan_presentation",
    { request_id: "pl-1", message: "选个方案", choices: ["A", "B"] });
  eq(plan.id, "pl-1", "plan requestId");
  eq(plan.questions[0].options, [{ label: "A", description: "" }, { label: "B", description: "" }], "plan choices → options");
  // 选项 value 与 label 不同时，应答映射回 value（pet.js:142-144）。
  eq(clarifyFieldsFrom([{ id: "q", options: [{ label: "显示", value: "v", description: "" }] }],
    [{ id: "q", selected: ["显示"] }]), { q: "v" }, "label → value mapping");
}

process.on("SIGINT", () => {
  shuttingDown = true;
  log("shutting down");
  try { ws && ws.close(); } catch { /* ignore */ }
  process.exit(0);
});

log(`pet-bridge start: raise=${CFG.raise || "(none)"} bridge=${CFG.bridge} cron=${CFG.includeCron ? "on" : "off"} answer=${CFG.answerInline ? "on" : "off"}`);
touchHeartbeat();
setInterval(touchHeartbeat, Math.min(30000, CFG.refreshMs));
connect();
