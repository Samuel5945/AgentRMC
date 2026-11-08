// OpenSquilla 定向派发探针（第 3 批·OpenSquilla 线，2026-11-08）。
//
// 通道：网关自带 WS RPC（ws://127.0.0.1:18791/ws，与 pet-bridge 同握手：
// connect.challenge → connect role=operator scopes=operator.admin → hello-ok）。
// 官方 RPC 面（读宿主源码 _internal/opensquilla/cli/gateway_client.py 核对）：
//   sessions.create {agentId, kind, displayName} → {key}
//   sessions.send   {key, message, attachments, client_message_id, _source}
//                   → 受理回执 {turn_id,...}（durable admission，异步跑轮）
//   sessions.list / sessions.messages.subscribe / sessions.messages.hydrate
//
// 用法：
//   node rc_probe/osquilla_dispatch_probe.mjs --list   # 只读：列会话 key/标题
//   node rc_probe/osquilla_dispatch_probe.mjs --send   # 自建牺牲会话发标记探针
//
// --send 判据（照搬 WorkBuddy/DSH 探针口径）：
//   1 受理回执拿到 turn_id（送达成立）；
//   2 目标会话跑完一轮（done 事件）且 hydrate 读回含标记的 user 正文；
//   3 零泄漏：桌面网关权威会话库是 SQLite sessions.db（state  home 跟配置走：
//     %APPDATA%\@opensquilla\desktop-electron\opensquilla\state；~/.opensquilla
//     是另一实例的家，2026-11-08 实测踩坑），transcript_entries 按 session_key
//     聚合 (count,max id) 快照 diff，只允许牺牲会话自己变化。
// 牺牲会话不自删：留给外壳派发行做端到端活体（它的 stop 事件会经 pet-bridge
// 落一张绑定该 session 的待验收卡，正好当派发入口）。要不要删由用户定。
// --key <已有牺牲会话> 可复跑不新建。

import { randomUUID } from "node:crypto";
import { existsSync } from "node:fs";
import { DatabaseSync } from "node:sqlite";
import os from "node:os";
import path from "node:path";

const argv = process.argv.slice(2);
const flag = (n) => argv.includes(`--${n}`);
const opt = (n, d) => {
  const i = argv.indexOf(`--${n}`);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : d;
};
const GATEWAY = opt("gateway", "ws://127.0.0.1:18791/ws");
const MARKER = `OSQ-${randomUUID().slice(0, 8).toUpperCase()}`;
const TEXT = `定向派发探针 ${MARKER}：只回复「收到」，不要调用任何工具。`;
const STATE = opt("state", (() => {
  const desktop = path.join(process.env.APPDATA || "", "@opensquilla",
    "desktop-electron", "opensquilla", "state");
  return existsSync(path.join(desktop, "sessions.db"))
    ? desktop : path.join(os.homedir(), ".opensquilla", "state");
})());

const log = (...a) => console.log(...a);

// 只读 SQLite：transcript_entries 按会话聚合快照，做零泄漏 diff。
function snapshot() {
  const db = new DatabaseSync(path.join(STATE, "sessions.db"), { readOnly: true });
  const out = new Map();
  for (const r of db.prepare(
    "SELECT session_key, COUNT(*) c, MAX(id) m FROM transcript_entries GROUP BY session_key",
  ).all()) out.set(r.session_key, `${r.c}:${r.m}`);
  db.close();
  return out;
}

function readBack(key) {
  const db = new DatabaseSync(path.join(STATE, "sessions.db"), { readOnly: true });
  const rows = db.prepare(
    "SELECT role, content FROM transcript_entries WHERE session_key=? ORDER BY id",
  ).all(key);
  db.close();
  return rows;
}

const ws = new WebSocket(GATEWAY);
let seq = 0;
const pending = new Map();
const waiters = []; // {match, resolve}
const call = (method, params = {}, timeoutMs = 20000) => {
  const id = `p-${++seq}`;
  ws.send(JSON.stringify({ type: "req", id, method, params }));
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    setTimeout(() => {
      if (pending.delete(id)) reject(new Error(`${method} timeout`));
    }, timeoutMs);
  });
};
const waitEvent = (match, timeoutMs) => new Promise((resolve, reject) => {
  const t = setTimeout(() => reject(new Error("event timeout")), timeoutMs);
  waiters.push({ match, resolve: (v) => { clearTimeout(t); resolve(v); } });
});

