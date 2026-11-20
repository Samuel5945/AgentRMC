"""OpenSquilla 宿主真删端到端（2026-11-20）：派发页行「删除」→ 原生确认框 →
桥取件 → 网关 sessions.delete → ack → resolve 即拒。

前置：新版 AgentRMC 在跑；桥是 2026-11-20 版（带真删取件）。脚本自己建
CLI 牺牲会话（不发消息），等 75s 过掉 60s 活跃闸再动手；全程只碰这一条。
"""
import ctypes, ctypes.wintypes as wt, json, os, subprocess, sys, time, urllib.request, uuid

ctypes.windll.shcore.SetProcessDpiAwareness(2)
user32 = ctypes.windll.user32
import comtypes.client
comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation, IUIAutomationInvokePattern

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
TITLE = "AgentRMC 真删探针 " + uuid.uuid4().hex[:8]

GW_JS = """
const ws=new WebSocket('ws://127.0.0.1:18791/ws');let seq=0;const p=new Map();
const call=(m,pp)=>new Promise((res,rej)=>{const id='e-'+(++seq);p.set(id,{res,rej});ws.send(JSON.stringify({type:'req',id,method:m,params:pp}));setTimeout(()=>{if(p.delete(id))rej(new Error(m+' timeout'))},15000)});
ws.onmessage=async ev=>{const f=JSON.parse(ev.data);
 if(f.type==='event'&&f.event==='connect.challenge'){ws.send(JSON.stringify({type:'req',id:'c-'+(++seq),method:'connect',params:{minProtocol:1,maxProtocol:3,role:'operator',scopes:['operator.admin'],client:{id:'agentrmc-e2e',version:'1.0',platform:'win32',mode:'tui'}}}));return}
 if(f.type==='hello-ok'){
  const mode=process.argv[2];
  try{
   if(mode==='create'){const c=await call('sessions.create',{agentId:'main',kind:'cli',displayName:process.argv[3]});console.log(c.key);}
   else{const k=process.argv[3];try{await call('sessions.resolve',{key:k});console.log('STILL_THERE')}catch{console.log('GONE')}}
   process.exit(0);
  }catch(e){console.log('ERR '+e.message);process.exit(1)}
  return}
 if(f.type==='res'&&p.has(f.id)){const x=p.get(f.id);p.delete(f.id);f.ok?x.res(f.payload):x.rej(new Error(JSON.stringify(f.error)))}};
setTimeout(()=>{console.log('HELLO TIMEOUT');process.exit(1)},20000);
"""
JS_PATH = os.path.join(os.environ.get("TEMP", "."), "_e2e_osq_gw.mjs")
with open(JS_PATH, "w", encoding="utf-8") as f:
    f.write(GW_JS)

def node(*args):
    r = subprocess.run(["node", JS_PATH, *args], capture_output=True, text=True, timeout=40)
    return (r.stdout or "").strip()

CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

def top_title(h, want):
    n = user32.GetWindowTextLengthW(h)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(h, b, n + 1)
    return b.value == want

key = node("create", TITLE)
assert key.startswith("agent:main:cli:"), key
print("sacrifice:", key)

# 清场：上一轮残留的「删除会话」确认框（模态）会盖住整页，先 Esc 关掉。
def close_stray_dialogs():
    victims = []
    def ed(h, _):
        if user32.IsWindowVisible(h) and top_title(h, "删除会话"):
            victims.append(h)
        return True
    user32.EnumWindows(CB(ed), 0)
    for h in victims:
        user32.SetForegroundWindow(h)
        time.sleep(0.2)
        user32.keybd_event(0x1B, 0, 0, 0); user32.keybd_event(0x1B, 0, 2, 0)
        time.sleep(0.3)
    return len(victims)
close_stray_dialogs()
print("wait 75s（过 60s 活跃窗口）…", flush=True)
time.sleep(75)

close_stray_dialogs()  # 等窗口期间又冒出来的也关掉

CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def top_title(h, want):
    n = user32.GetWindowTextLengthW(h)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(h, b, n + 1)
    return b.value == want

tops = []
def en(h, _):
    if user32.IsWindowVisible(h) and top_title(h, "AgentRMC Native"):
        tops.append(h)
    return True
user32.EnumWindows(CB(en), 0)
assert tops, "AgentRMC window not found"
hwnd = tops[0]
if user32.IsIconic(hwnd):
    user32.ShowWindow(hwnd, 9)
user32.SetForegroundWindow(hwnd)
time.sleep(1.0)

kids = []
@CB
def ce(h, _):
    c = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, c, 64)
    kids.append((h, c.value))
    return True
user32.EnumChildWindows(hwnd, ce, 0)
web = next((h for h, c in kids if c == "Chrome_WidgetWin_1"), None)
assert web, "WebView2 child not found"
walker = ui.ControlViewWalker

