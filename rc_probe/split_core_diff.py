"""把 agentrc.py 的 diff 按 hunk 范围切两份，方便分两个提交。

中间那段（resolve_profiles / VK_LABEL / describe_action / KeyRouter）是双击长按；
开头的 save_voice_config 和结尾的 Transcriber 属于本地模型切换。
"""
import re
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
d = subprocess.run(["git", "diff", "agentrc.py"], capture_output=True, text=True,
                   encoding="utf-8").stdout
lines = d.splitlines(keepends=True)
head = []
hunks = []          # (start_line_in_old, [lines...])
cur = None
for ln in lines:
    m = re.match(r"^@@ -(\d+)", ln)
    if m:
        cur = (int(m.group(1)), [ln])
        hunks.append(cur)
    elif cur is None:
        head.append(ln)
    else:
        cur[1].append(ln)

print("hunk old-starts:", [h[0] for h in hunks])

# 分界：>=800 且 <1700 的是手势映射；其余是模型切换
core = [h for h in hunks if 800 <= h[0] < 1700]
voice = [h for h in hunks if not (800 <= h[0] < 1700)]
print("core hunks:", len(core), "voice hunks:", len(voice))
Path("rc_probe/patch_core.diff").write_text(
    "".join(head) + "".join("".join(h[1]) for h in core),
    encoding="utf-8", newline="")
Path("rc_probe/patch_voice.diff").write_text(
    "".join(head) + "".join("".join(h[1]) for h in voice),
    encoding="utf-8", newline="")
