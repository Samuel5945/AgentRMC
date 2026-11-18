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
const ITEMS = () => ([
  {
    id: 'a1', title: 'WorkBuddy AI', stage: 'needs-me',
    project: 'D:\\githubrepos\\Agentrmc', session_id: 'sess-8f31c2a9',
    session_title: '手机页多选与滑动优化', done: false,
    message: 'WorkBuddy AI：等你确认是否把 0.5.0 插件覆盖安装到本机，覆盖会重启宿主。',
    updated_at: iso(NOW - 3 * 60e3), attention_since: iso(NOW - 22 * 60e3)
  },
  {
    id: 'a2', title: 'ZCode', stage: 'needs-me',
    project: 'D:\\githubrepos\\Agentrmc\\native', session_id: 'sess-2b91aa40',
    session_title: '打包链路排查', done: false,
    message: '要不要把 dist 里的 0.4.1 安装包归档？留着会占 58MB。',
    updated_at: iso(NOW - 9 * 60e3), attention_since: iso(NOW - 9 * 60e3)
  },
  {
    id: 'a3', title: 'Qoder CN', stage: 'review',
    project: 'D:\\githubrepos\\Agentrmc\\plugins', session_id: 'sess-77ce0193',
    session_title: '插件版本同步脚本', done: false,
    message: 'Qoder CN：sync-plugin-version.ps1 六处清单已对齐 0.5.0，等你验收。',
    updated_at: iso(NOW - 41 * 60e3), attention_since: iso(NOW - 41 * 60e3)
  },
  {
    id: 'a4', title: 'DeepSeek Harness', stage: 'working',
    project: 'D:\\githubrepos\\Agentrmc', session_id: 'sess-4a02be77',
    session_title: '转写队列', done: false,
    message: '正在重跑语音转写的长音频用例，预计 3 分钟。',
    updated_at: iso(NOW - 2 * 60e3)
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
  }
]);

function sessionTranscript(exe, session) {
  const agent = SESSIONS().find((a) => a.exe === exe);
  const row = agent && agent.sessions.find((s) => s.id === session);
  if (!row) return null;
  return {
    total: 3, supported: true, sessionTitle: row.title, agentExe: exe,
    agentName: agent.name, sessionId: session,
    messages: [
      { id: 'sm1', role: 'user', at: iso(NOW - 700e3), text: '这个会话现在做到哪一步了？' },
      { id: 'sm2', role: 'assistant', at: iso(NOW - 650e3),
        text: '卡片层级已经拆好了，现在在收滑动阈值和批量动效。产物：'
          + 'rc_probe/mobile_shots/chatAtt-light.png 和 dist/AgentRMC-Setup.exe' },
      { id: 'sm3', role: 'user', at: iso(NOW - 480e3), text: '继续，把交互再收一下。' }
    ],
    requests: []
  };
}

function state(theme) {
  return {
    theme: theme === 'dark' ? DARK : LIGHT,
    work: { items: ITEMS().map((i) => ({ ...i, done: i.done || ACK.has(i.id) })), attention_count: Math.max(0, 3 - ACK.size) },
    dispatch: { noticeId: 'a1', notice: '等你确认覆盖安装', noticeExe: 'WorkBuddyAI.exe', draftId: '', draftText: '' },
    health: { bridge: true, uptime_seconds: 5400, workers: [{ alive: true }, { alive: true }], spool_pending: 0, problems: [] },
    meta: { version: '0.5.0', devices: 2, listening: true }
  };
}

let CURRENT_THEME = 'light';
const ACK = new Set();
const UPLOADS = [];  // /m/upload 收到的定位参数 + 文件名/字节数，附件断言用
const FILES = [];    // /m/files、/m/file 收到的 query，电脑文件面板断言用

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
          ],
        }));
      }
      if (url.pathname === '/m/file') {
        FILES.push({ file: url.search });
        res.writeHead(200, {
          'Content-Type': 'application/octet-stream',
          'Content-Disposition': 'attachment; filename="a.txt"',
        });
        return res.end('DOWNLOAD');
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
  ok(await page.locator('#chatList .bub').count() === 3, '会话聊天记录已加载');

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
  ok((await page.inputValue('#chatText')) === '', '附件路径不落回输入框');
  ok(await page.locator('#chatAtts .catt').count() === 0, '派发成功后附件条清空');

  await page.setInputFiles('#chatFile', [
    { name: 'x.md', mimeType: 'text/markdown', buffer: Buffer.from('# x') }]);
  await page.waitForFunction(() => ATTS[''] && ATTS[''].length === 1);
  await page.locator('#chatAtts .catt .x').click();
  ok(await page.locator('#chatAtts .catt').count() === 0, '× 可移除附件');
  ok(await page.locator('#chatAtts').evaluate((el) => el.hidden), '移除后附件条收起');

  // Agent 产物：回复里提到的文件要在气泡下方生效，链接指向工作区下载。
  ok((await page.locator('.bubfile').count()) >= 2, 'Agent 产物的文件条渲染出来');
  const bf = await page.locator('.bubfile').first().getAttribute('href');
  ok(bf.includes('scope=project') && bf.includes('rel='),
    '文件条指向工作区下载（scope=project）', bf);
  const bfTxt = await page.locator('.bubfile').first().textContent();
  ok(bfTxt.includes('下载到手机'), '文件条带「下载到手机」', bfTxt);

  // 电脑文件面板：列出会话 inbox，行上带下载链接，× 能关。
  FILES.length = 0;
  await page.click('#chatFiles');
  await page.waitForSelector('#chatFilePanel:not([hidden]) .frow');
  ok(await page.locator('#chatFilePanel .frow').count() === 2, '电脑文件面板列出 inbox');
  ok((await page.textContent('#chatFileCount')) === '2', '面板报文件数');
  const fhref = await page.locator('#chatFilePanel .frow').first().getAttribute('href');
  ok(fhref.includes('/m/file?') && fhref.includes('name=') && fhref.includes('session=sess-8f31c2a9'),
    '文件行指向下载路由且带会话上下文', fhref);
  ok(FILES.length === 1 && FILES[0].req.includes('session=sess-8f31c2a9'),
    '列表请求带会话上下文', JSON.stringify(FILES));
  await page.click('#chatFileClose');
  ok(await page.locator('#chatFilePanel').evaluate((el) => el.hidden), '× 能关掉文件面板');

  await page.click('#chatBack');
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