def scan(root_el, pred, limit=8000, depth=26):
    out = []
    stack = [(root_el, 0)]
    seen = 0
    while stack and seen < limit:
        el, d = stack.pop()
        seen += 1
        try:
            if pred(el):
                out.append(el)
        except Exception:
            pass
        if d >= depth:
            continue
        ch = walker.GetFirstChildElement(el)
        while ch:
            stack.append((ch, d + 1))
            ch = walker.GetNextSiblingElement(ch)
    return out

def name(el):
    try:
        return el.CurrentName or ""
    except Exception:
        return ""

def center(el):
    r = el.CurrentBoundingRectangle
    return (r.left + (r.right - r.left) // 2, r.top + (r.bottom - r.top) // 2)

def click_pt(x, y):
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.1)
    user32.SetCursorPos(x, y); time.sleep(0.15)
    user32.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.4)

def click(el):
    click_pt(*center(el))

root = ui.ElementFromHandle(web)
# 切到派发页签（第 2 个 tab）；comtypes 绑不了 GetCurrentPatternAs，全部坐标点。
tabs = scan(root, lambda e: name(e) == "派发" and e.CurrentControlType == 0xC350)
assert tabs, "dispatch tab not found"
click(tabs[0])
time.sleep(1.0)
root = ui.ElementFromHandle(web)
# 选中 OpenSquilla 宿主 chip（chip 的 Name 是「OpenSquilla 条数」拼接，且首次
# 进页要等目录加载，重试着找）
chips = []
t0 = time.time()
while time.time() - t0 < 25 and not chips:
    root = ui.ElementFromHandle(web)
    chips = scan(root, lambda e: name(e).startswith("OpenSquilla") and e.CurrentControlType == 0xC350)
    if not chips:
        time.sleep(1.5)
assert chips, "OpenSquilla agent chip not found"
click(chips[0])
time.sleep(0.6)
root = ui.ElementFromHandle(web)
rf = scan(root, lambda e: "刷新会话" in name(e) and e.CurrentControlType == 0xC350)
if rf:
    click(rf[0])
    time.sleep(1.0)
row = None
t0 = time.time()
expanded = False
while time.time() - t0 < 40 and row is None:
    root = ui.ElementFromHandle(web)
    hits = scan(root, lambda e: TITLE in name(e))
    row = hits[0] if hits else None
    if row is None and not expanded:
        # 没项目路径的新会话收在默认折叠的「未分类任务」组里（组头 Name 带
        # 「›」前缀，只能用 in 匹配），点开再找。
        heads = scan(root, lambda e: "未分类任务" in name(e) and e.CurrentControlType == 0xC350)
        if heads:
            click(heads[0])
            expanded = True
            time.sleep(0.8)
            continue
    if row is None:
        time.sleep(1.5)
assert row is not None, "sacrifice row not found in dispatch list"
# 行内「删除」按钮：子树里找
btn = None
t0 = time.time()
while time.time() - t0 < 10 and btn is None:
    root = ui.ElementFromHandle(web)
    hits = scan(root, lambda e: name(e) == "删除" and e.CurrentControlType == 0xC350)
    # 只挑目标行里的：行与其按钮同在页面树，用包围盒包含判断
    r = row.CurrentBoundingRectangle
    for el in hits:
        b = el.CurrentBoundingRectangle
        if r.left <= b.left and b.right <= r.right and r.top <= b.top and b.bottom <= r.bottom:
            btn = el
            break
    if btn is None:
        time.sleep(0.8)
assert btn is not None, "delete button on target row not found"
click(btn)
print("row delete clicked", flush=True)
time.sleep(1.4)

# 原生确认框（顶层 Qt 窗口，标题「删除会话」）
dlg = None
t0 = time.time()
while time.time() - t0 < 10 and dlg is None:
    found = []
    def ed(h, _):
        global dlg
        if user32.IsWindowVisible(h) and top_title(h, "删除会话"):
            found.append(h)
        return True
    user32.EnumWindows(CB(ed), 0)
    dlg = found[0] if found else None
    if dlg is None:
        time.sleep(0.5)
assert dlg, "confirm dialog not found"
droot = ui.ElementFromHandle(dlg)
# 用只向下的 scan 找按钮（手摇栈会在窗口根 GetNextSibling 串到别的顶层窗，
# 实测把页面行里的「删除」当成了确认钮）。
hits = scan(droot, lambda e: e.CurrentControlType == 0xC350 and
            name(e) in ("按宿主接口删除", "移入回收站", "删除"))
choice = hits[0] if hits else None
assert choice, "confirm button not found"
print("dialog button:", name(choice), flush=True)
user32.SetForegroundWindow(dlg)
time.sleep(0.2)
_dx, _dy = center(choice)
user32.SetCursorPos(_dx, _dy); time.sleep(0.15)
user32.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.06)
user32.mouse_event(0x0004, 0, 0, 0, 0)

gone = False
t0 = time.time()
while time.time() - t0 < 40 and not gone:
    out = node("check", key)
    gone = out == "GONE"
    if not gone:
        time.sleep(2.5)
os.remove(JS_PATH)
print("HOST_DELETE:", "PASS" if gone else "FAIL")
sys.exit(0 if gone else 1)
