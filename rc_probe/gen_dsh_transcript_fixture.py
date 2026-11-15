"""生成 DSH 聊天页读取器的测试 fixture（多帧 zstd）。
native 只 vendored 解压子集，压缩靠这里（Python zstandard）预生成入库。
产出 native/tests/fixtures/dsh_session.zstd（帧1+帧2）与 dsh_append.zstd（帧3）。
用法：python rc_probe/gen_dsh_transcript_fixture.py
"""
import json
import os
import pathlib

import zstandard

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "native" / "tests" / "fixtures"
OUT.mkdir(parents=True, exist_ok=True)
BASE = 1794569091773
SID = "session-11111111-2222-4333-8444-555555555555"


def ev(type_, seq, dt, data, surface=False):
    o = {"type": type_, "seq": seq, "time": BASE + dt, "data": data}
    if surface:
        o["surfaceOp"] = "append"
    return json.dumps(o, ensure_ascii=False)


def user(seq, dt, mid, text):
    return ev("user/message", seq, dt,
              {"role": "user", "id": mid,
               "content": [{"type": "text", "text": text}],
               "source": {"kind": "user"}}, surface=True)


def assistant(seq, dt, mid, parts):
    return ev("assistant/message", seq, dt,
              {"turn": 1, "step": 1,
               "message": {"role": "assistant", "id": mid, "content": parts}},
              surface=True)


frame1 = [
    ev("session", 1, 0, {"version": 4, "id": SID, "createdAt": BASE, "cwd": "D:/x"}),
    user(10, 0, "u1", "第一句"),
    assistant(20, 1000, "a1", [
        {"type": "reasoning", "text": "思考过程不进聊天页"},
        {"type": "text", "text": "回复一"},
        {"type": "tool-call", "name": "read"},
    ]),
    ev("session/title", 21, 1500, {"title": "DSH 测试标题"}),
]
frame2 = [
    user(30, 2000, "u2", "第二句"),
    # 系统注入（source.kind=time-context）：应被过滤，不当「我」的气泡显示
    ev("user/message", 35, 2500,
       {"role": "user", "id": "sys1",
        "content": [{"type": "text", "text": "Time sampled while preparing turn 2"}],
        "source": {"kind": "time-context"}}, surface=True),
    # 纯工具调用 assistant 消息：无 text part → 应被跳过
    assistant(40, 3000, "a2", [{"type": "tool-call", "name": "write"}]),
    assistant(50, 4000, "a3", [{"type": "text", "text": "回复二"}]),
]
frame3 = [user(60, 5000, "u3", "追加句")]

cctx = zstandard.ZstdCompressor()


def frame_bytes(events):
    payload = ("\n".join(events) + "\n").encode("utf-8")
    return cctx.compress(payload)


# 多帧拼接：帧1+帧2 一个文件，帧3 单独（测试追加用）。
(OUT / "dsh_session.zstd").write_bytes(frame_bytes(frame1) + frame_bytes(frame2))
(OUT / "dsh_append.zstd").write_bytes(frame_bytes(frame3))
print("WROTE", OUT / "dsh_session.zstd", (OUT / "dsh_session.zstd").stat().st_size)
print("WROTE", OUT / "dsh_append.zstd", (OUT / "dsh_append.zstd").stat().st_size)
print("SID", SID)