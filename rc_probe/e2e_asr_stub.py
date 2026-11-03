"""Stub OpenAI-compatible ASR endpoint for the unattended handoff E2E.

POST /v1/chat/completions -> always returns MARKER as the transcription.
Logs each request (time, path, body bytes) to e2e_asr_stub.log so the ASR leg
of the product chain is provable. Loopback only; kill after the test.
"""
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MARKER = ("这是一条 AgentRMC 派活链路自测消息：请只回复「收到」两个字，"
          "不要调用任何工具，不要修改任何文件。")
LOG = r"D:\githubrepos\Agentrmc\rc_probe\e2e_asr_stub.log"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        with open(LOG, "a", encoding="utf-8") as f:
            f.write("%s POST %s bytes=%d\n" % (
                time.strftime("%H:%M:%S"), self.path, len(body)))
        resp = {"choices": [{"message": {"role": "assistant",
                                         "content": MARKER}}]}
        data = json.dumps(resp).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    with open(LOG, "a", encoding="utf-8") as f:
        f.write("%s STUB_START port=45999\n" % time.strftime("%H:%M:%S"))
    ThreadingHTTPServer(("127.0.0.1", 45999), Handler).serve_forever()
