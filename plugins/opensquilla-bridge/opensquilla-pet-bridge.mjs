// OpenSquilla → AgentRMC 桌宠联动桥（零依赖，Node 22+ 内置 WebSocket/fetch）。
//
// 作用：订阅 OpenSquilla 网关的会话事件流，在「对话结束」「需要你确认/选方案」时
// 调 AgentRMC 的 AgentBridge（127.0.0.1:45915）：
//   session.event.done / chat.done            → POST /notify event=stop       （出声 + 拉窗口）
//   session.event.approval_required           → POST /notify event=permission （出声 + 拉窗口）
//   session.event.plan_presentation           → POST /notify event=permission （出声 + 拉窗口）
//
// 用法：
//   node opensquilla-pet-bridge.mjs                       # 前台跑，Ctrl+C 退出
//   node opensquilla-pet-bridge.mjs --raise AgentRMCNative.exe   # 改拉哪个窗口（--no-raise 不拉）
//   node opensquilla-pet-bridge.mjs --include-cron --log out.log
//   node opensquilla-pet-bridge.mjs --selftest            # 只测一次 /notify，不连网关
//
// 文案模板（{session}=会话标题，{text}=回复开头，仅 --relay 时用）：
//   --msg-stop "..."        默认：「{session}」这轮跑完了，回来看看
//   --msg-permission "..."  默认：有个操作在等你确认
//   --msg-plan "..."        默认：「{session}」方案出来了，等你选一个
//   --relay                 结束提醒附带回复正文开头（默认只报标题，TTS 不念长文）
//
// 免打扰：创建 pause 文件即静音（默认 %TEMP%\opensquilla-pet-bridge.pause），
//   删除该文件恢复。--pause-file PATH 可改位置。
//
// 退出码：0 正常；1 网关连不上（--selftest 时为桥连不上）。

import { appendFileSync, existsSync } from "node:fs";

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
  msgStop: opt("msg-stop", "「{session}」这轮跑完了，回来看看"),
  msgPermission: opt("msg-permission", "有个操作在等你确认"),
  msgPlan: opt("msg-plan", "「{session}」方案出来了，等你选一个"),
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

// 同一会话同一种提醒，15s 内只发一次（done 和 chat.done 会前后脚到）。
const DEDUPE_MS = 15000;
const recent = new Map();
const dedupe = (key) => {
  const now = Date.now();
  const last = recent.get(key) || 0;
  if (now - last < DEDUPE_MS) return false;
  recent.set(key, now);
  if (recent.size > 200) {
    for (const [k, t] of recent) if (now - t > DEDUPE_MS) recent.delete(k);
  }
  return true;
};

async function notifyPet(kind, message) {
  const body = { event: kind, title: "OpenSquilla", message };
  if (CFG.raise) body.raise = CFG.raise;
  const res = await fetch(`${CFG.bridge}/notify`, {
    method: "POST",
    headers: { "Content-Type": "application/json; charset=utf-8" },
    body: JSON.stringify(body),
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
let callSeq = 0;
const pending = new Map();
const subscribed = new Set();
let refreshTimer = null;
let reconnectDelay = 1000;
let shuttingDown = false;

function call(method, params = {}, timeoutMs = 15000) {
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

async function refreshSessions() {
  try {
    const list = await call("sessions.list", {});
    const arr = Array.isArray(list) ? list : (list.sessions || list.items || []);
    for (const s of arr) {
      const key = s.key || s.session_key || s.id;
      if (!key) continue;
      const title = s.display_name || s.displayName || s.title || s.subject || "";
      if (title) sessionTitles.set(key, String(title).slice(0, 40));
      if (subscribed.has(key)) continue;
      if (!CFG.includeCron && key.startsWith("cron:")) continue;
      await subscribeKey(key);
    }
  } catch (e) {
    log(`refresh sessions failed: ${e.message}`);
  }
}

// 会话标题缓存：done/plan 事件的模板里用 {session}，比念整段回复友好。
const sessionTitles = new Map();

function renderTemplate(tpl, sessionKey, replyText) {
  const title = sessionTitles.get(sessionKey)
    || String(sessionKey).split(":").pop()?.slice(0, 24) || "这个会话";
  let out = tpl.replaceAll("{session}", title);
  out = out.replaceAll("{text}", replyText || "");
  return out;
}

function eventKind(evName) {
  if (evName === "session.event.done" || evName === "chat.done" || evName === "task.succeeded") {
    return { kind: "stop", tpl: CFG.msgStop };
  }
  if (evName === "session.event.approval_required") {
    return { kind: "permission", tpl: CFG.msgPermission };
  }
  if (evName === "session.event.plan_presentation") {
    return { kind: "permission", tpl: CFG.msgPlan };
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
  if (!dedupe(`${sessionKey}|${mapped.kind}`)) return;

  if (existsSync(CFG.pauseFile)) {
    log(`paused (${mapped.kind} suppressed, delete ${CFG.pauseFile} to resume)`);
    return;
  }

  const rawText = (p.message || p.text || "").toString().trim();
  const text = renderTemplate(mapped.tpl, sessionKey, CFG.relay ? rawText.slice(0, 80) : "").slice(0, 200);
  try {
    await notifyPet(mapped.kind, text);
    log(`NOTIFY ${mapped.kind} sess=${sessionKey} raise=${CFG.raise || "(none)"} msg=${text.slice(0, 40)}`);
  } catch (e) {
    log(`notify failed (${mapped.kind}): ${e.message} — AgentRMC 没在跑？`);
  }
}

function connect() {
  ws = new WebSocket(CFG.gateway);

  ws.onmessage = (ev) => {
    const raw = typeof ev.data === "string" ? ev.data : ev.data.toString();
    let f;
    try { f = JSON.parse(raw); } catch { return; }

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
    if (shuttingDown) return;
    if (refreshTimer) { clearInterval(refreshTimer); refreshTimer = null; }
    subscribed.clear();
    log(`gateway closed, reconnect in ${reconnectDelay}ms`);
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 30000);
  };

  ws.onerror = () => { /* onclose 会接管重连 */ };
}

process.on("SIGINT", () => {
  shuttingDown = true;
  log("shutting down");
  try { ws && ws.close(); } catch { /* ignore */ }
  process.exit(0);
});

log(`pet-bridge start: raise=${CFG.raise || "(none)"} bridge=${CFG.bridge} cron=${CFG.includeCron ? "on" : "off"}`);
connect();
