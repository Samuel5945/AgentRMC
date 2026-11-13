"""只生成一个新鲜配对码并打印（给浏览器验证用），不做别的。"""
import ctypes
import ctypes.wintypes as wt
import re
import sys
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)
user32 = ctypes.windll.user32
import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (CUIAutomation, IUIAutomation,
                                             IUIAutomationInvokePattern)

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
walker = ui.ControlViewWalker
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
    print("NO_WINDOW")
    sys.exit(1)
hwnd = tops[0]
user32.SetForegroundWindow(hwnd)
time.sleep(1.0)
ch = []


@ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def ce(h, _):
    c = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, c, 64)
    ch.append((h, c.value))
    return True


user32.EnumChildWindows(hwnd, ce, 0)
web = next(h for h, c in ch if c == "Chrome_WidgetWin_1")
root = ui.ElementFromHandle(web)


def scan(substr):
    out = []
    stack = [(root, 0)]
    seen = 0
    while stack and seen < 12000:
        el, d = stack.pop()
        seen += 1
        try:
            nm = el.CurrentName or ""
        except Exception:
            nm = ""
        if nm and substr in nm:
            out.append(el)
        if d >= 26:
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


def invoke(el):
    try:
        el.GetCurrentPattern(10000).QueryInterface(IUIAutomationInvokePattern).Invoke()
        return True
    except Exception:
        return False


def activate(substr, tries=8):
    for _ in range(tries):
        els = [e for e in scan(substr) if e.CurrentControlType == 0xC350]
        if els and invoke(els[0]):
            return True
        time.sleep(0.7)
    return False


activate("系统")
time.sleep(0.8)
if not scan("监听中"):
    activate("开启后局域网")
    time.sleep(1.0)
if scan("收起二维码"):
    activate("收起二维码")
    time.sleep(0.5)
activate("显示配对二维码")
time.sleep(0.8)
code = ""
for el in scan("配对码 "):
    m = re.search(r"[0-9a-f]{32}", el.CurrentName or "")
    if m:
        code = m.group(0)
        break
urls = []
for el in scan("手机浏览器打开"):
    urls = re.findall(r"http://[0-9.]+:\d+/m/", el.CurrentName or "")
    if urls:
        break
print("CODE=" + code)
print("URL=" + (urls[0] if urls else ""))