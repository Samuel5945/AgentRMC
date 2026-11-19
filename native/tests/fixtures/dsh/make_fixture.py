"""生成 DSH 会话测试夹具（vendored zstd 只带解压，测试里没法压缩）。"""
import json
import os
import zstandard

root = r'D:\githubrepos\Agentrmc\native\tests\fixtures\dsh\sessions\slug-x\session-abc123'
os.makedirs(root, exist_ok=True)
base = 1794569000000


def ev(t, ts, data):
    return json.dumps({"type": t, "time": ts, "data": data},
                      ensure_ascii=False, separators=(',', ':'))


cid = "call_1"
lines = [
    ev("user/message", base, {"content": "跑个构建", "source": {"kind": "user"}}),
    ev("tool/call", base + 1000,
       {"callId": cid, "name": "pwsh",
        "arguments": json.dumps({"command": "npm run build",
                                 "description": "构建前端"}, ensure_ascii=False)}),
    ev("tool/result", base + 6000, {"message": {"toolCallId": cid}}),
    ev("assistant/message", base + 7000,
       {"message": {"content": [{"type": "text", "text": "构建好了"}]}}),
]
plain = ("\n".join(lines) + "\n").encode("utf-8")
packed = zstandard.ZstdCompressor(level=3).compress(plain)
p = os.path.join(root, "session.v4.jsonl.zstd")
with open(p, "wb") as f:
    f.write(packed)
print(p, len(packed), "bytes")
