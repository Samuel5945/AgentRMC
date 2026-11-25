// 手机页（LAN /m/）离线验收：不装机、不连真机，就能跑交互断言和逐场景截图。
//
// 页面正文直接从 native/src/core/LanWebServer.cpp 的 R"HTML( ... )HTML" 段里抽，
// 不另存一份 HTML —— 只有一处真源，改完页面这里自动跟上。
//
// 用法：
//   node rc_probe/mobile_page_probe.mjs serve          # 只起假服务（127.0.0.1:4600/m/）
//   node rc_probe/mobile_page_probe.mjs flow           # 多选/长按/左滑销账/右滑启动的断言
//   node rc_probe/mobile_page_probe.mjs shots [场景…]  # 截图到 rc_probe/mobile_shots/
//
// 依赖 playwright-core（Chromium 用本机 ms-playwright 里那份）：
//   npm i -g playwright-core
// 也可以直接装在 rc_probe/node_modules（该目录被 gitignore 掉，不脏仓库）：
//   cd rc_probe && npm i playwright-core
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
const CPP = path.join(ROOT, 'native', 'src', 'core', 'LanWebServer.cpp');
const SHOTS = path.join(HERE, 'mobile_shots');
// Chromium 用本机 ms-playwright 里那份（版本号会变，按目录名倒序取第一个能用的）。
function chromePath() {
  const root = path.join(process.env.LOCALAPPDATA || '', 'ms-playwright');
  let names = [];
  try { names = fs.readdirSync(root).filter((n) => n.startsWith('chromium-')).sort().reverse(); } catch { /* 没装 */ }
  for (const n of names) {
    const exe = path.join(root, n, 'chrome-win64', 'chrome.exe');
    if (fs.existsSync(exe)) return exe;
  }
  return '';
}
const CHROME = chromePath();
const require = createRequire(import.meta.url);

function mobileHtml() {
  const src = fs.readFileSync(CPP, 'utf8');
  const chunks = [...src.matchAll(/R"HTML\(([\s\S]*?)\)HTML"/g)].map((m) => m[1]);
  if (!chunks.length) throw new Error('LanWebServer.cpp 里找不到 R"HTML( ... )HTML" 段');
  return chunks.join('');
}

function loadPlaywright() {
  const tries = ['playwright-core'];
  try {
    tries.push(path.join(execSync('npm root -g', { encoding: 'utf8' }).trim(), 'playwright-core'));
  } catch { /* npm 不在 PATH 上就算了 */ }
  tries.push(path.join(ROOT, 'node_modules', 'playwright-core'));
  tries.push(path.join(HERE, 'node_modules', 'playwright-core'));
  for (const p of tries) {
    try { return require(p); } catch { /* 换下一个 */ }
  }
  console.error('找不到 playwright-core：先跑 npm i -g playwright-core');
  process.exit(2);
}

// —— 假数据：四条待办覆盖三种阶段 + 一条带派发通知 ——
const LIGHT = {
  bg: '#f3f5f8', 'card.bg': '#ffffff', 'card.bg2': '#eef1f5',
  text: '#171e2a', 'text.soft': '#1c2331', 'text.dim': '#2a323f',
  muted: '#48525f', 'muted.2': '#5a6472', ghost: '#4a5361', faint: '#8f98a3',
  line: '#c9d0d9', accent: '#4f8cff', 'accent.hover': '#6299ff',
  'on.accent': '#ffffff', danger: '#e5484d', warn: '#d29922', ok: '#3fb950',
  pending: '#f0883e', info: '#58a6ff',
  w6: 'rgba(15,23,42,.047)', w8: 'rgba(15,23,42,.063)', w10: 'rgba(15,23,42,.078)',
  w14: 'rgba(15,23,42,.110)', w18: 'rgba(15,23,42,.141)', w30: 'rgba(15,23,42,.235)',
  a26: 'rgba(79,140,255,.102)', a36: 'rgba(79,140,255,.141)', a64: 'rgba(79,140,255,.251)'
};
const DARK = {
  bg: '#0f1319', 'card.bg': '#171d26', 'card.bg2': '#1d2530',
  text: '#eef2f7', 'text.soft': '#dbe3ec', 'text.dim': '#c2ccd8',
  muted: '#93a0b0', 'muted.2': '#7d8a9a', ghost: '#9aa7b6', faint: '#5c6878',
  line: '#2c3644', accent: '#4f8cff', 'accent.hover': '#6299ff',
  'on.accent': '#ffffff', danger: '#f2555a', warn: '#e3b341', ok: '#3fb950',
  pending: '#f0883e', info: '#58a6ff',
  w6: 'rgba(255,255,255,.055)', w8: 'rgba(255,255,255,.075)', w10: 'rgba(255,255,255,.095)',
  w14: 'rgba(255,255,255,.13)', w18: 'rgba(255,255,255,.17)', w30: 'rgba(255,255,255,.30)',
  a26: 'rgba(79,140,255,.20)', a36: 'rgba(79,140,255,.28)', a64: 'rgba(79,140,255,.46)'
};

const NOW = Date.now();
const iso = (ms) => new Date(ms).toISOString();
// 底部实时行的文案（flow 里会改它，验证「原位更新、不堆叠」）。
let LIVE_TEXT = '思考：…把那条内容校验断言的写法改成直白写法 · 本轮已 13m41s';
const ITEMS = () => ([
  {
    id: 'a1', title: 'WorkBuddy AI', stage: 'needs-me',
    project: 'D:\\githubrepos\\Agentrmc', session_id: 'sess-8f31c2a9',
    session_title: '手机页多选与滑动优化', done: false,
    message: 'WorkBuddy AI：等你确认是否把 0.6.0 插件覆盖安装到本机。',
    updated_at: iso(NOW - 3 * 60e3), attention_since: iso(NOW - 22 * 60e3)
  },
  {
    id: 'a2', title: 'ZCode', stage: 'needs-me',
    project: 'D:\\githubrepos\\Agentrmc\\native', session_id: 'sess-2b91aa40',
    session_title: '打包链路排查', done: false,
    message: '要不要把 dist 里的旧安装包归档？',
    updated_at: iso(NOW - 9 * 60e3), attention_since: iso(NOW - 9 * 60e3)
  },
  {
    id: 'a3', title: 'Qoder CN', stage: 'review',
    project: 'D:\\githubrepos\\Agentrmc\\plugins', session_id: 'sess-77ce0193',
    session_title: '插件版本同步脚本', done: false,
    message: 'Qoder CN：六处清单已对齐 0.6.0，等你验收。',
    updated_at: iso(NOW - 41 * 60e3), attention_since: iso(NOW - 41 * 60e3)
  },
  {
    id: 'a4', title: 'DeepSeek Harness', stage: 'working',
    project: 'D:\\githubrepos\\Agentrmc', session_id: 'sess-4a02be77',
    session_title: '转写队列', done: false,
    message: '正在重跑语音转写的长音频用例，预计 3 分钟。',
    updated_at: iso(NOW - 2 * 60e3)
  },
  {
    id: 'a5', title: 'Qoder', stage: 'needs-me',
    project: 'D:\\githubrepos\\lig-site', session_id: 'sess-5c81de20',
    session_title: '官网预览图重做', done: false,
    message: '要不要把这一批官网改动一起提交并推送？',
    updated_at: iso(NOW - 5 * 60e3), attention_since: iso(NOW - 5 * 60e3)
  },
  {
    id: 'a6', title: 'WorkBuddy', stage: 'review',
    project: 'D:\\work\\weekly-report', session_id: 'sess-9d2f4b18',
    session_title: '设计周报', done: false,
    message: '本周设计周报已生成，等你验收。',
    updated_at: iso(NOW - 16 * 60e3), attention_since: iso(NOW - 16 * 60e3)
  },
  {
    id: 'a7', title: 'OpenSquilla', stage: 'working',
    project: 'D:\\work\\research', session_id: 'agent:main:webchat:aaaaaaa1',
    session_title: '竞品数据整理', done: false,
    message: '正在整理本周竞品数据，预计 10 分钟。',
    updated_at: iso(NOW - 7 * 60e3)
  }
]);

