// 桌面派发页分组验收（离线）：直接从 WorkWebView.cpp 抽 HTML，用假目录数据跑
// 真实渲染，断言「活跃组只放未归档 / 已归档单独成组 / 未分类任务按归档拆两组」。
//
// 用法：node rc_probe/desktop_dispatch_groups.mjs
// 依赖 rc_probe/node_modules 里的 playwright-core（跟 mobile_page_probe.mjs 共用）。
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
const CPP = path.join(ROOT, 'native', 'src', 'ui', 'WorkWebView.cpp');
const SHOTS = path.join(HERE, 'desktop_shots');
const require = createRequire(import.meta.url);

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

function loadPlaywright() {
  const tries = ['playwright-core', path.join(HERE, 'node_modules', 'playwright-core')];
  try {
    tries.push(path.join(execSync('npm root -g', { encoding: 'utf8' }).trim(), 'playwright-core'));
  } catch { /* npm 不在 PATH 上就算了 */ }
  for (const p of tries) {
    try { return require(p); } catch { /* 换下一个 */ }
  }
  console.error('找不到 playwright-core：先跑 npm i -g playwright-core');
  process.exit(2);
}

// cpp 里的 HTML 由多段 R"HTML( ... )HTML" 拼起来，中间还插了 qrLib / stateJson。
// 这里按同样的规则拼回去：丢 qrLib，stateJson 用 {} 顶替。
function desktopHtml() {
  const src = fs.readFileSync(CPP, 'utf8');
  let out = '';
  const re = /R"HTML\(([\s\S]*?)\)HTML"/g;
  let m;
  let last = 0;
  while ((m = re.exec(src)) !== null) {
    if (last !== 0) {
      const gap = src.slice(last, m.index);
      out += gap.includes('jsEscape') ? '{}' : '';
    }
    out += m[1];
    last = re.lastIndex;
  }
  if (!out.includes('<script>')) throw new Error('WorkWebView.cpp 里找不到内联脚本');
  return out;
}

const iso = new Date(Date.now() - 3600 * 1000).toISOString();
const CATALOG = {
  agents: [{
    exe: 'WorkBuddy.exe', name: 'WorkBuddy', alive: true, can_dispatch: true,
    sessions: [
      // 运行中的项目：一条活跃、一条已归档（老路径/宿主归档）
      { id: 'a1', title: '优化待办多选', project: 'D:/githubrepos/Agentrmc', updated_at: iso, archived: false, generated: false },
      { id: 'a7', title: '运行项目里的老会话', project: 'D:/githubrepos/Agentrmc', updated_at: iso, archived: true, generated: false },
      // 已归档项目（路径已不存在）
      { id: 'a2', title: '老路径会话', project: 'C:/Users/PC/Desktop/tuwen-editor', updated_at: iso, archived: true, generated: false },
      // 单任务：日期目录（目录还在 → 未归档）
      { id: 'a3', title: '日期目录单任务', project: 'C:/Users/PC/WorkBuddy/2026-11-12-21-57-51', updated_at: iso, archived: false, generated: true },
      // 单任务：Qoder 临时目录里的真实工作（未归档）
      { id: 'a4', title: 'Qoder真实工作', project: 'C:/Users/PC/Documents/Qoder/2026-08-28/1b3e82f9/lig-site', updated_at: iso, archived: false, generated: true },
      // 单任务：完全没有项目
      { id: 'a5', title: '无项目语音测试', project: '', updated_at: iso, archived: false, generated: false },
      // 单任务：日期目录已经没了（已归档）
      { id: 'a6', title: '已归档单任务', project: 'C:/Users/PC/WorkBuddy/2026-01-01-00-00-00', updated_at: iso, archived: true, generated: true },
    ],
  }],
};

let bad = 0;
function check(cond, what, extra) {
  if (cond) {
    console.log('ok   ', what);
  } else {
    bad += 1;
    console.log('FAIL ', what, extra === undefined ? '' : JSON.stringify(extra));
  }
}

