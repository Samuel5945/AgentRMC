"""Probe the ZCode app-server stdio protocol (read-only discovery).

Spawns: ZCode.exe resources/glm/zcode.cjs app-server --stdio
(ELECTRON_RUN_AS_NODE=1), watches stdout for a greeting, then tries a few
handshake / list frames. NOTHING is sent to any real session; no session/send
in this probe. Prints every inbound line (truncated).

Usage: python rc_probe/zcode_appserver_probe.py [--probe-list=1]
"""
import json
import os
import subprocess
import sys
import threading
import time
import queue

EXE = r"D:\apps\AIAPPS\ZCode\ZCode.exe"
CJS = r"D:\apps\AIAPPS\ZCode\resources\glm\zcode.cjs"
CWD = r"D:\githubrepos\Agentrmc"


def reader(pipe, q, tag):
    for raw in iter(pipe.readline, b""):
        line = raw.decode("utf-8", "ignore").rstrip()
        if line:
            q.put((tag, line))
    q.put((tag, None))


def main():
    probe_list = "--probe-list=0" not in sys.argv
    env = dict(os.environ)
    env["ELECTRON_RUN_AS_NODE"] = "1"
    proc = subprocess.Popen(
        [EXE, CJS, "app-server", "--stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=env, cwd=CWD)
    q = queue.Queue()
    threads = [
        threading.Thread(target=reader, args=(proc.stdout, q, "OUT"), daemon=True),
        threading.Thread(target=reader, args=(proc.stderr, q, "ERR"), daemon=True),
    ]
    for t in threads:
        t.start()

    got_greeting = False
    deadline = time.time() + 6
    while time.time() < deadline:
        try:
            tag, line = q.get(timeout=0.5)
        except queue.Empty:
            continue
        if line is None:
            print("[%s STREAM END]" % tag)
            break
        print("[%s] %s" % (tag, line[:300]))
        got_greeting = True
    if not got_greeting:
        print("(no output within 6s — server may be waiting for input)")

    if proc.poll() is not None:
        print("EXITED rc=%s" % proc.returncode)
        return 1

    def send(obj, wait=3.0, mark=""):
        data = (json.dumps(obj) + "\n").encode()
        try:
            proc.stdin.write(data)
            proc.stdin.flush()
        except Exception as e:
            print("SEND FAILED: %s" % e)
            return
        print(">>> %s %s" % (mark, json.dumps(obj)[:200]))
        end = time.time() + wait
        while time.time() < end:
            try:
                tag, line = q.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                print("[%s STREAM END]" % tag)
                return
            print("[%s] %s" % (tag, line[:400]))

    if probe_list:
        rid = 1
        for frame in (
            {"method": "startup/storagePath", "params": {"path": r"C:\Users\PC\.zcode\cli"}},
            {"type": "request", "id": rid, "method": "session/list", "params": {}},
            {"id": rid, "method": "session/list", "params": {}},
            {"jsonrpc": "2.0", "id": rid, "method": "session/list", "params": {}},
        ):
            if proc.poll() is not None:
                break
            send(frame, wait=4.0, mark=frame.get("method", "?"))
            rid += 1

    print("ALIVE=%s" % (proc.poll() is None))
    proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
