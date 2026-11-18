// WebView2 网页层诊断：临时开 CDP 端口，用真实浏览器上下文读 DOM / computed
// style / 变量值。调网页样式问题时别再靠猜——这个能直接给出"浏览器到底算成了
// 什么颜色"。2026-11-18 就是靠它查出「模型面板变粉紫」是合成公式挑错了底
// （`--panel` 实际值是 rgb(248,137,249)，而 `--surface` 是 rgba(255,255,255,.388)）。
//
// 用法（PowerShell：起进程和连 CDP 必须在同一次调用里，因为 Start-Process 起的
// GUI 会随会话回收；想长期跑就用 explorer 当父进程，但没有调试端口）：
//   $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS='--remote-debugging-port=9222'
//   Start-Process "$env:LOCALAPPDATA\AgentRMC\AgentRMCNative.exe"   # 或注册表 InstallDir
//   Start-Sleep 7
//   node rc_probe/webview_cdp_probe.mjs [js 表达式]
//
// 不给表达式时读默认的诊断集（模型面板背景 + 相关 CSS 变量）。
const PORT = process.env.CDP_PORT || 9222;
const expr = process.argv[2] || `(() => {
  const p = document.querySelector('#dchatModelPanel');
  if (!p) return 'no #dchatModelPanel on this page (tab 可能不在派发页)';
  const cs = getComputedStyle(p);
  const root = getComputedStyle(document.documentElement);
  return JSON.stringify({
    inline: p.style.cssText,
    bg: cs.backgroundColor,
    overflow: cs.overflow,
    boxShadow: cs.boxShadow.slice(0, 48),
    backdrop: cs.backdropFilter,
    hidden: p.hidden,
    panelVar: root.getPropertyValue('--panel').trim(),
    surfaceVar: root.getPropertyValue('--surface').trim(),
    bgVar: root.getPropertyValue('--bg').trim(),
    textVar: root.getPropertyValue('--text').trim(),
  });
})()`;

let list;
try {
  list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
} catch (e) {
  console.error(`连不上 CDP ${PORT}：应用可能已被回收，或没带 ` +
    '--remote-debugging-port 启动（见文件头用法）');
  process.exit(1);
}
const page = list.find((t) => t.type === 'page');
if (!page) {
  console.error('没有 page target：', list.map((t) => t.type).join(','));
  process.exit(1);
}

const ws = new WebSocket(page.webSocketDebuggerUrl);
let seq = 0;
const pending = new Map();
const send = (method, params) =>
  new Promise((res) => {
    const id = ++seq;
    pending.set(id, res);
    ws.send(JSON.stringify({ id, method, params }));
  });
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m);
    pending.delete(m.id);
  }
};
await new Promise((r) => (ws.onopen = r));
const out = await send('Runtime.evaluate', { expression: expr, returnByValue: true });
const val = out.result?.result?.value;
console.log(val === undefined ? JSON.stringify(out).slice(0, 600) : val);
ws.close();
