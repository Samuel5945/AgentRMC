"""把 release_build 里的新包换进 release/，保留上一版做回滚。

顺序上有两处必须小心：包里有个 models 连接点（junction），任何递归删除都可能
顺着它把真正的 D:\\githubrepos\\Agentrmc\\models 掏空。所以先把连接点单独摘掉、
确认目标目录还在，再动文件夹。
"""
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = Path(__file__).resolve().parent.parent
REL = ROOT / "release"
NEW = REL / "AgentRMC"
PREV = REL / "AgentRMC.prev"
STAGED = ROOT / "release_build" / "AgentRMC"
MODELS = ROOT / "models"


def cmd(*a: str) -> int:
    print("$", " ".join(a), flush=True)
    return subprocess.run(list(a), shell=False).returncode


def models_ok() -> bool:
    """真模型目录必须还在，且非空。"""
    return MODELS.is_dir() and any(MODELS.iterdir())


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()[:12]


assert STAGED.is_dir(), f"没有暂存包 {STAGED}"
assert (STAGED / "AgentRMC.exe").exists(), "暂存包缺 exe"
before = models_ok()
assert before, "models 目录本身就不对，先别动"
live_cfg = sha(NEW / "config.json") if (NEW / "config.json").exists() else None
staged_cfg = sha(STAGED / "config.json")
print("线上 config:", live_cfg, "暂存 config:", staged_cfg)
assert live_cfg == staged_cfg, "暂存包的 config 和线上不一致，别换"

# 1) 先摘掉旧包里的 junction（只删链接，不碰目标）
if (NEW / "models").exists():
    rc = cmd("cmd", "/c", "rmdir", str(NEW / "models"))
    assert rc == 0, "摘 junction 失败"
    assert models_ok(), "!! models 目标被动了，立刻停手"
    print("junction 已摘除，models 目标完好：", sorted(p.name for p in MODELS.iterdir())[:3])

# 2) 清掉更早的那份回滚包（确认里面没有 junction）
if PREV.exists():
    assert not (PREV / "models").exists(), "prev 里还有 junction，手动处理"
    shutil.rmtree(PREV)
    print("旧的 AgentRMC.prev 已删除")

# 3) 现在的 -> prev，暂存 -> release
os.rename(NEW, PREV)
print("旧包 ->", PREV.name)
shutil.move(str(STAGED), str(NEW))
print("新包 ->", NEW)

# 4) 新包重新接上 models junction
rc = cmd("cmd", "/c", "mklink", "/J", str(NEW / "models"), str(MODELS))
assert rc == 0, "建 junction 失败"
assert models_ok() and (NEW / "models").exists(), "junction 没生效"
print("models 链接：", sorted((NEW / "models").iterdir())[0].name, "...")
print("exe mtime:", __import__("time").strftime(
    "%Y-%m-%d %H:%M:%S", __import__("time").localtime((NEW / "AgentRMC.exe").stat().st_mtime)))
print("OK")
