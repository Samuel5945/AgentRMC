#!/usr/bin/env node
// agentrmc-mcp：AgentRMC 桌宠 MCP server（stdio，零依赖）。
// 传输：本机 AgentBridge（127.0.0.1:45915），AgentRMC 没开时工具返回友好错误不崩。
// 工具：
//   pet_notify  阶段进度/自定义提示 → 桌宠气泡（不出声；收起时走托盘）
//   pet_speak   让桌宠说一句话     → 气泡 + TTS 朗读
//   pet_state   查桌宠状态（可见/监听/录音/转写/对话模式/TTS 就绪）
//   pet_work_update 主动汇报阶段    → 更新「待我处理」列表里的工单
//                   （progress 正在干 / blocked 卡住等人 / ready 做完等验收；
//                    progress 只落工单，blocked / ready 会冒气泡 + 语音提醒）
//   cursor_point Agent 专用指针      → 指给用户看 / 真点一下，全程不动系统鼠标
// 等授权、等选方案、等你回话（回复以问句收尾）、任务完成、模型出错（StopFailure，
// 仅 Qoder / Qoder CN）的提醒由本插件的 hook 自动完成，不要用 pet_notify 重复发。
"use strict";

const http = require("http");
const { resolveHost } = require("../lib/host");
const readVersion = require("../lib/version");

const PORT = Number(process.env.AGENTRMC_BRIDGE_PORT || 45915);
const HOST = "127.0.0.1";
// 气泡署名跟随宿主 Agent（与 hook 用同一套自检），别在 Qoder 里念成 ZCode，
// 也别把 Qoder CN 念成国际版那个 Qoder。环境变量显式给了仍以给的为准。
const HOST_INFO = resolveHost({
  scriptDir: __dirname,
  argv: [],
  env: process.env,
});
const AGENT = HOST_INFO.agent;
// 握手里的版本就是这份已加载的代码是哪一版（原先手写的 0.1.0 从来没跟着包走过）。
const PLUGIN_VERSION = readVersion() || "unknown";

function post(route, payload) {
  return new Promise((resolve, reject) => {
    // 每个 POST 都带上自己这一版：原生端据此知道现在答话的 MCP 是哪版
    // （宿主不重启，进程里还是旧 server，光看盘上目录判断不了）。
    const body = JSON.stringify(
      PLUGIN_VERSION
        ? Object.assign({}, payload, { plugin_version: PLUGIN_VERSION })
        : payload);
    const req = http.request(
      { host: HOST, port: PORT, path: route, method: "POST", timeout: 2000,
        headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body) } },
      (res) => {
        let data = "";
        res.on("data", (c) => { data += c; });
        res.on("end", () => resolve({ status: res.statusCode, body: data }));
      },
    );
    req.on("timeout", () => req.destroy(new Error("AgentRMC 响应超时")));
    req.on("error", (e) => reject(e));
    req.end(body);
  });
}

function getState() {
  return getRoute("/state");
}

// GET 任意路由。以前 callBridge(route, null) 会退回 getState()——而 getState 把
// 路由写死成 /state，于是 /cursor/state 的读取永远拿到桌宠状态（没有 last_click），
// 指针回执读不到、被当成「回执还没回来」（2026-11-17 实测踩到）。
function getRoute(route) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      { host: HOST, port: PORT, path: route, method: "GET", timeout: 2000 },
      (res) => {
        let data = "";
        res.on("data", (c) => { data += c; });
        res.on("end", () => resolve({ status: res.statusCode, body: data }));
      },
    );
    req.on("timeout", () => req.destroy(new Error("AgentRMC 响应超时")));
    req.on("error", (e) => reject(e));
    req.end();
  });
}

async function callBridge(route, payload) {
  try {
    // 没 payload 时走 GET 的**同一条路由**，不是永远打 /state：
    // /cursor/state 读的是指针回执，打错路由会静默拿到桌宠状态（实测踩到）。
    const res = payload ? await post(route, payload) : await getRoute(route);
    if (res.status === 0 || res.status === undefined) {
      throw new Error("无响应");
    }
    return res;
  } catch (e) {
    throw new Error(
      `AgentRMC 没连上（${e.message}）。请确认 AgentRMC 正在运行；没有它，桌宠交互工具不可用，但 hook 的自动提醒本来就只在它运行时才有意义。`,
    );
  }
}

