"""run 动作不再闪 cmd.exe 控制台（2026-10-30 现场回归）。

AgentRMC 是 GUI 子系统进程，`Popen(..., shell=True)` 不带 CREATE_NO_WINDOW
时 Windows 会为 cmd.exe 新建一个控制台窗口，每按一次「启动程序」黑框闪一下。

    .venv\\Scripts\\python.exe tests\\test_no_console_run.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import agentrc  # noqa: E402


def test_popen_shell_hides_console() -> None:
    calls = []
    orig = agentrc.subprocess.Popen

    class Dummy:
        pass

    def fake(cmd, **kw):
        calls.append((cmd, kw))
        return Dummy()

    agentrc.subprocess.Popen = fake
    try:
        agentrc._popen_shell("notepad")
    finally:
        agentrc.subprocess.Popen = orig
    assert len(calls) == 1, calls
    cmd, kw = calls[0]
    assert cmd == "notepad"
    assert kw.get("shell") is True, kw
    if sys.platform == "win32":
        assert kw.get("creationflags") == 0x08000000, kw
    print("PASS run 动作用 CREATE_NO_WINDOW 起 shell")


def test_rule_engine_uses_helper() -> None:
    calls = []
    orig = agentrc._popen_shell
    agentrc._popen_shell = lambda cmd: calls.append(cmd)
    try:
        eng = agentrc.RuleEngine([
            {"match": ["打开记事本"],
             "action": {"type": "run", "cmd": "notepad"}},
        ])
        assert eng.dispatch("打开记事本") is True
    finally:
        agentrc._popen_shell = orig
    assert calls == ["notepad"], calls
    print("PASS 语音指令 run 走静默出口")


def test_no_raw_shell_popen_left() -> None:
    src = (ROOT / "agentrc.py").read_text(encoding="utf-8")
    assert 'subprocess.Popen(action["cmd"], shell=True)' not in src
    assert src.count('_popen_shell(action["cmd"])') == 2
    print("PASS 两条 run 分支都接到静默出口")


if __name__ == "__main__":
    test_popen_shell_hides_console()
    test_rule_engine_uses_helper()
    test_no_raw_shell_popen_left()
    print("\n全部通过")
