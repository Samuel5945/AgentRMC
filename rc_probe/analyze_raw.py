"""分析 raw_reports.jsonl：判断原始 HID 报告里有没有音频流。

用法: python rc_probe/analyze_raw.py
"""
import json
from collections import Counter
from pathlib import Path

P = Path(__file__).resolve().parent / "raw_reports.jsonl"

if not P.exists():
    raise SystemExit(f"没有 {P}，先跑 tools\\probe_admin.bat")

events = [json.loads(l) for l in P.read_text(encoding="utf-8").splitlines() if l.strip()]
meta = [e for e in events if "event" in e]
reports = [e for e in events if "hex" in e]

print("=== 状态事件 ===")
for m in meta:
    print("  ", m)

if not reports:
    print("\n[!] 没有任何报告")
    raise SystemExit(0)

print(f"\n=== 报告统计 ===")
print(f"  总数: {len(reports)}")
lens = Counter(r["n"] for r in reports)
print(f"  长度分布: {dict(lens)}")
total = sum(r["n"] for r in reports)
print(f"  总字节: {total}")

ts = [r["t"] for r in reports]
if len(ts) > 1:
    span = ts[-1] - ts[0]
    print(f"  时间跨度: {span:.1f}s   平均速率: {len(reports)/span:.1f} 包/秒")

# 按包间隔切分成"段"，密集段可能是音频
print(f"\n=== 密集段（间隔 < 60ms 的连续包）===")
segments = []
cur = [reports[0]]
for prev, nxt in zip(reports, reports[1:]):
    if nxt["t"] - prev["t"] < 0.06:
        cur.append(nxt)
    else:
        segments.append(cur)
        cur = [nxt]
segments.append(cur)

big = [s for s in segments if len(s) >= 5]
if not big:
    print("  没有密集段")
for s in big:
    dur = s[-1]["t"] - s[0]["t"]
    rate = len(s) / dur if dur > 0 else 0
    print(f"  {s[0]['t']:.2f}s ~ {s[-1]['t']:.2f}s  {len(s)} 包  "
          f"{dur:.2f}s  {rate:.0f} 包/秒  avg_len={sum(x['n'] for x in s)/len(s):.0f}")

# 每段首个包的十六进制，看有没有规律
print(f"\n=== 前 12 个报告（前 32 字节）===")
for r in reports[:12]:
    print(f"  {r['t']:8.3f} n={r['n']:<4} {r['hex'][:95]}")

# 检查是否有包内容高度相似（按键包）还是持续变化（音频包）
print(f"\n=== 内容多样性 ===")
uniq = len({r["hex"] for r in reports})
print(f"  唯一内容 {uniq} / {len(reports)}  ({uniq/len(reports)*100:.0f}%)")
if uniq / len(reports) > 0.7:
    print("  → 内容高度变化，像音频或传感器流")
else:
    print("  → 内容重复度高，像按键/状态包")