const SESSIONS = () => ([
  {
    exe: 'WorkBuddyAI.exe', name: 'WorkBuddy AI', alive: true, window: true,
    can_dispatch: true, count: 6,
    sessions: [
      {
        id: 'sess-8f31c2a9', title: '手机页多选与滑动优化',
        project: 'D:\\githubrepos\\Agentrmc', updated_at: iso(NOW - 2 * 60e3),
        work_id: 'a1', stage: 'needs-me', can_dispatch: true
      },
      {
        id: 'sess-71ab09f3', title: '语音派发回执排查',
        project: 'D:\\githubrepos\\Agentrmc\\native', updated_at: iso(NOW - 58 * 60e3),
        can_dispatch: true
      },
      {
        id: 'sess-a0c14d82', title: '主题灵感板配色整理',
        project: 'D:\\design\\LigSite', updated_at: iso(NOW - 26 * 3600e3),
        can_dispatch: true
      },
      {
        id: 'sess-tuwen-live', title: '图文编辑器排版联调',
        project: 'D:\\githubrepos\\tuwen-editor', updated_at: iso(NOW - 2 * 3600e3),
        can_dispatch: true
      },
      {
        id: 'sess-no-project', title: '临时排障记录',
        project: '', updated_at: iso(NOW - 3 * 3600e3),
        can_dispatch: true
      },
      {
        id: 'sess-archived', title: '旧图文编辑器备份',
        project: 'C:\\Users\\PC\\Desktop\\tuwen-editor',
        updated_at: iso(NOW - 20 * 86400e3),
        archived: true, can_dispatch: false
      }
    ]
  },
  {
    exe: 'Qoder CN.exe', name: 'Qoder CN', alive: true, window: true,
    can_dispatch: false, count: 1,
    sessions: [
      {
        id: 'sess-77ce0193', title: '插件版本同步脚本',
        project: 'D:\\githubrepos\\Agentrmc\\plugins', updated_at: iso(NOW - 41 * 60e3),
        can_dispatch: false
      }
    ]
  },
  {
    exe: 'OpenSquilla.exe', name: 'OpenSquilla', alive: true, window: true,
    can_dispatch: true, count: 2,
    sessions: [
      {
        id: 'agent:main:webchat:aaaaaaa1', title: '竞品数据整理',
        project: 'D:\\work\\research', updated_at: iso(NOW - 7 * 60e3),
        work_id: 'a7', stage: 'working', can_dispatch: true
      },
      {
        id: 'agent:main:cli:bbbbbbbb', title: '网关连接排查',
        project: 'D:\\work\\research', updated_at: iso(NOW - 3 * 3600e3),
        can_dispatch: true
      }
    ]
  }
]);

function sessionTranscript(exe, session) {
  const agent = SESSIONS().find((a) => a.exe === exe);
  const row = agent && agent.sessions.find((s) => s.id === session);
  if (!row) return null;
  return {
    total: 3, supported: true, sessionTitle: row.title, agentExe: exe,
    agentName: agent.name, sessionId: session,
    // 底部实时行（2026-11-20）：长轮跑着时 transcript 没有新行，native 把进行中
    // 卡片的活动回声 + 本轮时长塞进 live；这里给一条让页面断言得到。
    live: session === 'sess-8f31c2a9' ? { text: LIVE_TEXT, at: iso(NOW - 5e3) } : undefined,
    messages: [
      { id: 'sm0', role: 'user', at: iso(NOW - 760e3),
        text: '请查看文件：D:/githubrepos/Agentrmc/.agentrmc/inbox/sess-8f31c2a9/20261118-204924-965-shot.jpg' },
      { id: 'sm1', role: 'user', at: iso(NOW - 700e3), text: '这个会话现在做到哪一步了？' },
      { id: 'sm2', role: 'assistant', at: iso(NOW - 650e3),
        text: '卡片层级已经拆好了，现在在收滑动阈值和批量动效。产物：'
          + 'rc_probe/mobile_shots/chatAtt-light.png 和 dist/AgentRMC-Setup.exe' },
      { id: 'sm3', role: 'user', at: iso(NOW - 480e3), text: '继续，把交互再收一下。' },
      // 系统信息行：工具调用耗时（不是对话气泡，灰色一行）
      { id: 'sm3b', role: 'system', at: iso(NOW - 460e3),
        text: '执行命令 5.7s · python build.py' },
      // present_files 摆出来的产物 + 正文里写的中文相对路径：同一份文件两条来源，
      // 卡片名要带扩展名、只出一张（2026-11-19 用户截图：卡片叫「凝蓝_OS8」，
      // 下载下来是 file.txt）。
      { id: 'sm4', role: 'assistant', at: iso(NOW - 400e3),
        text: '**已产出**：`产物/OS8成品/凝蓝_OS8.hwt`（43.25 MB）\n\n这个包只有 `.hwt`，没生成释放器 APK。',
        files: ['D:/githubrepos/Agentrmc/产物/OS8成品/凝蓝_OS8.hwt',
                'D:/githubrepos/Agentrmc/产物/_hyperos4_inspect/os8_package_preview.jpg'] },
      // Qoder / WorkBuddy 的产物链接是 file:/// URI，不是裸路径（2026-11-25
      // 用户报「rmc 里 Qoder 会话不显示文件下载到手机」的真凶）。中文还可能是
      // percent 编码——不解码就找不到磁盘上的文件。
      { id: 'sm5', role: 'assistant', at: iso(NOW - 360e3),
        text: '已产出：\n'
          + '- [75 秒成片 · 4500 帧](file:///D:/ld-bili-v150/out/LiquidDesk-1.5.0-75s-4K60-r03.mp4)\n'
          + '- [r03 验收报告](file:///D:/ld-bili-v150/out/verification-r03.json)\n'
          + '- [快休封面](file:///C:/Users/PC/Desktop/%E5%BF%AB%E4%BC%91B%E7%AB%99%E5%B0%81%E9%9D%A2-1920x1080.jpg)',
        files: [] }
    ],
    requests: []
  };
}

function state(theme) {
  return {
    theme: theme === 'dark' ? DARK : LIGHT,
    work: { items: ITEMS().map((i) => ({ ...i, done: i.done || ACK.has(i.id) })), attention_count: Math.max(0, 3 - ACK.size) },
        dispatch: { noticeId: '', notice: '', noticeExe: '', draftId: '', draftText: '' },
    health: { bridge: true, uptime_seconds: 5400, workers: [{ alive: true }, { alive: true }], spool_pending: 0, problems: [] },
    meta: { version: '0.6.0', devices: 2, listening: true },
    // 第三个页签（应用页）：与 AppPage::webState() 同结构。图标用 1px PNG 占位，
    // 真机上这里下发的是一张张真实 exe 图标的 data URL。
    apps: {
      summary: '本机找到 5 / 8 个 · 语音启动要用本地识别（SenseVoice / Qwen3）；'
        + '转发热键时语音交给前台应用。想用手上的遥控器，'
        + '可以在「按键映射」里把「打开 xx」设成某个键的单击 / 双击 / 长按。',
      voiceLaunch: true,
      systemControl: true,
      rows: APPS().map((r) => ({ ...r, icon: 'data:image/png;base64,' + PNG_1PX.toString('base64') }))
    }
  };
}

// 应用页行：名称/状态/是否可点，覆盖「能启动 / 没装 / 自定义」三种。
function APPS() {
  return [
    { id: 'browser', name: '浏览器', state: '默认浏览器', stateTip: '系统默认浏览器 · chrome.exe', found: true, custom: false, customPath: '' },
    { id: 'chrome', name: 'Chrome', state: '开始菜单', stateTip: '开始菜单 · Chrome.lnk', found: true, custom: false, customPath: '' },
    { id: 'jianying', name: '剪映', state: 'Windows 应用', stateTip: 'Windows 应用', found: true, custom: false, customPath: '' },
    { id: 'qoder', name: 'Qoder', state: '开始菜单', stateTip: '开始菜单 · Qoder.lnk', found: true, custom: false, customPath: '' },
    { id: 'codex', name: 'Codex', state: '没装', stateTip: '开始菜单里没有这个应用的快捷方式', found: false, custom: false, customPath: '' },
    { id: 'custom:opensquilla', name: 'OpenSquilla', state: '自定义', stateTip: 'D:\\apps\\OpenSquilla\\OpenSquilla.exe', found: true, custom: true, customPath: 'D:\\apps\\OpenSquilla\\OpenSquilla.exe' }
  ];
}

let CURRENT_THEME = 'light';
const ACK = new Set();
const UPLOADS = [];  // /m/upload 收到的定位参数 + 文件名/字节数，附件断言用
const FILES = [];    // /m/files、/m/file 收到的 query，电脑文件面板断言用
// 演示图：假服务给图片用。用一张真正能看清的 480x360 渐变，
// 否则图片查看器截图里只有一片黑，看不出「点图弹大图」。
function makeDemoPng(w, h) {
  // 手写一张不压缩（store）的 RGB PNG：尺寸小、无依赖、不用 zlib 参数。
  const crcTable = [];
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crcTable[n] = c >>> 0;
  }
  const crc32 = (buf) => {
    let c = 0xffffffff;
    for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2;  // 8-bit RGB
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    const off = y * (w * 3 + 1);
    raw[off] = 0;  // filter none
    for (let x = 0; x < w; x++) {
      const p = off + 1 + x * 3;
      raw[p] = 40 + Math.round((x / w) * 150);            // R
      raw[p + 1] = 90 + Math.round((y / h) * 120);        // G
      raw[p + 2] = 200 - Math.round((x / w) * 110);       // B
    }
  }
  const idat = require('node:zlib').deflateSync(raw);
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', idat), chunk('IEND', Buffer.alloc(0)),
  ]);
}
const PNG_1PX = makeDemoPng(480, 360);

