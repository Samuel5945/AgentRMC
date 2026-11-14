"""手机传文件端到端验收：配对一个临时设备 → POST /m/upload 传文件 → 校验落盘 → 吊销。
会短暂把 AgentRMC 主窗拉前台取配对码（lan_pair_code.py）。验完吊销临时设备。
用法：python rc_probe/lan_upload_e2e.py
"""
import json
import os
import pathlib
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent
BASE = "http://127.0.0.1:45916"
FAILS = []
# 本机回环绕过系统代理（否则 127.0.0.1 也被代理拦）。
urllib.request.install_opener(
    urllib.request.build_opener(urllib.request.ProxyHandler({})))


def check(cond, what, extra=""):
    print(("PASS " if cond else "FAIL ") + what + (f" | {extra}" if extra else ""))
    if not cond:
        FAILS.append(what)


def http(method, path, body=None, headers=None, token=None):
    h = dict(headers or {})
    if token:
        h["Cookie"] = "armc_dev=" + token
    data = body if isinstance(body, bytes) else (
        json.dumps(body).encode() if body is not None else None)
    if data is not None and "Content-Type" not in h:
        h["Content-Type"] = "application/json"
    req = urllib.request.Request(BASE + path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


# 1. 取配对码（短暂拉前台）。
out = subprocess.run([sys.executable, str(ROOT / "lan_pair_code.py")],
                     capture_output=True, text=True, encoding="utf-8",
                     errors="replace").stdout
code = ""
for line in out.splitlines():
    if line.startswith("CODE="):
        code = line[5:].strip()
check(bool(code), "got pairing code", code[:40])
if not code:
    sys.exit(1)

# 2. 配对换令牌。
st, body, hdr = http("POST", "/m/pair", {"code": code, "name": "upload-e2e"},
                     {"X-Requested-With": "fetch"})
token = ""
sc = hdr.get("Set-Cookie", "")
if "armc_dev=" in sc:
    token = sc.split("armc_dev=")[1].split(";")[0]
check(st == 200 and bool(token), "pair 200 + token", f"status={st}")
if not token:
    sys.exit(1)

# 3. 拿一个待办 id。
st, body, _ = http("GET", "/m/state", token=token)
items = (json.loads(body).get("work", {}).get("items", []))
items = [i for i in items if not i.get("done")]
check(bool(items), "has a work item")
if not items:
    sys.exit(1)
wid = items[0]["id"]

# 4. 上传一个测试文件。
payload = "AgentRMC 上传端到端测试 UPLOAD-E2E-7Q9X".encode("utf-8")
st, body, _ = http("POST", "/m/upload?id=" + wid, payload,
                   {"X-Requested-With": "fetch",
                    "X-AgentRMC-Filename": "e2e-test.txt",
                    "Content-Type": "application/octet-stream"}, token=token)
j = json.loads(body) if body else {}
check(st == 200 and j.get("ok"), "upload 200", f"status={st} body={body[:120]}")
path = j.get("path", "")
check(bool(path), "upload returned path", path)

# 5. 校验落盘内容。
if path:
    p = pathlib.Path(path)
    check(p.exists(), "file exists on disk", path)
    if p.exists():
        check(p.read_bytes() == payload, "file content intact")
        check(".agentrmc" in path or "uploads" in path, "landed in inbox/uploads",
              path)

# 6. 吊销临时设备（清理）。
st, body, _ = http("GET", "/m/state", token=token)
# 设备列表在桌面侧；这里用 /m/action 没有 revoke 白名单，改走配对设备自然过期即可。
# 为不留痕，尝试读设备 id 并经桌面吊销不可行（revoke 是桌面专用）——留待 TTL 过期。
print("NOTE: 临时设备 upload-e2e 留待配对设备管理吊销（revoke 为桌面专用动作）")

print()
if FAILS:
    print("E2E FAIL:", FAILS)
    sys.exit(1)
print("E2E PASS lan-upload")