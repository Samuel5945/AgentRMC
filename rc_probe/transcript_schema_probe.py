"""只读盘点各宿主会话库结构，给聊天页读取器定方案（不改任何库）。"""
import glob
import json
import os
import sqlite3

home = os.path.expanduser("~")

print("=== ZCode ===")
zdb = os.path.join(home, ".zcode", "cli", "db", "db.sqlite")
print("db:", zdb, os.path.exists(zdb))
if os.path.exists(zdb):
    con = sqlite3.connect("file:" + zdb + "?mode=ro", uri=True)
    tabs = [r[0] for r in con.execute(
        "select name from sqlite_master where type='table'")]
    print("tables:", tabs)
    for t in ("message", "session", "session_entry"):
        if t in tabs:
            cols = con.execute("PRAGMA table_info(%s)" % t).fetchall()
            print(t, "cols:", [(c[1], c[2]) for c in cols])
            print(t, "rows:", con.execute(
                "select count(*) from %s" % t).fetchone()[0])
    if "message" in tabs:
        cols = [c[1] for c in con.execute("PRAGMA table_info(message)")]
        row = con.execute(
            "select * from message order by rowid desc limit 1").fetchone()
        if row:
            for c, v in zip(cols, row):
                print("  ", c, "=", str(v)[:400])
    con.close()

print()
print("=== OpenSquilla ===")
candidates = glob.glob(os.path.join(
    os.environ.get("APPDATA", ""), "@opensquilla", "*", "opensquilla",
    "state", "sessions.db"))
print("candidates:", candidates)
for db in candidates[:1]:
    con = sqlite3.connect("file:" + db + "?mode=ro", uri=True)
    tabs = [r[0] for r in con.execute(
        "select name from sqlite_master where type='table'")]
    print("tables:", tabs)
    for t in ("transcript_entries", "sessions"):
        if t in tabs:
            cols = con.execute("PRAGMA table_info(%s)" % t).fetchall()
            print(t, "cols:", [(c[1], c[2]) for c in cols])
    if "transcript_entries" in tabs:
        cols = [c[1] for c in con.execute(
            "PRAGMA table_info(transcript_entries)")]
        row = con.execute(
            "select * from transcript_entries order by rowid desc limit 1"
        ).fetchone()
        if row:
            for c, v in zip(cols, row):
                print("  ", c, "=", str(v)[:400])
    con.close()

print()
print("=== WorkBuddy AI jsonl sample ===")
files = glob.glob(os.path.join(home, ".workbuddy-ai", "projects", "*", "*.jsonl"))
files.sort(key=os.path.getmtime, reverse=True)
print(" newest:", files[0] if files else None)
if files:
    with open(files[0], encoding="utf-8") as f:
        lines = f.readlines()
    print(" lines:", len(lines))
    shown = 0
    for ln in lines:
        try:
            o = json.loads(ln)
        except Exception:
            continue
        t = o.get("type")
        if t in ("user", "assistant", "message"):
            role = (o.get("message") or {}).get("role") or o.get("role")
            keys = list(o.keys())
            print("  type=%s role=%s keys=%s ts=%s" % (
                t, role, keys, o.get("timestamp") or o.get("ts") or o.get("time")))
            shown += 1
            if shown >= 6:
                break

print()
print("=== Qoder CN jsonl sample ===")
files = glob.glob(os.path.join(home, ".qoder-cn", "projects", "*", "*.jsonl"))
files.sort(key=os.path.getmtime, reverse=True)
print(" newest:", files[0] if files else None)
if files:
    with open(files[0], encoding="utf-8") as f:
        lines = f.readlines()
    print(" lines:", len(lines))
    shown = 0
    for ln in lines:
        try:
            o = json.loads(ln)
        except Exception:
            continue
        t = o.get("type")
        if t in ("user", "assistant"):
            keys = list(o.keys())
            msg = o.get("message") or {}
            content = msg.get("content")
            ctype = type(content).__name__
            print("  type=%s keys=%s content=%s ts=%s" % (
                t, keys, ctype, o.get("timestamp") or o.get("ts")))
            shown += 1
            if shown >= 4:
                break