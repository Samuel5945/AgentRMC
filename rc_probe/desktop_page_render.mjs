// 桌面外壳（WorkWebView）离线渲染：不抓 Qt 窗口，直接把 WorkWebView.cpp 里的
// 页面 HTML 抽出来，注入一份演示 state，用 Chromium 按真实视口出图。
//
// 为什么不用截图：Qt 窗口要先切页签（UIA 找元素 + 物理点击），失败就整批图
// 都是同一页；主题切换还得重启进程。渲染这条路是确定性的——页面本来就是 HTML，
// 数据从 window.__setState(json) 进，和真实外壳同一份代码。
//
// 用法：
//   node rc_probe/desktop_page_render.mjs            # 出全套到 rc_probe/desktop_shots/
//   node rc_probe/desktop_page_render.mjs inbox      # 只出某一页
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
const CPP = path.join(ROOT, 'native', 'src', 'ui', 'WorkWebView.cpp');
const OUT = path.join(HERE, 'desktop_shots');
const require = createRequire(import.meta.url);

// 与真实主窗一致的视口：main.cpp 里主窗默认 1060×720（比例约 1.47）。
// 之前用 1600×1000（1.6）渲染，横向被拉宽、行距显得更扁，和真机观感对不上
// （用户 2026-11-22 反馈「渲染的电脑端跟实际缩放不一样」）。
// 出图时再按 1600 宽等比放大（见 main 里的 resize），保持同一比例。
const VIEWPORT = { width: 1060, height: 720 };
const OUT_WIDTH = 1600;

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

function loadPlaywright() {
  const tries = ['playwright-core'];
  try { tries.push(path.join(execSync('npm root -g', { encoding: 'utf8' }).trim(), 'playwright-core')); } catch { /* 没在 PATH */ }
  tries.push(path.join(ROOT, 'node_modules', 'playwright-core'));
  tries.push(path.join(HERE, 'node_modules', 'playwright-core'));
  for (const p of tries) {
    try { return require(p); } catch { /* 换下一个 */ }
  }
  console.error('找不到 playwright-core，请 npm i -g playwright-core');
  process.exit(2);
}

// 三个 R"HTML(...)"HTML" 段按出现顺序拼起来就是完整文档：
// [0] 到 <script> 前、[1] 是 "let STATE = " 的前半、[2] 是后半 + </html>。
function shellHtml() {
  const src = fs.readFileSync(CPP, 'utf8');
  const chunks = [...src.matchAll(/R"HTML\(([\s\S]*?)\)HTML"/g)].map((m) => m[1]);
  if (chunks.length < 3) throw new Error('WorkWebView.cpp 里的 R"HTML 段数量不对');
  // 首帧 STATE 用占位对象，随后 __setState 覆盖。
  return chunks[0] + '{}' + chunks[2];
}

const THEMES = {
  light: {
    bg: '#f3f5f8', 'card.bg': '#ffffff', 'card.bg2': '#eef1f5',
    text: '#171e2a', 'text.soft': '#1c2331', 'text.dim': '#2a323f',
    muted: '#48525f', 'muted.2': '#5a6472', ghost: '#4a5361', faint: '#8f98a3',
    line: '#c9d0d9', raised: '#ffffff', accent: '#4f8cff', 'accent.hover': '#6299ff',
    'on.accent': '#ffffff', danger: '#e5484d', warn: '#d29922', ok: '#3fb950',
    pending: '#f0883e', info: '#58a6ff',
    w6: 'rgba(15,23,42,.047)', w8: 'rgba(15,23,42,.063)', w10: 'rgba(15,23,42,.078)',
    w14: 'rgba(15,23,42,.110)', w16: 'rgba(15,23,42,.125)', w18: 'rgba(15,23,42,.141)',
    w30: 'rgba(15,23,42,.235)', a26: 'rgba(79,140,255,.102)',
    a36: 'rgba(79,140,255,.141)', a64: 'rgba(79,140,255,.251)',
  },
  dark: {
    bg: '#0f1319', 'card.bg': '#171d26', 'card.bg2': '#1d2530',
    text: '#eef2f7', 'text.soft': '#dbe3ec', 'text.dim': '#c2ccd8',
    muted: '#93a0b0', 'muted.2': '#7d8a9a', ghost: '#9aa7b6', faint: '#5c6878',
    line: '#2c3644', raised: '#171d26', accent: '#4f8cff', 'accent.hover': '#6299ff',
    'on.accent': '#ffffff', danger: '#f2555a', warn: '#e3b341', ok: '#3fb950',
    pending: '#f0883e', info: '#58a6ff',
    w6: 'rgba(255,255,255,.055)', w8: 'rgba(255,255,255,.075)',
    w10: 'rgba(255,255,255,.095)', w14: 'rgba(255,255,255,.13)',
    w16: 'rgba(255,255,255,.15)', w18: 'rgba(255,255,255,.17)',
    w30: 'rgba(255,255,255,.30)', a26: 'rgba(79,140,255,.20)',
    a36: 'rgba(79,140,255,.28)', a64: 'rgba(79,140,255,.46)',
  },
};

