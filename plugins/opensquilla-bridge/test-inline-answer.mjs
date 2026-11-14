// 内联应答端到端自检（零依赖）：假网关 WS + 假原生 HTTP，跑真桥子进程。
// 验证「事件 → /requests body」与「answer item → 网关 RPC params」两条映射，
// 以及 ack 回执、销账重推、审批 resolved/turn_committed 清账、--no-answer 关闭。
//
// 跑法：node plugins/opensquilla-bridge/test-inline-answer.mjs
// 退出码：0 全部通过；1 有失败（打印桥日志协助定位）。

import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const BRIDGE = path.join(HERE, "opensquilla-pet-bridge.mjs");
const SESSION = "agent:main:cli:test";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "osq-bridge-test-"));
const logFile = path.join(tmp, "bridge.log");
const logFile2 = path.join(tmp, "bridge-noanswer.log");

// ---------- 极简 WebSocket 服务端（只够测试用：文本帧、小负载） ----------
function wsAccept(key) {
  return createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
}
function wsEncode(text) {
  const data = Buffer.from(text, "utf8");
  let header;
  if (data.length < 126) {
    header = Buffer.from([0x81, data.length]);
  } else {
    header = Buffer.alloc(4);
    header[0] = 0x81; header[1] = 126;
    header.writeUInt16BE(data.length, 2);
  }
  return Buffer.concat([header, data]);
}
function wsDecode(buf) {
  if (buf.length < 2) return null;
  const opcode = buf[0] & 0x0f;
  const masked = (buf[1] & 0x80) !== 0;
  let len = buf[1] & 0x7f;
  let off = 2;
  if (len === 126) { if (buf.length < 4) return null; len = buf.readUInt16BE(2); off = 4; }
  else if (len === 127) { if (buf.length < 10) return null; len = Number(buf.readBigUInt64BE(2)); off = 10; }
  const maskOff = off;
  if (masked) off += 4;
  if (buf.length < off + len) return null;
  let payload = buf.subarray(off, off + len);
  if (masked) {
    const mask = buf.subarray(maskOff, maskOff + 4);
    const out = Buffer.alloc(len);
    for (let i = 0; i < len; i += 1) out[i] = payload[i] ^ mask[i % 4];
    payload = out;
  }
  return { opcode, payload, rest: buf.subarray(off + len) };
}

// ---------- 假网关 ----------
const gwSockets = new Set();
const subscribed = new Set();
const rpcCalls = [];      // {method, params}
let rpcFailNext = null;   // 下一条应答 RPC 返回的网关错误
let gwListCalls = 0;

function gwPush(event, payload) {
  for (const s of gwSockets) s.send({ type: "event", event, payload });
}
function handleGwReq(socket, msg) {
  if (msg.type !== "req") return;
  const reply = (payload) => socket.send({ type: "res", id: msg.id, ok: true, payload });
  switch (msg.method) {
    case "connect":
      socket.send({ type: "hello-ok", protocol: 3 });
      return;
    case "sessions.list":
      gwListCalls += 1;
      reply({ sessions: [{ key: SESSION, workspace: "D:\\proj\\Demo", display_name: "Demo" }] });
      return;
    case "sessions.messages.subscribe":
      subscribed.add(msg.params.key);
      reply({});
      return;
    case "exec.approval.resolve":
    case "plugin.approval.resolve":
    case "chat.clarify_submit":
      rpcCalls.push({ method: msg.method, params: msg.params });
      if (rpcFailNext) {
        const error = rpcFailNext; rpcFailNext = null;
        socket.send({ type: "res", id: msg.id, ok: false, error });
      } else {
        reply({});
      }
      return;
    default:
      reply({});
  }
}
const gw = http.createServer();
gw.on("upgrade", (req, socket) => {
  socket.write(
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
    `Sec-WebSocket-Accept: ${wsAccept(req.headers["sec-websocket-key"])}\r\n\r\n`,
  );
  socket.send = (obj) => { try { socket.write(wsEncode(JSON.stringify(obj))); } catch { /* 已断 */ } };
  gwSockets.add(socket);
  let buf = Buffer.alloc(0);
  socket.on("data", (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    let frame;
    while ((frame = wsDecode(buf))) {
      buf = frame.rest;
      if (frame.opcode === 8) { socket.end(); return; }
      if (frame.opcode === 9) { socket.write(Buffer.from([0x8a, 0])); continue; }
      if (frame.opcode !== 1) continue;
      let msg;
      try { msg = JSON.parse(frame.payload.toString("utf8")); } catch { continue; }
      handleGwReq(socket, msg);
    }
  });
  socket.on("error", () => {});
  socket.on("close", () => gwSockets.delete(socket));
  socket.send({ type: "event", event: "connect.challenge", payload: {} });
});

