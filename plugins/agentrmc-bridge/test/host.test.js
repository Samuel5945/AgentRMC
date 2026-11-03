'use strict';
// 宿主判定回归测试：零依赖，`node test/host.test.js` 直接跑。
// 覆盖两层——
//   A) lib/host.js 的判据优先级（纯函数，注入 scriptDir/argv/env）；
//   B) 真脚本端到端：把插件树复制到假装的 ~/.qoder 与 ~/.qoder-cn 配置家里，
//      用 AGENTRMC_DRY_RUN=1 跑 hooks/agentrmc-notify.js，断言它自己报出的
//      {title, raise, source}——这才是「装到哪个 app 就念哪个、拉哪个窗」。
// 判据是 8 个具名用例 + 2 个端到端用例，全过才打 ALL PASS。

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const { resolveHost, workbuddyVariantFromPath } = require('../lib/host');

const PLUGIN_ROOT = path.resolve(__dirname, '..');
let passed = 0;
const failures = [];

function check(name, fn) {
  try {
    fn();
    passed += 1;
    console.log('PASS ' + name);
  } catch (e) {
    failures.push(name + ': ' + e.message);
    console.log('FAIL ' + name + ' :: ' + e.message);
  }
}

// ---------------------------------------------------------------- A) 纯函数
const INTL_HOOKS = 'C:/Users/PC/.qoder/plugins/cache/local/agentrmc-bridge/0.3.0/hooks';
const CN_HOOKS = 'C:/Users/PC/.qoder-cn/plugins/cache/local/agentrmc-bridge/0.3.0/hooks';
const INTL_MCP = 'C:/Users/PC/.qoder/plugins/cache/local/agentrmc-bridge/0.3.0/mcp';
const CN_MCP = 'C:/Users/PC/.qoder-cn/plugins/cache/local/agentrmc-bridge/0.3.0/mcp';
const ZCODE_HOOKS =
  'C:/Users/PC/.zcode/cli/plugins/cache/dev-agentrmc-local/agentrmc-bridge/0.3.0/hooks';
const WB_HOOKS =
  'C:/Users/PC/.workbuddy/plugins/cache/dev-agentrmc-local/agentrmc-bridge/0.3.0/hooks';
const WB_AI_HOOKS =
  'C:/Users/PC/.workbuddy-ai/plugins/cache/dev-agentrmc-local/agentrmc-bridge/0.3.0/hooks';

check('intl-hook: 路径在 .qoder 下 -> Qoder + Qoder.exe', () => {
  const h = resolveHost({ scriptDir: INTL_HOOKS, argv: ['stop'], env: {} });
  assert.strictEqual(h.agent, 'Qoder');
  assert.strictEqual(h.raise, 'Qoder.exe');
  assert.strictEqual(h.source, 'qoder-intl');
});

check('cn-hook: 路径在 .qoder-cn 下 -> Qoder CN + Qoder CN.exe', () => {
  const h = resolveHost({ scriptDir: CN_HOOKS, argv: ['permission'], env: {} });
  assert.strictEqual(h.agent, 'Qoder CN');
  assert.strictEqual(h.raise, 'Qoder CN.exe');
  assert.strictEqual(h.source, 'qoder-cn');
});

check('intl-mcp / cn-mcp: mcp 目录同样认得出', () => {
  assert.strictEqual(resolveHost({ scriptDir: INTL_MCP, argv: [], env: {} }).agent, 'Qoder');
  assert.strictEqual(resolveHost({ scriptDir: CN_MCP, argv: [], env: {} }).agent, 'Qoder CN');
});

check('zcode: 非 Qoder 路径不冒充 Qoder，raise 留空走原生缺省', () => {
  const h = resolveHost({ scriptDir: ZCODE_HOOKS, argv: ['stop'], env: {} });
  assert.strictEqual(h.agent, 'ZCode');
  assert.strictEqual(h.raise, '');
  assert.strictEqual(h.source, 'zcode');
});

