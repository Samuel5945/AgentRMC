"""只读盘点（二）：ZCode message.data 文本形状 + part 表 + jsonl 样本。"""
import glob
import io
import json
import os
import sqlite3
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
home = os.path.expanduser("~")

print("=== ZCode message.data shapes ===")
zdb = os.path.join(home, ".zcode", "cli", "db", "db.sqlite")
con = sqlite3.connect("file:" + zdb + "?mode=ro", uri=True)
cols = [c[1] for c in con.execute("PRAGMA table_info(part)")]
print("part cols:", cols)
print("part rows:", con.execute("select count(*) from part").fetchone()[0])
prow = con.execute("select * from part order by rowid desc limit 1").fetchone()
if prow:
    for c, v in zip(cols, prow):
        print("  part.", c, "=", str(v)[:300])
# 找一条带正文的 assistant 与一条 user
for role in ("assistant", "user"):
    rows = con.execute(
        "select data from message where data like ? order by rowid desc limit 3",
        ('%"role":"' + role + '"%',)).fetchall()
    for (data,) in rows:
        try:
            o = json.loads(data)
        except Exception:
            continue
        keys = list(o.keys())
        parts = o.get("parts")
        ptypes = [p.get("type") for p in parts][:8] if isinstance(parts, list) else None
        print("msg role=%s keys=%s parts=%s" % (role, keys, ptypes))
        if isinstance(parts, list):
            for p in parts:
                if p.get("type") == "text":
                    print("   text sample:", str(p.get("text"))[:150])
                    break
        break
con.close()

print()
print("=== WorkBuddy AI jsonl ===")
files = glob.glob(os.path.join(home, ".workbuddy-ai", "projects", "*", "*.jsonl"))
files.sort(key=os.path.getmtime, reverse=True)
if files:
    print("newest:", files[0])
    with open(files[0], encoding="utf-8") as f:
        lines = f.readlines()
    print("lines:", len(lines))
    shown = 0
    for ln in lines:
        try:
            o = json.loads(ln)
        except Exception:
            continue
        t = o.get("type")
        if t in ("user", "assistant", "message"):
            role = (o.get("message") or {}).get("role") or o.get("role")
            print("  type=%s role=%s keys=%s ts=%s" % (
                t, role, list(o.keys()),
                o.get("timestamp") or o.get("ts") or o.get("time")))
            shown += 1
            if shown >= 6:
                break

print()
print("=== Qoder CN jsonl ===")
files = glob.glob(os.path.join(home, ".qoder-cn", "projects", "*", "*.jsonl"))
files.sort(key=os.path.getmtime, reverse=True)
if files:
    print("newest:", files[0])
    with open(files[0], encoding="utf-8") as f:
        lines = f.readlines()
    print("lines:", len(lines))
    shown = 0
    for ln in lines:
        try:
            o = json.loads(ln)
        except Exception:
            continue
        t = o.get("type")
        if t in ("user", "assistant"):
            msg = o.get("message") or {}
            content = msg.get("content")
            print("  type=%s keys=%s content=%s ts=%s" % (
                t, list(o.keys()), type(content).__name__,
                o.get("timestamp") or o.get("ts")))
            shown += 1
            if shown >= 4:
                break