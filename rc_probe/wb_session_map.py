"""只读：扫 WorkBuddy 系 projects/*.jsonl 的 ai-title / 首条 user 消息，
给出 session id -> 标题 -> 项目 的映射（定向派发要靠它找会话行）。

用法: python rc_probe/wb_session_map.py [.workbuddy|.workbuddy-ai] [limit]
"""
import glob
import json
import os
import sys

home = sys.argv[1] if len(sys.argv) > 1 else ".workbuddy"
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 15
base = os.path.join("C:/Users/PC", home, "projects")
rows = []
for p in glob.glob(base + "/*/*.jsonl"):
    if p.endswith(".file-rollback.ndjson"):
        continue
    title, first_user, sid = "", "", os.path.basename(p)[:-6]
    try:
        with open(p, encoding="utf-8") as f:
            for line in f:
                if '"ai-title"' in line:
                    d = json.loads(line)
                    if d.get("type") == "ai-title":
                        title = d.get("aiTitle", "")
                elif not first_user and '"user"' in line:
                    d = json.loads(line)
                    m = d.get("message", {})
                    if d.get("type") == "message" and m.get("role") == "user":
                        c = m.get("content")
                        if isinstance(c, str):
                            first_user = c[:40]
                        elif isinstance(c, list):
                            for part in c:
                                if isinstance(part, dict) and part.get("type") == "text":
                                    first_user = part.get("text", "")[:40]
                                    break
                if title and first_user:
                    break
    except Exception as e:
        print("skip", p, e)
        continue
    rows.append((os.path.getmtime(p), sid, title or first_user,
                 os.path.basename(os.path.dirname(p))))
rows.sort(reverse=True)
for mt, sid, t, proj in rows[:limit]:
    print(f"{sid}  {t!r}  proj={proj}")
