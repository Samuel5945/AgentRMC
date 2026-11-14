"""只读盘点 DSH session.v4.jsonl.zstd 的事件结构，给聊天页读取器定方案。
多帧拼接：必须 stream_reader(read_across_frames=True)。不改任何库。
用法：python dsh_transcript_schema.py [session-id]
"""
import glob
import json
import os
import sys
from collections import Counter

import zstandard

home = os.path.expanduser("~")
root = os.path.join(home, ".dsh", "sessions")
sid = sys.argv[1] if len(sys.argv) > 1 else None
files = glob.glob(os.path.join(root, "*", "session-*", "*.zstd"))
if sid:
    files = [f for f in files if sid in f]
files.sort(key=os.path.getmtime, reverse=True)
if not files:
    print("NO_FILES")
    raise SystemExit(1)
f = files[0]
print("FILE:", f)
dctx = zstandard.ZstdDecompressor()
with open(f, "rb") as fh, dctx.stream_reader(fh, read_across_frames=True) as r:
    text = r.read().decode("utf-8", "replace")
lines = text.splitlines()
print("LINES:", len(lines))
types = Counter()
samples = {}
for ln in lines:
    try:
        o = json.loads(ln)
    except Exception:
        continue
    t = o.get("type") or o.get("kind") or "?"
    types[t] += 1
    if t not in samples:
        samples[t] = o
print("=== TYPE COUNTS ===")
for t, n in types.most_common():
    print("  %-32s %d" % (t, n))
print("=== SAMPLE KEYS PER TYPE ===")
for t, o in samples.items():
    print("  %-32s keys=%s" % (t, list(o.keys())))
# 重点看 user / assistant 消息的完整结构
for want in ("user/message", "assistant/message", "user", "assistant", "message"):
    if want in samples:
        print("=== SAMPLE %s ===" % want)
        print(json.dumps(samples[want], ensure_ascii=False)[:1500])