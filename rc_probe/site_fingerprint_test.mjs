// 资源指纹回归：证明「内容变 → 文件名变；内容不变 → 文件名不变」。
//
// 背景（用户 2026-11-22）：图片文件名固定，Cloudflare Pages 与浏览器都按 URL
// 缓存，换图后线上会有一段时间仍是旧图。tools/publish.mjs 的 fingerprint()
// 把资源改名成 name.<hash>.ext 并重写引用，从根上解决。
//
// 这个脚本在临时目录跑两次构建（原样 / 改一个字节），断言文件名变化，
// 不碰 dist，也不改源码（改完立刻还原）。
//
// 用法：node rc_probe/site_fingerprint_test.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SITE = process.argv[2] || 'D:\\githubrepos\\lig-site';
const IMG = path.join(SITE, 'assets', 'img', 'agentrmc-main.jpg');

let fails = 0;
const ok = (cond, what, extra = '') => {
  console.log((cond ? 'PASS ' : 'FAIL ') + what + (extra ? ' | ' + extra : ''));
  if (!cond) fails++;
};

function buildTo(out) {
  execFileSync('node', ['tools/publish.mjs', '--out', out], { cwd: SITE, stdio: 'pipe' });
}

function fpNames(out) {
  const dir = path.join(out, 'assets', 'img');
  return fs.readdirSync(dir).filter((f) => f.startsWith('agentrmc-main.')).sort();
}

const tmpA = fs.mkdtempSync(path.join(os.tmpdir(), 'fpA-'));
const tmpB = fs.mkdtempSync(path.join(os.tmpdir(), 'fpB-'));
const backup = path.join(os.tmpdir(), 'agentrmc-main.fp_test_bak');

try {
  buildTo(tmpA);
  const namesA = fpNames(tmpA);
  ok(namesA.length === 1, '首次构建产出带指纹的文件名', namesA.join(','));
  ok(/\.[0-9a-f]{10}\.jpg$/.test(namesA[0] || ''), '文件名形如 name.<10位hash>.jpg', namesA[0]);

  // 改一个字节，内容哈希必变
  fs.copyFileSync(IMG, backup);
  fs.appendFileSync(IMG, Buffer.from([0]));
  buildTo(tmpB);
  const namesB = fpNames(tmpB);
  ok(namesB.length === 1 && namesB[0] !== namesA[0],
    '图片内容一变，文件名就变（缓存必然失效）', `${namesA[0]} -> ${namesB[0]}`);

  // 引用也要跟着改：dist 里不该再出现旧文件名
  const htmlB = fs.readFileSync(path.join(tmpB, 'agentrmc.html'), 'utf8');
  ok(!htmlB.includes(path.basename(namesA[0])), '页面不再引用旧文件名');
  ok(htmlB.includes(path.basename(namesB[0])), '页面引用新文件名');

  // 还原后再构建，名字应与第一次完全一致（幂等）
  fs.copyFileSync(backup, IMG);
  buildTo(tmpA);
  const namesC = fpNames(tmpA);
  ok(namesC[0] === namesA[0], '内容不变时文件名不变（幂等，不会无谓刷缓存）',
    `${namesA[0]} vs ${namesC[0]}`);

  // _headers 必须产出（长缓存只给带指纹的资源）
  const headers = fs.readFileSync(path.join(tmpA, '_headers'), 'utf8');
  ok(/\/assets\/\*[\s\S]*immutable/.test(headers), '_headers 给 /assets/* 长缓存');
  ok(/\*\.html[\s\S]*max-age=0/.test(headers), '_headers 给 HTML 短缓存');
} finally {
  if (fs.existsSync(backup)) { fs.copyFileSync(backup, IMG); fs.unlinkSync(backup); }
  fs.rmSync(tmpA, { recursive: true, force: true });
  fs.rmSync(tmpB, { recursive: true, force: true });
}

console.log(fails === 0 ? '\nALL PASS fingerprint' : `\n${fails} FAILED`);
process.exit(fails === 0 ? 0 : 1);
