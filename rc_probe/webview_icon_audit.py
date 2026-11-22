"""WebView2 应用页图标审计：CDP 直连 WebView2 子窗，DOM 查 .appcard img 的
src 长度 / naturalWidth——真实图标 base64 远大于占位图（>2KB vs <1KB）。
用法: python rc_probe/webview_icon_audit.py
"""
import json
import re
import sys
import time
import urllib.request

sys.path.insert(0, str(pathlib_path := __import__("pathlib").Path(__file__).parent))
import apps_page_shot as aps  # noqa: E402
import ctypes  # noqa: E402
import ctypes.wintypes as wt  # noqa: E402
import pathlib  # noqa: E402

user32 = ctypes.windll.user32


def cdp_call(ws, mid, method, params=None):
    ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == mid:
            return msg


def main():
    try:
        import websocket
    except ImportError:
        print("pip install websocket-client")
        sys.exit(2)

    hwnd = aps.find_main_hwnd()
    if hwnd is None:
        print("NO_MAIN")
        sys.exit(1)
    user32.ShowWindow(hwnd, 9)
    user32.ShowWindow(hwnd, 5)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)

    # 找 webview2 子窗（Chrome_WidgetWin），拿它的进程 → 找 CDP 端口
    # WebView2 默认不开 CDP；但 AgentRMC 若带 --remote-debugging-port 才行。
    # 换路：用 msedgewebview2 的 user data dir 里的 DevToolsActivePort 文件。
    import os
    base = os.path.expandvars(
        r"%LOCALAPPDATA%\AgentRMC\EBWebView\DevToolsActivePort")
    port = None
    if os.path.exists(base):
        port = open(base).read().splitlines()[0].strip()
    else:
        # 安装目录形态的 webview 数据目录
        for root, dirs, files in os.walk(
                os.path.expandvars(r"%LOCALAPPDATA%")):
            if "DevToolsActivePort" in files and "EBWebView" in root:
                port = open(os.path.join(root, "DevToolsActivePort")).read().splitlines()[0]
                break
    if not port:
        print("NO_CDP_PORT (WebView2 未开调试口)")
        sys.exit(1)
    print("CDP port", port)
    targets = json.load(urllib.request.urlopen(
        f"http://127.0.0.1:{port}/json/list", timeout=5))
    page = next((t for t in targets if t.get("type") == "page"), None)
    if not page:
        print("NO_PAGE_TARGET", [t.get("url") for t in targets][:5])
        sys.exit(1)
    ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=10)

    # 确保在应用页签
    cdp_call(ws, 1, "Runtime.evaluate", {
        "expression": "document.querySelector('#tabs') && [...document.querySelectorAll('#tabs button')].find(b=>b.textContent.includes('应用'))?.click(); 'clicked'",
        "returnByValue": True})
    time.sleep(1.0)

    # 抓所有卡片图标状态
    res = cdp_call(ws, 2, "Runtime.evaluate", {
        "expression": """
        JSON.stringify([...document.querySelectorAll('#apList .appcard')].map(c=>{
          const im=c.querySelector('img');
          return {name:(c.querySelector('.aname')||{}).textContent,
                  hasImg: !!im,
                  srcLen: im ? (im.src||'').length : 0,
                  natW: im ? im.naturalWidth : 0};
        }))
        """,
        "returnByValue": True})
    ws.close()
    data = json.loads(res["result"]["result"]["value"])
    real = ph = 0
    for card in data:
        big = card["srcLen"] > 2000 and card["natW"] > 0
        if big:
            real += 1
        else:
            ph += 1
        print(f'{card["name"]:>14} img={card["hasImg"]} len={card["srcLen"]:6d} natW={card["natW"]:3d} {"REAL" if big else "placeholder"}')
    print(f"REAL={real}/{len(data)}")


if __name__ == "__main__":
    main()