// ---------- 假原生端（AgentBridge 契约：/notify 204、/requests 204、取件 200 JSON） ----------
const pushes = [];     // POST /requests body
const acks = [];       // POST /request-answer/ack body
const notifies = [];   // POST /notify body
let answerQueue = [];  // GET /request-answer/pending 一次性吐完
let answerPolls = 0;
let lastAnswerPollUrl = "";
const native = http.createServer((req, res) => {
  let body = "";
  req.on("data", (c) => { body += c; });
  req.on("end", () => {
    const done = (code, data) => {
      res.writeHead(code, { "Content-Type": "application/json", Connection: "close" });
      res.end(data || "");
    };
    const url = req.url;
    if (req.method === "POST" && url === "/notify") { notifies.push(JSON.parse(body)); return done(204); }
    if (req.method === "POST" && url === "/requests") { pushes.push(JSON.parse(body)); return done(204); }
    if (req.method === "GET" && url.startsWith("/dispatch/pending")) return done(200, "[]");
    if (req.method === "GET" && url.startsWith("/request-answer/pending")) {
      answerPolls += 1; lastAnswerPollUrl = url;
      const items = answerQueue; answerQueue = [];
      return done(200, JSON.stringify(items));
    }
    if (req.method === "POST" && url === "/request-answer/ack") { acks.push(JSON.parse(body)); return done(204); }
    if (req.method === "POST" && url === "/dispatch/ack") return done(204);
    return done(404);
  });
});

// ---------- 工具 ----------
async function waitFor(cond, label, ms = 8000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (cond()) return;
    await sleep(50);
  }
  throw new Error("timeout waiting: " + label);
}
function listen(server) {
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve(server.address().port)));
}
function spawnBridge(extraArgs, log) {
  return spawn(process.execPath, [
    BRIDGE,
    "--gateway", `ws://127.0.0.1:${gwPort}/ws`,
    "--bridge", `http://127.0.0.1:${nativePort}`,
    "--log", log,
    "--heartbeat-file", path.join(tmp, "hb"),
    "--pause-file", path.join(tmp, "no-such-pause"),
    ...extraArgs,
  ], { env: { ...process.env, AGENTRMC_SPOOL_DIR: tmp }, stdio: "ignore" });
}
// 找 marker 之后第一条满足条件的 /requests 推送
function pushAfter(marker, pred) {
  return pushes.slice(marker).find(pred);
}
const reqsOf = (p) => p.requests.map((r) => r.id);

let gwPort = 0;
let nativePort = 0;
let child = null;
let child2 = null;