const NOW = Date.now();
const iso = (ms) => new Date(ms).toISOString();

const WORKERS = [
  { exe: 'Qoder.exe', name: 'Qoder', alive: true, window: true },
  { exe: 'Qoder CN.exe', name: 'Qoder CN', alive: true, window: true },
  { exe: 'ZCode.exe', name: 'ZCode', alive: true, window: true },
  { exe: 'WorkBuddy.exe', name: 'WorkBuddy', alive: true, window: true },
  { exe: 'WorkBuddyAI.exe', name: 'WorkBuddy AI', alive: true, window: true },
  { exe: 'DeepSeek Harness.exe', name: 'DeepSeek Harness', alive: true, window: true },
  { exe: 'OpenSquilla.exe', name: 'OpenSquilla', alive: true, window: true },
];

const ITEMS = [
  {
    id: 'w1', title: 'Qoder CN', agent_exe: 'Qoder CN.exe', stage: 'needs-me',
    message: '方案 A（原位替换）和方案 B（新增入口）你更想要哪个？',
    session_id: 'session-9de7fa5f-4851-4404-a30e-e003e1ad4ba3',
    session_title: '手机页会话目录分组',
    project: 'D:\\githubrepos\\Agentrmc', updated_at: iso(NOW - 2 * 60e3),
    attention_since: iso(NOW - 2 * 60e3),
  },
  {
    id: 'w2', title: 'Qoder', agent_exe: 'Qoder.exe', stage: 'needs-me',
    message: '要不要把这一批改动一起提交并推送？',
    session_id: 'session-10e69ba9-ee4c-4e62-a1bc-81366f3275a9',
    session_title: '网站点击滑动动效优化',
    project: 'D:\\githubrepos\\lig-site', updated_at: iso(NOW - 5 * 60e3),
    attention_since: iso(NOW - 5 * 60e3),
  },
  {
    id: 'w2b', title: 'WorkBuddy AI', agent_exe: 'WorkBuddyAI.exe', stage: 'needs-me',
    message: '等你确认是否把 0.6.0 插件覆盖安装到本机。',
    session_id: 'sess_demo_wbai_e', session_title: '插件覆盖安装',
    project: 'D:\\githubrepos\\Agentrmc', updated_at: iso(NOW - 6 * 60e3),
    attention_since: iso(NOW - 6 * 60e3),
  },
  {
    id: 'w3', title: 'WorkBuddy', agent_exe: 'WorkBuddy.exe', stage: 'review',
    message: '本周设计周报已生成，等你验收。',
    session_id: 'sess_demo_wb_d', session_title: '设计周报',
    project: 'D:\\work\\weekly-report', updated_at: iso(NOW - 8 * 60e3),
  },
  {
    id: 'w4', title: 'ZCode', agent_exe: 'ZCode.exe', stage: 'review',
    message: '远程任务中控台的发布物料已全部就绪，等你验收。',
    session_id: 'sess_demo_zcode_c', session_title: '发布物料整理',
    project: 'D:\\githubrepos\\Agentrmc', updated_at: iso(NOW - 11 * 60e3),
  },
  {
    id: 'w5', title: 'DeepSeek Harness', agent_exe: 'DeepSeek Harness.exe', stage: 'working',
    message: '正在跑全量回归测试（42 / 96）。',
    session_id: 'session-f8cc1236-31df-4df4-82ef-b83f6f456c9e',
    session_title: '系统界面滚动问题排查',
    project: 'D:\\githubrepos\\Agentrmc', updated_at: iso(NOW - 14 * 60e3),
  },
  {
    id: 'w6', title: 'OpenSquilla', agent_exe: 'OpenSquilla.exe', stage: 'working',
    message: '正在整理本周竞品数据，预计 10 分钟。',
    session_id: 'agent:main:webchat:aaaaaaa1', session_title: '竞品数据整理',
    project: 'D:\\work\\research', updated_at: iso(NOW - 17 * 60e3),
  },
];

