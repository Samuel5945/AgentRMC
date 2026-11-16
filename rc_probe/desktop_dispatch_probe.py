"""Desktop shell smoke probe for the new 「派发」 tab.

Read-only: opens the tab, waits for the session catalog, prints visible UIA names
and grabs a screenshot. Does not dispatch anything.

Usage:
  python rc_probe/desktop_dispatch_probe.py [shot-tag]
"""
import ctypes
ctypes.windll.shcore.SetProcessDpiAwareness(2)
import pathlib
import sys
import time
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

import comtypes.client
from PIL import ImageGrab

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation, IUIAutomation, IUIAutomationInvokePattern)

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, user32  # noqa: E402

TAG = sys.argv[1] if len(sys.argv) > 1 else "desktop_dispatch"
ROOT = pathlib.Path(r"D:\githubrepos\Agentrmc\rc_probe\desktop_shots")
ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
pids = set(pids_for("AgentRMCNative.exe"))


def windows():
    out = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            b = create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, b, n + 1)
            out.append((h, b.value))
        return True
    user32.EnumWindows(CB(cb), 0)
    return out


def elements(hwnd):
    root = ui.ElementFromHandle(hwnd)
    arr = root.FindAll(4, ui.CreateTrueCondition())
    return [arr.GetElement(i) for i in range(arr.Length)]


def click(el):
    r = el.GetCurrentPropertyValue(30001)
    l, t, w, h = (tuple(r) if isinstance(r, (tuple, list))
                  else (r.left, r.top, r.right - r.left, r.bottom - r.top))
    pt = wt.POINT()
    user32.GetCursorPos(byref(pt))
    old = (pt.x, pt.y)
    user32.SetCursorPos(int(l + w / 2), int(t + h / 2))
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    user32.SetCursorPos(*old)
    time.sleep(0.8)


def click_name(hwnd, name):
    for el in elements(hwnd):
        if (el.CurrentName or "") == name and el.GetCurrentPropertyValue(30003) == 50000:
            try:
                unk = el.GetCurrentPattern(10000)
                unk.QueryInterface(IUIAutomationInvokePattern).Invoke()
            except Exception:
                click(el)
            return True
    return False


def shot(hwnd, tag):
    ROOT.mkdir(parents=True, exist_ok=True)
    r = wt.RECT()
    user32.GetWindowRect(hwnd, byref(r))
    path = ROOT / f"{tag}.png"
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom),
                   all_screens=True).save(path)
    print("shot", path)


def click_contains(hwnd, needle):
    for el in elements(hwnd):
        if needle not in (el.CurrentName or ""):
            continue
        # WebView 的会话行常把「最近更新」暴露成 Text 子节点，直接点文字有时
        # 不冒泡到 row；往上找可点击的祖先再点。
        target = el
        try:
            walker = ui.ControlViewWalker
            for _ in range(4):
                parent = walker.GetParentElement(target)
                if parent is None:
                    break
                target = parent
                if target.GetCurrentPropertyValue(30003) == 50000:
                    break
        except Exception:
            pass
        try:
            unk = target.GetCurrentPattern(10000)
            unk.QueryInterface(IUIAutomationInvokePattern).Invoke()
        except Exception:
            click(target)
        return True
    return False


def topmost(hwnd, on):
    # 不移动尺寸，只临时压到最上层，避免设置/资源管理器窗口盖住截图。
    user32.SetWindowPos(hwnd, -1 if on else -2, 0, 0, 0, 0, 0x0001 | 0x0002)


wins = windows()
main = [h for h, title in wins if title == "AgentRMC Native"]
if not main:
    print("NO_MAIN")
    sys.exit(1)
hwnd = main[0]
user32.keybd_event(0x12, 0, 0, 0)
user32.keybd_event(0x12, 0, 2, 0)
user32.SetForegroundWindow(hwnd)
topmost(hwnd, True)
time.sleep(0.6)

if not click_name(hwnd, "派发"):
    # WebView DOM tabs can surface as text rather than a Button; click by name.
    found = [el for el in elements(hwnd) if (el.CurrentName or "") == "派发"]
    if not found:
        print("NO_DISPATCH_TAB")
        sys.exit(1)
    click(found[0])
time.sleep(2.0)

names = []
for el in elements(hwnd):
    name = el.CurrentName or ""
    if name and any(k in name for k in ("项目", "会话", "已归档", "未分组", "WorkBuddy", "刷新会话")):
        names.append(name)
print("visible dispatch labels:")
for name in names[:30]:
    print(" -", name)
if "--dump" in sys.argv:
    print("uia dump:")
    for el in elements(hwnd):
        nm = el.CurrentName or ""
        if nm:
            print(" ", el.GetCurrentPropertyValue(30003), repr(nm[:120]))
shot(hwnd, TAG)

# 归档清理界面冒烟：进入多选、选一条，只截图，不发删除动作。
if "--clean" in sys.argv:
    click_name(hwnd, "退出清理")
    time.sleep(0.2)
    if click_name(hwnd, "清理归档"):
        time.sleep(0.6)
        shot(hwnd, TAG + "-clean")
        if click_contains(hwnd, "，2026-11-12-21-57-51，"):
            time.sleep(0.3)
            counts = [el.CurrentName or "" for el in elements(hwnd)
                      if "已选" in (el.CurrentName or "")]
            print("clean selected:", counts[:3])
            shot(hwnd, TAG + "-clean-selected")
            # 弹出原生确认后只点取消，不执行任何删除。
            if click_name(hwnd, "删除所选"):
                time.sleep(0.8)
                dialogs = [(h, t) for h, t in windows()
                           if t == "删除已归档会话"]
                print("delete confirm dialog:", bool(dialogs))
                if dialogs:
                    click_name(dialogs[0][0], "取消")
                    time.sleep(0.5)
                else:
                    # 兜底：不让空的确认框卡住后续操作。
                    user32.keybd_event(0x1B, 0, 0, 0)
                    user32.keybd_event(0x1B, 0, 2, 0)
        else:
            print("clean: no archived session row found")
        click_name(hwnd, "退出清理")
    else:
        print("clean: no clean button")

# 只读冒烟：点一条可见会话，确认聊天页能打开并加载记录；不填、不派发。
if click_contains(hwnd, "，Agentrmc，"):
    time.sleep(2.0)
    labels = [el.CurrentName or "" for el in elements(hwnd)]
    print("chat open:", any("共 " in n and "条" in n for n in labels))
    for name in labels:
        if "共 " in name or "派发" in name:
            print(" chat:", name[:120])
            break
    shot(hwnd, TAG + "-chat")
topmost(hwnd, False)