check('workbuddy-classic: 路径在 .workbuddy 下 -> WorkBuddy + WorkBuddy.exe', () => {
  const h = resolveHost({
    scriptDir: WB_HOOKS,
    argv: ['stop'],
    env: { WORKBUDDY_APP_VERSION: '1.2.3' },
  });
  assert.strictEqual(h.agent, 'WorkBuddy');
  assert.strictEqual(h.raise, 'WorkBuddy.exe');
  assert.strictEqual(h.source, 'workbuddy-classic');
});

check('workbuddy-ai: 路径在 .workbuddy-ai 下 -> WorkBuddy AI + WorkBuddyAI.exe', () => {
  const h = resolveHost({
    scriptDir: WB_AI_HOOKS,
    argv: ['stop'],
    env: { WORKBUDDY_APP_VERSION: '1.2.3' },
  });
  assert.strictEqual(h.agent, 'WorkBuddy AI');
  assert.strictEqual(h.raise, 'WorkBuddyAI.exe');
  assert.strictEqual(h.source, 'workbuddy-ai');
});

check('workbuddy 路径判据: .workbuddy-ai 先于 .workbuddy，裸路径不误判', () => {
  assert.strictEqual(workbuddyVariantFromPath(WB_AI_HOOKS), 'ai');
  assert.strictEqual(workbuddyVariantFromPath(WB_HOOKS), 'classic');
  assert.strictEqual(workbuddyVariantFromPath('C:/Users/PC/.workbuddy-custom/hooks'), null);
  assert.strictEqual(workbuddyVariantFromPath(''), null);
});

check('workbuddy 未知变体: 路径判不出但产品名命中 -> AI 兜底并标记 unknown', () => {
  const h = resolveHost({
    scriptDir: 'D:/somewhere/else/hooks',
    argv: ['stop'],
    env: { WORKBUDDY_APP_VERSION: '1.2.3' },
  });
  assert.strictEqual(h.agent, 'WorkBuddy');
  assert.strictEqual(h.raise, 'WorkBuddyAI.exe');
  assert.strictEqual(h.source, 'workbuddy-unknown-variant');
});

check('argv 优先: 手写配置显式给了就照办（向后兼容）', () => {
  const h = resolveHost({
    scriptDir: CN_HOOKS,
    argv: ['stop', 'MyAgent', 'MyAgent.exe'],
    env: {},
  });
  assert.deepStrictEqual([h.agent, h.raise, h.source], ['MyAgent', 'MyAgent.exe', 'argv']);
});

check('env 兜底: 认不出路径时用 QODER_PRODUCT_ID / CONFIG_DIR', () => {
  const byProduct = resolveHost({ scriptDir: '', argv: ['stop'], env: { QODER_PRODUCT_ID: 'qoder-cn' } });
  assert.deepStrictEqual([byProduct.agent, byProduct.raise], ['Qoder CN', 'Qoder CN.exe']);
  const byDir = resolveHost({
    scriptDir: '',
    argv: ['stop'],
    env: { QODER_CONFIG_DIR: 'C:\\Users\\PC\\.qoder-cn' },
  });
  assert.strictEqual(byDir.agent, 'Qoder CN');
});

check('未知变体不空手: 只知是 Qoder 家族时按国际版，绝不退化成找 ZCode.exe', () => {
  const h = resolveHost({
    scriptDir: 'D:/somewhere/else/hooks',
    argv: ['stop'],
    env: { QODER_PLUGIN_ROOT: 'D:/somewhere/else' },
  });
  assert.strictEqual(h.agent, 'Qoder');
  assert.strictEqual(h.raise, 'Qoder.exe');
  assert.strictEqual(h.source, 'qoder-unknown-variant');
});

// OpenSquilla 不走插件包，没有注册表可判，只认显式声明与安装路径。
// 放错位置就会把 Qoder 的窗口拽起来，所以既要能认出来、也不能认错。
check('OpenSquilla: 环境变量显式声明', () => {
  const h = resolveHost({
    scriptDir: '',
    argv: ['stop'],
    env: { AGENTRMC_NOTIFY_HOST: 'OpenSquilla' },
  });
  assert.deepStrictEqual([h.agent, h.raise, h.source], ['OpenSquilla', 'OpenSquilla.exe', 'opensquilla-env']);
});

