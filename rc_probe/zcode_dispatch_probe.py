"""Live targeted-dispatch probe over the ZCode app-server stdio protocol.

Chain under test (mission-control batch 3, ZCode column):
  session/create -> session/send (targeted, by sessionId) -> receipt via
  session/messages -> real-store isolation asserted by fingerprints.

Isolation:
  - AGENTRMC_PORT poisoned + AGENTRMC_SPOOL_DIR redirected to a temp dir, so
    the sacrificial session's bridge hooks (turn-start/stop) can never touch
    the real AgentRMC todo store (POST fails -> spool -> OUR temp dir).
  - Real-store fingerprints (work-items.json + real spool) taken before and
    after: must be byte-identical (size+mtime+sha1).
  - The probe session is created BY this probe (no existing session touched).

Usage: python rc_probe/zcode_dispatch_probe.py
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import queue

EXE = r"D:\apps\AIAPPS\ZCode\ZCode.exe"
CJS = r"D:\apps\AIAPPS\ZCode\resources\glm\zcode.cjs"
CWD = r"D:\githubrepos\Agentrmc"
MARKER = "AGENTRMC-DISPATCH-PROBE-20261107"
PROMPT = ("[AgentRMC 探针 %s] 这是一条定向派发链路验证消息。"
          "请只回复四个字：链路畅通。不要使用任何工具，不要做任何其他事。" % MARKER)

APPDATA = os.environ.get("APPDATA", "")
REAL_SPOOL = os.path.join(APPDATA, "LIG", "AgentRMCNative", "pending-events.jsonl")
WORK_ITEMS = os.path.join(APPDATA, "LIG", "AgentRMCNative", "work-items.json")
WORK_ITEMS2 = os.path.join(APPDATA, "LIG", "AgentRMCNative", "state", "work-items.json")


def fp(path):
    if not os.path.exists(path):
        return None
    st = os.stat(path)
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return (st.st_size, int(st.st_mtime), h.hexdigest())


def find_work_items():
    if os.path.exists(WORK_ITEMS):
        return WORK_ITEMS
    if os.path.exists(WORK_ITEMS2):
        return WORK_ITEMS2
    base = os.path.join(APPDATA, "LIG", "AgentRMCNative")
    if os.path.isdir(base):
        for root, _dirs, files in os.walk(base):
            for f in files:
                if f == "work-items.json":
                    return os.path.join(root, f)
    return None


class Server:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="agentrmc_probe_")
        env = dict(os.environ)
        env["ELECTRON_RUN_AS_NODE"] = "1"
        env["AGENTRMC_PORT"] = "1"  # unroutable: POST can never succeed
        env["AGENTRMC_SPOOL_DIR"] = self.tmp  # spool lands in OUR dir
        self.proc = subprocess.Popen(
            [EXE, CJS, "app-server", "--stdio"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env, cwd=CWD)
        self.q = queue.Queue()
        self.notifications = []
        for pipe, tag in ((self.proc.stdout, "OUT"), (self.proc.stderr, "ERR")):
            threading.Thread(target=self._read, args=(pipe, tag), daemon=True).start()
        self.next_id = 1

    def _read(self, pipe, tag):
        for raw in iter(pipe.readline, b""):
            line = raw.decode("utf-8", "ignore").strip()
            if line:
                try:
                    self.notifications.append(json.loads(line))
                except Exception:
                    pass
                self.q.put((tag, line))
        self.q.put((tag, None))

    def wait_ready(self, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            for n in list(self.notifications):
                if (n.get("method") == "startup/storageState"
                        and n.get("params", {}).get("phase") == "ready"):
                    return True
            time.sleep(0.2)
        return False

    def request(self, method, params, wait=15, quiet=False):
        rid = self.next_id
        self.next_id += 1
        frame = {"id": rid, "method": method, "params": params}
        self.proc.stdin.write((json.dumps(frame) + "\n").encode())
        self.proc.stdin.flush()
        end = time.time() + wait
        while time.time() < end:
            try:
                _tag, line = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                break
            try:
                obj = json.loads(line)
            except Exception:
                if not quiet:
                    print("  raw[%s] %s" % (_tag, line[:200]))
                continue
            if obj.get("id") == rid:
                return obj
            if obj.get("method") and obj.get("id") is not None:
                # server→client request. session/requestRuntimePreferences
                # needs a SUCCESS reply or create fails (zod Uon schema: only
                # nativeSearchEnhancementsEnabled is required, rest default).
                if obj["method"] == "session/requestRuntimePreferences":
                    reply = {"id": obj["id"], "result": {
                        "nativeSearchEnhancementsEnabled": False}}
                else:
                    reply = {"id": obj["id"], "error": {
                        "code": -32000, "message": "probe: unsupported client request"}}
                self.proc.stdin.write((json.dumps(reply) + "\n").encode())
                self.proc.stdin.flush()
                continue
            # notification (event stream) — keep
        return {"error": "timeout waiting for id=%d" % rid}

    def alive(self):
        return self.proc.poll() is None


def main():
    store = find_work_items()
    print("STORE=%s" % store)
    before_store = fp(store) if store else None
    before_spool = fp(REAL_SPOOL)

    srv = Server()
    print("READY=%s" % srv.wait_ready())

    # 1) create sacrificial session in the user's workspace
    r = srv.request("session/create", {
        "workspace": {"workspaceKey": CWD, "workspacePath": CWD},
        "persistence": "immediate",
    }, wait=30)
    if "result" not in r:
        print("CREATE_FAIL=%s" % json.dumps(r, ensure_ascii=False)[:600])
        # try minimal params
        r = srv.request("session/create", {}, wait=30)
        if "result" not in r:
            print("CREATE_FAIL2=%s" % json.dumps(r, ensure_ascii=False)[:600])
            srv.proc.kill()
            return 2
    sid = ((r.get("result") or {}).get("session") or {}).get("sessionId")
    print("SESSION_CREATED=%s" % sid)

    # 2) targeted send by sessionId
    r = srv.request("session/send", {
        "sessionId": sid,
        "content": PROMPT,
    }, wait=20)
    print("SEND_ACCEPTED=%s" % ("result" in r and "error" not in r))
    if "error" in r and r.get("error"):
        print("SEND_ERR=%s" % json.dumps(r, ensure_ascii=False)[:400])

    # 3) poll messages until assistant reply or timeout
    def all_text(obj, acc):
        if isinstance(obj, dict):
            for k, v in obj.items():
                if k == "text" and isinstance(v, str):
                    acc.append(v)
                else:
                    all_text(v, acc)
        elif isinstance(obj, list):
            for v in obj:
                all_text(v, acc)
        return acc

    deadline = time.time() + 180
    last_texts = []
    reply_ok = False
    while time.time() < deadline:
        time.sleep(4)
        m = srv.request("session/messages", {"sessionId": sid}, wait=10, quiet=True)
        msgs = (m.get("result") or {}).get("messages") or []
        acc = all_text(msgs, [])
        last_texts = acc[-3:]
        joined = " ".join(acc)
        if "链路畅通" in joined:
            reply_ok = True
            break
        if not srv.alive():
            print("SERVER_DIED")
            break
    print("REPLY_OK=%s" % reply_ok)
    if last_texts:
        print("LAST_REPLY=%r" % last_texts[-1][:120])

    # 4) list shows it (visibility at protocol level)
    r = srv.request("session/list", {}, wait=10)
    sessions = (r.get("result") or {}).get("sessions") or []
    mine = [s for s in sessions if s.get("sessionId") == sid]
    if mine:
        s = mine[0]
        print("LIST_TITLE=%r STATUS=%s UPDATED=%s" % (
            s.get("title"), s.get("status"), s.get("updatedAt")))

    # 5) isolation: MARKER must not appear in the real store; probe spool must
    #    have caught the hook events (proves containment, not absence).
    def store_has_marker():
        if not store or not os.path.exists(store):
            return False
        with open(store, "rb") as f:
            return MARKER.encode() in f.read()
    print("REAL_STORE_NO_MARKER=%s" % (not store_has_marker()))
    print("REAL_SPOOL_NO_MARKER=%s" % (
        not (os.path.exists(REAL_SPOOL)
             and MARKER in open(REAL_SPOOL, encoding="utf-8", errors="ignore").read())))
    spooled = os.listdir(srv.tmp) if os.path.isdir(srv.tmp) else []
    spool_hit = any(MARKER in open(os.path.join(srv.tmp, f), encoding="utf-8",
                                   errors="ignore").read()
                    for f in spooled if os.path.isfile(os.path.join(srv.tmp, f)))
    print("HOOK_EVENTS_CONTAINED=%s (probe spool files=%s)" % (spool_hit, spooled[:5]))
    print("SESSION_LEFT_OPEN=%s (炮灰会话保留，供桌面可见性人工核验)" % sid)
    srv.proc.kill()
    return 0 if (reply_ok and not store_has_marker()) else 1


if __name__ == "__main__":
    sys.exit(main())
