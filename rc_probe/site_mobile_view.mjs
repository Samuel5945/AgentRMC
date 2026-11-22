// 手机视口验收：手机上看这两组轮播是否完整、不裁切。
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
// 与用户手机接近：390x844 CSS 像素，DPR 3
const page = await browser.newPage({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true });
await page.goto('http://127.0.0.1:4700/agentrmc.html?t=' + Date.now(), { waitUntil: 'networkidle' });
await page.waitForTimeout(1000);

// 滚到「界面」那一段
const shots = await page.$('.cover-carousel.is-phone');
await shots.scrollIntoViewIfNeeded();
await page.waitForTimeout(400);
await page.screenshot({ path: path.join(OUT, 'agentrmc-mobile-shots.png') });
console.log('saved agentrmc-mobile-shots.png');

const info = await page.$$eval('.cover-carousel.is-phone .cover-slide img', (els) =>
  els.map((e) => {
    const r = e.getBoundingClientRect();
    return { src: e.getAttribute('src'), w: Math.round(r.width), h: Math.round(r.height), nat: e.naturalWidth + 'x' + e.naturalHeight };
  }));
console.log('phone slides:', JSON.stringify(info));
await browser.close();
