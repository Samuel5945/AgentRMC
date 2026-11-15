"""局域网手机遥控（M1）端到端探针：外壳系统页真实入口 → 配对 → 鉴权 → 吊销。

链路（2026-11-13，移动端 M1 验收）：
  1. UIA 打开外壳「系统」页，确认「手机访问」监听中（关着就拨开）；
  2. 主动刷新配对码（先收起再显示，避免读到过期码），读码与 LAN 网址；
  3. HTTP 走 LAN 网址：静态页免鉴权 / 无令牌 401 / 错码 401 / 正码换 cookie /
     白名单外动作 403 / 缺 CSRF 头 403 / 主题路径越界 404 / 主题素材 200；
  4. 桥 45915 从 LAN 地址连必须拒连（回环锁死）；
  5. UIA 只吊销探针自己的设备（按名字定位行）→ 旧 cookie 立即 401。

收尾保持开关开启：用户真机可能正连着，别替用户关。
前置：新版 AgentRMC 已装机并在跑，主窗可见（隐藏时先再启动一次唤醒）。
用法: python rc_probe/lan_web_e2e.py
"""
import ctypes
import ctypes.wintypes as wt
import json
import re
import socket
import sys
import time
import urllib.error
import urllib.request

ctypes.windll.shcore.SetProcessDpiAwareness(2)
user32 = ctypes.windll.user32

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,  # noqa: E402
                                             IUIAutomationInvokePattern)

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
walker = ui.ControlViewWalker
DEPTH_MAX = 26
FAILS = []
PROBE_DEVICE = "E2E 探针手机"


def check(cond, what, extra=""):
    print(("PASS " if cond else "FAIL ") + what + (f" | {extra}" if extra else ""))
    if not cond:
        FAILS.append(what)


def http(method, url, body=None, headers=None):
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(req, timeout=5) as r:
            return r.status, dict(r.headers), r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode("utf-8", "replace")
    except Exception as e:
        return 0, {}, str(e)


def tcp_open(host, port, timeout=2.0):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


# —— 找 AgentRMC 主窗并拉前台（WebView2 的 a11y 树要前台被查询才建）——
_CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
tops = []


def enum_(h, _):
    if user32.IsWindowVisible(h):
        n = user32.GetWindowTextLengthW(h)
        b = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, b, n + 1)
        if b.value == "AgentRMC Native":
            tops.append(h)
    return True


user32.EnumWindows(_CB(enum_), 0)
if not tops:
    print("AgentRMC main window not visible (launch the exe once more to wake)")
    sys.exit(1)
hwnd = tops[0]
if user32.IsIconic(hwnd):
    user32.ShowWindow(hwnd, 9)
user32.SetForegroundWindow(hwnd)
time.sleep(1.2)

_children = []


@ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def _child_enum(h, _):
    cls = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, cls, 64)
    _children.append((h, cls.value))
    return True


user32.EnumChildWindows(hwnd, _child_enum, 0)
web = next((h for h, c in _children if c == "Chrome_WidgetWin_1"), None)
if web is None:
    print("WebView2 Chrome child not found (Qt fallback? run with shell)")
    sys.exit(1)
root = ui.ElementFromHandle(web)


def find_by_name_substr(substr, limit=12000):
    out = []
    stack = [(root, 0)]
    seen = 0
    while stack and seen < limit:
        el, d = stack.pop()
        seen += 1
        try:
            nm = el.CurrentName or ""
        except Exception:
            nm = ""
        if nm and substr in nm:
            out.append(el)
        if d >= DEPTH_MAX:
            continue
        try:
            child = walker.GetFirstChildElement(el)
        except Exception:
            continue
        while child:
            stack.append((child, d + 1))
            try:
                child = walker.GetNextSiblingElement(child)
            except Exception:
                break
    return out


def center(el):
    r = el.CurrentBoundingRectangle
    return (r.left + (r.right - r.left) // 2, r.top + (r.bottom - r.top) // 2)


def click(x, y):
    user32.SetCursorPos(x, y)
    time.sleep(0.15)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.6)


def invoke_el(el):
    try:
        # GetCurrentPattern 返回 IUnknown，再 QI 成 InvokePattern。
        # （GetCurrentPatternAs 在这版 comtypes 里返回裸指针 int，不好用。）
        # UIA Invoke 直接派发 DOM click，不依赖前台/坐标，元素被裁切也能点。
        unk = el.GetCurrentPattern(10000)  # UIA_InvokePatternId
        unk.QueryInterface(IUIAutomationInvokePattern).Invoke()
        return True
    except Exception:
        try:
            click(*center(el))
            return True
        except Exception:
            return False


def activate(substr, tries=8):
    for _ in range(tries):
        els = find_by_name_substr(substr)
        btns = [e for e in els if e.CurrentControlType == 0xC350]
        el = btns[0] if btns else (els[0] if els else None)
        if el is not None and invoke_el(el):
            return True
        time.sleep(0.8)
    return False


def text_of(substr, tries=10):
    for _ in range(tries):
        for el in find_by_name_substr(substr):
            try:
                nm = el.CurrentName
                if nm and substr in nm:
                    return nm
            except Exception:
                pass
        time.sleep(0.8)
    return ""


