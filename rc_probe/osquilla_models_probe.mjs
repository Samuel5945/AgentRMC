// OpenSquilla 模型目录 / 切模型 RPC 探针（只动自建牺牲会话，零泄漏）
// 握手照抄 pet-bridge：connect.challenge → connect(role=operator, scopes) → hello-ok。
// 用法：node rc_probe/osquilla_models_probe.mjs
import { randomUUID } from "node:crypto";
import { existsSync } from "node:fs";
import { DatabaseSync } from "node:sqlite";
import os from "node:os";
import path from "node:path";

const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(`--${n}`); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };
const GATEWAY = opt("gateway", "ws://127.0.0.1:18791/ws");
const MARKER = `OSQM-${randomUUID().slice(0, 8).toUpperCase()}`;
const STATE = opt("state", (() => {
  const desktop = path.join(process.env.APPDATA || "", "@opensquilla", "desktop-electron", "opensquilla", "state");
  return existsSync(path.join(desktop, "sessions.db")) ? desktop : path.join(os.homedir(), ".opensquilla", "state");
})());
const log = (...a) => console.log(...a);

const ws = new WebSocket(GATEWAY);
let seq = 0;
const pending = new Map();
const hello = {};
hello.promise = new Promise((r) => { hello.resolve = r; });
function call(method, params = {}, timeoutMs = 20000) {
  const id = `p-${++seq}`;
  ws.send(JSON.stringify({ type: "req", id, method, params }));
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    setTimeout(() => { if (pending.delete(id)) reject(new Error(`${method} timeout`)); }, timeoutMs);
  });
}
ws.onerror = (e) => { log("WS ERROR", e.message || ""); process.exit(1); };
ws.onmessage = (ev) => {
  let f; try { f = JSON.parse(String(ev.data)); } catch { return; }
  if (f.type === "event" && f.event === "connect.challenge") {
    ws.send(JSON.stringify({
      type: "req", id: `c-${++seq}`, method: "connect",
      params: {
        minProtocol: 1, maxProtocol: 3, role: "operator",
        scopes: ["operator.admin"],
        client: { id: "agentrmc-models-probe", version: "1.0.0", platform: process.platform, mode: "tui" },
      },
    }));
    return;
  }
  if (f.type === "hello-ok") { hello.resolve(f); return; }
  const p = f.id && pending.get(f.id);
  if (p) {
    pending.delete(f.id);
    if (f.ok === false || f.error) p.reject(new Error(JSON.stringify(f.error || f)));
    else p.resolve(f.payload ?? f.result ?? f);
  }
};
await hello.promise;
log("connected");

// 1) models.list
let models = null;
for (const params of [{ scope: "active" }, {}]) {
  try {
    models = await call("models.list", params);
    log(`models.list ${JSON.stringify(params)} ->`, Array.isArray(models) ? `array(${models.length})` : Object.keys(models || {}).join(","));
    if (models) break;
  } catch (e) { log("models.list failed", JSON.stringify(params), e.message.slice(0, 160)); }
}
const list = Array.isArray(models) ? models : (models?.models || []);
log("models total:", list.length);
const byProv = {};
for (const m of list) (byProv[m.provider] ||= []).push(m.id);
for (const [p, ids] of Object.entries(byProv)) log(`  ${p}: ${ids.length} [${ids.slice(0, 3).join(", ")}]`);
if (!list.length) { log("EMPTY CATALOG"); ws.close(); process.exit(1); }

// 2) 牺牲会话上验证 sessions.patch（provider+model 必须成对，见 _rpc_session_deployment_fields）
const created = await call("sessions.create", { agentId: "main", kind: "cli", displayName: `AgentRMC 模型探针 ${MARKER}` });
const key = created.key || created.session_key || created.sessionKey || "";
log("created", key);
if (!key) { log("NO KEY"); ws.close(); process.exit(1); }
try {
  const pick = list.find((m) => m.provider && m.id && !/egg/i.test(m.id)) || list[0];
  const patched = await call("sessions.patch", { key, provider: pick.provider, model: pick.id });
  log("patch ok:", JSON.stringify(patched).slice(0, 260));
  const db = new DatabaseSync(path.join(STATE, "sessions.db"), { readOnly: true });
  const row = db.prepare("SELECT model,provider_override FROM sessions WHERE session_key=?").get(key);
  db.close();
  log("readback:", JSON.stringify(row), "| expected", pick.provider, pick.id);
  log(row && row.model === pick.id ? "PATCH VERIFIED" : "PATCH NOT VISIBLE IN DB");
  try {
    await call("sessions.patch", { key, provider: pick.provider, model: "definitely-not-a-model-xyz" });
    log("BAD MODEL ACCEPTED (!!)");
  } catch (e) { log("bad model refused:", e.message.slice(0, 180)); }
  // 只带 model 不带 provider（会话已有 provider 时）
  try {
    const pick2 = list.find((m) => m.provider === pick.provider && m.id !== pick.id);
    if (pick2) {
      await call("sessions.patch", { key, model: pick2.id });
      log("model-only patch ok ->", pick2.id);
    } else { log("model-only: no second model in same provider, skipped"); }
  } catch (e) { log("model-only refused:", e.message.slice(0, 180)); }
} finally {
  try { await call("sessions.delete", { key }); log("deleted", key); }
  catch (e) { log("delete failed:", e.message.slice(0, 120)); }
}
ws.close();
process.exit(0);