check('OpenSquilla: 装在 ~/.opensquilla 下按路径认', () => {
  const h = resolveHost({
    scriptDir: 'C:/Users/PC/.opensquilla/skills/agentrmc/hooks',
    argv: ['stop'],
    env: {},
  });
  assert.deepStrictEqual([h.agent, h.raise, h.source], ['OpenSquilla', 'OpenSquilla.exe', 'opensquilla-path']);
  const byEnv = resolveHost({
    scriptDir: '',
    argv: ['stop'],
    env: { OPENSQUILLA_STATE_DIR: 'C:/Users/PC/.opensquilla' },
  });
  assert.strictEqual(byEnv.agent, 'OpenSquilla');
});

check('OpenSquilla: 不误伤别的宿主与普通技能目录', () => {
  // ~/.agents/skills 是个人技能层，装着大量非宿主技能，不能因为路径就认成 OpenSquilla。
  const skill = resolveHost({
    scriptDir: 'C:/Users/PC/.agents/skills/agentrmc-voice-reminder',
    argv: ['stop'],
    env: {},
  });
  assert.strictEqual(skill.agent, 'ZCode');
  assert.strictEqual(skill.source, 'zcode');
  // Qoder 仍然按自己的路径判，不被 OpenSquilla 抢走。
  const qoder = resolveHost({
    scriptDir: 'C:/Users/PC/.qoder/plugins/cache/local/agentrmc-bridge/0.3.9/hooks',
    argv: ['stop'],
    env: {},
  });
  assert.strictEqual(qoder.agent, 'Qoder');
});

// ------------------------------------------------------- B) 真脚本端到端
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'agentrmc-host-'));

function installInto(configHomeDirName) {
  const dest = path.join(tmp, configHomeDirName, 'plugins', 'cache', 'local', 'agentrmc-bridge', '0.3.0');
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.cpSync(PLUGIN_ROOT, dest, {
    recursive: true,
    filter: (src) => path.basename(src) !== 'test',
  });
  return path.join(dest, 'hooks', 'agentrmc-notify.js');
}

// 这套 e2e 靠「脚本装在哪个配置家」判宿主，所以子进程的环境里不能留着宿主家族
// 的变量：在 Qoder / WorkBuddy 会话里跑测试时，resolveHost 的判据 3（环境变量）
// 会抢在路径判据之前命中，WorkBuddy 用例会绿变红。清掉之后同一份测试在任何
// 宿主里跑结果一致。
function hostFreeEnv(extra) {
  const env = Object.assign({}, process.env, extra);
  for (const key of Object.keys(env)) {
    if (/^(QODER_|QODERCN_|CLAUDE_|WORKBUDDY|CODEBUDDY)/.test(key)) delete env[key];
  }
  return env;
}

function dryRun(scriptPath, event, stdinJson) {
  // spool 指到临时目录：脚本会在里面记 asked-question.json，别写到真实 %APPDATA%。
  const env = hostFreeEnv({
    AGENTRMC_DRY_RUN: '1',
    AGENTRMC_SPOOL_DIR: path.join(tmp, 'spool'),
  });
  const out = execFileSync(
    process.execPath,
    [scriptPath, event],
    {
      input: stdinJson,
      env,
      encoding: 'utf8',
      timeout: 8000,
    },
  );
  const line = out.trim().split(/\r?\n/).filter(Boolean).pop();
  // 空输出 = 脚本判定「这条不该发」，静默退出。
  return line ? JSON.parse(line) : null;
}

check('e2e-intl: 复制到 .qoder 里跑真脚本 -> title/raise 是国际版', () => {
  const r = dryRun(installInto('.qoder'), 'stop', JSON.stringify({ session_id: 'x' }));
  assert.strictEqual(r.dry_run, true);
  assert.strictEqual(r.payload.title, 'Qoder');
  assert.strictEqual(r.payload.raise, 'Qoder.exe');
  assert.strictEqual(r.payload.event, 'stop');
  assert.match(r.payload.message, /Qoder 做完了/);
  assert.strictEqual(r.source, 'qoder-intl');
});