# 1) 系统页。
check(activate("系统"), "open system tab")
time.sleep(0.8)

# 2) 确认监听中；关着就拨开关。
if not text_of("监听中", tries=3):
    check(activate("开启后局域网"), "toggle lan on")
    time.sleep(1.0)
check(bool(text_of("监听中", tries=6)), "lan listening")

# 3) 刷新配对码：先收起（若在展示）再显示，保证拿到新鲜码。
if find_by_name_substr("收起二维码"):
    activate("收起二维码")
    time.sleep(0.6)
check(activate("显示配对二维码"), "show pairing qr")
time.sleep(0.8)
code_text = text_of("配对码 ")
m = re.search(r"[0-9a-f]{32}", code_text)
check(m is not None, "pairing code readable", code_text[:60])
if not m:
    sys.exit(1)
code = m.group(0)
url_text = text_of("手机浏览器打开")
urls = re.findall(r"http://[0-9.]+:\d+/m/", url_text)
check(bool(urls), "lan url readable", url_text[:80])
if not urls:
    sys.exit(1)
base = re.match(r"http://[0-9.]+:\d+", urls[0]).group(0)
host = re.match(r"http://([0-9.]+):", base).group(1)
port = int(re.match(r"http://[0-9.]+:(\d+)", base).group(1))
print(f"lan base: {base} (host={host} port={port})")

# 4) HTTP 链路（走 LAN 地址，不是回环）。
st, _, body = http("GET", base + "/m/")
check(st == 200 and "配对" in body, "GET /m/ static page", str(st))
st, _, _ = http("GET", base + "/m/state")
check(st == 401, "state without token 401", str(st))
st, _, _ = http("POST", base + "/m/pair", {"code": "0" * 32},
                {"X-Requested-With": "fetch"})
check(st == 401, "wrong code 401", str(st))
st, hd, _ = http("POST", base + "/m/pair", {"code": code, "name": PROBE_DEVICE},
                 {"X-Requested-With": "fetch"})
cookie = hd.get("Set-Cookie", "")
token = ""
cm = re.search(r"armc_dev=([0-9a-f]+)", cookie)
if cm:
    token = cm.group(1)
check(st == 200 and bool(token), "pair exchanges token", str(st))
check("HttpOnly" in cookie and "SameSite=Strict" in cookie, "cookie flags", cookie)
if not token:
    sys.exit(1)
auth = {"Cookie": f"armc_dev={token}"}
st, _, body = http("GET", base + "/m/state", headers=auth)
ok_json = False
try:
    state = json.loads(body)
    ok_json = "work" in state and "meta" in state and "theme" in state
except Exception:
    pass
check(st == 200 and ok_json, "state with cookie", str(st))
check(code not in body and "lan" not in (state if ok_json else {}),
      "no pairing code leaks to phone")
st, _, _ = http("POST", base + "/m/action", {"cmd": "sysUpgrade"},
                {"X-Requested-With": "fetch", **auth})
check(st == 403, "non-whitelisted action 403", str(st))
st, _, _ = http("POST", base + "/m/action", {"cmd": "ack", "id": "x"}, auth)
check(st == 403, "action without csrf header 403", str(st))
st, _, _ = http("GET", base + "/m/theme/..%2F..%2Fconfig.json", headers=auth)
check(st == 404, "theme traversal 404", str(st))
st, _, _ = http("GET", f"http://{host}:45915/state")
check(st == 0, "bridge refuses LAN address (loopback only)", str(st))

# 5) 只吊销探针设备：Chromium 会把 .landev 行 div 拍扁，按祖先找按钮会抓到
# 别行的（实测误吊销过用户设备）。改用坐标：同一行的名字与按钮垂直居中，
# Y 坐标一致；行距 ~25px，取 |dy|<12 的那个按钮。
revoked = False
deadline = time.time() + 12
while not revoked and time.time() < deadline:
    names = find_by_name_substr(PROBE_DEVICE)
    if names:
        try:
            nr = names[0].CurrentBoundingRectangle
            ny = nr.top + (nr.bottom - nr.top) / 2
        except Exception:
            ny = None
        if ny is not None:
            for b in find_by_name_substr("吊销"):
                if b.CurrentControlType != 0xC350:
                    continue
                try:
                    br = b.CurrentBoundingRectangle
                    by = br.top + (br.bottom - br.top) / 2
                except Exception:
                    continue
                if abs(by - ny) < 12 and invoke_el(b):
                    revoked = True
                    break
    if not revoked:
        time.sleep(1.0)
check(revoked, "revoke probe device via UI")
# 点击 → Chromium → Qt → 落盘有一条异步链，轮询等令牌失效，别单次抢跑。
st = 200
deadline = time.time() + 10
while time.time() < deadline:
    st, _, _ = http("GET", base + "/m/state", headers=auth)
    if st == 401:
        break
    time.sleep(0.5)
check(st == 401, "revoked token 401", str(st))
check(tcp_open(host, port), "lan stays on for real devices")

print()
if FAILS:
    print(f"E2E FAIL: {len(FAILS)} step(s): {FAILS}")
    sys.exit(1)
print("E2E PASS lan-web")