const TOOLS = [
  {
    name: "pet_notify",
    description:
      "在 AgentRMC 桌宠上冒一条气泡提示（不出声）。适合长任务的阶段进度、" +
      "中途发现的重要信息等非阻塞提醒；等授权、等选方案、等你回话、" +
      "任务完成、模型出错的提醒由 hook 自动发，" +
      "不要用这个工具重复发。message 一句话以内，太长的信息用户来不及看。",
    inputSchema: {
      type: "object",
      properties: {
        message: { type: "string", description: "气泡文案，一句话以内" },
        event: { type: "string", description: "可选事件标记（如 progress），只影响事件流归类" },
      },
      required: ["message"],
    },
  },
  {
    name: "pet_speak",
    description:
      "让 AgentRMC 桌宠说一句话：冒气泡并用 TTS 念出来（用户开 TTS 时出声）。" +
      "适合把最终结论、需要用户立刻注意的事念给用户。一次一句，别连续调用刷屏。",
    inputSchema: {
      type: "object",
      properties: {
        message: { type: "string", description: "要说的话，一句话以内" },
      },
      required: ["message"],
    },
  },
  {
    name: "pet_state",
    description:
      "查询 AgentRMC 桌宠当前状态：pet_visible/pet_enabled（桌宠可见/开关）、" +
      "docked（贴边收纳）、listening/recording/transcribing（语音监听/录音/转写中）、" +
      "chat_mode（对话模式）、tts_ready（TTS 可用）、" +
      "mic（麦克风读数：config=配置点名的设备或 auto、pick/pick_source=这次会用哪台、" +
      "device/source=实际在录的那台、note=点名没用上的原因）。" +
      "想在桌宠上提示前可先查一眼；语音录不到就先看 mic。",
    inputSchema: { type: "object", properties: {} },
  },
  {
    name: "pet_work_update",
    description:
      "把当前任务的阶段汇报给 AgentRMC 的「待我处理」列表：" +
      "progress（正在干）、blocked（卡住了，等人）、ready（做完了，等验收）。" +
      "progress 只更新工单不出声；blocked 和 ready 会冒气泡并用语音提醒用户" +
      "（要人动手的事必须喊到），所以别拿它们当进度条刷——阶段真的变了才报。" +
      "回合以问句收尾等用户回答时不用发 blocked——hook 会按回复收尾的问句自动提醒。" +
      "想额外说一句话请用 pet_speak。",
    inputSchema: {
      type: "object",
      properties: {
        stage: {
          type: "string",
          enum: ["progress", "blocked", "ready"],
          description: "progress 正在干 / blocked 卡住等人 / ready 做完等验收",
        },
        message: {
          type: "string",
          description: "一句话说明在做什么、卡在哪、做完了什么；省略时按阶段给默认文案",
        },
        project: {
          type: "string",
          description: "可选，工作目录，用来区分同名的多个任务",
        },
      },
      required: ["stage"],
    },
  },
  {
    name: "cursor_point",
    description:
      "用 AgentRMC 的「Agent 专用指针」把要操作的位置指给用户看，可选真点一下——" +
      "全程不移动用户的系统鼠标（没有 SetCursorPos / SendInput），用户能同时用鼠标。" +
      "坐标是物理像素（与截图同一套数）。要跟用户说「我点的是这里」、演示操作、" +
      "或用户抱怨「你一动手我就用不了鼠标」时用它；目标程序有更好的接口" +
      "（浏览器 CDP、Office COM、命令行）时优先走那些。" +
      "点击是异步的，本工具会替你回读回执并在正文里给出 backend/verified/target。",
    inputSchema: {
      type: "object",
      properties: {
        x: { type: "number", description: "目标点横坐标（物理像素）" },
        y: { type: "number", description: "目标点纵坐标（物理像素）" },
        click: {
          type: "boolean",
          description: "true = 移过去并点一下（默认）；false = 只指不点",
        },
        button: {
          type: "string",
          enum: ["left", "right", "middle", "double"],
          description: "点哪个键，默认 left",
        },
        hold_ms: {
          type: "number",
          description: "这么久没有新动作就自动淡出（毫秒），默认 8000；0 = 一直留着",
        },
      },
      required: ["x", "y"],
    },
  },
];