check('e2e: payload 带 plugin_version，且就是包里那份 VERSION', () => {
  const expected = fs.readFileSync(path.join(PLUGIN_ROOT, 'VERSION'), 'utf8').trim();
  assert.match(expected, /^\d+\.\d+\.\d+$/, `VERSION 不是三段号: ${expected}`);
  for (const home of ['.qoder', '.qoder-cn', '.workbuddy']) {
    const r = dryRun(installInto(home), 'stop', JSON.stringify({ session_id: 'v' }));
    assert.strictEqual(r.payload.plugin_version, expected, `${home} 没带上版本`);
  }
});

check('e2e-cn: 复制到 .qoder-cn 里跑真脚本 -> 同一个包念出 Qoder CN', () => {
  const r = dryRun(installInto('.qoder-cn'), 'permission', JSON.stringify({ tool_name: 'Bash', tool_input: { command: 'ls' } }));
  assert.strictEqual(r.payload.title, 'Qoder CN');
  assert.strictEqual(r.payload.raise, 'Qoder CN.exe');
  assert.strictEqual(r.source, 'qoder-cn');
});

check('e2e-workbuddy-classic: 复制到 .workbuddy 里跑真脚本 -> 旧版标题 + WorkBuddy.exe', () => {
  const r = dryRun(installInto('.workbuddy'), 'stop', JSON.stringify({ session_id: 'x' }));
  assert.strictEqual(r.dry_run, true);
  assert.strictEqual(r.source, 'workbuddy-classic');
  assert.strictEqual(r.payload.title, 'WorkBuddy');
  assert.strictEqual(r.payload.raise, 'WorkBuddy.exe');
  assert.match(r.payload.message, /^WorkBuddy 做完了/);
  assert.doesNotMatch(JSON.stringify(r.payload), /WorkBuddyAI\.exe/);
});

check('e2e-workbuddy-ai: 复制到 .workbuddy-ai 里跑真脚本 -> AI 标题 + WorkBuddyAI.exe', () => {
  const r = dryRun(installInto('.workbuddy-ai'), 'permission', JSON.stringify({ tool_name: 'Bash', tool_input: { command: 'ls' } }));
  assert.strictEqual(r.dry_run, true);
  assert.strictEqual(r.source, 'workbuddy-ai');
  assert.strictEqual(r.payload.title, 'WorkBuddy AI');
  assert.strictEqual(r.payload.raise, 'WorkBuddyAI.exe');
  assert.strictEqual(r.payload.event, 'permission');
  assert.doesNotMatch(JSON.stringify(r.payload), /"WorkBuddy\.exe"/);
});

check('payload 透传 session/project/event_id: 给 Work Item 去重和分组', () => {
  const r = dryRun(
    installInto('.workbuddy-ai'),
    'stop',
    JSON.stringify({ session_id: 's1', cwd: 'D:/repo', tool_use_id: 'tool-1' }),
  );
  assert.strictEqual(r.payload.session_id, 's1');
  assert.strictEqual(r.payload.project, 'D:/repo');
  assert.strictEqual(r.payload.event_id, 'tool-1');
});

check('AgentRMC 未开时写 spool: 连接失败不丢通知', () => {
  const spoolDir = fs.mkdtempSync(path.join(os.tmpdir(), 'agentrmc-spool-'));
  const script = installInto('.workbuddy-ai');
  execFileSync(process.execPath, [script, 'permission'], {
    input: JSON.stringify({
      tool_name: 'Bash',
      tool_input: { command: 'ls' },
      session_id: 's-spool',
    }),
    env: hostFreeEnv({
      AGENTRMC_PORT: '1',
      AGENTRMC_SPOOL_DIR: spoolDir,
    }),
    encoding: 'utf8',
    timeout: 8000,
  });
  const lines = fs
    .readFileSync(path.join(spoolDir, 'pending-events.jsonl'), 'utf8')
    .trim()
    .split(/\r?\n/);
  const payload = JSON.parse(lines[0]);
  assert.strictEqual(payload.event, 'permission');
  assert.strictEqual(payload.raise, 'WorkBuddyAI.exe');
  assert.strictEqual(payload.session_id, 's-spool');
  fs.rmSync(spoolDir, { recursive: true, force: true });
});

check('e2e-permission 文案: AskUserQuestion 走 PermissionRequest 时也念「等你选方案」', () => {
  const r = dryRun(installInto('.qoder'), 'permission', JSON.stringify({ tool_name: 'AskUserQuestion' }));
  assert.strictEqual(r.payload.message, '等你选方案');
  assert.strictEqual(r.payload.event, 'permission');
});

