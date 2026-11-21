"""桌面外壳页「停止/插话」判定回归（2026-11-20）。

背景：待办会话页（chat 面）把 chatOpenedId（待办工单 id）当成会话 id 去和宿主
上报的 live.running 比对，永远匹配不上 → 那颗键从不变成「停止生成」，空输入
点下去什么都不发生（用户报「插话在了但停止生成没有」）。派发页（dchat 面）用的
是 row.id（会话 id），所以那边是好的。

这个探针不需要起应用：把 WorkWebView.cpp 里的内嵌 JS 抠出来，只执行判定用的
那几个函数，用假 STATE 断言。改页面逻辑时跑一下，别再把两种 id 搞混。

用法：python rc_probe/dchat_stop_logic_probe.py
"""

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "native" / "src" / "ui" / "WorkWebView.cpp"

# 只抽判定用的小函数：整段脚本在浏览器外跑不起来（顶层就摸 DOM）。
NEEDED = [
    "function liveFor(",
    "function liveIsOpen(",
    "function liveIsRunning(",
    "function chatExe(",
    "function chatSid(",
    "function dchatExe(",
    "function cRunning(",
]

HARNESS = r"""
const STATE = {dispatch: {live: {}}};
let CHAT = null, chatOpenedId = '', DSCHAT = null;

%s

const results = [];
function check(name, ok, extra) {
  results.push({name, ok, extra: extra === undefined ? '' : String(extra)});
}

// 宿主上报：DSH 里 session-A 正在跑，session-B 只开着没跑。
STATE.dispatch.live['DeepSeek Harness.exe'] = {
  open: ['session-A', 'session-B'],
  running: ['session-A'],
};

// —— 待办会话页（chat 面）：工单 id 与会话 id 不同，必须按 sessionId 判 ——
CHAT = {id: 'work-item-1', sessionId: 'session-A', agentExe: 'DeepSeek Harness.exe'};
chatOpenedId = 'work-item-1';
check('chat 面：运行中的会话要判成 running', cRunning('chat') === true, cRunning('chat'));
check('chat 面：不能拿工单 id 去比', chatSid() === 'session-A', chatSid());

// 同一工单但会话没在跑：不能误报
CHAT = {id: 'work-item-2', sessionId: 'session-B', agentExe: 'DeepSeek Harness.exe'};
chatOpenedId = 'work-item-2';
check('chat 面：只开着没跑 → 不是 running', cRunning('chat') === false, cRunning('chat'));

// 老 native 不带 sessionId 时退回工单 id（不做判断，别炸）
CHAT = {id: 'work-item-3', agentExe: 'DeepSeek Harness.exe'};
chatOpenedId = 'work-item-3';
check('chat 面：缺 sessionId 时退回工单 id 不崩', chatSid() === 'work-item-3', chatSid());
check('chat 面：缺 sessionId 时判不出 running', cRunning('chat') === false, cRunning('chat'));

// —— 派发页（dchat 面）：row.id 本来就是会话 id ——
DSCHAT = {agent: {exe: 'DeepSeek Harness.exe'}, row: {id: 'session-A'}};
check('dchat 面：运行中判成 running', cRunning('dchat') === true, cRunning('dchat'));
DSCHAT = {agent: {exe: 'DeepSeek Harness.exe'}, row: {id: 'session-B'}};
check('dchat 面：没在跑不误报', cRunning('dchat') === false, cRunning('dchat'));

// —— 别的宿主：账里没有它 → 不做判断（保持旧行为，别拦人） ——
CHAT = {id: 'w', sessionId: 'session-A', agentExe: 'WorkBuddy.exe'};
check('未知宿主：liveIsOpen 放行', liveIsOpen('WorkBuddy.exe', 'session-A') === true);
check('未知宿主：不判 running', liveIsRunning('WorkBuddy.exe', 'session-A') === false);

// —— 大小写兜底 ——
CHAT = {id: 'w', sessionId: 'session-A', agentExe: 'deepseek harness.exe'};
check('exe 大小写不敏感', cRunning('chat') === true, cRunning('chat'));

console.log(JSON.stringify(results));
"""


def extract_functions(source: str) -> str:
    """按函数名抠出定义（到下一个顶层 function 或空行为止，够用）。"""
    out = []
    for name in NEEDED:
        start = source.find(name)
        if start < 0:
            raise SystemExit("找不到 %s —— 页面结构变了，更新本探针" % name)
        # 从 function 起找到匹配的右大括号
        i = source.find("{", start)
        depth = 0
        end = -1
        for j in range(i, len(source)):
            if source[j] == "{":
                depth += 1
            elif source[j] == "}":
                depth -= 1
                if depth == 0:
                    end = j + 1
                    break
        out.append(source[start:end])
    return "\n\n".join(out)


def main() -> int:
    source = SRC.read_text(encoding="utf-8")
    funcs = extract_functions(source)
    script = HARNESS % funcs
    with tempfile.NamedTemporaryFile(
        "w", suffix=".js", delete=False, encoding="utf-8"
    ) as fh:
        fh.write(script)
        path = fh.name
    try:
        proc = subprocess.run(
            ["node", path], capture_output=True, text=True, encoding="utf-8"
        )
    finally:
        Path(path).unlink(missing_ok=True)
    if proc.returncode != 0:
        print("node 跑挂了：")
        print(proc.stdout)
        print(proc.stderr)
        return 2
    results = json.loads(proc.stdout.strip().splitlines()[-1])
    failed = 0
    for row in results:
        if row["ok"]:
            print("PASS %s" % row["name"])
        else:
            failed += 1
            print("FAIL %s :: %s" % (row["name"], row["extra"]))
    print()
    if failed:
        print("%d FAILED of %d" % (failed, len(results)))
        return 1
    print("ALL PASS (%d checks)" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
