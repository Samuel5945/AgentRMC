"""Qoder 系「按会话发送 + 回执」能力验证（CLI 通道，全隔离，零副作用）。

上一轮的教训：--tools "" 只禁模型工具，**插件 hook 照样跑**——探针会话的 Stop
经 agentrmc-bridge 落进了真实待办库（还可能喊人/抢前台）。所以这轮：
  * --settings '{"disableAllHooks":true}' 关全部 hook（含插件 hook）；
  * --strict-mcp-config + 空 mcpServers 关 MCP（含 agentrmc 常驻 server）；
  * cwd 用临时目录：会话落在临时项目 slug 下，不污染真实项目的会话列表；
  * 清掉 QODER_AGENT_SDK_*（否则 CLI 报 sdk_invalid_args 拒启动）；
  * 跑前后对真实 work-items.json 做 sha1+mtime 断言，证明 hook 真的没响；
  * 结束删掉本轮自建的探针会话文件（只删 sid 对应的，不碰别的）。
  注：--config-dir 临时配置家会丢登录态（rc=42 静默退出），不能用来隔离。
验证链：--session-id <新uuid> -p 发探针1 -> jsonl 回执；--resume <id> -p 问上一
条标记 -> 答对证明载入的是该会话；回执2 同文件；其它会话零泄漏。
用法: python rc_probe/qoder_send_receipt.py [cn|intl]
"""
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import uuid

variant = sys.argv[1] if len(sys.argv) > 1 else "cn"
home = ".qoder-cn" if variant == "cn" else ".qoder"
cli = os.path.join("C:/Users/PC", home, "bin",
                   "qoderclicn" if variant == "cn" else "qodercli",
                   "qoderclicn.exe" if variant == "cn" else "qodercli.exe")
real_base = os.path.join("C:/Users/PC", home, "projects")
real_store = "C:/Users/PC/AppData/Roaming/LIG/AgentRMCNative/work-items.json"

cwd = tempfile.mkdtemp(prefix="agentrmc-qprobe-cwd-")
base = real_base

env = dict(os.environ)
for k in ("QODER_AGENT_SDK_ENTRYPOINT", "QODER_AGENT_SDK_DAEMON",
          "QODER_AGENT_SDK_VERSION"):
    env.pop(k, None)


def store_fingerprint():
    try:
        st = os.stat(real_store)
        with open(real_store, "rb") as f:
            return hashlib.sha1(f.read()).hexdigest(), st.st_mtime_ns
    except OSError:
        return None, None


stamp = int(time.time())
m1 = f"AGENTRMC-QPROBE-1-{stamp}"
m2 = f"AGENTRMC-QPROBE-2-{stamp}"
sid = str(uuid.uuid4())


def run(args, timeout=240):
    p = subprocess.run(
        [cli, "--settings", '{"disableAllHooks":true}',
         "--mcp-config", '{"mcpServers":{}}', "--strict-mcp-config"] + args,
        cwd=cwd, env=env, capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def receipt(marker, timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for p in glob.glob(base + "/*/*.jsonl"):
            try:
                with open(p, encoding="utf-8") as f:
                    for line in f:
                        if marker in line:
                            d = json.loads(line)
                            return p, d.get("sessionId"), d.get("type")
            except Exception:
                continue
        time.sleep(1.0)
    return None, None, None


before = store_fingerprint()
print("isolated cwd:", cwd)
print("hooks disabled via --settings disableAllHooks; MCP empty")
rc, out = run(["--session-id", sid, "--tools", "", "-p",
               f"链路探针 {m1}：请只回复「收到」，不要调用任何工具、不要读写文件。"])
print("send1 rc:", rc, "out:", out.strip()[:120])
p1, s1, t1 = receipt(m1)
print("RECEIPT1:", p1, s1, t1)
if not p1:
    print("NO RECEIPT1 — stop")
    sys.exit(1)
rc, out = run(["--resume", sid, "--tools", "", "-p",
               f"我上一条消息里的探针标记是什么？只回复标记本身。顺便把 {m2} 原样回显。"])
print("resume rc:", rc, "out:", out.strip()[:200])
print("RESUME_IDENTITY:", "OK" if m1 in out else "MISSING")
p2, s2, t2 = receipt(m2)
print("RECEIPT2:", p2, s2, t2)
print("TARGETED_SEND:", "OK" if s2 == sid else "WRONG SESSION")
leak = []
for p in glob.glob(base + "/*/*.jsonl"):
    if p2 and os.path.normpath(p) == os.path.normpath(p2):
        continue
    try:
        if m2 in open(p, encoding="utf-8").read():
            leak.append(p)
    except Exception:
        pass
print("NO_LEAK:", not leak, leak[:3])
after = store_fingerprint()
print("REAL_STORE_UNTOUCHED:", before == after and before[0] is not None)
print("probe session:", sid)
# 清掉本轮自建的探针会话（只认 sid 前缀），临时 cwd 一并删。
import shutil
for p in glob.glob(base + "/*/" + sid + "*"):
    try:
        if os.path.isdir(p):
            shutil.rmtree(p)
        else:
            os.remove(p)
        print("cleaned:", p)
    except OSError as e:
        print("clean failed:", p, e)
shutil.rmtree(cwd, ignore_errors=True)