// —— 「等你选方案」在 Qoder 里靠 PreToolUse 触发（实测 PermissionRequest 不发），
//    见 hooks-qoder/hooks.json 的 matcher。原生端只对 stop/permission 出声，
//    所以 question 必须映射成 permission 发出去。
check('question→permission: 选方案提醒按 permission 发出，宿主各认各家', () => {
  const intl = dryRun(installInto('.qoder'), 'question',
    JSON.stringify({ session_id: 'q1', tool_name: 'AskUserQuestion' }));
  assert.strictEqual(intl.payload.event, 'permission');
  assert.strictEqual(intl.payload.message, '等你选方案');
  assert.strictEqual(intl.payload.title, 'Qoder');
  assert.strictEqual(intl.payload.raise, 'Qoder.exe');
  const cn = dryRun(installInto('.qoder-cn'), 'question',
    JSON.stringify({ session_id: 'q2', tool_name: 'AskUserQuestion' }));
  assert.strictEqual(cn.payload.title, 'Qoder CN');
  assert.strictEqual(cn.payload.raise, 'Qoder CN.exe');
});

// —— WorkBuddy 的准点信号：Notification(permission_prompt) 在弹窗出现那一刻派发。
//    同一件事它的 PreToolUse 要等用户答完才跑（实测 11:24:38 弹出、11:30:19 才 spawn），
//    所以 prompt 先喊，迟到的 question 必须被 asked-question.json 挡掉。
check('prompt: 弹窗出现就喊「等你选方案」，迟到的 PreToolUse 不再喊第二遍', () => {
  const s = installInto('.workbuddy-ai');
  const early = dryRun(s, 'prompt', JSON.stringify({
    session_id: 'w1',
    notification_type: 'permission_prompt',
    message: 'needs your permission to use AskUserQuestion',
  }));
  assert.strictEqual(early.payload.event, 'permission');
  assert.strictEqual(early.payload.message, '等你选方案');
  assert.strictEqual(early.payload.title, 'WorkBuddy AI');
  assert.strictEqual(early.payload.raise, 'WorkBuddyAI.exe');
  const late = dryRun(s, 'question',
    JSON.stringify({ session_id: 'w1', tool_name: 'AskUserQuestion' }));
  assert.strictEqual(late, null, '同一个弹窗的迟到提醒不该再喊一遍');
  // 普通工具授权已由 PermissionRequest hook 在弹窗前报过，prompt 通道不重复。
  assert.strictEqual(dryRun(s, 'prompt', JSON.stringify({
    session_id: 'w2',
    notification_type: 'permission_prompt',
    message: 'needs your permission to use Bash',
  })), null);
  // idle_prompt 之类的其它通知一律不管。
  assert.strictEqual(dryRun(s, 'prompt', JSON.stringify({
    session_id: 'w3',
    notification_type: 'idle_prompt',
    message: 'waiting for your input',
  })), null);
});