const EVENTS = [
  { kind: 'voice', text: '「把这一批改动一起提交并推送」→ Qoder', at: iso(NOW - 40e3) },
  { kind: 'system', text: 'Qoder 等你确认：要不要把这一批改动一起提交并推送？', at: iso(NOW - 2 * 60e3) },
  { kind: 'status', text: 'DeepSeek Harness 回合开始 · 全量回归测试', at: iso(NOW - 14 * 60e3) },
  { kind: 'error', text: 'WorkBuddy AI 未运行（窗口未打开），已跳过派发', at: iso(NOW - 22 * 60e3) },
  { kind: 'voice', text: '「打开剪映」→ 已启动', at: iso(NOW - 35 * 60e3) },
  { kind: 'system', text: 'OpenSquilla 会话已派发：整理本周竞品数据', at: iso(NOW - 48 * 60e3) },
  { kind: 'status', text: '手机 192.168.2.11 已配对', at: iso(NOW - 60 * 60e3) },
  { kind: 'system', text: '联动插件 0.6.0 已同步到 Qoder / Qoder CN / ZCode', at: iso(NOW - 75 * 60e3) },
];

const SYSTEM = {
  bridge: '桥接：正常（127.0.0.1:45915）',
  uptime: '运行时长：3 小时 12 分 40 秒',
  last: '最近事件：' + new Date(NOW - 40e3).toLocaleString('zh-CN', { hour12: false }).replace(/\//g, '-'),
  spool: '离线待补：0 条',
  plugin: '联动插件：内置 0.6.0 · 已同步 Qoder、Qoder CN、ZCode、WorkBuddy AI、WorkBuddy、DeepSeek Harness（desktop）插件 · OpenSquilla 外部桥（运行中）',
  problems: '问题：无',
  problemsOk: true,
  workers: WORKERS.map((w, i) => ({
    name: w.name, state: w.alive ? '在线' : '未运行', foreground: w.window,
    needs: [1, 1, 0, 0, 0, 1, 0][i], review: [0, 0, 1, 1, 0, 0, 0][i],
    working: [0, 0, 0, 0, 0, 1, 1][i],
    last: ['17:14', '17:12', '17:11', '17:13', '—', '17:09', '17:07'][i],
  })),
  lan: {
    enabled: true, running: true, port: 45916,
    urls: ['http://192.168.2.11:45916/m/', 'http://10.144.144.2:45916/m/'],
    pairing: { active: false, code: '' },
    devices: [
      { id: 'd1', name: 'Android 手机', pairedAt: NOW - 3 * 3600e3, lastSeen: NOW - 6 * 60e3 },
      { id: 'd2', name: 'PC2', pairedAt: NOW - 5 * 3600e3, lastSeen: NOW - 90 * 60e3 },
    ],
  },
  cursor: { enabled: true },
};

const KEYMAP = {
  pinned: '', sceneName: 'Qoder', sceneRule: '跟随前台 · 窗口标题匹配',
  scenes: ['Qoder', 'Qoder CN', 'ZCode', 'WorkBuddy', '浏览器'],
  groups: [
    {
      title: '方向键',
      rows: [
        { vk: 37, glyph: '←', name: 'Left', single: '上一个待办', double: '回到顶部', hold: '连续上移', off: false },
        { vk: 38, glyph: '↑', name: 'Up', single: '上一条事件', double: '跳到最新', hold: '连续上移', off: false },
        { vk: 39, glyph: '→', name: 'Right', single: '下一个待办', double: '回到列表', hold: '连续下移', off: false },
        { vk: 40, glyph: '↓', name: 'Down', single: '下一条事件', double: '清空已读', hold: '连续下移', off: false },
      ],
    },
    {
      title: '功能键',
      rows: [
        { vk: 13, glyph: '⏎', name: 'Enter', single: '标为已处理', double: '派回会话', hold: '—', off: false },
        { vk: 27, glyph: 'Esc', name: 'Esc', single: '返回列表', double: '收起面板', hold: '—', off: false },
        { vk: 32, glyph: '␣', name: 'Space', single: '开始 / 停止监听', double: '—', hold: '按住说话', off: false },
      ],
    },
  ],
  takeover: true,
  hint: '点击手势格改键 · 自动保存',
};

const APPS = {
  rows: [
    { id: 'jianying', name: '剪映', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'qoder', name: 'Qoder', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'qodercn', name: 'Qoder CN', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'zcode', name: 'ZCode', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'workbuddy', name: 'WorkBuddy', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'workbuddyai', name: 'WorkBuddy AI', icon: '', state: '可启动', stateTip: '商店应用', found: true, custom: false, customPath: '' },
    { id: 'deepseek', name: 'DeepSeek Harness', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'opensquilla', name: 'OpenSquilla', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'chrome', name: 'Chrome', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'photoshop', name: 'Photoshop', icon: '', state: '可启动', stateTip: '开始菜单快捷方式', found: true, custom: false, customPath: '' },
    { id: 'browser', name: '浏览器', icon: '', state: '可启动', stateTip: '系统默认浏览器', found: true, custom: false, customPath: '' },
    { id: 'custom:我的工具', name: '我的工具', icon: '', state: '路径待确认', stateTip: '自定义应用', found: false, custom: true, customPath: 'C:/tools/mytool.exe' },
  ],
  summary: '12 个应用 · 11 个可启动 · 已卸载的不再占位',
  voiceLaunch: true,
  systemControl: true,
};

const GUIDE = {
  intro: '没有遥控器也能用：语音说一句话、按耳机键、或者用手机遥控。下面是常用入口。',
  version: '0.6.0',
  sections: [
    {
      title: '语音怎么用',
      rows: [
        ['按住说话', '按住波形键或耳机键说话，松开即发；「试一句」录 3 秒验证识别是否正常'],
        ['派给谁', '说「让 Qoder 把这个改一下」直接派到对应宿主；不点名就派给当前前台'],
        ['系统控制', '「音量调到 50」「亮度暗一点」「下一首」直接执行；关机要再说一句确认'],
      ],
    },
    {
      title: '手机遥控',
      rows: [
        ['配对', '系统 → 手机访问 → 显示配对二维码，手机扫码即可（10 分钟内有效）'],
        ['看与管', '手机上能看会话、派发任务、看产物；删除进回收站，可恢复'],
        ['图片', '点图片是放大查看，想存再从浮层里点「保存到手机」'],
      ],
    },
    {
      title: '接入 Agent',
      rows: [
        ['一句话安装', '系统 → 接入 Agent，复制安装话术粘给任意 Agent，它自己按所在宿主装好'],
        ['支持的宿主', 'Qoder / Qoder CN / ZCode / WorkBuddy / WorkBuddy AI / DeepSeek Harness / OpenSquilla'],
        ['升级', '系统 → 升级联动插件；插件与桌面端同号 0.6.0'],
      ],
    },
  ],
  installSentence: '请帮我安装 AgentRMC 联动插件（版本 0.6.0）。',
};

const DISPATCH = {
  agents: WORKERS.filter((w) => w.alive).map((w, i) => ({
    exe: w.exe, name: w.name, alive: w.alive, window: w.window,
    can_dispatch: true, count: [3, 2, 5, 4, 0, 4, 2][i],
    sessions: [],
  })),
  generated_at: iso(NOW),
  noticeId: '', notice: '', draftId: '', draftText: '',
  thoughtLevels: ['low', 'medium', 'high', 'xhigh', 'max'],
  models: [],
  osqModels: [],
  dshModels: [],
};

// 派发页的会话行（按 Agent 分组，组内再按项目分）
function dispatchSessions() {
  const mk = (id, title, project, mins, extra = {}) => ({
    id, title, project, updated_at: iso(NOW - mins * 60e3),
    archived: false, generated: false, can_dispatch: true, can_delete: true, ...extra,
  });
  return [
    {
      exe: 'DeepSeek Harness.exe', name: 'DeepSeek Harness', alive: true, window: true,
      can_dispatch: true, count: 4,
      sessions: [
        mk('session-f8cc1236-31df-4df4-82ef-b83f6f456c9e', '系统界面滚动问题排查', 'D:\\githubrepos\\Agentrmc', 13, { stage: 'working' }),
        mk('session-eb750b8a-118d-4740-84ed-90f117a6446a', '优化应用界面图标瀑布流显示', 'D:\\githubrepos\\Agentrmc', 60),
        mk('session-097e8dd7-faff-428b-bd30-3d4c794cd538', 'OS11 景深主题设计', 'D:\\githubrepos\\一镜到底主题构建器', 180),
        mk('session-10e69ba9-ee4c-4e62-a1bc-81366f3275a9', '网站点击滑动动效全面优化', 'D:\\githubrepos\\lig-site', 300),
      ],
    },
    {
      exe: 'ZCode.exe', name: 'ZCode', alive: true, window: true, can_dispatch: true, count: 2,
      sessions: [
        mk('sess_zcode_1', '远程任务中控台发布物料', 'D:\\githubrepos\\Agentrmc', 25, { stage: 'review' }),
        mk('sess_zcode_2', '按键映射表重排', 'D:\\githubrepos\\Agentrmc', 240),
      ],
    },
    {
      exe: 'Qoder.exe', name: 'Qoder', alive: true, window: true, can_dispatch: true, count: 2,
      sessions: [
        mk('session_qoder_1', '官网预览图重做', 'D:\\githubrepos\\lig-site', 40, { stage: 'needs-me' }),
        mk('session_qoder_2', '立格编辑器发布前审查', 'D:\\githubrepos\\lig-editor', 600),
      ],
    },
  ];
}

const SESSION_CHAT = {
  id: 'w5',
  title: 'DeepSeek Harness',
  agentExe: 'DeepSeek Harness.exe',
  agentName: 'DeepSeek Harness',
  sessionId: 'session-f8cc1236-31df-4df4-82ef-b83f6f456c9e',
  sessionTitle: '系统界面滚动问题排查',
  project: 'D:\\githubrepos\\Agentrmc',
  supported: true, error: '',
  model: 'DeepSeek-V4.1-Flash',
  stage: 'working',
  total: 6, hasMore: false, offset: 0,
  live: { text: '正在跑全量回归测试（42 / 96）', at: iso(NOW - 20e3) },
  requests: [],
  messages: [
    {
      id: 'm1', role: 'user', at: iso(NOW - 40 * 60e3),
      text: '系统界面滚动有问题，内容下面还有内容但滚不到',
    },
    {
      id: 'm2', role: 'assistant', at: iso(NOW - 38 * 60e3),
      text: '根因是系统页没有滚动容器：卡片展开后内容超高，窗口直接裁掉。我把中间整块装进一个可滚区域，底部按钮留在滚动区外固定贴底。',
    },
    {
      id: 'm3', role: 'system', at: iso(NOW - 30 * 60e3),
      text: '工具调用 · Read WorkWebView.cpp（0.4 秒）',
    },
    {
      id: 'm4', role: 'assistant', at: iso(NOW - 28 * 60e3),
      text: '改完了，构建通过。顺带发现会话被 /cd 到子目录后会归错项目组，也一并修了——项目身份只认 identity.cwd。',
    },
    {
      id: 'm5', role: 'user', at: iso(NOW - 10 * 60e3),
      text: '好，跑一遍测试再出包',
    },
    {
      id: 'm6', role: 'assistant', at: iso(NOW - 9 * 60e3),
      text: '正在跑：lanweb 159 项、transcript 153 项、dsh_sessions 40 项，全绿后出包装机。',
    },
  ],
};

function stateFor(page, themeName) {
  const theme = THEMES[themeName];
  const base = {
    theme,
    work: { items: ITEMS, attention_count: 3 },
    workers: WORKERS,
    health: {
      bridge: true, uptime_seconds: 11560, spool_pending: 0, problems: [],
      workers: WORKERS.map((w) => ({ alive: w.alive })),
    },
    events: EVENTS,
    system: SYSTEM,
    keymap: KEYMAP,
    apps: APPS,
    guide: GUIDE,
    header: { listening: true, recording: false, maximized: false },
    left: {
      form: false,
      voice: {
        backend: 'sense', kind: 'dim',
        status: '就绪 · 本地 SenseVoice 模型已加载',
        transcript: '把这一批改动一起提交并推送',
        testEnabled: true,
        foot: '本地转写 · 音频不出本机', footVisible: true,
      },
      picker: {
        agents: ['DeepSeek Harness', 'Qoder', 'Qoder CN', 'ZCode', 'WorkBuddy', 'OpenSquilla'],
        modes: ['续当前会话', '新开一轮'],
        agent: 'DeepSeek Harness', mode: '续当前会话',
      },
      remote: {
        device: 'HONOR Earbuds A', battery: 81, connected: true,
        badge: '待机', state: '耳机已连接 · 按播放键说话',
        legend: '单击 播放/暂停 · 双击 下一首 · 长按 语音',
        keys: [],
      },
      vpage: {},
    },
    dispatch: { ...DISPATCH, agents: dispatchSessions() },
    chat: null,
    winDiag: {},
  };
  if (page === 'chat') base.chat = SESSION_CHAT;
  return base;
}

const PAGES = [
  { key: 'inbox', tab: 0, name: '待我处理' },
  { key: 'dispatch', tab: 1, name: '派发' },
  { key: 'events', tab: 2, name: '事件流' },
  { key: 'system', tab: 3, name: '系统' },
  { key: 'keymap', tab: 4, name: '按键映射' },
  { key: 'apps', tab: 5, name: '应用' },
  { key: 'guide', tab: 6, name: '使用引导' },
  { key: 'chat', tab: 0, name: '会话聊天' },
];

async function main() {
  const only = process.argv[2];
  if (!CHROME) { console.error('找不到本机 Chromium（ms-playwright）'); process.exit(2); }
  const { chromium } = loadPlaywright();
  fs.mkdirSync(OUT, { recursive: true });

  const html = shellHtml();
  const tmp = path.join(OUT, '_shell.html');
  fs.writeFileSync(tmp, html, 'utf8');

  const browser = await chromium.launch({ executablePath: CHROME, args: ['--force-device-scale-factor=1'] });
  // deviceScaleFactor ≈ 1600/1060，出图约 1600 宽且比例与真窗一致。
  const page = await browser.newPage({
    viewport: VIEWPORT, deviceScaleFactor: OUT_WIDTH / VIEWPORT.width,
  });

  const jobs = [];
  for (const p of PAGES) {
    if (only && p.key !== only) continue;
    for (const themeName of ['light', 'dark']) {
      jobs.push({ page: p, themeName });
    }
  }

  for (const job of jobs) {
    await page.goto('file://' + tmp.replace(/\\/g, '/'));
    await page.waitForTimeout(120);
    await page.evaluate(({ s, tab, sessions }) => {
      window.__setState(s);
      // 派发页的会话目录走独立接口（不进 5 秒整窗 state），和真实外壳同一条路。
      if (typeof window.__setSessions === 'function') window.__setSessions(sessions);
      // 切页签：直接点按钮，和真人操作同一条路径。
      const btns = [...document.querySelectorAll('.tab')];
      if (btns[tab]) btns[tab].click();
    }, {
      s: stateFor(job.page.key, job.themeName),
      tab: job.page.tab,
      sessions: { agents: dispatchSessions(), generated_at: iso(NOW) },
    });
    await page.waitForTimeout(220);
    const file = path.join(OUT, `${job.page.key}_${job.themeName}.png`);
    // 按 OUT_WIDTH 等比放大：1060×720 → 1600×1087，比例不变，字体渲染更清晰。
    await page.setViewportSize({ width: VIEWPORT.width, height: VIEWPORT.height });
    await page.screenshot({ path: file });
    console.log('saved', path.basename(file));
  }

  await browser.close();
  fs.unlinkSync(tmp);
  console.log('DONE ->', OUT);
}

main().catch((e) => { console.error(e); process.exit(1); });
