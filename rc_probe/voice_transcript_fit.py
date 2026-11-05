"""语音页转写框「最后一行被裁半截」像素臂（真机，零副作用）。

只读用户报的那个框：唤醒主窗 → 切到嵌入语音页 → 截窗口 → 按 UIA 给的框
矩形量里面每一行文字的墨迹高度。被裁的那一行会比正常行矮（旧版实测：
正常行 23px、被裁行 18px），所以判据是「最后一行 ≥ 最高行的 70%」，
而不是「文字离下边框还有多远」——旧版文字正好贴着裁切线，后者会假绿。

用法： python rc_probe/voice_transcript_fit.py
输出： TRANSCRIPT_FIT verdict=PASS|FAIL ...（FAIL 退出码 2）
"""
import subprocess
import sys
import time
import pathlib
import ctypes
import ctypes.wintypes as wt
from ctypes import byref, create_unicode_buffer

# 副屏不同 DPI 时 system-aware 进程的 GetWindowRect 会被虚拟化→截图错位。
ctypes.windll.shcore.SetProcessDpiAwareness(2)

import comtypes.client  # noqa: E402
import numpy as np  # noqa: E402
from PIL import ImageGrab  # noqa: E402

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation, IUIAutomation)

user32 = ctypes.windll.user32
SHOT = pathlib.Path(__file__).parent / "voice_transcript_fit.png"


def fail(reason):
    print("TRANSCRIPT_FIT verdict=FAIL reason=%s" % reason)
    sys.exit(2)


pids = set()
raw = subprocess.check_output(
    ["tasklist", "/FI", "IMAGENAME eq AgentRMCNative.exe", "/FO", "CSV",
     "/NH"]).decode("gbk", "replace")
for row in raw.splitlines():
    parts = row.split('","')
    if len(parts) >= 3 and parts[0].strip('"') == "AgentRMCNative.exe":
        pids.add(int(parts[1].strip('"')))
if not pids:
    fail("no_process")

wins = []
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def cb(h, l):
    p = wt.DWORD()
    user32.GetWindowThreadProcessId(h, byref(p))
    if p.value in pids:
        n = user32.GetWindowTextLengthW(h)
        b = create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, b, n + 1)
        wins.append((h, b.value))
    return True


user32.EnumWindows(CB(cb), 0)
main = [h for h, t in wins if t == "AgentRMC Native"]
if not main:
    fail("no_main_window:%r" % (wins,))
hwnd = main[0]

# 唤醒 = 双击图标那条消息；隐藏态的窗口截图是洞，必须先拉回屏内。
user32.PostMessageW(hwnd, user32.RegisterWindowMessageW(
    "AgentRMC.Native.Wake"), 0, 0)
time.sleep(1.2)

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
root = ui.ElementFromHandle(hwnd)


def walk():
    return root.FindAll(4, ui.CreateTrueCondition())


def find(name=None, ct=None, aid_suffix=None):
    arr = walk()
    for i in range(arr.Length):
        el = arr.GetElement(i)
        if name is not None and (el.CurrentName or "") != name:
            continue
        if ct is not None and el.GetCurrentPropertyValue(30003) != ct:
            continue
        if aid_suffix is not None and not (
                el.GetCurrentPropertyValue(30011) or "").endswith(aid_suffix):
            continue
        return el
    return None


def click(el):
    r = el.CurrentBoundingRectangle
    pt = wt.POINT()
    user32.GetCursorPos(byref(pt))
    old = (pt.x, pt.y)
    user32.SetCursorPos((r.left + r.right) // 2, (r.top + r.bottom) // 2)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.SetCursorPos(*old)
    time.sleep(0.6)


label = find(aid_suffix="voiceTranscript")
if label is None:
    fail("no_transcript_label")
# 遥控器形态下没有语音页：点左栏「语音」切过去（切完再查一次）。
if label.CurrentBoundingRectangle.right <= 0:
    fail("label_offscreen")
btn = find(name="语音", ct=50000)
if btn is not None:
    click(btn)
    label = find(aid_suffix="voiceTranscript")
    if label is None:
        fail("no_label_after_toggle")

rect = label.CurrentBoundingRectangle
text = label.CurrentName or ""

rc = wt.RECT()
user32.GetWindowRect(hwnd, byref(rc))
shot = ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom),
                      all_screens=True)
shot.save(SHOT)
gray = np.asarray(shot.convert("L")).astype(int)
x0, y0 = rect.left - rc.left, rect.top - rc.top
x1, y1 = rect.right - rc.left, rect.bottom - rc.top
if x1 - x0 < 40 or y1 - y0 < 10:
    fail("label_rect_suspicious:%d,%d,%d,%d" % (x0, y0, x1, y1))
box = gray[y0:y1, x0:x1]
# 深浅色都成立：墨迹 = 与框内中位背景的差值大的像素（旧版按亮度判会在浅色下失效）
bg = int(np.median(box))
ink = np.abs(box - bg) > 60
rows = ink.sum(axis=1)
# 左右各缩进 4px，避开边框和圆角
rows[:4] = 0
rows[-4:] = 0
hits = [y for y in range(len(rows)) if rows[y] > 2]
if not hits:
    fail("no_ink box=%dx%d bg=%d" % (x1 - x0, y1 - y0, bg))

runs = []
start = prev = hits[0]
for y in hits[1:]:
    if y - prev > 3:
        runs.append((start, prev))
        start = y
    prev = y
runs.append((start, prev))
heights = [b - a + 1 for a, b in runs]
# 裁切线 = 框高 - 箱体厚度（QSS padding 10 + border 1，逻辑像素，随 DPI 缩放）。
# 被裁的最后一行必然正好贴在这条线上（旧版实测 slack=1），放得下则至少留出一个
# 字体下沉的余量。绝对阈值会被 DPI/字号吃掉，跟裁切线作差不会。
dpi = user32.GetDpiForWindow(hwnd)
scale = dpi / 96.0 if dpi else 1.0
chrome = int(round(11 * scale))
content_bottom = (y1 - y0) - chrome
slack = content_bottom - runs[-1][1]
ratio = heights[-1] / float(heights[0])
print("TRANSCRIPT_FIT label=%dx%d dpi=%d chrome=%d text=%r" %
      (x1 - x0, y1 - y0, dpi, chrome, text[:44]))
print("TRANSCRIPT_FIT runs=%s heights=%s content_bottom=%d last_ink=%d "
      "slack=%d ratio=%.2f" % (runs, heights, content_bottom, runs[-1][1],
                               slack, ratio))
if len(runs) < 2:
    fail("expected_wrapped_placeholder lines=%d" % len(runs))
# 两条信号都要过：不贴裁切线（slack）+ 最后一行不缺斤短两（ratio）。
# 旧版真机读数：slack=1、ratio=0.82 → FAIL；修后：slack=6、ratio=0.97 → PASS。
verdict = "PASS" if slack >= 4 and ratio >= 0.85 else "FAIL"
print("TRANSCRIPT_FIT verdict=%s slack=%d(need>=4) ratio=%.2f(need>=0.85)" %
      (verdict, slack, ratio))
sys.exit(0 if verdict == "PASS" else 2)
