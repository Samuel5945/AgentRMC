// 桌面视口验收：两组轮播在宽屏上的排布。
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.join(HERE, 'site_shots');
const require = createRequire(import.meta.url);

function chromePath() {
  const root = path.join(process.env.LOCALAPPDATA || '', 'ms-playwright');
  let names = [];
  try { names = fs.readdirSync(root).filter((n) => n.startsWith('chromium-')).sort().reverse(); } catch { /* none */ }
  for (const n of names) {
    const exe = path.join(root, n, 'chrome-win64', 'chrome.exe');
    if (fs.existsSync(exe)) return exe;
  }
  return '';
}
function loadPlaywright() {
  const tries = ['playwright-core'];
  try { tries.push(path.join(execSync('npm root -g', { encoding: 'utf8' }).trim(), 'playwright-core')); } catch { /* none */ }
  tries.push(path.join(HERE, 'node_modules', 'playwright-core'));
  for (const p of tries) { try { return require(p); } catch { /* next */ } }
  process.exit(2);
}

const { chromium } = loadPlaywright();
fs.mkdirSync(OUT, { recursive: true });
const browser = await chromium.launch({ executablePath: chromePath() });
const page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1 });
await page.goto('http://127.0.0.1:4700/agentrmc.html?t=' + Date.now(), { waitUntil: 'networkidle' });
await page.waitForTimeout(900);

const el = await page.$('.cover-carousel.is-phone');
await el.scrollIntoViewIfNeeded();
await page.waitForTimeout(500);
// 截「界面」整段（含两组）
const sec = await page.$('section.section:has(.cover-carousel)');
await sec.screenshot({ path: path.join(OUT, 'agentrmc-shots-section.png') });
console.log('saved agentrmc-shots-section.png');

const boxes = await page.$$eval('.cover-carousel', (els) =>
  els.map((e) => {
    const r = e.getBoundingClientRect();
    return { cls: e.className, w: Math.round(r.width), h: Math.round(r.height) };
  }));
console.log('carousels:', JSON.stringify(boxes));
await browser.close();