async function main() {
  gwPort = await listen(gw);
  nativePort = await listen(native);
  child = spawnBridge([], logFile);
  await waitFor(() => gwListCalls >= 1 && subscribed.has(SESSION), "桥握手+订阅");

  // 1) approval_required → /requests 审批卡（同时 /notify permission 照旧）
  let marker = pushes.length;
  gwPush("session.event.approval_required", {
    session_key: SESSION, approval_id: "ap-1", namespace: "exec",
    tool_name: "shell", command: "rm -rf x", warning: "会删文件", destructive: true, stream_seq: 1,
  });
  await waitFor(() => pushAfter(marker, (p) => reqsOf(p).includes("ap-1")), "审批卡推送");
  const apPush = pushAfter(marker, (p) => reqsOf(p).includes("ap-1"));
  assert.equal(apPush.exe, "OpenSquilla.exe");
  assert.equal(apPush.session_id, SESSION);
  assert.equal(apPush.project, "D:\\proj\\Demo");
  const apReq = apPush.requests.find((r) => r.id === "ap-1");
  assert.equal(apReq.kind, "approval");
  assert.equal(apReq.answerable, true);
  assert.equal(apReq.toolName, "shell");
  assert.equal(apReq.namespace, "exec");
  assert.ok(apReq.reason.includes("rm -rf x") && apReq.reason.includes("会删文件"));
  await waitFor(() => notifies.some((n) => n.event === "permission" && n.session_id === SESSION),
    "permission 通知照旧");

  // 2) 交互式 user_input（tool_result + JSON 字符串 result + 归一化 questions）→ 提问卡
  marker = pushes.length;
  gwPush("session.event.tool_result", {
    session_key: SESSION,
    result: JSON.stringify({
      kind: "user_input", paused: true, status: "input_required", request_id: "rq-1",
      clarify_schema: { fields: [{ name: "q1", prompt: "选一个", type: "enum", choices: ["甲", "乙"], allow_other: true }] },
      questions: [{ id: "q1", question: "选一个", options: [{ label: "甲", description: "第一个" }, { label: "乙" }] }],
    }),
  });
  await waitFor(() => pushAfter(marker, (p) => reqsOf(p).includes("rq-1")), "澄清卡推送");
  const qReq = pushAfter(marker, (p) => reqsOf(p).includes("rq-1")).requests.find((r) => r.id === "rq-1");
  assert.equal(qReq.kind, "question");
  assert.equal(qReq.answerable, true);
  assert.deepEqual(qReq.questions, [{
    id: "q1", question: "选一个",
    options: [{ label: "甲", description: "第一个" }, { label: "乙", description: "" }],
    multi_select: false,
  }]);

  // 3) plan_presentation → 提问卡（choices 回落）
  marker = pushes.length;
  gwPush("session.event.plan_presentation", {
    session_key: SESSION, request_id: "pl-1", message: "选个方案", choices: ["A", "B"],
  });
  await waitFor(() => pushAfter(marker, (p) => reqsOf(p).includes("pl-1")), "方案卡推送");

  // 4) 答案取件 → 网关 RPC → ack → 销账重推
  marker = pushes.length;
  const rpcBase = rpcCalls.length;
  answerQueue = [
    { answer_id: "ans-1", session_id: SESSION, kind: "approval", id: "ap-1", outcome: "allowed-once" },
    { answer_id: "ans-2", session_id: SESSION, kind: "question", id: "rq-1", answers: [{ id: "q1", selected: ["乙"] }] },
  ];
  await waitFor(() => rpcCalls.length >= rpcBase + 2, "两条应答 RPC");
  assert.deepEqual(rpcCalls[rpcBase], { method: "exec.approval.resolve", params: { id: "ap-1", approved: true } });
  assert.deepEqual(rpcCalls[rpcBase + 1], {
    method: "chat.clarify_submit",
    params: { sessionKey: SESSION, requestId: "rq-1", fields: { q1: "乙" } },
  });
  await waitFor(() => acks.some((a) => a.answer_id === "ans-1" && a.ok === true) &&
    acks.some((a) => a.answer_id === "ans-2" && a.ok === true), "两条 ok ack");
  await waitFor(() => pushAfter(marker, (p) => p.session_id === SESSION &&
    reqsOf(p).includes("pl-1") && !reqsOf(p).includes("ap-1") && !reqsOf(p).includes("rq-1")),
  "应答后销账重推");
  assert.ok(lastAnswerPollUrl.includes("exe=OpenSquilla.exe"));

  // 5) plugin 命名空间审批 → plugin.approval.resolve
  marker = pushes.length;
  gwPush("session.event.approval_required", {
    session_key: SESSION, approval_id: "ap-2", namespace: "plugin", tool_name: "some-plugin",
  });
  await waitFor(() => pushAfter(marker, (p) => reqsOf(p).includes("ap-2")), "plugin 审批卡");
  const rpcBase2 = rpcCalls.length;
  answerQueue = [{ answer_id: "ans-3", session_id: SESSION, kind: "approval", id: "ap-2", outcome: "allowed-once" }];
  await waitFor(() => rpcCalls.length >= rpcBase2 + 1, "plugin resolve");
  assert.deepEqual(rpcCalls[rpcBase2], { method: "plugin.approval.resolve", params: { id: "ap-2", approved: true } });

  // 6) 网关拒绝 → ack ok:false 带原因，卡片保留；随后 approval.resolved 清账
  marker = pushes.length;
  gwPush("session.event.approval_required", { session_key: SESSION, approval_id: "ap-3", tool_name: "shell" });
  await waitFor(() => pushAfter(marker, (p) => reqsOf(p).includes("ap-3")), "ap-3 卡");
  rpcFailNext = { code: "NOT_FOUND", message: "approval gone" };
  answerQueue = [{ answer_id: "ans-4", session_id: SESSION, kind: "approval", id: "ap-3", outcome: "rejected" }];
  await waitFor(() => acks.some((a) => a.answer_id === "ans-4"), "拒绝 ack");
  const failAck = acks.find((a) => a.answer_id === "ans-4");
  assert.equal(failAck.ok, false);
  assert.ok(failAck.error.includes("网关拒绝应答") && failAck.error.includes("approval gone"));
  marker = pushes.length;
  gwPush("exec.approval.resolved", { session_key: SESSION, approval_id: "ap-3", approved: false });
  await waitFor(() => pushAfter(marker, (p) => p.session_id === SESSION && !reqsOf(p).includes("ap-3")),
    "resolved 清账");

  // 7) turn_committed → 整会话销账（空集推送）
  marker = pushes.length;
  gwPush("session.event.turn_committed", { session_key: SESSION, turn_id: "t-2" });
  await waitFor(() => pushAfter(marker, (p) => p.session_id === SESSION && p.requests.length === 0),
    "turn_committed 空集");

  // 8) --no-answer：零 /requests、零取件，但 /notify 照旧
  child.kill();
  await waitFor(() => child.exitCode !== null || child.signalCode !== null, "桥1退出");
  const listBase = gwListCalls;
  child2 = spawnBridge(["--no-answer"], logFile2);
  await waitFor(() => gwListCalls >= listBase + 1, "桥2握手");
  await sleep(500); // 让桥2跑完订阅
  const pushesBase = pushes.length;
  const pollsBase = answerPolls;
  const rpcBase3 = rpcCalls.length;
  const notifyBase = notifies.length;
  gwPush("session.event.approval_required", { session_key: SESSION, approval_id: "ap-x", tool_name: "shell" });
  await waitFor(() => notifies.length > notifyBase, "no-answer 下 notify 照旧");
  await sleep(2500); // 覆盖两个取件周期 + 推送窗口
  assert.equal(pushes.length, pushesBase, "--no-answer 不应推 /requests");
  assert.equal(answerPolls, pollsBase, "--no-answer 不应取件");
  assert.equal(rpcCalls.length, rpcBase3, "--no-answer 不应答 RPC");

  console.log("TEST OK: 事件→/requests、答案→RPC、ack/销账/清账、--no-answer 全部通过");
}

function cleanup() {
  try { child && child.kill(); } catch { /* ignore */ }
  try { child2 && child2.kill(); } catch { /* ignore */ }
  for (const s of gwSockets) { try { s.destroy(); } catch { /* ignore */ } }
  gw.close();
  native.close();
}

const watchdog = setTimeout(() => {
  console.error("TEST FAIL: 总超时 90s");
  try { console.error(fs.readFileSync(logFile, "utf8")); } catch { /* 无日志 */ }
  cleanup();
  process.exit(1);
}, 90000);

try {
  await main();
  clearTimeout(watchdog);
  cleanup();
  process.exit(0);
} catch (e) {
  console.error("TEST FAIL:", e.message);
  try { console.error("—— 桥日志 ——\n" + fs.readFileSync(logFile, "utf8")); } catch { /* 无日志 */ }
  cleanup();
  process.exit(1);
}