const { chromium } = loadPlaywright();
const browser = await chromium.launch({ executablePath: chromePath() || undefined });
const page = await browser.newPage({ viewport: { width: 1100, height: 760 } });
page.on('pageerror', (e) => { bad += 1; console.log('FAIL  pageerror:', e.message); });
await page.addInitScript((catalog) => {
  // WebView2 的宿主通道：页面 postObj → 这里 → 立刻回灌目录。
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage(msg) {
      if (msg && msg.cmd === 'sessionsLoad') {
        setTimeout(() => window.__setSessions && window.__setSessions(catalog), 0);
      }
    },
  };
}, CATALOG);
// 必须走真实导航（file://）：setContent 不会重跑 init script，宿主通道会缺。
fs.mkdirSync(SHOTS, { recursive: true });
const htmlPath = path.join(SHOTS, '_groups_probe.html');
fs.writeFileSync(htmlPath, desktopHtml(), 'utf8');
await page.goto('file:///' + htmlPath.replace(/\\/g, '/'), { waitUntil: 'load' });
await page.click('#tabs .tab:nth-child(2)');
await page.waitForSelector('#dspList .dspgroup', { timeout: 8000 });

const groups = await page.evaluate(() => [...document.querySelectorAll('#dspList .dspgroup')].map((g) => ({
  name: g.querySelector('.dspgname').textContent,
  archived: g.classList.contains('archived'),
  count: g.querySelector('.dspgcount').textContent,
  rows: [...g.querySelectorAll('.dspsess')].map((r) => ({
    title: r.querySelector('.title').textContent,
    del: !!r.querySelector('.dspdel'),
    arrow: !!r.querySelector('.arr'),
  })),
})));
const summary = await page.textContent('#dspSummary');

const byName = (n, arch) => groups.filter((g) => g.name === n && g.archived === arch);
check(groups.length === 5, '一共 5 个组', groups.map((g) => [g.name, g.archived]));
check(byName('Agentrmc', false).length === 1, 'Agentrmc 活跃组存在');
check(byName('Agentrmc', true).length === 1, 'Agentrmc 已归档组存在（老会话单独成组）');
check(byName('tuwen-editor', true).length === 1, 'tuwen-editor 已归档组存在');
check(byName('未分类任务', false).length === 1, '未分类任务（活跃）存在');
check(byName('未分类任务', true).length === 1, '未分类任务（已归档）存在');

const live = byName('Agentrmc', false)[0];
if (live) {
  check(live.rows.length === 1 && live.rows[0].title === '优化待办多选', '活跃组只有未归档会话', live.rows);
  check(live.rows.every((r) => !r.del), '活跃组里没有删除按钮', live.rows);
}
const dead = byName('Agentrmc', true)[0];
if (dead) {
  check(dead.rows.length === 1 && dead.rows[0].title === '运行项目里的老会话', '已归档组只放已归档会话', dead.rows);
  check(dead.rows.every((r) => r.del), '已归档组每行都有删除按钮', dead.rows);
}
const uncLive = byName('未分类任务', false)[0];
if (uncLive) {
  check(uncLive.rows.length === 3, '未分类任务（活跃）3 条单任务', uncLive.rows.map((r) => r.title));
  check(uncLive.rows.every((r) => !r.del && r.arrow), '未分类任务（活跃）里没有删除按钮', uncLive.rows);
  check(uncLive.rows.some((r) => r.title === '日期目录单任务'), '日期目录单任务按单任务处理');
  check(uncLive.rows.some((r) => r.title === 'Qoder真实工作'), 'Qoder 临时目录里的工作不再算归档');
  check(uncLive.rows.some((r) => r.title === '无项目语音测试'), '无项目会话进未分类任务');
}
const uncDead = byName('未分类任务', true)[0];
if (uncDead) {
  check(uncDead.rows.length === 1 && uncDead.rows[0].title === '已归档单任务', '已归档单任务单独一组', uncDead.rows);
  check(uncDead.rows.every((r) => r.del), '已归档单任务带删除按钮', uncDead.rows);
}
// 组内不允许混合：每组的删除按钮状态必须一致
for (const g of groups) {
  const dels = new Set(g.rows.map((r) => r.del));
  check(dels.size <= 1, `组「${g.name}」不混归档/未归档`, g.rows);
}
check(/1 个项目/.test(summary), '摘要项目数=1（只有 Agentrmc 有活跃会话）', summary);
check(/4 个未分类任务/.test(summary), '摘要未分类任务=4', summary);
check(/2 个已归档/.test(summary), '摘要已归档=2', summary);

fs.mkdirSync(SHOTS, { recursive: true });
await page.screenshot({ path: path.join(SHOTS, 'groups_probe.png') });
await browser.close();
console.log(bad ? `FAILED ${bad}` : 'ALL PASS desktop dispatch groups');
process.exit(bad ? 1 : 0);