// —— 老宿主不带 session_id 时，去重键退回「宿主身份 + 项目目录」。
//    曾经的键是 `pid${process.ppid}`：hook 的父进程是宿主每次新起的 bash.exe，
//    两条信号的 pid 天生不同，去重从来没生效过（2026-11-03 实测复现）。
check('去重键兜底: 没 session_id 时按宿主+项目认，大小写与斜杠归一', () => {
  const s = installInto('.workbuddy-ai');
  const early = dryRun(s, 'prompt', JSON.stringify({
    notification_type: 'permission_prompt',
    message: 'needs your permission to use AskUserQuestion',
    cwd: 'D:\\Repo\\Proj-NoSess\\',
  }));
  assert.strictEqual(early.payload.message, '等你选方案');
  // 同一次弹窗的迟到信号：斜杠方向、大小写、尾斜杠都不一样，仍要认成同一件事。
  const late = dryRun(s, 'question', JSON.stringify({
    tool_name: 'AskUserQuestion',
    cwd: 'd:/repo/proj-nosess',
  }));
  assert.strictEqual(late, null, '同一宿主同一项目：迟到的 question 不该再喊');
  // 换一个项目就是另一件事，不能被上一条的记账吞掉。
  const other = dryRun(s, 'question', JSON.stringify({
    tool_name: 'AskUserQuestion',
    cwd: 'D:/repo/another',
  }));
  assert.strictEqual(other.payload.message, '等你选方案');
  // 有的宿主会把路径写成 D:\\a\\b（字面双反斜杠），归一后也得算同一个项目。
  const dbl = dryRun(s, 'prompt', JSON.stringify({
    notification_type: 'permission_prompt',
    message: 'AskUserQuestion',
    cwd: 'D:\\\\Repo\\\\Proj-Dbl',
  }));
  assert.strictEqual(dbl.payload.message, '等你选方案');
  assert.strictEqual(dryRun(s, 'question', JSON.stringify({
    tool_name: 'AskUserQuestion',
    cwd: 'd:/repo/proj-dbl',
  })), null, '双反斜杠路径应归一成同一个项目');
  // 连 cwd 都不给的宿主本来就没法区分会话，只能退化成宿主级键，与带 cwd 的
  // question 对不上——按「宁可多喊一次也别漏」接受这一次重复。
  const noCwd = dryRun(s, 'prompt', JSON.stringify({
    notification_type: 'permission_prompt',
    message: 'AskUserQuestion',
  }));
  assert.strictEqual(noCwd.payload.message, '等你选方案');
  const afterNoCwd = dryRun(s, 'question', JSON.stringify({
    tool_name: 'AskUserQuestion',
    cwd: 'D:/repo/third',
  }));
  assert.notStrictEqual(afterNoCwd, null, '给不出 cwd 的宿主只许多喊一声，不能静默漏掉');
});

// 语音和气泡要念/显示两回事：「等你确认 Bash：D:\path」看着有用，念出来是噪音；
// 而语音少了宿主名就分不清是谁在等。speech 只给语音，缺省才退回念 message。
check('speech 与 message 分开: 语音短文案带宿主名，气泡留工具名和路径', () => {
  const r = dryRun(installInto('.qoder'), 'permission', JSON.stringify({
    tool_name: 'Bash',
    tool_input: { command: 'ls D:/tmp' },
  }));
  assert.strictEqual(r.payload.message, '等你确认 Bash：ls D:/tmp');
  assert.strictEqual(r.payload.speech, 'Qoder 在等你点确认');
  const q = dryRun(installInto('.qoder-cn'), 'question',
    JSON.stringify({ session_id: 'q-speech', tool_name: 'AskUserQuestion' }));
  assert.strictEqual(q.payload.speech, 'Qoder CN 在等你选方案');
  const stop = dryRun(installInto('.qoder'), 'stop', JSON.stringify({}));
  assert.strictEqual(stop.payload.speech, undefined,
    'stop 的 message 本来就带宿主名，不该再塞一份');
});

check('question 防刷屏: matcher 没生效时非 AskUserQuestion 一律静默不发', () => {
  const s = installInto('.qoder');
  assert.strictEqual(dryRun(s, 'question', JSON.stringify({ tool_name: 'Bash', tool_input: { command: 'ls' } })), null);
  assert.strictEqual(dryRun(s, 'question', JSON.stringify({ tool_name: 'Write' })), null);
  // stdin 空 / 没有 tool_name 也不发——宁可少一条，不能把授权提醒刷成屏。
  assert.strictEqual(dryRun(s, 'question', ''), null);
  assert.strictEqual(dryRun(s, 'question', 'not json at all'), null);
});

check('question 仍走 stop/permission 之外的静默边界: 未知事件名不误伤', () => {
  const r = dryRun(installInto('.qoder'), 'stop', JSON.stringify({ tool_name: 'AskUserQuestion' }));
  assert.strictEqual(r.payload.event, 'stop');
  assert.match(r.payload.message, /做完了/);
});