async function callTool(name, args) {
  if (name === "pet_notify") {
    const message = String(args?.message || "").trim();
    if (!message) return { isError: true, text: "message 不能为空" };
    const res = await callBridge("/notify", {
      event: String(args?.event || "progress"),
      title: AGENT,
      message,
    });
    if (res.status >= 400) {
      return { isError: true, text: `AgentRMC 拒绝了这条提示（HTTP ${res.status}）：${res.body}` };
    }
    return { text: `已发送（HTTP ${res.status}）` };
  }
  if (name === "pet_speak") {
    const message = String(args?.message || "").trim();
    if (!message) return { isError: true, text: "message 不能为空" };
    const res = await callBridge("/speak", { message });
    if (res.status >= 400) {
      return { isError: true, text: `AgentRMC 拒绝了这句话（HTTP ${res.status}）：${res.body}` };
    }
    return { text: `已让桌宠说（HTTP ${res.status}）` };
  }
  if (name === "pet_state") {
    const res = await callBridge("/state", null);
    return { text: res.body };
  }
  if (name === "pet_work_update") {
    const stage = String(args?.stage || "").trim().toLowerCase();
    if (stage !== "progress" && stage !== "blocked" && stage !== "ready") {
      return { isError: true, text: "stage 只支持 progress / blocked / ready" };
    }
    const res = await callBridge("/work-items/update", {
      stage,
      message: String(args?.message || "").trim(),
      // 产品名让原生端反查进程映像名：Worker 行按 exe 归堆，认错就挂到别人头上。
      title: AGENT,
      raise: HOST_INFO.raise || "",
      project: String(args?.project || "").trim(),
    });
    if (res.status >= 400) {
      return {
        isError: true,
        text: `AgentRMC 拒绝了这次汇报（HTTP ${res.status}）：${res.body}`,
      };
    }
    return { text: `已汇报 stage=${stage}（HTTP ${res.status}）${res.body || ""}` };
  }
  if (name === "cursor_point") {
    const x = Number(args?.x);
    const y = Number(args?.y);
    if (!Number.isFinite(x) || !Number.isFinite(y)) {
      return { isError: true, text: "x / y 必须是数字（物理像素）" };
    }
    const wantsClick = args?.click !== false;
    const button = String(args?.button || "left");
    const holdMs = Number.isFinite(Number(args?.hold_ms))
      ? Number(args?.hold_ms)
      : 8000;
    const route = wantsClick ? "/cursor/click" : "/cursor/move";
    const payload = {
      x: Math.round(x),
      y: Math.round(y),
      agent: AGENT,
      duration_ms: 240,
      hold_ms: holdMs,
    };
    if (wantsClick) {
      payload.button = button;
      payload.backend = "auto";
    }
    const res = await callBridge(route, payload);
    if (res.status === 409) {
      return {
        isError: true,
        text:
          "Agent 指针在 AgentRMC 里被用户关掉了（系统页 → Agent 指针）。" +
          "改用别的通道，或先请用户打开。",
      };
    }
    if (res.status >= 400) {
      return {
        isError: true,
        text: `AgentRMC 拒绝了这次指针动作（HTTP ${res.status}）：${res.body}`,
      };
    }
    // 点击异步：回读一次 last_click，别让 clicked:true 被当成「真点到了」。
    // 先记下点击前的 at_ms 当基线——上一次点击的回执会一直留在 state 里，
    // 不比基线就会把旧回执当成本次结果（假阳性比没有回执更坏）。
    // UIA 的 Invoke 能阻塞到秒级，所以给到 ~2s。
    let receipt = "";
    if (wantsClick) {
      let baseline = 0;
      try {
        const before = await callBridge("/cursor/state", null);
        baseline = Number((JSON.parse(before.body || "{}").last_click || {}).at_ms) || 0;
      } catch {
        // 读不到基线就按 0 处理：最多多等一轮，不会给出错回执。
      }
      for (let i = 0; i < 14; i += 1) {
        await new Promise((r) => setTimeout(r, 150));
        try {
          const st = await callBridge("/cursor/state", null);
          const parsed = JSON.parse(st.body || "{}");
          const last = parsed.last_click;
          if (last && Number(last.at_ms) > baseline) {
            receipt =
              ` backend=${last.backend || "?"}` +
              ` verified=${last.verified ? "true" : "false"}` +
              ` target=${last.target || "?"}` +
              (last.ok ? "" : ` error=${last.error || "?"}`);
            break;
          }
        } catch {
          break;
        }
      }
      if (!receipt) {
        receipt = "（回执还没回来：稍后查 /cursor/state 的 last_click，别当成一定点到了）";
      }
    }
    return {
      text:
        (wantsClick ? "已让指针移过去并点击" : "已把指针移过去") +
        `（x=${Math.round(x)}, y=${Math.round(y)}，HTTP ${res.status}）` +
        receipt,
    };
  }
  return { isError: true, text: `未知工具: ${name}` };
}

function send(msg) {
  process.stdout.write(JSON.stringify(msg) + "\n");
}

async function handle(req) {
  const { id, method, params } = req;
  if (id === undefined || id === null) return; // notification
  let result;
  switch (method) {
    case "initialize":
      result = {
        protocolVersion: params?.protocolVersion || "2024-11-05",
        capabilities: { tools: {} },
        serverInfo: { name: "agentrmc-mcp", version: PLUGIN_VERSION },
      };
      break;
    case "tools/list":
      result = { tools: TOOLS };
      break;
    case "tools/call": {
      const name = params?.name;
      if (!TOOLS.some((t) => t.name === name)) {
        send({ jsonrpc: "2.0", id, error: { code: -32602, message: `未知工具: ${name}` } });
        return;
      }
      try {
        const r = await callTool(name, params.arguments || {});
        result = { content: [{ type: "text", text: r.text }], isError: !!r.isError };
      } catch (e) {
        result = { content: [{ type: "text", text: e.message }], isError: true };
      }
      break;
    }
    case "ping":
      result = {};
      break;
    default:
      send({ jsonrpc: "2.0", id, error: { code: -32601, message: `方法不存在: ${method}` } });
      return;
  }
  send({ jsonrpc: "2.0", id, result });
}

let buf = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, i).trim();
    buf = buf.slice(i + 1);
    if (!line) continue;
    let req;
    try { req = JSON.parse(line); } catch { continue; }
    handle(req).catch((e) => {
      if (req.id !== undefined && req.id !== null) {
        send({ jsonrpc: "2.0", id: req.id, error: { code: -32603, message: e.message } });
      }
    });
  }
});
process.stdin.on("end", () => process.exit(0));
console.error(
  `[agentrmc-mcp] ready | bridge ${HOST}:${PORT} | host ${AGENT} (${HOST_INFO.source})`,
);