function startServer() {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      const url = new URL(req.url, 'http://127.0.0.1');
      const theme = url.searchParams.get('theme') || CURRENT_THEME;
      if (url.pathname === '/m/state') {
        res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
        return res.end(JSON.stringify(state(theme)));
      }
      if (url.pathname === '/m/sessions') {
        res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
        return res.end(JSON.stringify({ agents: SESSIONS(), generated_at: iso(Date.now()) }));
      }
      if (url.pathname === '/m/session') {
        const exe = url.searchParams.get('exe') || '';
        const session = url.searchParams.get('session') || '';
        const chat = sessionTranscript(exe, session);
        if (!chat) { res.writeHead(404); return res.end('no'); }
        res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
        return res.end(JSON.stringify(chat));
      }
      if (url.pathname === '/m/transcript') {
        res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
        return res.end(JSON.stringify({
          total: 3, supported: true, sessionTitle: '手机页多选与滑动优化',
          agentExe: 'WorkBuddyAI.exe',
          messages: [
            { id: 'm1', role: 'user', at: iso(NOW - 600e3), text: '把手机页的多选和滑动再打磨一下' },
            { id: 'm2', role: 'assistant', at: iso(NOW - 540e3),
              text: '好，我先把卡片拆成手势层和卡面层。安装包已出 dist/AgentRMC-Setup.exe，'
                + '截图在 rc_probe/mobile_shots/chatAtt-light.png 。' },
            { id: 'm3', role: 'user', at: iso(NOW - 300e3), text: '动效要轻，别挡着看内容' }
          ],
          requests: []
        }));
      }
      if (url.pathname === '/m/upload') {
        let body = '';
        req.on('data', (c) => (body += c));
        req.on('end', () => {
          const name = decodeURIComponent(req.headers['x-agentrmc-filename'] || 'f.bin');
          const rec = {
            id: url.searchParams.get('id') || '',
            session: url.searchParams.get('session') || '',
            project: url.searchParams.get('project') || '',
            name, size: Buffer.byteLength(body),
          };
          UPLOADS.push(rec);
          console.log('[upload]', JSON.stringify(rec));
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({
            ok: true, code: 200, name, size: rec.size,
            path: 'D:/githubrepos/Agentrmc/.agentrmc/inbox/sess-8f31c2a9/20261118-' + name,
          }));
        });
        return;
      }
      // 电脑文件（会话 inbox）：列表 + 下载。
      if (url.pathname === '/m/files') {
        FILES.length = 0;
        FILES.push({ req: url.search });
        res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
        return res.end(JSON.stringify({
          ok: true,
          files: [
            { name: '会议记录.md', size: 2048, mtime: iso(NOW) },
            { name: '截图一.png', size: 40960, mtime: iso(NOW - 60e3) },
            { name: 'missing.txt', size: 0, mtime: iso(NOW - 120e3) },
            { name: 'renamed.png', size: 1024, mtime: iso(NOW - 150e3) },
          ],
          artifacts: [
            { rel: 'dist/AgentRMC-Setup.exe', name: 'AgentRMC-Setup.exe',
              size: 36619313, mtime: iso(NOW - 30e3) },
          ],
          // 正文里提到的项目外产物（桌面）：面板要看得见、也要能下（abs 形态）。
          mentioned: [
            { abs: 'C:/Users/PC/Desktop/LiquidDesk-1.4.7-B站封面-4x3.jpg',
              name: 'LiquidDesk-1.4.7-B站封面-4x3.jpg', size: 807971,
              mtime: iso(NOW - 90e3), stale: false },
          ],
        }));
      }
      if (url.pathname === '/m/file/resolve') {
        // 失效链接兜底：原路径 404、同一目录里有同名文件时，这里 200。
        // 只有 renamed.png 能兜底找到；missing.txt 两级都失败。
        FILES.push({ resolve: url.search, method: req.method });
        if (url.searchParams.get('name') === 'missing.txt') {
          res.writeHead(404, { 'Content-Type': 'application/json' });
          return res.end('{"ok":false,"error":"file_missing"}');
        }
        const headers = {
          'Content-Type': 'application/octet-stream',
          'Content-Disposition': 'attachment; filename="a.txt"',
        };
        if (req.method === 'HEAD') { res.writeHead(200, headers); return res.end(); }
        res.writeHead(200, headers);
        return res.end('DOWNLOAD');
      }
      if (url.pathname === '/m/file') {
        // 真实下载契约：文件名/类型由服务端 Content-Disposition 定；
        // 文件不在了回 404 JSON，页面先 HEAD 探活，不该把 404 存成 file.txt。
        // renamed.png 模拟「正文给的路径已失效」（原路径 404，resolve 兜底找到）。
        if (url.searchParams.get('name') === 'missing.txt' ||
            url.searchParams.get('name') === 'renamed.png') {
          res.writeHead(404, { 'Content-Type': 'application/json' });
          return res.end('{"ok":false,"error":"file_missing"}');
        }
        FILES.push({ file: url.search, method: req.method });
        // 图片给真 PNG：否则 <img> 解不出来、onerror 把缩略图退化成
        // 文件条，「点图查看」的断言就够不着了。下载字节断言用的是
        // 非图片名（a.txt），不受影响。
        // 图片可能走 name=（inbox）也可能走 rel=/abs=（对话里的图），
        // 所以看整个 query 里的扩展名，不能只看 name。
        // 真服务的名字取「解析后的真实文件」（name / rel / abs 的 basename），
        // disposition 一律 attachment（图片也是）；<img src> 是子资源，
        // 不受 disposition 影响，所以缩略图照样出得来。
        const qName = url.searchParams.get('name') || '';
        const qRel = url.searchParams.get('rel') || '';
        const qAbs = url.searchParams.get('abs') || '';
        const full = qName || qRel || qAbs || 'a.txt';
        const fname = full.split(/[\\/]/).pop() || 'a.txt';
        const isImg = /\.(png|jpe?g|gif|webp|bmp)$/i.test(fname);
        // 真服务不把中文直接写进 header（非法字符）：
        // ASCII 兜底 + filename*=UTF-8''percent（见 docs/download-contract.md）。
        const asciiFallback = fname.replace(/[^\x20-\x7e]/g, '_');
        const disp = 'attachment; filename="' + asciiFallback +
          "\"; filename*=UTF-8''" + encodeURIComponent(fname);
        const headers = {
          'Content-Type': isImg ? 'image/png' : 'application/octet-stream',
          'Content-Disposition': disp,
        };
        if (req.method === 'HEAD') { res.writeHead(200, headers); return res.end(); }
        res.writeHead(200, headers);
        return res.end(isImg ? PNG_1PX : 'DOWNLOAD');
      }
      if (url.pathname === '/m/action') {
        let body = '';
        req.on('data', (c) => (body += c));
        req.on('end', () => {
          console.log('[action]', body);
          let action = {};
          try { action = JSON.parse(body || '{}'); } catch { /* 页面断言会报坏请求 */ }
          if (action.cmd === 'ack' && action.id) ACK.add(action.id);
          if (action.cmd === 'dispatchSession' && String(action.text || '').includes('拒绝')) {
            res.writeHead(409, { 'Content-Type': 'application/json' });
            return res.end(JSON.stringify({ ok: false, code: 409, error: '该会话已有派发在途，等回执后再派' }));
          }
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end('{"ok":true}');
        });
        return;
      }
      if (url.pathname === '/m/' || url.pathname === '/m' || url.pathname === '/') {
        ACK.clear();  // 每个截图/测试场景都从同一份四条假数据开始
        CURRENT_THEME = url.searchParams.get('theme') || 'light';
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
        return res.end(mobileHtml());
      }
      res.writeHead(404).end('no');
    });
    server.listen(4600, '127.0.0.1', () => {
      console.log('mock on http://127.0.0.1:4600/m/');
      resolve(server);
    });
  });
}

// 页面里只监听 touch 事件，鼠标点不动，所以手势用合成 TouchEvent 发。
const TOUCH = `
window.__touch=function(el,type,x,y){const mk=()=>new Touch({identifier:1,target:el,clientX:x,clientY:y});
 const done=type==='touchend'||type==='touchcancel';
 el.dispatchEvent(new TouchEvent(type,{bubbles:true,cancelable:true,
  touches:done?[]:[mk()],targetTouches:done?[]:[mk()],changedTouches:[mk()]}));};
window.__swipe=function(el,dx,dy,hold){const r=el.getBoundingClientRect();
 const x0=r.left+r.width*0.5,y0=r.top+r.height*0.5;
 window.__touch(el,'touchstart',x0,y0);
 for(let i=1;i<=6;i++)window.__touch(el,'touchmove',x0+dx*i/6,y0+dy*i/6);
 if(!hold)window.__touch(el,'touchend',x0+dx,y0+dy);};
window.__press=function(el,ms){const r=el.getBoundingClientRect();
 window.__touch(el,'touchstart',r.left+20,r.top+r.height/2);
 return new Promise(res=>setTimeout(()=>{
  window.__touch(el,'touchend',r.left+20,r.top+r.height/2);res(true);},ms));};`;

