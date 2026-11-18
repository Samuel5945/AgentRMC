// 桌面聊天页 composer / 附件验收（离线）：从 WorkWebView.cpp 抽 HTML 真渲染，
// mock WebView2 宿主通道（postMessage / __setFiles / __setSessionChat），
// 断言 📎 选文件 → 附件条 → 只有附件也能发 → 路径拼进正文 → 回执清空/失败保留。
//
// 用法：node rc_probe/desktop_chat_attachments.mjs
// 依赖 rc_probe/node_modules 里的 playwright-core（跟其它桌面探针共用）。
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
  try {
    names = fs.readdirSync(root).filter((n) => n.startsWith('chromium-')).sort().reverse();
  } catch { /* 没装 */ }
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

// 多段 R"HTML 拼回去：中间夹 jsEscape 的地方就是 stateJson 注入位，用 {} 顶。
function desktopHtml() {
  const src = fs.readFileSync(CPP, 'utf8');
  let out = '', last = 0, m;
  const re = /R"HTML\(([\s\S]*?)\)HTML"/g;
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
      { id: 's1', title: '附件验收会话', project: 'D:/githubrepos/Agentrmc', updated_at: iso, archived: false, generated: false },
    ],
  }],
};
const CHAT = {
  sessionId: 's1', agentExe: 'WorkBuddy.exe', supported: true,
  sessionTitle: '附件验收会话', agentName: 'WorkBuddy', total: 2,
  messages: [
    { id: 'c1', role: 'user', at: iso, text: '把附件路径读一下' },
    { id: 'c2', role: 'assistant', at: iso, text: '好' },
  ],
  requests: [],
};

let bad = 0;
function check(cond, what, extra) {
  if (cond) console.log('ok   ', what);
  else { bad += 1; console.log('FAIL ', what, extra === undefined ? '' : JSON.stringify(extra)); }
}

const { chromium } = loadPlaywright();
const browser = await chromium.launch({ executablePath: chromePath() || undefined });
const page = await browser.newPage({ viewport: { width: 1100, height: 760 } });
page.on('pageerror', (e) => { bad += 1; console.log('FAIL  pageerror:', e.message); });
await page.addInitScript(({ catalog, chat }) => {
  const posted = [];
  window.__posted = posted;
  window.__nextResult = { ok: true };  // 用例改这个控制派发回执
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage(msg) {
      posted.push(msg);
      if (!msg) return;
      if (msg.cmd === 'sessionsLoad') setTimeout(() => window.__setSessions && window.__setSessions(catalog), 0);
      if (msg.cmd === 'sessionOpen') setTimeout(() => window.__setSessionChat && window.__setSessionChat(chat), 0);
      if (msg.cmd === 'pickFiles') {
        setTimeout(() => window.__setFiles && window.__setFiles({
          plane: msg.plane || 'dchat',
          files: [
            { name: '报告.pdf', path: 'D:/tmp/报告.pdf', size: 2048 },
            { name: '图.png', path: 'D:/tmp/图.png', size: 1024 },
          ],
        }), 0);
      }
      // Ctrl+V 粘贴：native 落盘后回推同名路径（这里模拟落盘成功）。
      if (msg.cmd === 'savePaste') {
        setTimeout(() => window.__setFiles && window.__setFiles({
          plane: msg.plane,
          files: [{ name: msg.name, path: 'D:/githubrepos/Agentrmc/.agentrmc/inbox/s1/20261118-' + msg.name, size: 4 }],
        }), 0);
      }
      if (msg.cmd === 'dispatchSession' || msg.cmd === 'dispatch') {
        const r = window.__nextResult;
        setTimeout(() => window.__setDispatchResult && window.__setDispatchResult(r), 0);
      }
    },
  };
}, { catalog: CATALOG, chat: CHAT });

fs.mkdirSync(SHOTS, { recursive: true });
const htmlPath = path.join(SHOTS, '_chat_att_probe.html');
fs.writeFileSync(htmlPath, desktopHtml(), 'utf8');
await page.goto('file:///' + htmlPath.replace(/\\/g, '/'), { waitUntil: 'load' });
await page.click('#tabs .tab:nth-child(2)');
await page.waitForSelector('#dspList .dspsess', { timeout: 8000 });
await page.locator('#dspList .dspsess').first().click();
await page.waitForSelector('#dchat:not([hidden])', { timeout: 8000 });
await page.waitForFunction(() => dchatMsgs && dchatMsgs.length === 2, null, { timeout: 8000 });

check(await page.locator('#dchatAtts').evaluate((el) => el.hidden), '无附件时附件条隐藏');
check(await page.isDisabled('#dchatSend'), '空输入时发送键禁用');

await page.locator('#dchatAttach').click();
const pick = await page.evaluate(() => window.__posted.filter((m) => m.cmd === 'pickFiles').pop());
check(pick && pick.plane === 'dchat', '📎 走 native 选文件（plane=dchat）', pick);

await page.waitForFunction(() => ATTS.dchat && ATTS.dchat.length === 2, null, { timeout: 8000 });
check(await page.locator('#dchatAtts .catt').count() === 2, '多选文件都进 chip 条');
check((await page.textContent('#dchatAtts')).includes('报告.pdf'), 'chip 显示文件名');
check((await page.textContent('#dchatAtts')).includes('2.0KB'), 'chip 显示文件大小');
check(await page.isEnabled('#dchatSend'), '只有附件也能点亮发送键');
await page.screenshot({ path: path.join(SHOTS, 'chat_att_chips.png') });

