// 官网 agentrmc 页验收：本地起静态服务 → 打开页面 → 断言版本/轮播图/新文案 → 截图。
//
// 用法：
//   node rc_probe/site_page_probe.mjs [站点目录]
// 缺省站点目录是 D:\githubrepos\lig-site（发布 runbook 里的检出位置），
// 会自己起一个 4700 端口的静态服务，跑完关掉。
//
// 断言三件事（都是这次踩过的）：
//   ① 版本徽标来自 agentrmc-update.json —— 忘了改就还是上一版的号；
//   ② 轮播八张图都指向新文件 —— 图换了但 HTML 没换，页面还是旧截图；
//   ③ 功能文案是新版 —— 中文页不走 i18n apply，只改 i18n.js 不生效，
//      必须同时改 HTML 正文（这次就是先只改了 i18n，页面显示旧标题）。
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SITE = process.argv[2] || 'D:\\githubrepos\\lig-site';
const OUT = path.join(HERE, 'site_shots');
const PORT = 4700;
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
  const tries = ['playwright-core'];
  try { tries.push(path.join(execSync('npm root -g', { encoding: 'utf8' }).trim(), 'playwright-core')); } catch { /* 没在 PATH */ }
  tries.push(path.join(HERE, 'node_modules', 'playwright-core'));
  for (const p of tries) { try { return require(p); } catch { /* 换下一个 */ } }
  console.error('找不到 playwright-core，请 npm i -g playwright-core');
  process.exit(2);
}

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'application/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
  '.png': 'image/png', '.svg': 'image/svg+xml', '.json': 'application/json',
  '.woff2': 'font/woff2', '.mp4': 'video/mp4',
};

// 站点根优先 dist（publish.mjs 的产物），没有就退回源码目录。
function serveRoot() {
  const dist = path.join(SITE, 'dist');
  return fs.existsSync(path.join(dist, 'agentrmc.html')) ? dist : SITE;
}

function startServer(root) {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      let u = decodeURIComponent(req.url.split('?')[0]);
      if (u === '/') u = '/agentrmc.html';
      const fp = path.join(root, u);
      if (!fp.startsWith(root) || !fs.existsSync(fp) || fs.statSync(fp).isDirectory()) {
        res.writeHead(404); return res.end('not found');
      }
      res.writeHead(200, { 'Content-Type': MIME[path.extname(fp).toLowerCase()] || 'application/octet-stream' });
      fs.createReadStream(fp).pipe(res);
    });
    server.listen(PORT, '127.0.0.1', () => resolve(server));
  });
}

let fails = 0;
const ok = (cond, what, extra = '') => {
  console.log((cond ? 'PASS ' : 'FAIL ') + what + (extra ? ' | ' + extra : ''));
  if (!cond) fails++;
};

async function main() {
  if (!fs.existsSync(path.join(SITE, 'agentrmc.html'))) {
    console.error('站点目录里没有 agentrmc.html:', SITE);
    process.exit(2);
  }
  const root = serveRoot();
  const server = await startServer(root);
  console.log('serving', root, 'on', PORT);

  const { chromium } = loadPlaywright();
  fs.mkdirSync(OUT, { recursive: true });
  const browser = await chromium.launch({ executablePath: chromePath() });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1 });

  const errs = [];
  page.on('pageerror', (e) => errs.push(e.message));
  page.on('requestfailed', (r) => errs.push('request failed: ' + r.url()));

  // 带时间戳绕过缓存：本地反复跑同一份 dist 时，浏览器会拿旧图旧脚本。
  await page.goto(`http://127.0.0.1:${PORT}/agentrmc.html?t=${Date.now()}`,
    { waitUntil: 'networkidle' });
  await page.waitForTimeout(900);

  // ① 版本徽标（页面从 agentrmc-update.json 读）
  const expected = JSON.parse(fs.readFileSync(path.join(root, 'agentrmc-update.json'), 'utf8')).version;
  const badge = (await page.textContent('#agentrmc-latest-ver')) || '';
  ok(badge.trim() === expected, '版本徽标与 update.json 一致', `${badge.trim()} vs ${expected}`);

  // ② 轮播图
  const figs = await page.$$eval('.cover-slide img', (els) => els.map((e) => e.getAttribute('src')));
  ok(figs.length === 8, '轮播八张', String(figs.length));
  const missing = figs.filter((f) => !fs.existsSync(path.join(root, f)));
  ok(missing.length === 0, '轮播图文件都在', missing.join(', '));
  const sizes = figs.map((f) => {
    const st = fs.statSync(path.join(root, f));
    return { f, kb: Math.round(st.size / 1024), mtime: st.mtimeMs };
  });
  // 图片比 HTML 旧太多 → 大概率没重新导出
  const htmlTime = fs.statSync(path.join(root, 'agentrmc.html')).mtimeMs;
  const stale = sizes.filter((s) => htmlTime - s.mtime > 7 * 24 * 3600 * 1000);
  ok(stale.length === 0, '轮播图不比页面老太多（防旧图残留）', stale.map((s) => s.f).join(', '));

  // ③ 文案：中文页不走 apply，正文必须自己更新
  const titles = await page.$$eval('#features .card h3', (els) => els.map((e) => e.textContent.trim()));
  ok(titles.length === 6, '六张功能卡', String(titles.length));
  const i18nZh = await page.evaluate(() => {
    const z = window.LIG_I18N && window.LIG_I18N.zh;
    return z && z.agentrmc && z.agentrmc.feat ? z.agentrmc.feat.f3t : null;
  });
  ok(!!i18nZh, 'i18n 里 agentrmc.feat 段在');
  // 正文标题应与 i18n 中文一致（两边同源，漂移就是漏改一边）
  const pairs = await page.evaluate(() => {
    const z = window.LIG_I18N && window.LIG_I18N.zh && window.LIG_I18N.zh.agentrmc;
    const out = [];
    for (let i = 1; i <= 6; i++) {
      const k = 'f' + i + 't';
      out.push({ key: k, i18n: z && z.feat ? z.feat[k] : null });
    }
    return out;
  });
  const drift = pairs
    .map((p, i) => ({ key: p.key, i18n: p.i18n, dom: titles[i] }))
    .filter((p) => p.i18n && p.i18n !== p.dom);
  ok(drift.length === 0, '中文正文与 i18n 中文段一致',
    drift.map((d) => `${d.key}: dom="${d.dom}" i18n="${d.i18n}"`).join(' ; '));

  // 页面错误
  ok(errs.length === 0, '页面无脚本/资源错误', errs.slice(0, 3).join(' ; '));

  await page.screenshot({ path: path.join(OUT, 'agentrmc-page.png'), fullPage: true });
  console.log('saved site_shots/agentrmc-page.png');

  await browser.close();
  server.close();
  console.log(fails === 0 ? '\nALL PASS site' : `\n${fails} FAILED`);
  process.exit(fails === 0 ? 0 : 1);
}

main().catch((e) => { console.error(e); process.exit(1); });
