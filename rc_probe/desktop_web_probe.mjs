// 桌面外壳离线探针：从 WorkWebView.cpp 的 R"HTML( ... )HTML" 段抽页面，
// 在 Chromium 里跑「派发页 → 会话 → 聊天页」状态机。只验证前端，不发真实动作。
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { execSync } from 'node:child_process';

const ROOT = path.join(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..');
const CPP = path.join(ROOT, 'native', 'src', 'ui', 'WorkWebView.cpp');
const require = createRequire(import.meta.url);

function chromePath() {
  const root = path.join(process.env.LOCALAPPDATA || '', 'ms-playwright');
  let names = [];
  try { names = fs.readdirSync(root).filter((n) => n.startsWith('chromium-')).sort().reverse(); } catch { /* noop */ }
  for (const n of names) {
    const exe = path.join(root, n, 'chrome-win64', 'chrome.exe');
    if (fs.existsSync(exe)) return exe;
  }
  return '';
}

function loadPlaywright() {
  for (const p of ['playwright-core', path.join(execSync('npm root -g', { encoding: 'utf8' }).trim(), 'playwright-core'),
    path.join(ROOT, 'node_modules', 'playwright-core'), path.join(ROOT, 'rc_probe', 'node_modules', 'playwright-core')]) {
    try { return require(p); } catch { /* next */ }
  }
  throw new Error('playwright-core not found');
}

const MOCK_STATE = {
  theme: { bg: '#f3f5f8', 'card.bg': '#fff', 'card.bg2': '#eef1f5', text: '#171e2a',
    'text.soft': '#1c2331', 'text.dim': '#2a323f', muted: '#48525f', 'muted.2': '#5a6472',
    ghost: '#4a5361', faint: '#8f98a3', line: '#c9d0d9', accent: '#4f8cff',
    'accent.hover': '#6299ff', 'on.accent': '#fff', danger: '#e5484d', warn: '#d29922',
    ok: '#3fb950', pending: '#f0883e', info: '#58a6ff', w6: 'rgba(15,23,42,.047)',
    w8: 'rgba(15,23,42,.063)', w10: 'rgba(15,23,42,.078)', w14: 'rgba(15,23,42,.110)',
    w16: 'rgba(15,23,42,.125)', w18: 'rgba(15,23,42,.141)', w30: 'rgba(15,23,42,.235)',
    a26: 'rgba(79,140,255,.102)', a36: 'rgba(79,140,255,.141)', a64: 'rgba(79,140,255,.251)' },
  work: { items: [], attention_count: 0 }, workers: [], events: [],
  health: { bridge: true, workers: [] }, header: {}, left: {}, system: {},
  keymap: {}, apps: {}, guide: {}, dispatch: {}, chat: {}
};

const MOCK_CATALOG = {
  agents: [{
    exe: 'WorkBuddy.exe', name: 'WorkBuddy', alive: true, window: true,
    can_dispatch: true, count: 3,
    sessions: [
      { id: 'sess-live', title: '桌面派发页联调', project: 'D:\\githubrepos\\Agentrmc',
        updated_at: new Date(Date.now() - 120000).toISOString(), can_dispatch: true },
      { id: 'sess-tuwen-live', title: '图文编辑器排版联调', project: 'D:\\githubrepos\\tuwen-editor',
        updated_at: new Date(Date.now() - 3600000).toISOString(), can_dispatch: true },
      { id: 'sess-tuwen-old', title: '旧图文编辑器记录', project: 'C:\\Users\\PC\\Desktop\\tuwen-editor',
        updated_at: new Date(Date.now() - 30 * 86400e3).toISOString(), archived: true,
        can_dispatch: false, can_delete: true },
      { id: 'sess-old', title: '旧项目归档记录', project: 'C:\\Users\\PC\\Desktop\\Windowsicon',
        updated_at: new Date(Date.now() - 20 * 86400e3).toISOString(), archived: true,
        can_dispatch: false, can_delete: true }
    ]
  }]
};

function html() {
  const src = fs.readFileSync(CPP, 'utf8');
  const chunks = [...src.matchAll(/R"HTML\(([\s\S]*?)\)HTML"/g)].map((m) => m[1]);
  let text = chunks.join('');
  text = text.replace(/let STATE = ;/, 'let STATE = ' + JSON.stringify(MOCK_STATE) + ';');
  // WebView2 的宿主对象在 Chromium 里不存在：在页面脚本前补一个桩，点击动作
  // 才能被探针读到；也避免把 stub 缺失误报成页面错误。
  text = text.replace('<script>', '<script>window.chrome={webview:{postMessage:function(o){window.__lastPost=o;}}};</script><script>');
  return text;
}

const { chromium } = loadPlaywright();
const browser = await chromium.launch({ executablePath: chromePath() });
const ctx = await browser.newContext({ viewport: { width: 1080, height: 760 } });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
await page.setContent(html(), { waitUntil: 'load' });
const tabs = await page.locator('#tabs .tab').allTextContents();
console.log('tabs=', tabs.join(' / '));
await page.locator('#tabs .tab').nth(1).click();
await page.evaluate((c) => window.__setSessions(c), MOCK_CATALOG);
await page.waitForSelector('#dspList .dspsess');
const visible = await page.locator('#dspList .dspsess').count();
const groups = await page.locator('#dspList .dspgroup').count();
const groupLabels = await page.locator('#dspList .dspgname').allTextContents();
const activeOpen = await page.locator('#dspList .dspgroup:not(.archived) .dspbody:not([hidden])').count();
// 归档区的收起在区级 .dspsec.closed（2026-11-17 改版后；旧探针盯的
// .dspgroup.archived 是区内的项目组，收起状态不在它身上）。归档区固定排最后。
const archivedClosed = await page.locator('#dspList .dspsec').last()
  .evaluate((el) => el.classList.contains('closed') && el.querySelector('.dspsecbody')?.hidden === true);
await page.locator('#dspList .dspsess').nth(0).click();
await page.waitForTimeout(200);
const dchatVisible = await page.locator('#dchat').evaluate((el) => !el.hidden);
const title = await page.textContent('#dchatTitle');
const openPost = await page.evaluate(() => window.__lastPost);
const now = Date.now();
await page.evaluate(({ now }) => window.__setSessionChat({
  sessionId: 'sess-live', offset: 0, total: 2, supported: true,
  sessionTitle: '桌面派发页联调',
  // 底部实时行（2026-11-20）：长轮里 transcript 不更新时显示「正在干什么」。
  live: { text: '思考：…把那条内容校验断言的写法改成直白写法 · 本轮已 13m41s',
          at: new Date(now - 5000).toISOString() },
  messages: [
    { id: 'dm1', role: 'user', at: new Date(now - 300000).toISOString(), text: '把桌面端的会话目录接上' },
    { id: 'dm2', role: 'assistant', at: new Date(now - 200000).toISOString(), text: '目录和聊天页已经接入，底部可以直接派发。' }
  ]
}), { now });
const liveRows = await page.locator('#dchatList .bublive').count();
const liveText = liveRows ? await page.textContent('#dchatList .bublive') : '';
await page.fill('#dchatText', '继续优化项目分类');
await page.click('#dchatSend');
await page.waitForTimeout(100);
const sendPost = await page.evaluate(() => window.__lastPost);
const sendDraft = await page.inputValue('#dchatText');
await page.evaluate(() => window.__setDispatchResult({ ok: false, error: '模拟拒绝' }));
const failedNotice = await page.textContent('#dchatNotice');
await page.evaluate(() => window.__setDispatchResult({ ok: true }));
const okNotice = await page.textContent('#dchatNotice');
await page.click('#dchatBack');
await page.waitForTimeout(100);
const backToDispatch = await page.locator('#dispatch').evaluate((el) => !el.hidden);

// 会话删除（对 Magpie：任意会话可删）：单条入口在所有行 + 批量模式 +
// 原生确认后的失败/成功结果。批量开关在页头（旧顶栏 #dspClean、后改区头
// .dspsecclean，2026-11-20 再升回页头 #dspBatchToggle）。
const singleDelete = await page.locator('#dspList .dspdel').count();
await page.click('#dspBatchToggle');
await page.waitForTimeout(120);
const cleanGroups = await page.locator('#dspList .dspgroup').count();
const cleanExpanded = await page.locator('#dspList .dspgroup.archived .dspbody').first().evaluate((el) => !el.hidden);
const selectable = await page.locator('#dspList .dspcheck').count();
await page.locator('#dspList .dspsess', { hasText: '旧图文编辑器记录' }).click();
await page.waitForTimeout(80);
const batchCount = await page.textContent('#dspBatchCount');
await page.click('#dspDelete');
await page.waitForTimeout(80);
const deletePost = await page.evaluate(() => window.__lastPost);
await page.evaluate(() => window.__setDeleteResult({ ok: false, deleted: 0, error: '模拟拒绝' }));
const deleteFail = await page.textContent('#dspNotice');
const batchStillOn = await page.locator('#dspBatch').evaluate((el) => !el.hidden);
await page.click('#dspDelete');
await page.waitForTimeout(80);
await page.evaluate(() => window.__setDeleteResult({ ok: true, deleted: 1, backup: 'C:\\trash\\demo' }));
await page.waitForTimeout(100);
const batchOff = await page.locator('#dspBatch').evaluate((el) => el.hidden);

// 回收站（Magpie 口径）：常驻入口 → 打开先拉列表 → 条目行 → 恢复命令接线。
const trashBtn = await page.locator('#dspTrash').isVisible();
await page.click('#dspTrash');
await page.waitForTimeout(80);
const trashLoadPost = await page.evaluate(() => window.__lastPost);
await page.evaluate(() => window.__setTrash({ ok: true, items: [
  { key: 'WorkBuddy.exe/20261120T000000Z-clean-sid-0001', exe: 'WorkBuddy.exe',
    session: 'clean-sid-0001', title: '回收站里的会话',
    deleted: '2026-11-20T00:00:00', size: 2048, damaged: false },
] }));
await page.waitForTimeout(100);
const trashRows = await page.locator('#dspList .dsptrashrow').count();
await page.locator('#dspList .dsptrashrow .dsptrashops button').nth(0).click();
await page.waitForTimeout(80);
const restorePost = await page.evaluate(() => window.__lastPost);

console.log('groups=', groups, 'sessions=', visible, 'labels=', groupLabels.join(' | '),
  'activeOpen=', activeOpen, 'archivedClosed=', archivedClosed);
console.log('cleanGroups=', cleanGroups, 'cleanExpanded=', cleanExpanded,
  'selectable=', selectable, 'singleDelete=', singleDelete, 'batch=', batchCount);
console.log('deletePost=', JSON.stringify(deletePost), 'deleteFail=', deleteFail,
  'batchStillOn=', batchStillOn, 'batchOffAfterSuccess=', batchOff);
console.log('trashBtn=', trashBtn, 'trashLoad=', JSON.stringify(trashLoadPost),
  'trashRows=', trashRows, 'restorePost=', JSON.stringify(restorePost));
console.log('tabOrder=', tabs.slice(0, 3).join(' / '));
console.log('dchatVisible=', dchatVisible, 'title=', title.split('\n')[0]);
console.log('liveRow=', liveRows, 'liveText=', liveText);
console.log('openPost=', JSON.stringify(openPost));
console.log('sendPost=', JSON.stringify(sendPost), 'draftBeforeResult=', sendDraft);
console.log('failedNotice=', failedNotice, 'okNotice=', okNotice, 'backToDispatch=', backToDispatch);
console.log('errors=', errors.length ? errors : 'none');
const ok = dchatVisible && backToDispatch && archivedClosed && activeOpen === 1
  && tabs[0] === '待我处理' && tabs[1] === '派发'
  && liveRows === 1 && liveText.includes('思考：')
  && groupLabels.includes('tuwen-editor · githubrepos')
  && groupLabels.includes('tuwen-editor · Desktop')
  && openPost?.cmd === 'sessionOpen' && sendPost?.cmd === 'dispatchSession'
  && failedNotice.includes('模拟拒绝') && okNotice.includes('已派发')
  && cleanGroups === 4 && cleanExpanded && selectable === 4 && singleDelete === 4
  && deletePost?.cmd === 'sessionDelete' && deletePost?.items?.length === 1
  && deleteFail.includes('模拟拒绝') && batchStillOn && batchOff
  && trashBtn && trashLoadPost?.cmd === 'trashList' && trashRows === 1
  && restorePost?.cmd === 'trashRestore'
  && String(restorePost?.key || '').includes('clean-sid-0001')
  && errors.length === 0;
await browser.close();
process.exit(ok ? 0 : 1);