async function openPage(browser, theme) {
  const ctx = await browser.newContext({
    viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148'
  });
  const page = await ctx.newPage();
  await page.goto(`http://127.0.0.1:4600/m/?theme=${theme}`, { waitUntil: 'networkidle' });
  await page.waitForSelector('#list .card');
  await page.addScriptTag({ content: TOUCH });
  await page.waitForTimeout(120);
  return { ctx, page };
}

const SCENES = {
  list: async () => {},
  multi: async (page) => {
    await page.click('#multiSw');
    await page.waitForTimeout(120);
    await page.locator('#list .card').nth(0).click();
    await page.locator('#list .card').nth(2).click();
    await page.waitForTimeout(260);
  },
  swipeR: async (page) => {
    await page.evaluate(() => window.__swipe(document.querySelectorAll('#list .card')[1], 78, 4, true));
    await page.waitForTimeout(80);
  },
  swipeL: async (page) => {
    await page.evaluate(() => window.__swipe(document.querySelectorAll('#list .card')[0], -78, 4, true));
    await page.waitForTimeout(80);
  },
  batch: async (page) => {
    await page.click('#multiSw');
    await page.locator('#list .card').nth(0).click();
    await page.locator('#list .card').nth(1).click();
    await page.click('#batchAck');
    await page.waitForTimeout(110);
  },
  dispatch: async (page) => {
    await page.click('#tabbar .tab[data-tab="1"]');
    await page.waitForSelector('#sessList .sescard');
    await page.waitForTimeout(160);
  },
  dispatchCollapsed: async (page) => {
    await page.click('#tabbar .tab[data-tab="1"]');
    await page.waitForSelector('#sessList .sescard');
    await page.locator('#sessList .projhead').first().click();
    await page.waitForTimeout(200);
  },
  // 图片查看器：点对话里的图弹全屏查看（本版亮点）。
  imageView: async (page) => {
    await page.click('#tabbar .tab[data-tab="1"]');
    await page.waitForSelector('#sessList .sescard');
    await page.locator('#sessList .sescard').nth(0).click();
    await page.waitForSelector('#pchat:not([hidden]) .chatlist .bub');
    const img = page.locator('.bubatt.img').first();
    await img.waitFor({ timeout: 8000 });
    await img.click();
    await page.waitForSelector('#imgView:not([hidden])');
    await page.waitForTimeout(220);
  },
  sessionChat: async (page) => {
    await page.click('#tabbar .tab[data-tab="1"]');
    await page.waitForSelector('#sessList .sescard');
    await page.locator('#sessList .sescard').nth(0).click();
    await page.waitForSelector('#pchat:not([hidden]) .chatlist .bub');
    await page.waitForTimeout(160);
  },
  // 附件态：会话聊天页挂两个附件（图片带缩略图、文档走图标）。
  chatAtt: async (page) => {
    await page.click('#tabbar .tab[data-tab="1"]');
    await page.waitForSelector('#sessList .sescard');
    await page.locator('#sessList .sescard').nth(0).click();
    await page.waitForSelector('#pchat:not([hidden]) .chatlist .bub');
    await page.waitForFunction(() => chatMeta && chatMeta.sessionTitle);
    await page.setInputFiles('#chatFile', [
      { name: '截图一.png', mimeType: 'image/png', buffer: Buffer.from('PNG') },
      { name: '会议纪要.md', mimeType: 'text/markdown', buffer: Buffer.from('# 纪要') },
    ]);
    await page.waitForFunction(() => ATTS[''] && ATTS[''].length === 2);
    await page.waitForTimeout(200);
  },
  // 电脑文件面板：列会话 inbox。
  chatFiles: async (page) => {
    await page.click('#tabbar .tab[data-tab="1"]');
    await page.waitForSelector('#sessList .sescard');
    await page.locator('#sessList .sescard').nth(0).click();
    await page.waitForSelector('#pchat:not([hidden]) .chatlist .bub');
    await page.click('#chatFiles');
    await page.waitForSelector('#chatFilePanel:not([hidden]) .frow');
    await page.waitForTimeout(200);
  }
};

async function runShots(names) {
  const { chromium } = loadPlaywright();
  fs.mkdirSync(SHOTS, { recursive: true });
  const browser = await chromium.launch({ executablePath: CHROME });
  for (const theme of ['light', 'dark']) {
    for (const name of names) {
      if (!SCENES[name]) { console.log('skip', name); continue; }
      const { ctx, page } = await openPage(browser, theme);
      await SCENES[name](page);
      const file = path.join(SHOTS, `${name}-${theme}.png`);
      await page.screenshot({ path: file });
      console.log('shot', path.basename(file));
      await ctx.close();
    }
  }
  await browser.close();
}