ws.onmessage = (ev) => {
  let f;
  try { f = JSON.parse(typeof ev.data === "string" ? ev.data : ev.data.toString()); }
  catch { return; }
  if (f.type === "event" && f.event === "connect.challenge") {
    ws.send(JSON.stringify({
      type: "req", id: `c-${++seq}`, method: "connect",
      params: {
        minProtocol: 1, maxProtocol: 3, role: "operator",
        scopes: ["operator.admin"],
        client: { id: "agentrmc-dispatch-probe", version: "1.0.0", platform: process.platform, mode: "tui" },
      },
    }));
    return;
  }
  if (f.type === "hello-ok") { clearTimeout(helloTimer); hello.resolve(); return; }
  if (f.type === "res" && pending.has(f.id)) {
    const p = pending.get(f.id);
    pending.delete(f.id);
    f.ok ? p.resolve(f.payload) : p.reject(new Error(JSON.stringify(f.error)));
    return;
  }
  if (f.type === "event") {
    for (let i = waiters.length - 1; i >= 0; i--) {
      if (waiters[i].match(f)) {
        const w = waiters.splice(i, 1)[0];
        w.resolve(f);
      }
    }
  }
};
const hello = {};
hello.promise = new Promise((r) => { hello.resolve = r; });
ws.onerror = (e) => { log("WS ERROR", e.message || ""); process.exit(1); };
const helloTimer = setTimeout(() => { log("HELLO TIMEOUT"); process.exit(1); }, 15000);

await hello.promise;
log(`connected ${GATEWAY}`);

if (flag("list") || !flag("send")) {
  // sessions.list 是游标分页（view=session-list-v1）；只取首页会漏会话。
  const arr = [];
  let cursor = "";
  for (let page = 0; page < 20; page++) {
    const params = { view: "session-list-v1", limit: 200 };
    if (cursor) params.cursor = cursor;
    const res = await call("sessions.list", params);
    if (Array.isArray(res)) { arr.push(...res); break; }
    arr.push(...(res?.sessions || res?.items || res?.keys || []));
    const next = res?.nextCursor || res?.next_cursor || "";
    if (!next || next === cursor) break;
    cursor = next;
  }
  log(`sessions: ${arr.length}`);
  for (const s of arr) {
    log(`  key=${s.key || s.session_key || s.id}  title=${s.display_name || s.displayName || s.title || ""}  kind=${s.kind || ""}`);
  }
  ws.close();
  process.exit(0);
}

// —— --send：牺牲会话全链 ——
const before = snapshot();
let key = opt("key", "");
if (key) {
  log(`reusing sacrifice session key=${key}`);
} else {
  const created = await call("sessions.create", {
    agentId: "main", kind: "cli",
    displayName: `AgentRMC 链路探针 ${MARKER}`,
  });
  key = created.key;
  log(`sacrifice session key=${key}`);
}
await call("sessions.messages.subscribe", { key });

const cmid = randomUUID().replace(/-/g, "");
const doneP = waitEvent(
  (f) => (f.event === "session.event.done" || f.event === "chat.done") &&
         (f.payload?.session_key === key || f.payload?.key === key),
  180000,
);
const accepted = await call("sessions.send", {
  key,
  message: TEXT,
  attachments: [],
  client_message_id: cmid,
  _source: {
    caller_kind: "cli", channel_kind: "cli", channel_id: "cli:agentrmc-probe",
    source_kind: "cli", source_name: "agentrmc-probe", client_message_id: cmid,
  },
}, 30000);
const turnId = accepted?.turn_id || accepted?.task_id || "";
log(`ACCEPTED turn_id=${turnId || "(none)"} payload=${JSON.stringify(accepted).slice(0, 200)}`);
if (!turnId) { log("TARGETED_SEND FAIL: no turn_id in acceptance"); process.exit(1); }

await doneP;
log("TURN DONE");
// 读回走权威库（hydrate 的窗口形状不适合做断言，SQLite 只读最稳）。
await new Promise((r) => setTimeout(r, 1500)); // WAL 落盘异步，等一拍
const rows = readBack(key);
const userHit = rows.some((r) => r.role === "user" && String(r.content).includes(MARKER));
const reply = [...rows].reverse().find((r) => r.role === "assistant");
log(`READ_BACK ${userHit}  reply=${String(reply?.content || "").slice(0, 60)}`);

// 零泄漏：全库按会话聚合 diff，只许牺牲会话自己变化。
const after = snapshot();
const changed = [];
for (const [k, v] of after) if (before.get(k) !== v) changed.push(k);
for (const k of before.keys()) if (!after.has(k)) changed.push(k + " (deleted)");
const leak = changed.filter((k) => k !== key);
log(`changed sessions: ${changed.length} -> ${changed.join(", ")}`);
log(`NO_LEAK ${leak.length === 0}`);
if (leak.length) for (const k of leak) log(`  LEAK ${k}`);

log(userHit && leak.length === 0 ? "TARGETED_SEND OK" : "TARGETED_SEND FAIL");
log(`sacrifice left for e2e: ${key}`);
ws.close();
process.exit(userHit && leak.length === 0 ? 0 : 1);