// 重复选同一个文件不该叠 chip
await page.evaluate(() => window.__setFiles({ plane: 'dchat', files: [{ name: '报告.pdf', path: 'D:/tmp/报告.pdf', size: 2048 }] }));
await page.waitForTimeout(80);
check(await page.locator('#dchatAtts .catt').count() === 2, '同一路径重复选不叠 chip');

// Ctrl+V 截图：剪贴板只有 blob、没有本机路径 → base64 送 native 落盘 → 回推路径。
await page.evaluate(() => {
  const b64 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8AAAwAB/AF+H+YAAAAASUVORK5CYII=';
  const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  const dt = new DataTransfer();
  dt.items.add(new File([bytes], 'image.png', { type: 'image/png' }));
  document.dispatchEvent(new ClipboardEvent('paste', {
    clipboardData: dt, bubbles: true, cancelable: true,
  }));
});
await page.waitForFunction(() => ATTS.dchat && ATTS.dchat.length === 3, null, { timeout: 8000 });
const pasteMsg = await page.evaluate(() => window.__posted.filter((m) => m.cmd === 'savePaste').pop());
check(pasteMsg && pasteMsg.plane === 'dchat' && pasteMsg.session === 's1',
  'Ctrl+V 带上会话上下文', pasteMsg && { plane: pasteMsg.plane, session: pasteMsg.session });
check(pasteMsg && pasteMsg.name === '粘贴图片.png', '粘贴图按类型取扩展名', pasteMsg && pasteMsg.name);
check(pasteMsg && /^[A-Za-z0-9+/=]+$/.test(pasteMsg.data || '') && pasteMsg.data.length > 20,
  '粘贴图走 base64 送 native');
check(await page.locator('#dchatAtts .catt').count() === 3, '粘贴图片进附件条');

await page.evaluate(() => { window.__posted.length = 0; });
await page.locator('#dchatSend').click();
const sent = await page.evaluate(() => window.__posted.filter((m) => m.cmd === 'dispatchSession').pop());
check(sent && sent.text.includes('请查看文件：D:/tmp/报告.pdf') && sent.text.includes('请查看文件：D:/tmp/图.png'),
  '发送时把附件路径拼进正文', sent);
check(sent && sent.session === 's1' && sent.exe === 'WorkBuddy.exe', '派发参数仍带会话身份', sent);

await page.waitForFunction(() => ATTS.dchat && ATTS.dchat.length === 0, null, { timeout: 8000 });
check(await page.locator('#dchatAtts .catt').count() === 0, '受理回执后附件条清空');

// 失败路径：附件要留着（路径还在硬盘上，改完正文能直接重发）
await page.evaluate(() => { window.__nextResult = { ok: false, error: '会话已有派发在途' }; });
await page.evaluate(() => window.__setFiles({ plane: 'dchat', files: [{ name: 'a.txt', path: 'D:/tmp/a.txt', size: 12 }] }));
await page.waitForFunction(() => ATTS.dchat && ATTS.dchat.length === 1, null, { timeout: 8000 });
await page.locator('#dchatText').fill('再试一次');
await page.locator('#dchatSend').click();
await page.waitForTimeout(220);
check(await page.locator('#dchatAtts .catt').count() === 1, '派发失败保留附件');
check((await page.textContent('#dchatNotice')).includes('没发出去'), '失败原因写在状态行');
check(await page.isEnabled('#dchatSend'), '失败后发送键解禁（不卡在派发中）');

// 正文 + 附件混发时，回填草稿不含路径行
await page.evaluate(() => { window.__nextResult = { ok: true }; });
await page.locator('#dchatSend').click();
await page.waitForTimeout(220);
check((await page.inputValue('#dchatText')) === '', '成功后输入框清空');

// 美术主题（card.bg 可能是半透明 rgba）下面板必须实心——用户 2026-11-18 报的
// 「切换模型背景是透明的看不清」就是这条。两种写法都要覆盖：
//   rgba(...,0.216) → 合成成实色
//   rgba(...,55)    → Qt 的 0-255 写法，CSS 会钳成 1，浏览器渲染就是实色
const panelCheck = await page.evaluate(() => {
  const s = document.documentElement;
  const read = () => getComputedStyle(s).getPropertyValue('--panel').trim();
  applyTheme({ 'card.bg': 'rgba(8,12,20,0.216)', 'card.bg2': 'rgba(10,15,24,0.188)', text: '#171e2a' });
  const frac = read();
  applyTheme({ 'card.bg': 'rgba(8,12,20,55)', 'card.bg2': 'rgba(10,15,24,48)', text: '#171e2a' });
  const byte = read();
  const box = document.querySelector('#dchatModelPanel');
  const bg = box ? getComputedStyle(box).backgroundColor : '';
  return { frac, byte, bg };
});
check(/^rgb\(\s*\d+,\s*\d+,\s*\d+\s*\)$/.test(panelCheck.frac),
  '半透明小数 alpha → 面板色合成成实色', panelCheck.frac);
check(panelCheck.frac !== 'rgb(8,12,20)',
  '合成结果确实被提亮了（不是直接取原色）', panelCheck.frac);
check(panelCheck.byte === 'rgb(8,12,20)',
  '0-255 写法按浏览器语义当不透明原色', panelCheck.byte);
check(/^rgb\(/.test(panelCheck.bg) && !/rgba/.test(panelCheck.bg),
  '模型面板背景不含 alpha', panelCheck.bg);

console.log(bad ? `\n${bad} 项失败` : '\nALL PASS desktop chat attachments');
await browser.close();
process.exit(bad ? 1 : 0);