check('stop 念出最终回复: transcript 最后一条 assistant 压成一句', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'agentrmc-tr-'));
  const file = path.join(dir, 'transcript.jsonl');
  fs.writeFileSync(
    file,
    [
      JSON.stringify({ type: 'user', message: { role: 'user', content: '改一下' } }),
      JSON.stringify({
        type: 'assistant',
        message: { role: 'assistant', content: [{ type: 'text', text: '先看代码。' }] },
      }),
      JSON.stringify({
        type: 'assistant',
        message: {
          role: 'assistant',
          content: [{ type: 'text', text: '**改好了**，`dist` 已重新打包。后面还有一堆细节不用念。' }],
        },
      }),
    ].join('\n'),
  );
  const r = dryRun(installInto('.qoder'), 'stop', JSON.stringify({ transcript_path: file }));
  assert.match(r.payload.message, /改好了/);
  assert.doesNotMatch(r.payload.message, /先看代码/);  // 只取最后一条
  assert.ok(r.payload.message.length <= 90);  // 一句话，别整段念
});

check('stop 兜底: transcript 不存在时仍发通用文案', () => {
  const r = dryRun(
    installInto('.qoder'),
    'stop',
    JSON.stringify({ transcript_path: path.join(os.tmpdir(), 'agentrmc-none.jsonl') }),
  );
  assert.match(r.payload.message, /做完了/);
});

check('hooks-qoder 注册了 PreToolUse + matcher=AskUserQuestion', () => {
  const h = JSON.parse(fs.readFileSync(path.join(PLUGIN_ROOT, 'hooks-qoder', 'hooks.json'), 'utf8'));
  const group = h.hooks.PreToolUse[0];
  assert.strictEqual(group.matcher, 'AskUserQuestion');
  assert.strictEqual(group.async, true);
  assert.deepStrictEqual(group.hooks[0].args, ['${QODER_PLUGIN_ROOT}/hooks/agentrmc-notify.js', 'question']);
  assert.strictEqual(group.hooks[0].timeout, 3);
});

check('hooks 注册 UserPromptSubmit: 新一轮对话清旧验收', () => {
  const root = JSON.parse(
    fs.readFileSync(path.join(PLUGIN_ROOT, 'hooks', 'hooks.json'), 'utf8'),
  );
  assert.match(root.hooks.UserPromptSubmit[0].hooks[0].command, /turn-start/);
  const qoder = JSON.parse(
    fs.readFileSync(path.join(PLUGIN_ROOT, 'hooks-qoder', 'hooks.json'), 'utf8'),
  );
  assert.deepStrictEqual(qoder.hooks.UserPromptSubmit[0].hooks[0].args, [
    '${QODER_PLUGIN_ROOT}/hooks/agentrmc-notify.js',
    'turn-start',
  ]);
});

check('turn-start DRY_RUN: 带 session 且不出声', () => {
  const r = dryRun(
    installInto('.workbuddy'),
    'turn-start',
    JSON.stringify({ session_id: 's1', cwd: 'D:/repo' }),
  );
  assert.strictEqual(r.payload.event, 'turn-start');
  assert.strictEqual(r.payload.session_id, 's1');
  assert.strictEqual(r.payload.title, 'WorkBuddy');
  assert.strictEqual(r.payload.message, '');
});

check('hooks-workbuddy 注册了 PreToolUse + matcher=AskUserQuestion（清单指向该文件）', () => {
  const h = JSON.parse(fs.readFileSync(path.join(PLUGIN_ROOT, 'hooks-workbuddy', 'hooks.json'), 'utf8'));
  const group = h.hooks.PreToolUse[0];
  assert.strictEqual(group.matcher, 'AskUserQuestion');
  assert.strictEqual(group.hooks[0].command, 'node "${CLAUDE_PLUGIN_ROOT}/hooks/agentrmc-notify.js" question');
  assert.strictEqual(group.hooks[0].timeout, 3);
  const manifest = JSON.parse(
    fs.readFileSync(path.join(PLUGIN_ROOT, '.codebuddy-plugin', 'plugin.json'), 'utf8'),
  );
  assert.strictEqual(manifest.hooks, './hooks-workbuddy/hooks.json');
});

fs.rmSync(tmp, { recursive: true, force: true });

const total = passed + failures.length;
if (failures.length) {
  console.log('\n' + failures.length + ' FAILED of ' + total);
  failures.forEach((f) => console.log('  ' + f));
  process.exit(1);
}
console.log('\nALL PASS (' + passed + '/' + total + ')');