async function runFlow() {
  const { chromium } = loadPlaywright();
  let fails = 0;
  const ok = (cond, what, extra = '') => {
    console.log((cond ? 'PASS ' : 'FAIL ') + what + (extra ? ' | ' + extra : ''));
    if (!cond) fails++;
  };
  const browser = await chromium.launch({ executablePath: CHROME });
  const { ctx, page } = await openPage(browser, 'light');
  const posts = [];
  page.on('request', (r) => { if (r.url().endsWith('/m/action')) posts.push(JSON.parse(r.postData() || '{}')); });
  page.on('pageerror', (e) => console.log('[pageerror]', e.message));
  const cards = () => page.locator('#list .card').count();

  ok(await cards() === 4, '初始 4 条待办', String(await cards()));
  await page.evaluate(() => { window.__stableCard = document.querySelector('#list .card'); });
  const multiHeight = await page.locator('#multiSw').evaluate((el) => el.getBoundingClientRect().height);
  ok(multiHeight >= 44, '多选按钮触控高度 ≥44px', String(multiHeight));

  await page.click('#multiSw');
  await page.waitForTimeout(150);
  ok(await page.isVisible('#batchBar'), '多选条出现');
  ok(await page.locator('#list .card.multi').count() === 4, '所有卡片进入多选态');
  ok(await page.evaluate(() => window.__stableCard === document.querySelector('#list .card')), '进入多选不重建卡片');
  ok(await page.locator('#multiSw').evaluate((el) => el.closest('#workHint') !== null), '多选入口在提示行而非顶栏');
  await page.locator('#list .card').nth(0).click();
  await page.waitForTimeout(120);
  ok((await page.textContent('#batchCount')).includes('1'), '点一条后计数=1', await page.textContent('#batchCount'));
  ok(await page.locator('#list .card.sel').count() === 1, '选中态只有一张');
  ok(await page.evaluate(() => window.__stableCard === document.querySelector('#list .card')), '勾选不重建卡片');
  ok(await page.locator('#list .card').nth(0).getAttribute('aria-checked') === 'true', '选中态同步 aria-checked');
  await page.waitForTimeout(2700);
  ok(await page.evaluate(() => window.__stableCard === document.querySelector('#list .card')), '相同轮询数据不重排列表');
  await page.click('#batchAll');
  await page.waitForTimeout(120);
  ok((await page.textContent('#batchCount')).includes('4'), '全选后计数=4', await page.textContent('#batchCount'));
  ok((await page.textContent('#batchAll')).includes('全不选'), '全选后按钮变「全不选」');
  await page.click('#batchCancel');
  await page.waitForTimeout(150);
  ok(!(await page.isVisible('#batchBar')), '取消后多选条收起');
  ok(await page.locator('#list .card.multi').count() === 0, '取消后卡片退出多选态');

  await page.evaluate(() => window.__press(document.querySelectorAll('#list .card')[1], 560));
  await page.waitForTimeout(200);
  ok(await page.isVisible('#batchBar'), '长按 560ms 进多选');
  ok(await page.locator('#list .card.sel').count() === 1, '长按那条被勾上');
  ok(!(await page.isVisible('#pchat')), '长按不误进聊天页');
  await page.click('#multiSw');
  await page.waitForTimeout(150);

  posts.length = 0;
  await page.evaluate(() => window.__swipe(document.querySelectorAll('#list .card')[0], -120, 6, false));
  await page.waitForTimeout(150);
  ok(await page.locator('#list .card.gone').count() >= 1 || await cards() === 3, '左滑后卡片开始收走');
  await page.waitForTimeout(700);
  ok(await cards() === 3, '左滑后列表少一条', String(await cards()));
  ok(posts.some((p) => p.cmd === 'ack'), '发出 ack 请求', JSON.stringify(posts));
  ok(await page.isVisible('#toast'), '销账后弹出提示');
  ok((await page.textContent('#toast')).includes('已标记'), '提示文案是「已标记」', await page.textContent('#toast'));

  posts.length = 0;
  await page.evaluate(() => window.__swipe(document.querySelectorAll('#list .card')[0], 110, 6, false));
  await page.waitForTimeout(400);
  ok(posts.some((p) => p.cmd === 'open'), '右滑发出 open 请求', JSON.stringify(posts));
  ok((await page.textContent('#toast')).includes('正在打开'), '右滑提示「正在打开」', await page.textContent('#toast'));
  ok(await cards() === 3, '右滑不删卡片', String(await cards()));
  ok(!(await page.isVisible('#pchat')), '右滑不附带误进聊天页');

  posts.length = 0;
  await page.evaluate(() => window.__swipe(document.querySelectorAll('#list .card')[0], -40, 4, false));
  await page.waitForTimeout(300);
  ok(posts.length === 0, '40px 小滑动不触发动作', JSON.stringify(posts));
  ok(await page.locator('#list .card').nth(0).evaluate((el) => Math.abs(new DOMMatrixReadOnly(getComputedStyle(el.querySelector('.face')).transform).m41) < 1), '小滑动后回弹归零');

  await page.evaluate(() => window.__swipe(document.querySelectorAll('#list .card')[0], 28, 64, false));
  await page.waitForTimeout(280);
  ok(posts.length === 0, '纵向手势不触发左右滑动作', JSON.stringify(posts));

  await page.evaluate(() => window.__swipe(document.querySelectorAll('#list .card')[0], 82, 3, true));
  ok((await page.textContent('#list .card .hint.l .label')).includes('松手'), '过阈值显示松手动作');
  await page.evaluate(() => window.__touch(document.querySelectorAll('#list .card')[0], 'touchcancel', 0, 0));
  await page.waitForTimeout(300);
  ok(posts.length === 0, 'touchcancel 不提交动作', JSON.stringify(posts));
  ok(await page.locator('#list .card').nth(0).evaluate((el) => Math.abs(new DOMMatrixReadOnly(getComputedStyle(el.querySelector('.face')).transform).m41) < 1), 'touchcancel 后回弹归零');

  await page.locator('#list .card').nth(0).click();
  await page.waitForTimeout(400);
  ok(await page.isVisible('#pchat'), '点卡片进入聊天页');
  ok(!(await page.isVisible('#tabbar')), '聊天页隐藏底部页签');
  await page.click('#chatBack');

  await page.setViewportSize({ width: 360, height: 800 });
  await page.waitForTimeout(150);
  posts.length = 0;
  await page.click('#multiSw');
  await page.locator('#list .card').nth(0).click();
  await page.locator('#list .card').nth(1).click();
  ok(await page.evaluate(() => {
    const bar = document.querySelector('#batchBar');
    const r = bar.getBoundingClientRect();
    const kids = [...bar.children].map((el) => el.getBoundingClientRect());
    return bar.scrollWidth <= bar.clientWidth + 1 && kids.every((k) => k.left >= r.left && k.right <= r.right + 1);
  }), '360px 小屏批量操作栏不溢出');
  await page.click('#batchAck');
  await page.waitForTimeout(120);
  ok(await page.locator('#list .card.gone').count() === 2, '批量销账两张卡片错峰收走');
  await page.waitForTimeout(1000);
  ok(await cards() === 1, '批量销账后只剩一条', String(await cards()));
  ok(posts.filter((p) => p.cmd === 'ack').length === 2, '批量销账只发两次 ack', JSON.stringify(posts));
  ok(!(await page.isVisible('#batchBar')), '批量完成后退出多选');

  ok((await page.textContent('#tabbar .tab[data-tab="1"]')).includes('派发'), '中间页签已改为派发');
  ok(!(await page.textContent('#tabbar .tab[data-tab="1"]')).includes('事件流'), '事件流不再出现在手机页');
  // 第三个页签是应用页（2026-11-23 取代「状态」）：图标瀑布流 + 点卡启动 + 两个开关。
  ok((await page.textContent('#tabbar .tab[data-tab="2"]')).includes('应用'),
    '第三个页签是应用', await page.textContent('#tabbar .tab[data-tab="2"]'));
  ok(!(await page.textContent('#tabbar .tab[data-tab="2"]')).includes('状态'),
    '第三个页签不再叫状态');
  posts.length = 0;
  await page.click('#tabbar .tab[data-tab="2"]');
  await page.waitForSelector('#papps:not([hidden]) .appcard');
  ok(await page.isVisible('#papps'), '点页签切到应用页');
  ok(!(await page.isVisible('#pwork')) && !(await page.isVisible('#pdispatch')),
    '应用页时另两个页面都隐藏');
  const appCards = page.locator('#papps .appcard');
  ok(await appCards.count() === 6, '应用卡片按目录条数渲染', String(await appCards.count()));
  ok(await page.locator('#papps .appcard img').count() === 6, '每张卡片都有图标');
  ok(await page.locator('#papps .appcard .aico img').count() === 6, '图标在 .aico 里');
  // 图标下方带应用名（只留图标会认不出谁是谁）；状态文案仍只在电脑端。
  ok(await page.locator('#papps .appcard .aname').count() === 6, '每张卡片都有应用名');
  ok((await appCards.nth(0).locator('.aname').textContent()) === '浏览器',
    '卡片名就是应用名', await appCards.nth(0).locator('.aname').textContent());
  ok((await appCards.nth(2).locator('.aname').textContent()) === '剪映',
    '第三张是剪映', await appCards.nth(2).locator('.aname').textContent());
  ok(await page.locator('#papps .appcard .astate').count() === 0, '卡片上没有状态文案');
  ok(await page.locator('#papps .appcard.on').count() === 5, '只有装了的是可点态');
  // 没有标题/说明/底部汇总与开关。
  ok(await page.locator('#papps .apphead').count() === 0, '应用页没有标题/说明');
  ok(await page.locator('#papps #appsSummary').count() === 0, '应用页没有底部汇总');
  ok(await page.locator('#appsVoice').count() === 0, '应用页没有语音启动开关');
  ok(await page.locator('#appsSystem').count() === 0, '应用页没有语音系统控制开关');
  // 瀑布流：CSS columns 生效，卡片按列排而不是单列。
  const gridCols = await page.locator('#appsGrid').evaluate((el) => getComputedStyle(el).columnCount);
  ok(gridCols !== 'auto' && Number(gridCols) >= 2, '应用页是瀑布流多列', gridCols);
  // 点卡启动 → appOpen（只发动作，不跳页）。
  await appCards.nth(0).click();
  await page.waitForTimeout(260);
  const appOpenPost = posts.filter((p) => p.cmd === 'appOpen');
  ok(appOpenPost.length === 1, '点卡片发 appOpen', JSON.stringify(posts));
  ok(appOpenPost[0] && appOpenPost[0].id === 'browser', 'appOpen 带应用 id', JSON.stringify(appOpenPost));
  ok((await page.textContent('#toast')).includes('浏览器'), '点卡后有提示', await page.textContent('#toast'));
  // 没装的那张点了不发请求。
  posts.length = 0;
  await page.locator('#papps .appcard').nth(4).click({ force: true });
  await page.waitForTimeout(220);
  ok(posts.filter((p) => p.cmd === 'appOpen').length === 0, '没装的应用点了不发启动');
  await page.click('#tabbar .tab[data-tab="1"]');
  await page.waitForSelector('#sessList .sescard');
  ok(await page.locator('#agentChips .chip').count() === 2, '会话目录按 Agent 列出');
  ok(await page.locator('#agentChips .chip').nth(0).evaluate((el) => el.classList.contains('on')), '默认选中最近 Agent');
  ok(await page.locator('#sessList .sescard').count() === 6, '展示所选 Agent 的完整项目会话');
  ok(await page.locator('#sessList .projhead').count() === 6, '项目、未分组、已归档分别成组');
  const heads = page.locator('#sessList .projhead');
  const labels = await page.locator('#sessList .projlabel').allTextContents();
  ok(labels.includes('tuwen-editor · githubrepos'), '同名活跃项目显示上级目录');
  ok(labels.includes('tuwen-editor · Desktop'), '同名归档项目显示上级目录');
  ok(labels.filter((s) => s.includes('tuwen-editor')).length === 2, '同名项目不合并');
  ok((await heads.nth(4).textContent()).includes('未分类'), '未分类任务单独成组');
  ok(await page.locator('#sessList .arcsec').count() === 1, '已归档单独成区');
  ok((await page.locator('#sessList .arcsec .arcname').textContent()) === '已归档', '区头写「已归档」');
  ok((await page.locator('#sessList .arcsec .arccount').textContent()).includes('1 个会话'), '区头报会话数=1');
  ok(await page.locator('#sessList .arcsec').evaluate((el) => el.classList.contains('closed')), '已归档区默认收起');
  ok(await page.locator('#sessList').evaluate((el) => el.lastElementChild.classList.contains('arcsec')), '归档区挂在列表最底部');
  ok(await page.locator('#sessList .projgroup.archived .projarch').count() === 0, '归档区内组头不重复「已归档」徽标');
  ok(await page.locator('#sessList .projgroup.archived .sescard.readonly').count() === 1, '归档项目默认只读');
  ok((await heads.last().textContent()).includes('1 个'), '分组标题显示会话数');
  ok(await page.locator('#sessList .projbody:not([hidden])').evaluateAll((els) => els.filter((el) => el.getBoundingClientRect().height > 0).length) === 1, '默认只展开最近一个项目');
  await page.click('#sessList .archead');
  await page.waitForTimeout(120);
  ok(!(await page.locator('#sessList .arcsec').evaluate((el) => el.classList.contains('closed'))), '点区头能展开归档区');
  ok(!(await page.locator('#sessList .arcsec .projbody').first().evaluate((el) => el.hidden)), '归档区里第一个项目默认展开');
  ok(await page.locator('#sessList .arcsec .sescard').first().isVisible(), '展开后归档会话可见');
  await page.click('#sessList .archead');
  await page.waitForTimeout(120);
  ok(await page.locator('#sessList .arcsec').evaluate((el) => el.classList.contains('closed')), '再点一次收起归档区');
  ok((await page.textContent('#sessList .sescard .sestitle')).includes('多选与滑动'), '会话标题可见');
  ok((await page.textContent('#sessList .sescard .sestop')).includes('等你处理'), '会话目录带活卡状态');

  const firstGroup = page.locator('#sessList .projgroup').first();
  await firstGroup.locator('.projhead').click();
  ok(await firstGroup.locator('.projhead').getAttribute('aria-expanded') === 'false', '项目组可收起');
  ok(await firstGroup.locator('.projbody').evaluate((el) => el.hidden), '收起后组内容隐藏');
  ok(await page.locator('#sessList .projbody:not([hidden]) .sescard').evaluateAll((els) => els.filter((el) => el.getBoundingClientRect().height > 0).length) === 0, '收起只隐藏本组会话');

  await page.locator('#agentChips .chip').nth(1).click();
  await page.waitForTimeout(120);
  await page.locator('#agentChips .chip').nth(0).click();
  await page.waitForTimeout(120);
  ok(await page.locator('#sessList .projgroup').first().evaluate((el) => el.classList.contains('closed')), '切换 Agent 后折叠状态保持');
  await page.locator('#sessList .projgroup').first().locator('.projhead').click();
  ok(await page.locator('#sessList .projgroup').first().locator('.projhead').getAttribute('aria-expanded') === 'true', '项目组可重新展开');

  await page.locator('#sessList .sescard').nth(0).click();
  await page.waitForSelector('#pchat:not([hidden]) .chatlist .bub');
  // 聊天页 DOM 是复用的：切会话时 .bub 可能还是上一屏的，等**数据到齐**
  // （chatMeta 由 /m/session 的响应写入）再断言标题/条数，否则会在
  // fetch 往返的空窗里读到初始 {}（2026-11-18 踩到）。
  await page.waitForFunction(() => chatMeta && chatMeta.sessionTitle);
  // 返回键是纯图标（文字退到 aria-label），别断言 textContent。
  ok((await page.getAttribute('#chatBack', 'aria-label')).includes('派发'),
    '会话聊天页返回派发');
  ok((await page.textContent('#chatTitle')).includes('手机页多选'), '会话聊天页标题正确');
  ok((await page.textContent('#chatSub')).includes('WorkBuddy AI'), '会话聊天页显示 Agent');
  ok(await page.locator('#chatList .bub').count() === 6, '会话聊天记录已加载');
  // 系统信息行：工具调用耗时/后台任务，灰色一行，不算对话气泡
  ok(await page.locator('#chatList .bubsys:not(.bublive)').count() === 1, '系统信息行渲染出来');
  ok((await page.locator('#chatList .bubsys:not(.bublive)').first().textContent())
    .includes('执行命令 5.7s'), '系统信息行带耗时与命令摘要');
  // 底部实时行（2026-11-20 用户报「长时间不显示进度内容」）：长轮里 transcript
  // 没新行，靠这行说明正在干什么；文案变了原位刷新，不能一条条堆上去。
  ok(await page.locator('#chatList .bublive').count() === 1, '实时行渲染且只有一条');
  ok((await page.locator('#chatList .bublive .t').textContent()).includes('思考：'),
    '实时行显示活动内容（不只是时长）');
  LIVE_TEXT = '执行 exec_command · npm run build · 本轮已 13m50s';
  await page.waitForFunction(
    () => document.querySelectorAll('#chatList .bublive').length === 1 &&
          (document.querySelector('#chatList .bublive .t') || {}).textContent.includes('exec_command'),
    null, { timeout: 6000 });
  ok(await page.locator('#chatList .bublive').count() === 1, '实时行原位更新、不堆叠');

  posts.length = 0;
  await page.fill('#chatText', '拒绝这条派发');
  await page.click('#chatSend');
  await page.waitForTimeout(260);
  ok((await page.textContent('#chatNotice')).includes('没发出去'), '派发拒绝原因可见');
  ok((await page.inputValue('#chatText')) === '拒绝这条派发', '派发失败保留草稿');

  await page.fill('#chatText', '把这个会话的说明改短一点');
  await page.click('#chatSend');
  await page.waitForTimeout(320);
  ok(posts.some((p) => p.cmd === 'dispatchSession' && p.session === 'sess-8f31c2a9' && p.agent === 'WorkBuddyAI.exe'), '任意会话派发参数正确', JSON.stringify(posts));
  ok((await page.textContent('#chatNotice')).includes('已派发'), '派发成功提示可见');
  ok((await page.inputValue('#chatText')) === '', '派发成功后清空草稿');

  // 附件：多选上传 → chip 条 → 只有附件也能发 → 路径拼进正文 → 送达后清空。
  // 上传走的是「无待办 id、只带 session/project」那条路（派发页直进会话），
  // 正是 2026-11-18 修掉必 404 的分支。
  UPLOADS.length = 0;
  ok(await page.locator('#chatAtts').evaluate((el) => el.hidden), '无附件时附件条隐藏');
  await page.setInputFiles('#chatFile', [
    // 真 PNG 字节（1x1）：坏图会走 onerror 退回图标，缩略图断言就测不到了。
    { name: '截图一.png', mimeType: 'image/png',
      buffer: Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8AAAwAB/AF+H+YAAAAASUVORK5CYII=', 'base64') },
    { name: 'notes.txt', mimeType: 'text/plain', buffer: Buffer.from('hello') },
  ]);
  await page.waitForFunction(() => ATTS[''] && ATTS[''].length === 2);
  ok(await page.locator('#chatAtts .catt').count() === 2, '多选附件都进 chip 条');
  ok(await page.locator('#chatAtts img').count() === 1, '图片附件带缩略图');
  ok(UPLOADS.length === 2 && UPLOADS[0].session === 'sess-8f31c2a9' && UPLOADS[0].id === '',
    '上传带会话上下文（无待办 id 也能定位）', JSON.stringify(UPLOADS));
  ok((await page.textContent('#chatAtts')).includes('截图一.png'), 'chip 显示文件名');
  ok(await page.isEnabled('#chatSend'), '只有附件也能点亮发送键');

  posts.length = 0;
  await page.click('#chatSend');
  await page.waitForTimeout(360);
  const sentAtt = posts.filter((p) => p.cmd === 'dispatchSession').pop();
  ok(sentAtt && sentAtt.text.includes('请查看文件：') && sentAtt.text.includes('notes.txt'),
    '发送时把附件路径拼进正文', JSON.stringify(sentAtt));
  // 只给路径会让 agent 不读就编（2026-11-18 的根因），正文必须带强制指令。
  ok(sentAtt && sentAtt.text.includes('请先用 Read 工具') &&
     sentAtt.text.includes('不要凭文件名或路径猜测内容'),
    '正文带「先读再答、不许猜」的指令', sentAtt && sentAtt.text.slice(0, 80));
  ok((await page.inputValue('#chatText')) === '', '附件路径不落回输入框');
  ok(await page.locator('#chatAtts .catt').count() === 0, '派发成功后附件条清空');

  await page.setInputFiles('#chatFile', [
    { name: 'x.md', mimeType: 'text/markdown', buffer: Buffer.from('# x') }]);
  await page.waitForFunction(() => ATTS[''] && ATTS[''].length === 1);
  await page.locator('#chatAtts .catt .x').click();
  ok(await page.locator('#chatAtts .catt').count() === 0, '× 可移除附件');
  ok(await page.locator('#chatAtts').evaluate((el) => el.hidden), '移除后附件条收起');

  // 手机传的附件：正文里的「请查看文件：路径」要折叠成缩略图/文件条。
  ok((await page.locator('.bubatt').count()) >= 1, '附件行折叠成缩略图/文件条');
  const uTxt = await page.locator('.bub.user').first().textContent();
  ok(!uTxt.includes('请查看文件'), '正文里不再露出「请查看文件：路径」', uTxt);
  // 图片是「看」不是「下」（用户 2026-11-22）：点一下弹全屏查看器，
  // 不能像改之前那样直接落盘。想存去浮层里点「保存到手机」。
  {
    const imgAtt = page.locator('.bubatt.img').first();
    ok(await imgAtt.count() >= 1, '对话里的图片渲染成缩略图');
    ok(await imgAtt.evaluate((el) => !el.hasAttribute('download')),
      '图片气泡不带 download 属性（长按/中键也不会直接落盘）');
    let imgDownload = false;
    page.once('download', () => { imgDownload = true; });
    await imgAtt.click();
    await page.waitForTimeout(700);
    ok(!imgDownload, '点图片不会触发下载');
    ok(await page.locator('#imgView').evaluate((el) => !el.hidden),
      '点图片弹出查看浮层');
    ok((await page.locator('#ivStage img').count()) === 1, '浮层里有大图');
    const ivName = await page.textContent('#ivName');
    ok(ivName.includes('shot.jpg'), '浮层标题是文件名', ivName);
    // 浮层里的保存才真下载（下载契约不变）。
    const [ivDl] = await Promise.all([
      page.waitForEvent('download', { timeout: 8000 }),
      page.click('#ivSave'),
    ]);
    ok(ivDl.suggestedFilename() === '20261118-204924-965-shot.jpg',
      '浮层里的保存走同一下载契约（服务端真实文件名）',
      ivDl.suggestedFilename());
    await page.click('#ivClose');
    ok(await page.locator('#imgView').evaluate((el) => el.hidden), '关闭后浮层隐藏');
  }

  // Agent 产物：回复里提到的文件要在气泡下方生效，链接指向工作区下载。
  ok((await page.locator('.bubfile').count()) >= 2, 'Agent 产物的文件条渲染出来');
  const bf = await page.locator('.bubfile').first().getAttribute('href');
  ok(bf.includes('scope=project') && bf.includes('rel='),
    '文件条指向工作区下载（scope=project）', bf);
  const bfTxt = await page.locator('.bubfile').first().textContent();
  ok(bfTxt.includes('下载到手机'), '文件条带「下载到手机」', bfTxt);

  // present_files 的产物：正文里没有路径，也要有卡片（靠 messages[].files）。
  // 注意定位到「含 .hwt 的那条气泡」而不是 last()：后面还有一条 file:/// URI 的
  // 产物消息（sm5），last() 会指错气泡。
  const pfBub = page.locator('.bub.assistant', { hasText: '凝蓝_OS8.hwt' }).first();
  const pfCard = pfBub.locator('.bubfile', { hasText: '凝蓝_OS8.hwt' });
  ok(await pfCard.count() === 1, 'present_files 产物渲染成文件条');
  const pfHref = await pfCard.getAttribute('href');
  ok(pfHref && pfHref.includes('scope=project') && decodeURIComponent(pfHref).includes('凝蓝_OS8.hwt'),
    'present_files 文件条指向工作区下载', pfHref);
  // 正文里的中文相对路径：卡片名必须带扩展名，且不能跟 present_files 重复成两张。
  const pfNames = await pfBub.locator('.bubfile b').allTextContents();
  ok(pfNames.includes('凝蓝_OS8.hwt'), '文件条名字带扩展名（不是「凝蓝_OS8」）', pfNames);
  ok(pfNames.filter((n) => n.startsWith('凝蓝_OS8')).length === 1, '同一产物只出一张卡片', pfNames);
  ok(await pfBub.locator('.bubfile').count() === 2, '气泡里正好两张产物卡片（hwt + 预览图）',
    pfNames);

  // Qoder / WorkBuddy 的产物链接是 file:/// URI（2026-11-25 用户报「Qoder 会话
  // 不显示文件下载到手机」的真凶）：必须同样长出「下载到手机」文件条，中文
  // percent 编码要解码成真实文件名，且项目外路径走 scope=abs。
  // 上面第三条 .json 不在下载白名单里（那是刻意的安全边界），所以只出两张卡。
  {
    const uriBub = page.locator('.bub.assistant', { hasText: '75 秒成片' }).first();
    const uriCards = uriBub.locator('.bubfile');
    ok(await uriCards.count() === 2, 'file:/// URI 的产物也渲染成文件条（非白名单扩展名不算）',
      await uriCards.allTextContents());
    const names = await uriBub.locator('.bubfile b').allTextContents();
    ok(names.some((n) => n === 'LiquidDesk-1.5.0-75s-4K60-r03.mp4'),
      'file:/// 产物卡片名带完整扩展名', names);
    ok(names.some((n) => n === '快休B站封面-1920x1080.jpg'),
      'percent 编码的中文文件名被解码', names);
    const hrefs = await uriCards.evaluateAll((els) => els.map((e) => e.getAttribute('href')));
    ok(hrefs.every((h) => h && h.includes('scope=abs') && h.includes('/m/file?')),
      'file:/// 产物走项目外绝对路径下载（scope=abs）', hrefs);
    const txt = await uriCards.first().textContent();
    ok(txt.includes('下载到手机'), 'file:/// 产物文件条带「下载到手机」', txt);
  }

  // 电脑文件面板：列出会话 inbox，行上带下载链接，× 能关。
  // 产物 = 工作区产物（rel）+ 正文提到的项目外产物（abs，2026-11-21 用户报
  // 「桌面的封面在面板里一条都看不到」）。合并按 mtime 排，上限 30。
  FILES.length = 0;
  await page.click('#chatFiles');
  await page.waitForSelector('#chatFilePanel:not([hidden]) .frow');
  ok(await page.locator('#chatFilePanel .frow').count() === 6, '面板列出产物（含 abs）+ inbox');
  const fc = await page.textContent('#chatFileCount');
  ok(fc.includes('产物 2') && fc.includes('文件 4'), '面板报文件数（产物 · 文件）', fc);
  // 产物行：第一行是 rel 路径的位置换成时间戳（用户 2026-11-19：上级目录没用）。
  const aHref = await page.locator('#chatFilePanel .frow').first().getAttribute('href');
  ok(aHref.includes('scope=project') && aHref.includes('rel='),
    '产物行指向工作区下载', aHref);
  const aDir = await page.locator('#chatFilePanel .frow').first().locator('.fdir').textContent();
  ok(aDir && !aDir.includes('/') && /\d/.test(aDir), '产物行第二行是时间戳不是路径', aDir);
  // abs 产物行：正文提到的桌面文件，必须出现在产物区、指向 scope=abs。
  const absRow = page.locator('#chatFilePanel .frow', { hasText: 'LiquidDesk-1.4.7-B站封面-4x3.jpg' });
  ok(await absRow.count() === 1, '正文提到的桌面产物出现在产物区');
  const absHref = await absRow.getAttribute('href');
  ok(absHref.includes('scope=abs') && absHref.includes('4x3.jpg'),
    'abs 产物行指向 abs 下载', absHref);
  const fhref = await page.locator('#chatFilePanel .frow').nth(2).getAttribute('href');
  ok(fhref.includes('/m/file?') && fhref.includes('name=') && fhref.includes('session=sess-8f31c2a9'),
    '文件行指向下载路由且带会话上下文', fhref);
  const fDir = await page.locator('#chatFilePanel .frow').nth(2).locator('.fdir').textContent();
  ok(fDir && /\d/.test(fDir), 'inbox 行也带时间戳', fDir);
  ok(FILES.length === 1 && FILES[0].req.includes('session=sess-8f31c2a9'),
    '列表请求带会话上下文', JSON.stringify(FILES));
  // 真下载 E2E（2026-11-20）：点一行必须真的拿到文件，而且文件名来自服务端
  // Content-Disposition——「下载下来是 file.txt」就是这么反复出现的。
  const [dl] = await Promise.all([
    page.waitForEvent('download', { timeout: 8000 }),
    page.locator('#chatFilePanel .frow').first().click(),
  ]);
  ok(dl.suggestedFilename() === 'AgentRMC-Setup.exe',
    '下载文件名来自服务端（不是 file.txt）',
    dl.suggestedFilename());
  const dlPath = await dl.path();
  ok(dlPath && fs.readFileSync(dlPath).toString() === 'DOWNLOAD', '下载字节完整');
  // 项目外绝对路径：链接必须走 /m/file（以前漏了 abs 分支，拼成 /m/files 列表
  // 路由 → 404 → 页面只能报「文件不在」，而文件其实就在桌面上）。
  const absUrl = await page.evaluate(() =>
    fpUrl(null, '', 'C:/Users/PC/Desktop/LiquidDesk-1.4.7-B站封面-4x3.jpg'));
  ok(absUrl.startsWith('/m/file?') && absUrl.includes('scope=abs'),
    'abs 链接走下载路由（不是列表路由）', absUrl);
  const [dlAbs] = await Promise.all([
    page.waitForEvent('download', { timeout: 8000 }),
    page.evaluate((u) => saveFile(u), absUrl),
  ]);
  ok(dlAbs.suggestedFilename() === 'LiquidDesk-1.4.7-B站封面-4x3.jpg',
    'abs 下载也走同一出口（真实文件名）', dlAbs.suggestedFilename());
  // 失效链接兜底（2026-11-21）：原路径 404 但同目录有同名文件时，服务端
  // resolve 出真文件继续下，页面提示一下，不再直接说「取不到」。
  const [dlResolve] = await Promise.all([
    page.waitForEvent('download', { timeout: 8000 }),
    page.evaluate(() => saveFile(fpUrl('renamed.png'))),
  ]);
  ok(dlResolve.suggestedFilename() === 'a.txt', '失效链接走 resolve 兜底也能下',
    dlResolve.suggestedFilename());
  const resolveReqs = FILES.filter((x) => x.resolve);
  ok(resolveReqs.length >= 2 &&
     resolveReqs.every((x) => x.resolve.includes('name=renamed.png')),
    'resolve 请求带原文件名', JSON.stringify(resolveReqs));
  // 彻底没有的文件：两级都 404 → 页面提示，不许再触发一次空下载。
  let ghostDownload = false;
  page.once('download', () => { ghostDownload = true; });
  await page.locator('#chatFilePanel .frow', { hasText: 'missing.txt' }).click();
  await page.waitForTimeout(600);
  ok(!ghostDownload, '文件不存在时不触发下载（不会落成 file.txt）');
  ok((await page.textContent('#toast')).includes('取不到这个文件'),
    '文件不存在时页面说清楚', await page.textContent('#toast'));
  await page.click('#chatFileClose');
  ok(await page.locator('#chatFilePanel').evaluate((el) => el.hidden), '× 能关掉文件面板');

  // 展形箭头居中（用户 2026-11-19 报「⌄ 太靠下」）：量 caret 与模型名的中心偏差。
  const caretGeo = await page.evaluate(() => {
    const chip = document.querySelector('#chatModelChip');
    if (!chip || chip.hidden) return null;
    const svg = chip.querySelector('.caret svg');
    const k = svg && svg.getBoundingClientRect();
    const label = chip.querySelector('b').getBoundingClientRect();
    return { hasSvg: !!svg, text: chip.querySelector('.caret').textContent.trim(),
      caretMid: k ? k.top + k.height / 2 : null, labelMid: label.top + label.height / 2 };
  });
  ok(caretGeo && caretGeo.hasSvg && caretGeo.text === '',
    '手机页 caret 是 SVG、没留「⌄」字符', JSON.stringify(caretGeo));
  if (caretGeo && caretGeo.caretMid !== null) {
    ok(Math.abs(caretGeo.caretMid - caretGeo.labelMid) <= 2,
      '手机页箭头与模型名垂直居中', { c: caretGeo.caretMid, l: caretGeo.labelMid });
  }

  // —— 换模型按宿主分流（2026-11-21 DSH 通道）：手机页与桌面同一套口径 ——
  // DSH 读 dshModels、档位来自所选模型的 details（不套固定五档）、选中后多一颗
  // 「设为这条会话的模型」提交键；ZCode / OpenSquilla 的目录与行为不许被串。
  const mpDsh = await page.evaluate(() => {
    const savedState = STATE, savedExe = chatExe;
    STATE = { ...STATE, dispatch: { ...(STATE.dispatch || {}), models: [
        { providerId: 'prov-1', name: 'TestOne', models: ['m1'] }],
      osqModels: [{ providerId: 'tokenrhythm', name: 'tokenrhythm', models: ['glm-5.1'] }],
      dshModels: [{ providerId: 'tr', name: '基元律动', models: ['glm-5.3-flash'],
        details: { 'glm-5.3-flash': { name: 'GLM 5.3 Flash',
          efforts: [{ id: 'off', name: 'off' }, { id: 'high', name: 'high' }],
          defaultEffort: 'high' } } }] } };
    const out = {};
    chatExe = 'DeepSeek Harness.exe';
    out.switchable = mpSwitchable();
    out.list = mpListFor().map((p) => p.providerId);
    out.sessionLevel = mpSessionLevel();
    mpResetState(); mpOpen = true; mpLevel = 'providers';
    mpRenderPanel();
    const footDsh = document.querySelector('#chatMpFoot').textContent;
    out.foot = footDsh;
    mpProvider = mpListFor()[0]; mpLevel = 'models'; mpRenderPanel();
    const rows = document.querySelectorAll('#chatMpList .mprow');
    out.rowText = rows.length ? rows[rows.length - 1].querySelector('.mpname').textContent : '';
    out.commitBeforePick = !document.querySelector('#chatMpCommit').hidden;
    if (rows.length) rows[rows.length - 1].onclick();
    out.thoughts = mpThoughts();
    out.levelDefault = mpSel && mpSel.level;
    out.commitAfterPick = !document.querySelector('#chatMpCommit').hidden;
    // 换回 ZCode：同一面板不许出提交键，目录也不许串成 DSH 的。
    chatExe = 'ZCode.exe';
    out.zcodeList = mpListFor().map((p) => p.providerId);
    out.zcodeSessionLevel = mpSessionLevel();
    mpResetState(); mpOpen = true; mpLevel = 'models';
    mpProvider = { providerId: 'prov-1', models: ['m1'] };
    mpSel = { providerId: 'prov-1', modelId: 'm1', level: null };
    mpRenderPanel();
    out.zcodeCommit = !document.querySelector('#chatMpCommit').hidden;
    // 还原，别污染后面的用例。
    mpResetState(); mpOpen = false; document.querySelector('#chatModelPanel').hidden = true;
    chatExe = savedExe; STATE = savedState;
    return out;
  });
  ok(mpDsh.switchable && mpDsh.list.join() === 'tr',
    '手机页 DSH 读 dshModels（不与 ZCode / OpenSquilla 串目录）', mpDsh);
  ok(mpDsh.sessionLevel === true, '手机页 DSH 是会话级切换', mpDsh.sessionLevel);
  ok(mpDsh.thoughts.join() === 'off,high',
    '手机页 DSH 档位来自所选模型的 details', mpDsh.thoughts);
  ok(mpDsh.levelDefault === 'high', '手机页选中模型后默认档位取 defaultEffort', mpDsh.levelDefault);
  ok(mpDsh.rowText === 'GLM 5.3 Flash', '手机页模型行显示 details.name', mpDsh.rowText);
  ok(!mpDsh.commitBeforePick && mpDsh.commitAfterPick,
    '手机页提交键只在选中模型后出现', mpDsh);
  ok(mpDsh.zcodeList.join() === 'prov-1' && mpDsh.zcodeSessionLevel === false
    && mpDsh.zcodeCommit === false,
    '手机页 ZCode 不串 DSH 目录、也不出提交键', mpDsh);

  // 系统返回键 / 侧边手势在网页里就是 history.back()：必须回到上一级（列表），
  // 而不是把整个页面退掉回浏览器主页（2026-11-19 用户反馈）。
  ok(await page.evaluate(() => history.state && history.state.v === 'chat'),
    '进入会话时压了视图栈');
  await page.evaluate(() => history.back());
  await page.waitForTimeout(400);
  ok(await page.locator('#pchat').evaluate((el) => el.hidden),
    '系统返回回到上一级（列表）');

  await page.locator('#agentChips .chip').nth(1).click();
  await page.waitForTimeout(120);
  ok(await page.locator('#sessList .sescard.readonly').count() === 1, '不支持派发的 Agent 标为只读');
  await page.locator('#sessList .sescard').nth(0).click();
  await page.waitForSelector('#pchat:not([hidden])');
  ok(await page.isDisabled('#chatText'), '只读会话禁用派发输入');
  ok((await page.textContent('#chatNotice')).includes('只读'), '只读会话给出说明');
  await page.click('#chatBack');

  // 美术主题（card.bg 半透明）下面板必须实心，同桌面那条断言。
  const pcheck = await page.evaluate(() => {
    applyTheme({ 'card.bg': 'rgba(8,12,20,55)', 'card.bg2': 'rgba(10,15,24,48)', text: '#171e2a' });
    return getComputedStyle(document.documentElement).getPropertyValue('--panel').trim();
  });
  ok(/^rgb\(\s*\d+,\s*\d+,\s*\d+\s*\)$/.test(pcheck), '半透明主题下 --panel 合成成实色', pcheck);

  console.log(fails ? `\n${fails} 项失败` : '\n全部通过');
  await ctx.close();
  await browser.close();
  process.exit(fails ? 1 : 0);
}

const mode = process.argv[2] || 'serve';
if (mode === 'flow' || mode === 'shots') {
  await startServer();
  if (mode === 'flow') await runFlow();
  else {
    await runShots(process.argv.slice(3).length ? process.argv.slice(3) : Object.keys(SCENES));
    process.exit(0);   // 假服务还挂着监听，不显式退会一直等
  }
} else {
  await startServer();
}
