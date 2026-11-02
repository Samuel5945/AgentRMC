'use strict';
// 宿主判定：同一个插件包会被装进好几个互不相干的 Agent，而 Qoder 国际版和
// Qoder CN 是**两个独立 app**——各自有配置家（~/.qoder 与 ~/.qoder-cn）、各自的
// 插件注册表，进程映像名也不同（Qoder.exe / Qoder CN.exe）。0.2.0 之前把
// 「拉前台」的目标写死成 Qoder CN.exe，结果国际版里桌宠把另一个 app 的窗拽起来，
// 且重装/更新就把任何手工修改覆盖回去。
//
// 现在改成自检，判据按可靠性排序：
//   1) 调用方 argv 显式给了宿主名和进程名 → 照办（老的手写配置行为不变）；
//   2) 脚本自己的安装路径落在哪个配置家 —— 每个 app 只会把插件解到自己的
//      ~/.qoder(-cn)/plugins/cache/... 下，这是最硬的证据，不依赖宿主注入什么；
//   3) 宿主注入的环境变量 QODER_CONFIG_DIR / QODERCN_CONFIG_DIR / QODER_PRODUCT_ID；
//   4) 都不是 Qoder 家族 → 沿用旧逻辑，按环境变量认 WorkBuddy / ZCode。
// 只认出「是 Qoder 家族」但认不出变体（自定义配置家）时按国际版处理，并在返回值
// 里标 source，方便排查。
//
// 零依赖：hooks 与 MCP server 两个入口都 require 这里。

const VARIANTS = {
  intl: { agent: 'Qoder', raise: 'Qoder.exe' },
  cn: { agent: 'Qoder CN', raise: 'Qoder CN.exe' },
};

function normPath(value) {
  return String(value || '')
    .toLowerCase()
    .replace(/\\/g, '/');
}

// 从一条路径里认 Qoder 变体：看它挂在哪个配置家目录下。
function variantFromPath(value) {
  const s = normPath(value);
  if (!s) return null;
  // 先认 CN，再认国际版：'.qoder-cn' 不含 '/.qoder/'，两者不会互相误命中。
  if (/(^|\/)\.qoder-cn(\/|$)/.test(s)) return 'cn';
  if (/(^|\/)\.qoder(\/|$)/.test(s)) return 'intl';
  // 自定义配置家（例如 .qoder-cn-2、.qoderworkcn）：目录名里带 cn 就按 CN。
  if (/(^|\/)\.qoder[a-z0-9_-]*cn[a-z0-9_-]*(\/|$)/.test(s)) return 'cn';
  return null;
}

function isQoderFamily(env) {
  return Boolean(
    env.QODER_CONFIG_DIR ||
      env.QODERCN_CONFIG_DIR ||
      env.QODER_PRODUCT_ID ||
      env.QODER_PLUGIN_ROOT ||
      env.QODER_WORKER_RUNTIME_ASSET_ROOT,
  );
}

/**
 * @param {object} [options] 全部可注入，方便测试矩阵不真去建目录。
 * @param {string} [options.scriptDir] 调用方自己的 __dirname。
 * @param {string[]} [options.argv] process.argv.slice(2)：[0] 事件名，[1] 宿主名，[2] 进程映像名。
 * @param {object} [options.env] process.env。
 * @returns {{agent: string, raise: string, source: string}}
 */
function resolveHost(options) {
  const opts = options || {};
  const scriptDir = opts.scriptDir || '';
  const argv = opts.argv || [];
  const env = opts.env || {};

  const argvAgent = String(argv[1] || '').trim();
  const argvRaise = String(argv[2] || '').trim();

  // 1) 显式给了完整一对就用它。
  if (argvAgent && argvRaise) {
    return { agent: argvAgent, raise: argvRaise, source: 'argv' };
  }

  // 2) 自己的安装路径。
  let variant = variantFromPath(scriptDir);

  // 3) 宿主注入的环境变量（路径类判据优先于产品 ID 字符串）。
  if (!variant) {
    variant =
      variantFromPath(env.QODERCN_CONFIG_DIR) ||
      variantFromPath(env.QODER_CONFIG_DIR) ||
      variantFromPath(env.QODER_PLUGIN_ROOT);
  }
  if (!variant && env.QODER_PRODUCT_ID) {
    const pid = normPath(env.QODER_PRODUCT_ID);
    if (/(^|[-_])cn([-_]|$)/.test(pid)) variant = 'cn';
    else if (pid.indexOf('qoder') >= 0) variant = 'intl';
  }

  if (variant) {
    const v = VARIANTS[variant];
    return {
      agent: argvAgent || v.agent,
      raise: argvRaise || v.raise,
      source: variant === 'cn' ? 'qoder-cn' : 'qoder-intl',
    };
  }

  // 认出是 Qoder 家族但认不出变体 → 按国际版，别让 raise 空掉退化成找 ZCode.exe。
  if (isQoderFamily(env) || argvAgent) {
    return {
      agent: argvAgent || VARIANTS.intl.agent,
      raise: argvRaise || VARIANTS.intl.raise,
      source: 'qoder-unknown-variant',
    };
  }

  // 4) 非 Qoder 宿主：沿用旧逻辑（ZCode 那份 hooks.json 不传 argv）。
  const workbuddy = Boolean(env.WORKBUDDY_APP_VERSION || env.CLIENT_INFO_PRODUCT_NAME === 'WorkBuddy');
  const agent = env.AGENTRMC_NOTIFY_HOST || (workbuddy ? 'WorkBuddy' : 'ZCode');
  return {
    agent,
    raise: argvRaise || (agent === 'WorkBuddy' ? 'WorkBuddyAI.exe' : ''),
    source: agent === 'WorkBuddy' ? 'workbuddy' : 'zcode',
  };
}

module.exports = { resolveHost, variantFromPath, VARIANTS };
